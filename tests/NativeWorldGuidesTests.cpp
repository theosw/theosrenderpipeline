#include "FrameGen/NativeWorldGuides.h"
#include "FrameGen/NativeUIAttachments.h"
#include "FrameGen/LoadingScreenUpscaler.h"
#include "FrameGen/SourceNvidiaFrameEvaluator.h"
#include "UpscalerTextureHook.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using namespace TheosRenderPipeline;
using Microsoft::WRL::ComPtr;
using Observation = NativeWorldGuides::Observation;

static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Check(HRESULT hr, const char* why) { Require(SUCCEEDED(hr), why); }
static std::atomic<unsigned> diagnosticCalls{};
static std::atomic<DWORD> diagnosticThread{};

// The production hook links unchanged. Only its inventory logging sink is
// replaced; creation, HRESULT/output forwarding and the resolver are real.
void TheosRenderPipeline::ObserveInventoryTextureCreation(const D3D11_TEXTURE2D_DESC* desc)
{
    if (desc) { ++diagnosticCalls; diagnosticThread = GetCurrentThreadId(); }
}

static constexpr UINT width = 12, height = 8;
static constexpr std::uint32_t motionPixel = 0x34003800, postPixel = 0x00666666, mainPixel = 0x00333333;

static ComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, DXGI_FORMAT format, UINT bindings,
    std::uint32_t pixel, UINT w = width, UINT h = height, UINT samples = 1, UINT arraySize = 1)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w; desc.Height = h; desc.Format = format; desc.BindFlags = bindings;
    desc.MipLevels = 1; desc.ArraySize = arraySize; desc.SampleDesc.Count = samples;
    std::vector<std::uint32_t> pixels(w * h, pixel);
    D3D11_SUBRESOURCE_DATA initial{pixels.data(), w * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    Check(hk_ID3D11Device_CreateTexture2D(device, &desc, samples == 1 && arraySize == 1 ? &initial : nullptr, &texture), "hook creates real WARP texture");
    return texture;
}

static ComPtr<ID3D11Texture2D> Motion(ID3D11Device* device, std::uint32_t pixel = motionPixel, UINT w = width)
{ return Texture(device, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, pixel, w); }
static ComPtr<ID3D11Texture2D> Depth(ID3D11Device* device, std::uint32_t pixel = postPixel, UINT w = width)
{ return Texture(device, DXGI_FORMAT_R24G8_TYPELESS, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL, pixel, w); }

static std::vector<std::uint32_t> Pixels(ID3D11DeviceContext* context, ID3D11Texture2D* texture)
{
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&desc, nullptr, &staging), "staging texture");
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "pixel readback");
    std::vector<std::uint32_t> pixels(desc.Width * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y) {
        std::memcpy(pixels.data() + y * desc.Width, static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, desc.Width * 4);
    }
    context->Unmap(staging.Get(), 0);
    return pixels;
}

static void AllPixels(ID3D11DeviceContext* context, ID3D11Texture2D* texture, std::uint32_t expected, const char* why)
{
    for (const auto pixel : Pixels(context, texture)) { Require(pixel == expected, why); }
}

static void IdentityAndRecreation(ID3D11Device* device, ID3D11DeviceContext* context)
{
    // An intruder created before the engine targets is no more authoritative
    // than one created later. All four go through the real production hook.
    auto earlyIntruder = Motion(device, 0x3c003c00);
    auto motion = Motion(device), post = Depth(device), main = Depth(device, mainPixel);
    NativeGuideSources slots{motion.Get(), post.Get(), main.Get()};
    NativeWorldGuides owner;
    unsigned resets{}, retires{};
    bool retirementOK = true;
    auto retire = [&] { ++retires; return retirementOK; };
    auto reset = [&] { ++resets; };
    auto observe = [&](std::uint64_t frame) {
        return owner.Observe(device, slots, {width, height}, frame, 100 + frame, true, retire, reset);
    };
    Require(observe(0) == Observation::Changed && resets == 1 && retires == 0, "first producer publishes one generation");
    const auto first = *owner.Current(0);
    Require(first.depthRole == NativeDepthRole::PostZPrepass && first.depth.Get() == post.Get(), "native post-z takes precedence over main");
    Require(first.motion.Get() == motion.Get() && first.gameFrame == 100, "actual engine motion and producer frame are retained");
    AllPixels(context, first.motion.Get(), motionPixel, "selected motion has engine pixels");
    AllPixels(context, first.depth.Get(), postPixel, "selected post-z has engine pixels");

    ComPtr<ID3D11Texture2D> intruderMotion, intruderDepth;
    const auto mainThread = GetCurrentThreadId();
    const auto callsBefore = diagnosticCalls.load();
    std::thread worker([&] {
        intruderMotion = Motion(device, 0x3c003c00);
        intruderDepth = Depth(device, 0x00cccccc);
        Require(owner.Current(0) == nullptr, "off-thread readers cannot borrow render-thread publication");
        Require(owner.Observe(device, {intruderMotion.Get(), intruderDepth.Get(), nullptr}, {width, height}, 1, 101, true,
            [] { return true; }, [] { Require(false, "worker cannot request a guide reset"); }) == Observation::WrongThread,
            "off-thread publication is rejected");
    });
    worker.join();
    Require(diagnosticCalls == callsBefore + 2 && diagnosticThread != mainThread, "actual hook forwards worker creations and inventory observation");
    D3D11_TEXTURE2D_DESC actual{}, unrelated{};
    motion->GetDesc(&actual); intruderMotion->GetDesc(&unrelated);
    Require(std::memcmp(&actual, &unrelated, sizeof(actual)) == 0, "motion intruder descriptor is identical");
    post->GetDesc(&actual); intruderDepth->GetDesc(&unrelated);
    Require(std::memcmp(&actual, &unrelated, sizeof(actual)) == 0, "depth intruder descriptor is identical");
    Require(owner.Current(0)->motion.Get() == motion.Get() && owner.Current(0)->depth.Get() == post.Get(), "creation cannot replace published pair");
    Require(observe(1) == Observation::Unchanged && resets == 1 && retires == 0, "intruders do not create generations or resets at next boundary");
    AllPixels(context, owner.Current(1)->motion.Get(), motionPixel, "later intruder does not contaminate selected pixels");
    std::puts("PASS: identical-descriptor intruders before/after selection and actual worker-thread creation hook");

    slots.postZDepth = nullptr;
    Require(observe(2) == Observation::Changed && resets == 2 && retires == 1, "missing post-z explicitly falls back once");
    Require(owner.Current(2)->depthRole == NativeDepthRole::Main && owner.Current(2)->depth.Get() == main.Get(), "main fallback role is recorded");
    AllPixels(context, owner.Current(2)->depth.Get(), mainPixel, "fallback selects main's pixels");
    slots.postZDepth = main.Get();
    Require(observe(3) == Observation::Changed && resets == 3 && retires == 1, "role-only change resets without retiring identical resources");
    Require(owner.Current(3)->depthRole == NativeDepthRole::PostZPrepass, "role is independent of aliased resource identity");
    std::puts("PASS: native post-z preference, explicit main fallback and role-only generation change");

    auto replacement = Motion(device, 0x30003000);
    slots.motion = replacement.Get();
    const auto generation = owner.Current(3)->generation;
    Require(observe(4) == Observation::Changed && owner.Current(4)->generation == generation + 1, "same-size replacement creates one generation");
    const auto resetCount = resets, retireCount = retires;
    Require(observe(4) == Observation::Unchanged && observe(5) == Observation::Unchanged && resets == resetCount && retires == retireCount,
        "duplicate and subsequent stable frames do not reset or retire again");
    AllPixels(context, owner.Current(5)->motion.Get(), 0x30003000, "replacement pixels are published");
    AllPixels(context, first.motion.Get(), motionPixel, "borrower owning an old snapshot retains its original pair");
    Require(owner.Current(6) == nullptr, "an absent producer cannot reuse the previous frame temporally");
    Require(observe(7) == Observation::Changed && resets == resetCount + 1, "recovery after a missing producer requests reset");

    slots.motion = motion.Get(); retirementOK = false;
    const auto beforeFailure = resets;
    Require(observe(8) == Observation::RetirementFailed && !owner.Current(8) && resets == beforeFailure, "failed retirement removes readiness without publishing a replacement");
    retirementOK = true;
    const auto failedRetires = retires;
    Require(observe(9) == Observation::Changed && retires == failedRetires + 1, "failed retirement retained old ownership and must retry before replacement");
    std::puts("PASS: same-size recreation, retained snapshots, missing-frame freshness and retirement failure/retry");

    auto tooWide = Motion(device, motionPixel, width + 1);
    slots.motion = tooWide.Get();
    Require(observe(10) == Observation::Unavailable && !owner.Current(10), "wrong native extent rejected");
    slots.motion = motion.Get(); slots.postZDepth = intruderMotion.Get();
    Require(observe(11) == Observation::Changed && owner.Current(11)->depthRole == NativeDepthRole::Main, "invalid preferred depth falls back to validated main");
    slots.mainDepth = nullptr;
    Require(observe(12) == Observation::Unavailable, "no usable depth means no qualified pair");
    ComPtr<ID3D11Device> other; ComPtr<ID3D11DeviceContext> otherContext;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &other, nullptr, &otherContext), "second WARP device");
    auto foreign = Motion(other.Get());
    slots = {foreign.Get(), post.Get(), main.Get()};
    Require(observe(13) == Observation::Unavailable, "foreign-device motion rejected");
    auto arrayMotion = Texture(device, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, width, height, 1, 2);
    slots.motion = arrayMotion.Get();
    Require(observe(14) == Observation::Unavailable, "array-shaped motion rejected");
    slots.motion = motion.Get();
    Require(owner.Observe(device, slots, {width, height}, 15, 115, false, retire, reset) == Observation::Unavailable,
        "loading/main boundary cannot qualify world guides");
    auto otherDepth = Depth(other.Get());
    const auto beforeDeviceChange = resets;
    Require(owner.Observe(other.Get(), {foreign.Get(), otherDepth.Get(), nullptr}, {width, height}, 16, 116, true, retire, reset) == Observation::Changed &&
        resets == beforeDeviceChange + 1 && D3D11FrameCopy::SameObject(owner.Current(16)->device.Get(), other.Get()),
        "a coherent replacement device is recorded as a new generation");
    auto resizedMotion = Motion(other.Get(), motionPixel, width + 1), resizedDepth = Depth(other.Get(), postPixel, width + 1);
    Require(owner.Observe(other.Get(), {resizedMotion.Get(), resizedDepth.Get(), nullptr}, {width + 1, height}, 17, 117, true, retire, reset) == Observation::Changed &&
        resets == beforeDeviceChange + 2 && owner.Current(17)->extent.width == width + 1, "coherent resized pair gets one new generation");
    const auto beforeLifecycle = owner.Current(17)->generation;
    owner.ResetAfterRetirement();
    Require(!owner.Current(17), "lifecycle retirement invalidates publication");
    Require(owner.Observe(device, {motion.Get(), post.Get(), main.Get()}, {width, height}, 0, 200, true, retire, reset) == Observation::Changed &&
        owner.Current(0)->generation == beforeLifecycle + 1, "lifecycle reacquisition retains monotonic resource generation");
    std::puts("PASS: extent, format, device, subresource shape and non-world rejection");
}

struct BoundaryHost
{
    ID3D11Device* device;
    NativeGuideSources slots;
    bool nativeUI;
    NativeWorldGuides owner;
    unsigned observations{}, callbacks{}, uiPreparations{}, resets{};
    void ObserveWorldGuides(bool menu)
    {
        ++observations;
        owner.Observe(device, slots, {width, height}, 0, 50, !menu, [] { return true; }, [&] { ++resets; });
    }
    void OnBackgroundReady(BackgroundBoundary boundary, bool)
    {
        ++callbacks;
        Require(boundary == BackgroundBoundary::World && owner.Current(0), "publication precedes UI coordination");
        if (!nativeUI) { return; }
        ++uiPreparations;
    }
};

static ComPtr<ID3D11ShaderResourceView> DepthView(ID3D11Device* device, ID3D11Texture2D* depth)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> view;
    Check(device->CreateShaderResourceView(depth, &desc, &view), "depth SRV");
    return view;
}

static void UIBoundary(ID3D11Device* device, ID3D11DeviceContext* context)
{
    auto motion = Motion(device), post = Depth(device), main = Depth(device, mainPixel), unrelated = Depth(device, 0x00999999);
    for (const bool ui : {false, true}) {
        BoundaryHost host{device, {motion.Get(), post.Get(), main.Get()}, ui};
        ObserveNativeWorldCompletion(host, false);
        Require(host.observations == 1 && host.callbacks == 1 && host.uiPreparations == unsigned(ui) && host.resets == 1,
            "native UI on/off both acquire through production boundary sequence");
        const auto& pair = *host.owner.Current(0);
        NativeUIAttachments attachments;
        Check(attachments.Ensure(device, context, pair.motion.Get(), pair.depth.Get(), 18, 12), "UI companions use published pair");
        auto selected = DepthView(device, pair.depth.Get()), otherMain = DepthView(device, main.Get()), intruder = DepthView(device, unrelated.Get());
        ID3D11ShaderResourceView* inputs[]{selected.Get(), otherMain.Get(), intruder.Get()};
        ID3D11ShaderResourceView* outputs[3]{};
        Require(attachments.SubstituteDepths(3, inputs, outputs, pair.depth.Get()) == 1 &&
            outputs[0] == attachments.DepthSRV() && outputs[1] == inputs[1] && outputs[2] == inputs[2],
            "depth substitution follows the published identity, never an identical descriptor");
        // Inventory draw-time substitution uses this same production scope.
        context->PSSetShaderResources(0, 3, inputs);
        {
            NativeUIDepthScope scope(context, {&attachments, pair.depth.Get(), nullptr});
            Require(scope.Count() == 1, "inventory depth hint uses the same accepted pair");
        }
        ID3D11ShaderResourceView* restored{}; context->PSGetShaderResources(0, 1, &restored);
        Require(restored == selected.Get(), "inventory depth scope restores source binding");
        restored->Release();
        ID3D11ShaderResourceView* empty[3]{}; context->PSSetShaderResources(0, 3, empty);
    }
    std::puts("PASS: production world-boundary sequence acquires with native UI on/off; UI and inventory share depth identity");
}

struct SpatialOperations
{
    ID3D11DeviceContext* context;
    LoadingScreenUpscaler scaler;
    unsigned neural{}, temporal{}, cameras{}, prepares{}, completed{}, decisions{};
    bool generation{true};
    void CopyInput(ID3D11DeviceContext* ctx, const SourceNvidiaFrameInputs& frame) { ctx->CopyResource(frame.input, frame.color); }
    bool EvaluateNeuralBeforeDLSS(SourceNvidiaFrameInputs&) { ++neural; return false; }
    void RenderReShade(const SourceNvidiaFrameInputs&, bool) {}
    bool EvaluateDLSS(const SourceNvidiaFrameInputs& frame)
    {
        if (!frame.spatialOnly) { ++temporal; return false; }
        Require(!frame.motion && !frame.depth, "spatial path has no old guide pointers");
        return SUCCEEDED(scaler.Evaluate(context, frame.input, frame.output));
    }
    void UpscaleSucceeded() { ++completed; }
    bool CaptureCamera(const SourceNvidiaFrameGuides&) { ++cameras; return true; }
    bool Prepare(const SourceNvidiaFrameGuides&) { ++prepares; return true; }
    void PublishGeneration(bool prepared) { ++decisions; generation = prepared; }
};

static void SpatialFallback(ID3D11Device* device, ID3D11DeviceContext* context)
{
    NativeWorldGuides owner;
    auto motion = Motion(device), depth = Depth(device);
    owner.Observe(device, {motion.Get(), depth.Get(), nullptr}, {width, height}, 0, 20, true, [] { return true; }, [] {});
    SpatialOperations ops{context};
    for (const auto pixel : {0xff336699u, 0xffcc8844u}) {
        const UINT bindings = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        auto color = Texture(device, DXGI_FORMAT_R8G8B8A8_UNORM, bindings, pixel);
        auto input = Texture(device, DXGI_FORMAT_R8G8B8A8_UNORM, bindings, 0);
        auto output = Texture(device, DXGI_FORMAT_R8G8B8A8_UNORM, bindings, 0xff000000, 18, 12);
        // No world callback on the next host frame. The retained pair from frame
        // zero must never feed this evaluation, even though its extent matches.
        const auto* pair = owner.Current(1 + ops.completed);
        SourceNvidiaFrameInputs frame{};
        frame.color = color.Get(); frame.input = input.Get(); frame.output = output.Get();
        frame.motion = pair ? pair->motion.Get() : nullptr; frame.depth = pair ? pair->depth.Get() : nullptr;
        frame.spatialOnly = !pair;
        const auto result = SourceNvidiaFrameEvaluator::Evaluate(context, frame, ops);
        Require(result.upscaled && !result.cameraValid && !result.prepared && !ops.generation, "missing guides preserve current real frame with NR/FG ineligible");
        AllPixels(context, output.Get(), pixel, "real spatial scaler fills the output with this frame's current color");
        NativeUIAttachments attachments;
        Check(attachments.Ensure(device, context, nullptr, nullptr, 18, 12), "spatial frames can still prepare native UI companions");
        Require(attachments.AuxiliaryRTV() && attachments.DepthSRV(), "color-only UI has both required companion views");
    }
    Require(ops.completed == 2 && ops.decisions == 2 && !ops.neural && !ops.temporal && !ops.cameras && !ops.prepares,
        "spatial fallback skips all temporal consumers and advances no camera history");
    std::puts("PASS: missing-boundary frames show new output-sized spatial pixels, retain native UI, and never run NR/camera/FG preparation");
}

int main()
{
    ptrCreateTexture2D = &ID3D11Device::CreateTexture2D;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "WARP device");
    IdentityAndRecreation(device.Get(), context.Get());
    UIBoundary(device.Get(), context.Get());
    SpatialFallback(device.Get(), context.Get());
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R16G16_FLOAT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    Require(hk_ID3D11Device_CreateTexture2D(device.Get(), &desc, nullptr, nullptr) == S_FALSE,
        "hook preserves validation-only success without dereferencing an output");
    std::puts("PASS: texture hook preserves HRESULT and legal null-output validation");
}
