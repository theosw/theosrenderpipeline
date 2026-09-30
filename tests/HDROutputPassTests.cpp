// Executes the production HDR output pass on D3D12 WARP and compares every
// pixel with the CPU reference in HDROutput.h. The key contract is DLSS-G's
// composition: backbuffer = UI + (1 - UI.alpha) * HUD-less, in HDR10 codes.
#include "FrameGen/SourceDLSSGHDROutput.h"
#include <dxgi1_6.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace TheosRenderPipeline;
using namespace TheosRenderPipeline::SourceDLSSG;

static void Require(bool value, const char* why)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static void Check(HRESULT hr, const char* why)
{
    if (FAILED(hr)) { std::fprintf(stderr, "FAIL: %s HRESULT=0x%08lX\n", why, static_cast<unsigned long>(hr)); std::exit(1); }
}

namespace
{
    constexpr UINT W = 64, H = 64;
    bool Overlay(UINT x, UINT y) { return x >= W / 2 - 6 && x < W / 2 && y < 12; }

    struct GPU
    {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        UINT64 value{};
        std::vector<ComPtr<ID3D12Resource>> keep;

        void Submit()
        {
            Check(list->Close(), "close list");
            ID3D12CommandList* lists[]{ list.Get() };
            queue->ExecuteCommandLists(1, lists);
            Check(queue->Signal(fence.Get(), ++value), "signal");
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            Check(fence->SetEventOnCompletion(value, event), "fence event");
            WaitForSingleObject(event, INFINITE);
            CloseHandle(event);
            Check(device->GetDeviceRemovedReason(), "device not removed");
            Check(allocator->Reset(), "reset allocator");
            Check(list->Reset(allocator.Get(), nullptr), "reset list");
            keep.clear();
        }

        ComPtr<ID3D12Resource> Texture(DXGI_FORMAT format, bool renderTarget)
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = W; desc.Height = H;
            desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Format = format;
            desc.Flags = renderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            ComPtr<ID3D12Resource> texture;
            Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                nullptr, IID_PPV_ARGS(&texture)), "texture");
            return texture;
        }

        static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
        {
            D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
            list->ResourceBarrier(1, &barrier);
        }

        ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE type)
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = size; desc.Height = 1;
            desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
            ComPtr<ID3D12Resource> buffer;
            Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&buffer)), "buffer");
            return buffer;
        }

        void Upload(ID3D12Resource* texture, const std::vector<std::uint32_t>& rgba8)
        {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 total{};
            const auto desc = texture->GetDesc();
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
            auto upload = Buffer(total, D3D12_HEAP_TYPE_UPLOAD);
            std::uint8_t* mapped{};
            Check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map upload");
            for (UINT y = 0; y < H; ++y) { std::memcpy(mapped + y * footprint.Footprint.RowPitch, &rgba8[y * W], W * 4); }
            upload->Unmap(0, nullptr);
            D3D12_TEXTURE_COPY_LOCATION dst{ texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
            D3D12_TEXTURE_COPY_LOCATION src{ upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
            src.PlacedFootprint = footprint;
            Barrier(list.Get(), texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Barrier(list.Get(), texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            keep.push_back(upload);
        }

        // Returns rows of raw texels (4 or 8 bytes each).
        std::vector<std::uint8_t> Read(ID3D12Resource* texture, UINT bytesPerTexel)
        {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 total{};
            const auto desc = texture->GetDesc();
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
            auto readback = Buffer(total, D3D12_HEAP_TYPE_READBACK);
            D3D12_TEXTURE_COPY_LOCATION src{ texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
            D3D12_TEXTURE_COPY_LOCATION dst{ readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
            dst.PlacedFootprint = footprint;
            Barrier(list.Get(), texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Barrier(list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            keep.push_back(readback);
            Submit();
            std::vector<std::uint8_t> out(W * H * bytesPerTexel);
            std::uint8_t* mapped{};
            D3D12_RANGE range{ 0, static_cast<SIZE_T>(total) };
            Check(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "map readback");
            for (UINT y = 0; y < H; ++y) { std::memcpy(&out[y * W * bytesPerTexel], mapped + y * footprint.Footprint.RowPitch, W * bytesPerTexel); }
            readback->Unmap(0, nullptr);
            return out;
        }
    };

    std::uint32_t Pack(float r, float g, float b, float a)
    {
        auto q = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
    }
    float Channel8(std::uint32_t v, int c) { return ((v >> (c * 8)) & 0xFF) / 255.0f; }
    HDROutput::RGB Unpack10(const std::vector<std::uint8_t>& data, UINT i)
    {
        std::uint32_t v; std::memcpy(&v, &data[i * 4], 4);
        return { (v & 1023) / 1023.0f, ((v >> 10) & 1023) / 1023.0f, ((v >> 20) & 1023) / 1023.0f };
    }
    std::array<float, 4> Unpack16(const std::vector<std::uint8_t>& data, UINT i)
    {
        std::uint16_t v[4]; std::memcpy(v, &data[i * 8], 8);
        return { v[0] / 65535.0f, v[1] / 65535.0f, v[2] / 65535.0f, v[3] / 65535.0f };
    }
}

int main()
{
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter> warp;
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
    GPU gpu;
    Check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu.device)), "WARP device");
    D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&gpu.queue)), "queue");
    Check(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator)), "allocator");
    Check(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr, IID_PPV_ARGS(&gpu.list)), "list");
    Check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)), "fence");

    // Scene: grey ramp across x, colour families by row band. UI: alpha ramp
    // down y with coloured premultiplied content in the right half.
    std::vector<std::uint32_t> scene(W * H), ui(W * H), composite(W * H);
    for (UINT y = 0; y < H; ++y) {
        for (UINT x = 0; x < W; ++x) {
            const float v = x / float(W - 1);
            const int band = y / 16;
            const float r = band == 1 ? v : band == 2 ? v * 0.3f : v;
            const float g = band == 1 ? v * 0.8f : band == 2 ? v * 0.9f : v;
            const float b = band == 1 ? v * 0.5f : band == 2 ? v : band == 3 ? v * 0.2f : v;
            scene[y * W + x] = Pack(r, g, b, 1);
            const float a = x < W / 2 ? 0.0f : ((y % 16) / 15.0f);
            const float ur = 0.9f * a, ug = 0.85f * a, ub = 0.4f * a;
            ui[y * W + x] = Pack(ur, ug, ub, a);
            // Producer composition in SDR, as TRP's native UI compositor does.
            const auto s = scene[y * W + x], u = ui[y * W + x];
            const float ua = Channel8(u, 3);
            composite[y * W + x] = Pack(Channel8(s, 0) * (1 - ua) + Channel8(u, 0), Channel8(s, 1) * (1 - ua) + Channel8(u, 1),
                Channel8(s, 2) * (1 - ua) + Channel8(u, 2), 1);
            // A late overlay drawn only into the composed frame (not a tagged layer).
            if (Overlay(x, y)) { composite[y * W + x] = Pack(1, 1, 1, 1); }
        }
    }
    auto sceneTexture = gpu.Texture(DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto uiTexture = gpu.Texture(DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto compositeTexture = gpu.Texture(DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto backbuffer = gpu.Texture(HDROutputPass::kOutputFormat, true);
    gpu.Upload(sceneTexture.Get(), scene);
    gpu.Upload(uiTexture.Get(), ui);
    gpu.Upload(compositeTexture.Get(), composite);
    gpu.Submit();

    HDROutputPass pass;
    Require(pass.RecordCompose(gpu.device.Get(), gpu.list.Get(), 0, {}, compositeTexture.Get(), uiTexture.Get(),
        sceneTexture.Get(), backbuffer.Get()) == E_INVALIDARG, "compose rejects missing targets");
    Check(pass.CreateTargets(gpu.device.Get(), W, H), "targets");
    Require(pass.TargetsMatch(W, H) && !pass.TargetsMatch(W, H + 1), "target extent");
    Require(pass.RecordEncode(gpu.device.Get(), gpu.list.Get(), 0, {}, compositeTexture.Get(), compositeTexture.Get()) == E_INVALIDARG,
        "encode rejects a non-RGB10A2 destination");

    const HDROutput::Settings settings{ true, 200.0f, 1000.0f, 150.0f, 1.0f, 0.6f, HDROutput::Transfer::Gamma22 };
    const auto constants = HDROutput::MakeShaderConstants(settings, false);
    Check(pass.RecordCompose(gpu.device.Get(), gpu.list.Get(), 1, constants, compositeTexture.Get(), uiTexture.Get(),
        sceneTexture.Get(), backbuffer.Get()), "record compose");
    gpu.Submit();
    const auto out = gpu.Read(backbuffer.Get(), 4);
    const auto hudless = gpu.Read(pass.HudlessTarget(), 4);
    const auto uiOut = gpu.Read(pass.UITarget(), 8);

    constexpr float code10 = 1.0f / 1023.0f;
    float worstHudless = 0, worstUI = 0, worstIdentity = 0, worstBehind = 0, worstOverlay = 0;
    for (UINT i = 0; i < W * H; ++i) {
        const HDROutput::RGB s{ Channel8(scene[i], 0), Channel8(scene[i], 1), Channel8(scene[i], 2) };
        const auto expectHudless = HDROutput::EncodeScene(s, settings);
        const auto gotHudless = Unpack10(hudless, i);
        const float a = Channel8(ui[i], 3);
        const HDROutput::RGB straight = a > 1.0f / 1024.0f ?
            HDROutput::RGB{ std::clamp(Channel8(ui[i], 0) / a, 0.0f, 1.0f), std::clamp(Channel8(ui[i], 1) / a, 0.0f, 1.0f),
                std::clamp(Channel8(ui[i], 2) / a, 0.0f, 1.0f) } : HDROutput::RGB{};
        const auto expectUI = HDROutput::EncodeUI(straight, settings);
        const auto gotUI = Unpack16(uiOut, i);
        const auto gotOut = Unpack10(out, i);
        for (int c = 0; c < 3; ++c) {
            worstHudless = (std::max)(worstHudless, std::fabs(gotHudless[c] - expectHudless[c]));
            worstUI = (std::max)(worstUI, std::fabs(gotUI[c] - expectUI[c] * a));
            if (Overlay(i % W, i / W)) {
                // The late overlay survives: its pixels are recovered from the composed frame.
                worstOverlay = (std::max)(worstOverlay, std::fabs(gotOut[c] - HDROutput::EncodeScene({ 1, 1, 1 }, settings)[c]));
                continue;
            }
            // DLSS-G composition identity on the actual encoded outputs.
            const float composed = gotUI[c] + (1.0f - gotUI[3]) * gotHudless[c];
            worstIdentity = (std::max)(worstIdentity, std::fabs(gotOut[c] - composed));
            if (a == 0.0f) { worstBehind = (std::max)(worstBehind, std::fabs(gotOut[c] - gotHudless[c])); }
        }
        Require(std::fabs(gotUI[3] - a) <= 1.0f / 65535.0f + 1e-6f, "UI alpha preserved at 16-bit precision");
    }
    std::printf("max error: hudless=%.2f codes ui=%.5f identity=%.2f codes uncovered=%.2f codes overlay=%.2f codes\n",
        worstHudless / code10, worstUI, worstIdentity / code10, worstBehind / code10, worstOverlay / code10);
    Require(worstHudless <= 1.5f * code10, "HUD-less matches CPU reference within 1.5 10-bit codes");
    Require(worstUI <= 2e-4f, "UI matches CPU reference");
    Require(worstBehind <= 0.5f * code10 + 1e-6f, "uncovered pixels equal the HUD-less encoding");
    Require(worstIdentity <= 1.5f * code10, "backbuffer satisfies DLSS-G UI + (1 - a) * HUD-less");
    Require(worstOverlay <= 1.5f * code10, "content outside the tagged layers is retained");

    // Encode: SDR passthrough is exact; UI-brightness encoding matches the reference.
    Check(pass.RecordEncode(gpu.device.Get(), gpu.list.Get(), 2, HDROutput::MakeShaderConstants(settings, true),
        compositeTexture.Get(), backbuffer.Get()), "record passthrough");
    gpu.Submit();
    const auto passthrough = gpu.Read(backbuffer.Get(), 4);
    Check(pass.RecordEncode(gpu.device.Get(), gpu.list.Get(), 0, constants, compositeTexture.Get(), backbuffer.Get()), "record encode");
    gpu.Submit();
    const auto encoded = gpu.Read(backbuffer.Get(), 4);
    float worstPassthrough = 0, worstEncoded = 0;
    for (UINT i = 0; i < W * H; ++i) {
        const HDROutput::RGB c{ Channel8(composite[i], 0), Channel8(composite[i], 1), Channel8(composite[i], 2) };
        const auto expect = HDROutput::EncodeUI(c, settings);
        const auto p = Unpack10(passthrough, i), e = Unpack10(encoded, i);
        for (int k = 0; k < 3; ++k) {
            worstPassthrough = (std::max)(worstPassthrough, std::fabs(p[k] - c[k]));
            worstEncoded = (std::max)(worstEncoded, std::fabs(e[k] - expect[k]));
        }
    }
    std::printf("max error: passthrough=%.2f codes encode=%.2f codes\n", worstPassthrough / code10, worstEncoded / code10);
    Require(worstPassthrough <= 0.5f * code10 + 1e-6f, "SDR passthrough");
    Require(worstEncoded <= 1.5f * code10, "UI-brightness encode matches CPU reference");

    // Repeated slot reuse with retained views stays valid; timing harvests each
    // slot's previous pair when that slot is recorded again.
    UINT64 frequency{};
    Check(gpu.queue->GetTimestampFrequency(&frequency), "timestamp frequency");
    Check(pass.EnableTiming(gpu.device.Get(), frequency), "enable timing");
    Check(pass.EnableTiming(gpu.device.Get(), frequency), "timing is idempotent");
    Require(pass.TakeTiming().samples == 0, "no samples before a timed slot is reused");
    HDROutputTiming timing;
    for (std::size_t slot = 0; slot < kCommandSlots * 2; ++slot) {
        Check(pass.RecordCompose(gpu.device.Get(), gpu.list.Get(), slot % kCommandSlots, constants, compositeTexture.Get(),
            uiTexture.Get(), sceneTexture.Get(), backbuffer.Get()), "repeated compose");
        gpu.Submit();
        timing.Add(pass.TakeTiming());
    }
    std::printf("timing: samples=%llu avg=%.1f us max=%.1f us\n", static_cast<unsigned long long>(timing.samples),
        timing.AverageUs(), timing.maxUs);
    Require(timing.samples == kCommandSlots, "each reused slot yields one GPU sample");
    Require(std::isfinite(timing.AverageUs()) && timing.maxUs >= timing.AverageUs(), "finite GPU timing");
    std::printf("HDR output pass checks passed\n");
    return 0;
}
