#include "FrameGen/SourceNvidiaFrameEvaluator.h"
#include "FrameGen/SourceGenerationPolicy.h"
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;
using TheosRenderPipeline::SourceNvidiaFrameInputs;
using TheosRenderPipeline::SourceNvidiaFrameEvaluator;
using TheosRenderPipeline::SourceNvidiaFrameGuides;
using TheosRenderPipeline::SourceNvidiaFramePreparation;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Check(HRESULT hr, const char* why) { Require(SUCCEEDED(hr), why); }

struct Surface
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    Surface(ID3D11Device* device, UINT width, UINT height)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        Check(device->CreateTexture2D(&desc, nullptr, &texture), "texture");
        Check(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv), "RTV");
    }
    void Paint(ID3D11DeviceContext* context, const std::array<float, 4>& color)
    { context->ClearRenderTargetView(rtv.Get(), color.data()); }
};

static std::array<float, 4> Pixel(ID3D11DeviceContext* context, ID3D11Texture2D* texture)
{
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&desc, nullptr, &staging), "staging");
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "readback");
    const auto* p = static_cast<const float*>(mapped.pData);
    std::array<float, 4> result{p[0], p[1], p[2], p[3]};
    context->Unmap(staging.Get(), 0);
    return result;
}

// The production evaluator performs real D3D11 copies. Only the proprietary
// evaluation and camera source are scripted. Reading pixels inside those calls
// catches wrong source/destination, missing unbind/copy, and preparation before
// reconstruction. A failed upscale must not advance camera history or publish
// a generation decision; a failed Prepare must still return a usable real frame.
struct Operations
{
    ID3D11DeviceContext* context;
    Surface& world;
    Surface& output;
    const SourceNvidiaFrameInputs& expected;
    bool dlssOK, cameraOK, prepareOK, requested, blocked;
    int warmup;
    unsigned upscales{}, cameras{}, prepares{}, decisions{};
    bool generation{};
    bool neuralOK{true}, earlyNR{}, neuralReset{};
    unsigned neuralCalls{}, dlssCalls{}, reShadeCalls{};
    bool reShade{}, reShadeBefore{};
    const std::array<float, 4> shaded{0.5f, 0.125f, 0.25f, 1};
    std::array<float, 4> ExpectedInput() const { return reShade && reShadeBefore ? shaded : earlyNR ? neuralScene : scene; }
    std::array<float, 4> ExpectedOutput() const { return reShade && !reShadeBefore ? shaded : reconstructed; }
    const std::array<float, 4> scene{0.25f, 0.5f, 0.75f, 1};
    const std::array<float, 4> neuralScene{0.125f, 0.75f, 0.375f, 1};
    const std::array<float, 4> reconstructed{0.75f, 0.25f, 0.5f, 1};

    void CopyInput(ID3D11DeviceContext* copyContext, const SourceNvidiaFrameInputs& frame)
    { copyContext->CopyResource(frame.input, frame.color); }

    bool EvaluateNeuralBeforeDLSS(SourceNvidiaFrameInputs& frame)
    {
        ++neuralCalls;
        Require(Pixel(context, frame.input) == scene, "NR receives the frozen world before DLSS");
        Require(!upscales && !cameras && !prepares, "NR precedes reconstruction and history consumption");
        if (!neuralOK) { return false; }
        frame.reset |= neuralReset;
        if (earlyNR) {
            ComPtr<ID3D11Device> device; context->GetDevice(&device);
            ComPtr<ID3D11RenderTargetView> target;
            Check(device->CreateRenderTargetView(frame.input, nullptr, &target), "scripted NR output view");
            context->ClearRenderTargetView(target.Get(), neuralScene.data());
        }
        return true;
    }

    bool EvaluateDLSS(const SourceNvidiaFrameInputs& frame)
    {
        ++dlssCalls;
        Require(neuralCalls == 1, "early stage runs exactly once before DLSS");
        ID3D11RenderTargetView* bound[8]{}; ComPtr<ID3D11DepthStencilView> depth;
        context->OMGetRenderTargets(8, bound, &depth);
        Require(!depth, "depth detached before input capture");
        for (auto* rtv : bound) { Require(!rtv, "all MRTs detached before input capture"); }
        Require(Pixel(context, frame.input) == ExpectedInput(), "DLSS consumes the selected NR/world image");
        Require(frame.motion == expected.motion && frame.depth == expected.depth, "guide identity");
        Require(frame.renderWidth == 12 && frame.renderHeight == 8 && frame.outputWidth == 18 && frame.outputHeight == 12, "render/output extents stay distinct");
        Require(frame.sharpness == 0.25f && frame.jitterX == -0.375f && frame.jitterY == 0.125f && frame.motionScaleX == 12 && frame.motionScaleY == 8, "evaluation scalars preserved");
        Require(frame.reset == (expected.reset || neuralReset) && frame.jitterEnabled, "NR appearance reset reaches DLSS");
        // A later producer can overwrite the outer color; the frozen DLSS input
        // must retain the original frame rather than aliasing that resource.
        world.Paint(context, {1, 0, 0, 1});
        Require(Pixel(context, frame.input) == ExpectedInput(), "input snapshot does not alias outer color");
        if (dlssOK) { output.Paint(context, reconstructed); }
        return dlssOK;
    }
    void UpscaleSucceeded() { ++upscales; }
    void RenderReShade(const SourceNvidiaFrameInputs& frame, bool before)
    {
        if (!reShade || before != reShadeBefore) { return; }
        Require(neuralCalls == 1 && !cameras && !prepares, "effects follow early NR and precede FG snapshots");
        Require(dlssCalls == unsigned(!before), "effects run on the selected side of DLSS");
        auto* color = before ? frame.input : frame.output;
        Require(Pixel(context, color) == (before ? (earlyNR ? neuralScene : scene) : reconstructed), "effect input has completed producer pixels");
        ComPtr<ID3D11Device> device; context->GetDevice(&device);
        ComPtr<ID3D11RenderTargetView> rtv;
        Check(device->CreateRenderTargetView(color, nullptr, &rtv), "effect view");
        context->ClearRenderTargetView(rtv.Get(), shaded.data());
        ++reShadeCalls;
    }
    bool CaptureCamera(const SourceNvidiaFrameGuides& frame)
    {
        Require(upscales == 1 && decisions == 0, "upscale counted before camera history advances");
        Require(Pixel(context, output.texture.Get()) == ExpectedOutput(), "camera capture follows completed reconstruction");
        Require(frame.reset == (expected.reset || neuralReset), "reset from reconstruction reaches camera history");
        ++cameras;
        return cameraOK;
    }
    bool Prepare(const SourceNvidiaFrameGuides& frame)
    {
        Require(cameras == 1 && decisions == 0, "prepare follows valid camera, before generation publication");
        Require(frame.uiColorAndAlpha == expected.uiColorAndAlpha && frame.hudLessColor == expected.hudLessColor, "native UI and HUD-less tag identity, including null fallback");
        if (frame.hudLessColor) { Require(Pixel(context, frame.hudLessColor) == ExpectedOutput(), "HUD-less tag contains native reconstruction"); }
        ++prepares;
        return prepareOK;
    }
    void PublishGeneration(bool prepared)
    {
        ++decisions;
        generation = TheosRenderPipeline::SourceGenerationEnabled(warmup, requested, prepared, blocked);
    }
};

// A producer that already completed reconstruction needs no CopyInput, DLSS,
// early-NR or UpscaleSucceeded operation. Compilation enforces that separation.
struct SuppliedFrameOperations
{
    ID3D11DeviceContext* context;
    const SourceNvidiaFrameGuides& expected;
    bool cameraOK, prepareOK, requested, blocked;
    int warmup;
    unsigned cameras{}, prepares{}, decisions{};
    bool generation{true};

    bool CaptureCamera(const SourceNvidiaFrameGuides& frame)
    {
        Require(!cameras && !prepares && !decisions, "supplied frame starts with camera capture");
        Require(frame.motion == expected.motion && frame.depth == expected.depth,
            "supplied guides retain producer identity");
        Require(frame.renderWidth == 12 && frame.renderHeight == 8 &&
            frame.outputWidth == 18 && frame.outputHeight == 12,
            "supplied render and output extents remain distinct");
        Require(frame.jitterX == -0.375f && frame.jitterY == 0.125f &&
            frame.jitterEnabled == expected.jitterEnabled && frame.reset == expected.reset,
            "supplied camera values reach history unchanged");
        Require(Pixel(context, frame.hudLessColor) == std::array<float, 4>{0.5f, 0.75f, 0.25f, 1},
            "preparation consumes the producer's completed world");
        ++cameras;
        return cameraOK;
    }
    bool Prepare(const SourceNvidiaFrameGuides& frame)
    {
        Require(cameraOK && cameras == 1 && !prepares && !decisions,
            "supplied frame preparation follows valid camera exactly once");
        Require(frame.uiColorAndAlpha == expected.uiColorAndAlpha &&
            frame.hudLessColor == expected.hudLessColor,
            "supplied native UI and world tags are preserved");
        ++prepares;
        return prepareOK;
    }
    void PublishGeneration(bool prepared)
    {
        Require(cameras == 1 && prepares == unsigned(cameraOK) && !decisions,
            "supplied generation decision follows preparation exactly once");
        ++decisions;
        generation = TheosRenderPipeline::SourceGenerationEnabled(warmup, requested, prepared, blocked);
    }
};

int main()
{
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context), "WARP device");
    Surface world(device.Get(), 12, 8), input(device.Get(), 12, 8), motion(device.Get(), 12, 8), depth(device.Get(), 12, 8);
    Surface output(device.Get(), 18, 12), ui(device.Get(), 18, 12);
    // Include disabled FG, warm-up and transition blocks independently of vendor
    // outcomes; all three must retain Prepare/NR and a completed DLSS image.
    for (unsigned mask = 0; mask < 8192; ++mask) {
        const bool neuralOK = !(mask & 256), earlyNR = mask & 512, neuralReset = mask & 1024;
        const bool dlss = (mask & 1) && neuralOK, camera = mask & 2, prepare = mask & 4;
        const bool requested = mask & 8, blocked = mask & 16, warming = mask & 32, tagged = mask & 64, reset = mask & 128;
        world.Paint(context.Get(), {0.25f, 0.5f, 0.75f, 1});
        input.Paint(context.Get(), {0, 0, 0, 0});
        output.Paint(context.Get(), {0, 0, 0, 0});
        ID3D11RenderTargetView* rtvs[]{world.rtv.Get(), nullptr, nullptr, input.rtv.Get()};
        context->OMSetRenderTargets(4, rtvs, nullptr);
        SourceNvidiaFrameInputs frame{};
        frame.color = world.texture.Get(); frame.input = input.texture.Get(); frame.output = output.texture.Get();
        frame.motion = motion.texture.Get(); frame.depth = depth.texture.Get();
        frame.uiColorAndAlpha = tagged ? ui.texture.Get() : nullptr;
        frame.hudLessColor = tagged ? output.texture.Get() : nullptr;
        frame.renderWidth = 12; frame.renderHeight = 8; frame.outputWidth = 18; frame.outputHeight = 12;
        frame.sharpness = 0.25f; frame.jitterX = -0.375f; frame.jitterY = 0.125f;
        frame.motionScaleX = 12; frame.motionScaleY = 8; frame.reset = reset; frame.jitterEnabled = true;
        Operations ops{context.Get(), world, output, frame, dlss, camera, prepare, requested, blocked, warming ? 3 : 0};
        ops.reShade = mask & 2048; ops.reShadeBefore = mask & 4096;
        ops.neuralOK = neuralOK; ops.earlyNR = earlyNR; ops.neuralReset = neuralReset;
        const auto result = SourceNvidiaFrameEvaluator::Evaluate(context.Get(), frame, ops);
        Require(result.upscaled == dlss && result.cameraValid == (dlss && camera) && result.prepared == (dlss && camera && prepare), "separate reconstruction/camera/preparation outcomes");
        Require(ops.upscales == unsigned(dlss) && ops.cameras == unsigned(dlss) && ops.prepares == unsigned(dlss && camera) && ops.decisions == unsigned(dlss), "failed upscale leaves camera/preparation/generation untouched");
        Require(ops.neuralCalls == 1 && ops.dlssCalls == unsigned(neuralOK), "failed NR stops DLSS and all later stages");
        Require(ops.reShadeCalls == unsigned(ops.reShade && neuralOK && (ops.reShadeBefore || dlss)), "selected effect stage runs once and skips failed producers");
        Require(frame.reset == reset, "per-frame NR resets do not mutate the caller's input snapshot");
        Require(ops.generation == (dlss && camera && prepare && requested && !blocked && !warming), "generation gate with preparation independent of checkbox");
        Require(Pixel(context.Get(), output.texture.Get()) == (dlss ? ops.ExpectedOutput() : std::array<float, 4>{0, 0, 0, 0}), "real output survives preparation failure");
    }
    for (unsigned mask = 0; mask < 256; ++mask) {
        SourceNvidiaFrameGuides frame{};
        frame.motion = motion.texture.Get(); frame.depth = depth.texture.Get();
        frame.uiColorAndAlpha = (mask & 32) ? ui.texture.Get() : nullptr;
        frame.hudLessColor = output.texture.Get();
        frame.renderWidth = 12; frame.renderHeight = 8; frame.outputWidth = 18; frame.outputHeight = 12;
        frame.jitterX = -0.375f; frame.jitterY = 0.125f;
        frame.reset = mask & 64; frame.jitterEnabled = mask & 128;
        const bool camera = mask & 1, prepare = mask & 2, requested = mask & 4, blocked = mask & 8;
        const int warmup = (mask & 16) ? 3 : 0;
        output.Paint(context.Get(), {0.5f, 0.75f, 0.25f, 1});
        world.Paint(context.Get(), {1, 0, 0, 1});
        input.Paint(context.Get(), {0, 0, 0, 0});
        SuppliedFrameOperations ops{context.Get(), frame, camera, prepare, requested, blocked, warmup};
        const auto result = SourceNvidiaFramePreparation::PrepareCompletedFrame(frame, ops);
        Require(result.cameraValid == camera && result.prepared == (camera && prepare),
            "supplied camera and preparation results are independent");
        Require(ops.cameras == 1 && ops.prepares == unsigned(camera) && ops.decisions == 1,
            "FG off, warmup and transitions retain preparation for late NR");
        Require(ops.generation == (camera && prepare && requested && !blocked && !warmup),
            "supplied preparation failure clears a previous enabled generation decision");
        Require(Pixel(context.Get(), output.texture.Get()) == std::array<float, 4>{0.5f, 0.75f, 0.25f, 1},
            "supplied world survives camera or preparation failure");
        Require(Pixel(context.Get(), world.texture.Get()) == std::array<float, 4>{1, 0, 0, 1} &&
            Pixel(context.Get(), input.texture.Get()) == std::array<float, 4>{0, 0, 0, 0},
            "supplied frame path does not touch native reconstruction surfaces");
    }
    std::puts("PASS: 8192 native and 256 supplied-frame ordering/failure/reset/generation combinations");
}
