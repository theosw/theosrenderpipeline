#pragma once
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace TheosRenderPipeline
{
    // Reads back one real presented frame for a screenshot; generated frames
    // never pass through the host. Record, Submitted and Poll run on the thread
    // that records and submits the presentation copy. A readback that submitted
    // work may still reference is retained until its fence completes, and
    // indefinitely after a device fault.
    class FinalFrameCapture
    {
    public:
        // A completed copy. Map and convert it on any thread; the GPU no longer
        // references the buffer once Poll has returned it.
        struct Readback
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            DXGI_FORMAT format{};
            UINT width{}, height{};
        };
        struct Image
        {
            UINT width{}, height{};
            std::vector<std::uint8_t> bgr; // Tightly packed 24-bit rows.
        };

        // SDR presentation formats only. HDR10 output is PQ-encoded, so a plain
        // 8-bit image of it would have the wrong colours.
        static UINT BytesPerPixel(DXGI_FORMAT format)
        {
            switch (format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            case DXGI_FORMAT_R10G10B10A2_UNORM: return 4;
            default: return 0;
            }
        }

        // Presented values are already display-encoded; alpha is discarded.
        static bool ToBGR(DXGI_FORMAT format, UINT width, UINT height,
            const std::uint8_t* rows, std::size_t pitch, std::vector<std::uint8_t>& out)
        {
            if (!BytesPerPixel(format) || !rows || !width || !height || pitch < std::size_t{width} * 4) { return false; }
            out.resize(std::size_t{width} * height * 3);
            for (UINT y = 0; y < height; ++y) {
                const auto* source = rows + y * pitch;
                auto* destination = out.data() + std::size_t{y} * width * 3;
                for (UINT x = 0; x < width; ++x, source += 4, destination += 3) {
                    switch (format) {
                    case DXGI_FORMAT_R10G10B10A2_UNORM: {
                        const std::uint32_t value = source[0] | source[1] << 8 | source[2] << 16 | std::uint32_t{source[3]} << 24;
                        destination[0] = static_cast<std::uint8_t>((value >> 22) & 0xFF);
                        destination[1] = static_cast<std::uint8_t>((value >> 12) & 0xFF);
                        destination[2] = static_cast<std::uint8_t>((value >> 2) & 0xFF);
                        break;
                    }
                    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                        destination[0] = source[2]; destination[1] = source[1]; destination[2] = source[0];
                        break;
                    default:
                        destination[0] = source[0]; destination[1] = source[1]; destination[2] = source[2];
                        break;
                    }
                }
            }
            return true;
        }

        static HRESULT Read(const Readback& readback, Image& image)
        {
            if (!readback.buffer) { return E_INVALIDARG; }
            // The final row is not padded to RowPitch; read to the buffer's end.
            const D3D12_RANGE range{static_cast<SIZE_T>(readback.footprint.Offset),
                static_cast<SIZE_T>(readback.buffer->GetDesc().Width)};
            void* mapped{};
            auto hr = readback.buffer->Map(0, &range, &mapped);
            if (FAILED(hr)) { return hr; }
            Image result{readback.width, readback.height, {}};
            const bool converted = ToBGR(readback.format, readback.width, readback.height,
                static_cast<const std::uint8_t*>(mapped) + readback.footprint.Offset,
                readback.footprint.Footprint.RowPitch, result.bgr);
            const D3D12_RANGE noWrites{0, 0};
            readback.buffer->Unmap(0, &noWrites);
            if (!converted) { return E_INVALIDARG; }
            image = std::move(result);
            return S_OK;
        }

        bool Busy() const { return recorded_; }

        // Records a copy of `output` after the presentation copy in the same
        // list. The resource enters and leaves COMMON. Nothing is recorded on
        // failure, so the caller's list remains valid.
        HRESULT Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* output)
        {
            if (recorded_) { return E_ILLEGAL_METHOD_CALL; }
            if (!device || !list || !output) { return E_INVALIDARG; }
            const auto desc = output->GetDesc();
            if (!BytesPerPixel(desc.Format) || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                !desc.Width || !desc.Height || desc.Width > 16384 || desc.Height > 16384 ||
                desc.MipLevels != 1 || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1) { return E_INVALIDARG; }
            if (!fence_) {
                const auto hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
                if (FAILED(hr)) { return hr; }
            }
            Readback pending{};
            UINT64 total{};
            device->GetCopyableFootprints(&desc, 0, 1, 0, &pending.footprint, nullptr, nullptr, &total);
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = total; buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            const auto hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pending.buffer));
            if (FAILED(hr)) { return hr; }
            pending.format = desc.Format;
            pending.width = static_cast<UINT>(desc.Width);
            pending.height = desc.Height;

            D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
            list->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = pending.buffer.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = pending.footprint;
            D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = output;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(1, &barrier);
            pending_ = std::move(pending);
            recorded_ = true;
            return S_OK;
        }

        // Only after the list containing Record has been submitted to `queue`.
        HRESULT Submitted(ID3D12CommandQueue* queue)
        {
            if (!recorded_ || submitted_) { return E_ILLEGAL_METHOD_CALL; }
            if (!queue) { return E_INVALIDARG; }
            const auto hr = queue->Signal(fence_.Get(), value_ + 1);
            if (FAILED(hr)) { return fault_ = hr; } // The copy may still be live; keep it.
            ++value_;
            submitted_ = true;
            return S_OK;
        }

        // S_FALSE while idle or in flight. S_OK hands over the completed copy
        // and makes the capture available for the next request.
        HRESULT Poll(Readback& readback)
        {
            if (FAILED(fault_)) { return fault_; }
            if (!submitted_) { return S_FALSE; }
            const auto completed = fence_->GetCompletedValue();
            if (completed == (std::numeric_limits<std::uint64_t>::max)()) { return fault_ = DXGI_ERROR_DEVICE_REMOVED; }
            if (completed < value_) { return S_FALSE; }
            readback = std::move(pending_);
            pending_ = {};
            recorded_ = submitted_ = false;
            return S_OK;
        }

    private:
        Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
        Readback pending_;
        std::uint64_t value_{};
        bool recorded_{}, submitted_{};
        HRESULT fault_{S_OK};
    };
}
