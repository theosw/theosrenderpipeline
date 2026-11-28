#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstring>

namespace TheosRenderPipeline::D3D12Helpers
{
	inline std::uint64_t LuidToUInt64(LUID luid) noexcept
	{
		std::uint64_t value{};
		static_assert(sizeof(value) == sizeof(luid));
		std::memcpy(&value, &luid, sizeof(value));
		return value;
	}

	inline void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
		D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		if (!list || !resource || before == after) { return; }
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
		list->ResourceBarrier(1, &barrier);
	}

	// Issues transitions recorded together as one ResourceBarrier call. Null
	// resources are skipped; a resource must appear at most once per flush.
	class BarrierBatch final
	{
	public:
		explicit BarrierBatch(ID3D12GraphicsCommandList* list) : list_(list) {}
		BarrierBatch& Add(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
		{
			if (!resource || before == after) { return *this; }
			if (count_ == barriers_.size()) { Flush(); }
			auto& barrier = barriers_[count_++];
			barrier = {};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
			return *this;
		}
		void Flush()
		{
			if (count_ && list_) { list_->ResourceBarrier(count_, barriers_.data()); }
			count_ = 0;
		}

	private:
		ID3D12GraphicsCommandList* list_;
		std::array<D3D12_RESOURCE_BARRIER, 8> barriers_{};
		UINT count_{};
	};

	inline HRESULT CreateDefaultTexture2D(ID3D12Device* device, UINT64 width, UINT height,
		DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, ID3D12Resource** out,
		D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE,
		D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON)
	{
		if (!device || !out) { return E_INVALIDARG; }
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = width;
		desc.Height = height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Flags = flags;
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		return device->CreateCommittedResource(&heap, heapFlags, &desc, initialState, nullptr, IID_PPV_ARGS(out));
	}

	// Owns a timestamp query heap and persistently mapped readback buffer.
	// Unmaps with an empty written range on destruction as D3D12 requires.
	class GpuTimestampPool final
	{
	public:
		GpuTimestampPool() = default;
		GpuTimestampPool(const GpuTimestampPool&) = delete;
		GpuTimestampPool& operator=(const GpuTimestampPool&) = delete;
		~GpuTimestampPool() { Reset(); }

		bool Initialize(ID3D12Device* device, UINT queryCount)
		{
			Reset();
			if (!device || !queryCount) { return false; }
			D3D12_QUERY_HEAP_DESC query{};
			query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
			query.Count = queryCount;
			if (FAILED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&heap_)))) { return false; }
			D3D12_HEAP_PROPERTIES readHeap{};
			readHeap.Type = D3D12_HEAP_TYPE_READBACK;
			D3D12_RESOURCE_DESC buffer{};
			buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			buffer.Width = sizeof(std::uint64_t) * queryCount;
			buffer.Height = 1;
			buffer.DepthOrArraySize = 1;
			buffer.MipLevels = 1;
			buffer.SampleDesc.Count = 1;
			buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (FAILED(device->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE, &buffer,
					D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_)))) {
				heap_.Reset();
				return false;
			}
			D3D12_RANGE range{ 0, static_cast<SIZE_T>(buffer.Width) };
			if (FAILED(readback_->Map(0, &range, reinterpret_cast<void**>(&mapped_)))) {
				readback_.Reset();
				heap_.Reset();
				mapped_ = nullptr;
				return false;
			}
			return true;
		}

		void Reset()
		{
			if (readback_ && mapped_) {
				const D3D12_RANGE written{ 0, 0 };
				readback_->Unmap(0, &written);
				mapped_ = nullptr;
			}
			readback_.Reset();
			heap_.Reset();
		}

		explicit operator bool() const { return heap_ && readback_ && mapped_; }
		ID3D12QueryHeap* Heap() const { return heap_.Get(); }
		ID3D12Resource* Readback() const { return readback_.Get(); }
		const std::uint64_t* Values() const { return mapped_; }

	private:
		Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
		Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
		std::uint64_t* mapped_{};
	};
}
