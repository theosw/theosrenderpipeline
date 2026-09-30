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
    // Cross the native UI boundary and all alpha values, including fully opaque.
    bool Overlay(UINT x, UINT) { return x >= W / 2 - 6 && x < W / 2 + 6; }
    // Additive UI (a glow): colour with zero alpha, blended as dest + src.
    bool Additive(UINT x, UINT y) { return x >= W - 6 && (y % 16) < 6; }

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

    void CheckLateOverlays(GPU& gpu, HDROutputPass& pass, HDROutput::Settings settings)
    {
        // Columns: unchanged, one-code rounding, opaque white/black,
        // translucent coloured/tinted UI, opaque grey, opaque blue.
        // Rows exercise native alpha 0, 0.5 and 1; HDR layers must agree in all cases.
        std::vector<std::uint32_t> scene(W * H), ui(W * H), composite(W * H);
        for (UINT y = 0; y < H; ++y) {
            for (UINT x = 0; x < W; ++x) {
                const UINT i = y * W + x, kind = x % 8;
                const float a = (y % 3) * 0.5f;
                scene[i] = Pack(0.15f + 0.4f * x / (W - 1), 0.25f, 0.35f, 1);
                ui[i] = Pack(0.4f * a, 0.2f * a, 0.05f * a, a);
                const float alpha = Channel8(ui[i], 3);
                float base[3]{};
                for (int k = 0; k < 3; ++k) { base[k] = Channel8(ui[i], k) + (1-alpha) * Channel8(scene[i], k); }
                composite[i] = Pack(base[0], base[1], base[2], 1);
                if (kind == 1) { composite[i] = Pack(Channel8(composite[i],0) + 1.0f/255, Channel8(composite[i],1), Channel8(composite[i],2), 1); }
                if (kind == 2) { composite[i] = Pack(1,1,1,1); }
                if (kind == 3) { composite[i] = Pack(0,0,0,1); }
                if (kind == 4) { composite[i] = Pack(0.35f*0.9f + 0.65f*base[0], 0.35f*0.2f + 0.65f*base[1], 0.35f*0.5f + 0.65f*base[2], 1); }
                if (kind == 5) { composite[i] = Pack(0.65f*0.05f + 0.35f*base[0], 0.65f*0.85f + 0.35f*base[1], 0.65f*0.3f + 0.35f*base[2], 1); }
                if (kind == 6) { composite[i] = Pack(0.8f,0.8f,0.8f,1); }
                if (kind == 7) { composite[i] = Pack(0,0,1,1); }
            }
        }
        auto s = gpu.Texture(DXGI_FORMAT_R8G8B8A8_UNORM, false);
        auto u = gpu.Texture(DXGI_FORMAT_R8G8B8A8_UNORM, false);
        auto c = gpu.Texture(DXGI_FORMAT_R8G8B8A8_UNORM, false);
        auto b = gpu.Texture(HDROutputPass::kOutputFormat, true);
        gpu.Upload(s.Get(), scene); gpu.Upload(u.Get(), ui); gpu.Upload(c.Get(), composite); gpu.Submit();
        for (auto transfer : {HDROutput::Transfer::Gamma22, HDROutput::Transfer::SRGB}) {
            settings.transfer = transfer;
            Check(pass.RecordCompose(gpu.device.Get(), gpu.list.Get(), 0, HDROutput::MakeShaderConstants(settings, false),
                c.Get(), u.Get(), s.Get(), b.Get()), "late-overlay matrix");
            gpu.Submit();
            const auto output = gpu.Read(b.Get(), 4), world = gpu.Read(pass.HudlessTarget(), 4), layer = gpu.Read(pass.UITarget(), 8);
            float worstIdentity = 0;
            for (UINT i = 0; i < W * H; ++i) {
                const UINT kind = (i % W) % 8;
                const float a = Channel8(ui[i], 3);
                const auto out = Unpack10(output, i), hudless = Unpack10(world, i);
                const auto tag = Unpack16(layer, i);
                const auto originalWorld = HDROutput::EncodeScene({Channel8(scene[i],0), Channel8(scene[i],1), Channel8(scene[i],2)}, settings);
                const auto originalUI = HDROutput::EncodeUIPremultiplied({Channel8(ui[i],0), Channel8(ui[i],1), Channel8(ui[i],2)}, a, settings);
                float visibleChange = 0;
                for (int k = 0; k < 3; ++k) {
                    Require(std::fabs(hudless[k] - originalWorld[k]) <= 1.5f/1023, "late overlay never contaminates the world tag");
                    worstIdentity = (std::max)(worstIdentity, std::fabs(out[k] - std::clamp(tag[k] + (1-tag[3])*hudless[k], 0.0f, 1.0f)));
                    const float before = std::clamp(originalUI[k] + (1-a)*originalWorld[k], 0.0f, 1.0f);
                    visibleChange = (std::max)(visibleChange, std::fabs(out[k] - before));
                    if (kind <= 1) {
                        Require(std::fabs(out[k]-before) <= 1.5f/1023 && std::fabs(tag[k]-originalUI[k]) <= 2e-4f,
                            "unchanged and one-code noise preserve native composition");
                    }
                    if (kind == 2 || kind == 3 || kind == 7) {
                        const auto expected = HDROutput::EncodeUI(kind == 2 ? HDROutput::RGB{1,1,1} :
                            kind == 3 ? HDROutput::RGB{0,0,0} : HDROutput::RGB{0,0,1}, settings);
                        Require(std::fabs(out[k]-expected[k]) <= 1.5f/1023, "opaque late UI survives at UI brightness");
                    }
                }
                Require(tag[3] >= a - 1.0f/65535 && tag[3] <= 1, "late coverage is bounded and includes native UI");
                if (kind <= 1) { Require(std::fabs(tag[3]-a) <= 1.0f/65535 + 1e-6f, "rounding does not create late coverage"); }
                else { Require(visibleChange > 0.5f/1023, "late content changes output even over opaque native UI"); }
                if (kind == 2 || kind == 3 || kind == 7) { Require(tag[3] == 1, "opaque late extremes have opaque coverage"); }
            }
            std::printf("late-overlay matrix transfer=%d identity=%.2f codes\n", int(transfer), worstIdentity*1023);
            Require(worstIdentity <= 1.5f/1023, "every late-overlay pixel recomposes from the FG tags");
        }
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
            ui[y * W + x] = Additive(x, y) ? Pack(0.5f, 0.3f, 0.1f, 0.0f) : Pack(ur, ug, ub, a);
            // Producer composition in SDR, as TRP's native UI compositor does.
            const auto s = scene[y * W + x], u = ui[y * W + x];
            const float ua = Channel8(u, 3);
            composite[y * W + x] = Pack(Channel8(s, 0) * (1 - ua) + Channel8(u, 0), Channel8(s, 1) * (1 - ua) + Channel8(u, 1),
                Channel8(s, 2) * (1 - ua) + Channel8(u, 2), 1); // Pack saturates, as UNORM blending does.
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

    const HDROutput::Settings settings{ .enabled = true, .matchWindowsSDR = false, .paperWhiteNits = 200.0f, .peakNits = 1000.0f,
        .uiNits = 150.0f, .highlightStrength = 1.0f, .expansionStart = 0.6f, .transfer = HDROutput::Transfer::Gamma22 };
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
        const auto expectUI = HDROutput::EncodeUIPremultiplied(
            { Channel8(ui[i], 0), Channel8(ui[i], 1), Channel8(ui[i], 2) }, a, settings);
        const auto gotUI = Unpack16(uiOut, i);
        const auto gotOut = Unpack10(out, i);
        for (int c = 0; c < 3; ++c) {
            worstHudless = (std::max)(worstHudless, std::fabs(gotHudless[c] - expectHudless[c]));
            if (Overlay(i % W, i / W)) {
                // Opaque late white is UI at UI brightness, over any prior alpha.
                const auto whiteUI = HDROutput::EncodeUI({ 1, 1, 1 }, settings);
                worstOverlay = (std::max)(worstOverlay, std::fabs(gotOut[c] - whiteUI[c]));
                worstUI = (std::max)(worstUI, std::fabs(gotUI[c] - whiteUI[c]));
            }
            else { worstUI = (std::max)(worstUI, std::fabs(gotUI[c] - expectUI[c])); }
            // DLSS-G composition identity on the actual encoded outputs.
            const float composed = (std::min)(1.0f, gotUI[c] + (1.0f - gotUI[3]) * gotHudless[c]);
            worstIdentity = (std::max)(worstIdentity, std::fabs(gotOut[c] - composed));
            if (a == 0.0f && !Additive(i % W, i / W) && !Overlay(i % W, i / W)) {
                worstBehind = (std::max)(worstBehind, std::fabs(gotOut[c] - gotHudless[c]));
            }
        }
        const float expectedAlpha = Overlay(i % W, i / W) ? 1.0f : a;
        Require(std::fabs(gotUI[3] - expectedAlpha) <= 1.0f / 65535.0f + 1e-6f,
            "native or late overlay alpha preserved at 16-bit precision");
        if (Additive(i % W, i / W)) {
            Require(gotUI[0] > 0.3f && gotOut[0] >= gotHudless[0], "additive UI survives in the UI tag and backbuffer");
        }
    }
    std::printf("max error: hudless=%.2f codes ui=%.5f identity=%.2f codes uncovered=%.2f codes overlay=%.2f codes\n",
        worstHudless / code10, worstUI, worstIdentity / code10, worstBehind / code10, worstOverlay / code10);
    Require(worstHudless <= 1.5f * code10, "HUD-less matches CPU reference within 1.5 10-bit codes");
    Require(worstUI <= 2e-4f, "UI matches CPU reference");
    Require(worstBehind <= 0.5f * code10 + 1e-6f, "uncovered pixels equal the HUD-less encoding");
    Require(worstIdentity <= 1.5f * code10, "backbuffer satisfies DLSS-G UI + (1 - a) * HUD-less");
    Require(worstOverlay <= 1.5f * code10, "content outside the tagged layers is retained");

    CheckLateOverlays(gpu, pass, settings);

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

    // A world frame without separate layers still expands the whole frame.
    Check(pass.RecordEncode(gpu.device.Get(), gpu.list.Get(), 1, HDROutput::MakeShaderConstants(settings, false, true),
        compositeTexture.Get(), backbuffer.Get()), "record whole-frame expansion");
    gpu.Submit();
    const auto expanded = gpu.Read(backbuffer.Get(), 4);
    float worstExpanded = 0;
    for (UINT i = 0; i < W * H; ++i) {
        const HDROutput::RGB c{ Channel8(composite[i], 0), Channel8(composite[i], 1), Channel8(composite[i], 2) };
        const auto expect = HDROutput::EncodeScene(c, settings);
        const auto e = Unpack10(expanded, i);
        for (int k = 0; k < 3; ++k) { worstExpanded = (std::max)(worstExpanded, std::fabs(e[k] - expect[k])); }
    }
    std::printf("max error: whole-frame expansion=%.2f codes\n", worstExpanded / code10);
    Require(worstExpanded <= 1.5f * code10, "whole-frame expansion matches CPU reference");

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
    Require(QuerySDRWhiteNits(L"\\\\.\\TRP-NO-SUCH-DISPLAY") == 0.0f && QuerySDRWhiteNits(nullptr) == 0.0f,
        "unknown display has no SDR white level");
    std::printf("HDR output pass checks passed\n");
    return 0;
}
