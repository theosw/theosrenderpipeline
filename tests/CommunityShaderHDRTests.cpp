#include "CommunityShaderUIBoundary.h"
#include "FrameGen/CommunityShaderFrame.h"
#include "FrameGen/D3D11EntryObservers.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

using Microsoft::WRL::ComPtr;
using namespace TheosRenderPipeline;
using State = CommunityShaderFrame::PresentationState;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Check(HRESULT hr, const char* why) { Require(SUCCEEDED(hr), why); }

static ComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.Format = format; desc.BindFlags = flags;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    ComPtr<ID3D11Texture2D> texture;
    Check(device->CreateTexture2D(&desc, nullptr, &texture), "texture creation");
    return texture;
}

static UINT Pixel(ID3D11DeviceContext* context, ID3D11Texture2D* source, UINT x = 0, UINT y = 0)
{
    Require(source != nullptr, "readback source");
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc, nullptr, &staging), "staging");
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE map{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "readback");
    const UINT bytes = desc.Format == DXGI_FORMAT_R8_UNORM ? 1 : sizeof(UINT);
    UINT pixel{}; std::memcpy(&pixel, static_cast<const char*>(map.pData) + y * map.RowPitch + x * bytes, bytes);
    context->Unmap(staging.Get(), 0);
    return pixel;
}

static D3D_DRIVER_TYPE driver = D3D_DRIVER_TYPE_WARP;
static unsigned replayCount{};
static void STDMETHODCALLTYPE Replay(ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
{
    ++replayCount; context->Dispatch(x, y, z);
}

// An independent, small display producer: FP16 linear BT.709 scene, separate
// alpha UI, BT.2020/PQ RGB10 output. This tests ordering/format transport, not
// the visual tuning or implementation of any particular CS HDR shader.
constexpr char shaderSource[] = R"(
Texture2D<float4> scene : register(t0);
Texture2D<float4> ui : register(t1);
RWTexture2D<float4> output : register(u0);
float3 pq(float3 nits) {
    float3 p = pow(max(nits, 0) / 10000, 2610.0 / 16384.0);
    return pow((3424.0 / 4096.0 + (2413.0 / 128.0) * p) /
        (1 + (2392.0 / 128.0) * p), 2523.0 / 32.0);
}
[numthreads(4,2,1)] void main(uint3 p : SV_DispatchThreadID) {
    float4 overlay = ui.Load(int3(p.xy, 0));
    float3 rgb = lerp(scene.Load(int3(p.xy, 0)).rgb * 203, overlay.rgb * 203, overlay.a);
    float3 rec2020 = mul(float3x3(0.627404,0.329283,0.043313,
        0.069097,0.919540,0.011362, 0.016391,0.088013,0.895595), rgb);
    output[p.xy] = float4(pq(rec2020), 1);
})";

struct Fixture
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Texture2D> guides, scene, ui, converted, presentation;
    ComPtr<ID3D11RenderTargetView> sceneRTV, uiRTV;
    ComPtr<ID3D11ShaderResourceView> sceneSRV, uiSRV;
    ComPtr<ID3D11UnorderedAccessView> outputUAV;
    CommunityShaderFrame frame;
    CommunityShaderUIBoundary boundary;
    D3D11_TEXTURE2D_DESC desc{};
    unsigned overlays{}, presents{};
    UINT width{}, height{};

    Fixture()
    {
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        Check(D3D11CreateDevice(nullptr, driver, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &device, nullptr, &context), "WARP device");
        ComPtr<ID3DBlob> code, errors;
        Check(D3DCompile(shaderSource, sizeof(shaderSource) - 1, nullptr, nullptr, nullptr,
            "main", "cs_5_0", 0, 0, &code, &errors), "HDR compositor shader");
        Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader), "compute shader");
        Resize(8, 4);
    }
    void Resize(UINT w, UINT h)
    {
        context->ClearState(); frame.ResetAfterRetirement();
        width = w; height = h;
        guides = Texture(device.Get(), w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        scene = Texture(device.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        ui = Texture(device.Get(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        converted = Texture(device.Get(), w, h, DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_BIND_UNORDERED_ACCESS);
        presentation = Texture(device.Get(), w, h, DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_BIND_RENDER_TARGET);
        Check(device->CreateRenderTargetView(scene.Get(), nullptr, sceneRTV.ReleaseAndGetAddressOf()), "scene RTV");
        Check(device->CreateRenderTargetView(ui.Get(), nullptr, uiRTV.ReleaseAndGetAddressOf()), "UI RTV");
        Check(device->CreateShaderResourceView(scene.Get(), nullptr, sceneSRV.ReleaseAndGetAddressOf()), "scene SRV");
        Check(device->CreateShaderResourceView(ui.Get(), nullptr, uiSRV.ReleaseAndGetAddressOf()), "UI SRV");
        Check(device->CreateUnorderedAccessView(converted.Get(), nullptr, outputUAV.ReleaseAndGetAddressOf()), "output UAV");
        presentation->GetDesc(&desc);
    }
    void Begin(std::uint64_t number, bool world = true)
    {
        context->ClearState();
        const float hdr[]{2, .5f, .25f, 1}, empty[4]{};
        context->ClearRenderTargetView(sceneRTV.Get(), hdr);
        context->ClearRenderTargetView(uiRTV.Get(), empty);
        if (world) {
            Check(frame.CaptureGuides(context.Get(), number, guides.Get(), guides.Get(), {width / 2, height / 2}, {width, height}), "guides");
            Check(frame.CaptureScene(context.Get(), scene.Get()), "FP16 scene capture");
        }
    }
    bool FinishUI()
    {
        frame.SetUIBoundary(CommunityShaderFrame::Texture(uiRTV.Get()).Get());
        ++overlays;
        const float green[]{0, 1, 0, 1}; context->ClearRenderTargetView(uiRTV.Get(), green);
        return true;
    }
    void Composite(bool capture, bool captureAlpha = false)
    {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CSSetShader(shader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* inputs[]{sceneSRV.Get(), uiSRV.Get()};
        context->CSSetShaderResources(0, 2, inputs);
        context->CSSetUnorderedAccessViews(0, 1, outputUAV.GetAddressOf(), nullptr);
        const auto result = frame.CaptureDisplayTransform(context.Get(), width / 4, height / 2, 1, Replay, captureAlpha);
        Require(result == (capture ? S_OK : S_FALSE), "only an identified current-frame compositor is replayed");
        if (capture) { Require(frame.PresentationStatus(desc) == State::AwaitingCopy, "replay awaits actual presentation copy"); }
        Require(!frame.UI(desc), "UI alpha remains unpublished until the actual display copy");
        ComPtr<ID3D11ShaderResourceView> restoredScene, restoredUI;
        ComPtr<ID3D11UnorderedAccessView> restoredOutput;
        ComPtr<ID3D11ComputeShader> restoredShader;
        context->CSGetShader(&restoredShader, nullptr, nullptr);
        context->CSGetShaderResources(0, 1, &restoredScene); context->CSGetShaderResources(1, 1, &restoredUI);
        context->CSGetUnorderedAccessViews(0, 1, &restoredOutput);
        Require(restoredScene == sceneSRV && restoredUI == uiSRV && restoredOutput == outputUAV && restoredShader == shader, "HDR producer shader and bindings restored");
        context->Dispatch(width / 4, height / 2, 1);
        context->ClearState();
        Require(!frame.ConfirmPresentationCopy(scene.Get()), "unrelated copy rejected");
        Require(frame.ConfirmPresentationCopy(converted.Get()) == capture, "presentation confirmation");
        context->CopyResource(presentation.Get(), converted.Get());
    }
    void Present(bool test = false)
    {
        boundary.BeforePresent(test, false, [&] { return FinishUI(); });
        if (!test) { ++presents; frame.Consume(); }
        boundary.AfterPresent(test);
    }
};

namespace LiveObservers
{
    static Fixture* fixture{};
    static unsigned dispatchCalls{}, copyCalls{}, transforms{};
    static void Dispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z, D3D11EntryObservers::Dispatch original)
    {
        if (!fixture || context != fixture->context.Get()) { return; }
        ++dispatchCalls;
        if (fixture->frame.CaptureDisplayTransform(context, x, y, z, original, true) == S_OK) { ++transforms; }
    }
    static void CopyResource(ID3D11DeviceContext* context, ID3D11Resource* destination, ID3D11Resource* source)
    {
        if (!fixture || context != fixture->context.Get()) { return; }
        ++copyCalls;
        if (D3D11FrameCopy::SameObject(destination, fixture->presentation.Get())) {
            fixture->frame.ConfirmPresentationCopy(source);
        }
    }
    static void Composite()
    {
        auto& f = *fixture;
        f.context->OMSetRenderTargets(0, nullptr, nullptr);
        f.context->CSSetShader(f.shader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* inputs[]{f.sceneSRV.Get(), f.uiSRV.Get()};
        f.context->CSSetShaderResources(0, 2, inputs);
        f.context->CSSetUnorderedAccessViews(0, 1, f.outputUAV.GetAddressOf(), nullptr);
        // Capture is reached only through real D3D11 calls and production entry
        // hooks. Direct helper calls cannot conceal a missing observer.
        f.context->Dispatch(f.width / 4, f.height / 2, 1);
        f.context->ClearState();
        f.context->CopyResource(f.presentation.Get(), f.converted.Get());
    }
    static void Handoff()
    {
        Fixture f; fixture = &f;
        Require(D3D11EntryObservers::Ensure(f.context.Get(), Dispatch, CopyResource), "install entry observers");
        ComPtr<ID3D11Device1> device1; ComPtr<ID3D11DeviceContext1> context1;
        Check(f.device.As(&device1), "device1"); Check(f.context.As(&context1), "context1");
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
        ComPtr<ID3DDeviceContextState> scratch, saved;
        Check(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &scratch), "scratch state");
        ComPtr<ID3D11Query> query;
        D3D11_QUERY_DESC queryDesc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        Check(f.device->CreateQuery(&queryDesc, &query), "query");
        UINT clean{}, visible{};
        for (unsigned number = 1; number <= 64; ++number) {
            f.Begin(number); f.FinishUI();
            // No reinstallation after the UI boundary or these runtime calls.
            switch (number % 4) {
            case 0: f.context->Flush(); break;
            case 1:
                context1->SwapDeviceContextState(scratch.Get(), &saved);
                context1->SwapDeviceContextState(saved.Get(), nullptr); saved.Reset();
                break;
            case 2: {
                f.context->Begin(query.Get()); f.context->End(query.Get());
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
                f.context->GetData(query.Get(), &data, sizeof(data), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                break;
            }
            case 3: f.context->ClearState(); break;
            }
            dispatchCalls = copyCalls = transforms = 0;
            Composite();
            Require(dispatchCalls == 1 && copyCalls == 1 && transforms == 1 && f.frame.PresentationStatus(f.desc) == State::Ready,
                "entry observers prepare each HDR frame exactly once after runtime mutations");
            const auto actualClean = Pixel(f.context.Get(), f.frame.Hudless(f.desc));
            const auto actualVisible = Pixel(f.context.Get(), f.presentation.Get());
            if (number == 1) { clean = actualClean; visible = actualVisible; }
            Require(clean != visible && actualClean == clean && actualVisible == visible,
                "fresh HUDless replay and visible HDR UI survive every mutation");
            Require(Pixel(f.context.Get(), f.frame.UI(f.desc)) == 255,
                "producer alpha reaches each real observed frame without recursive dispatch capture");
            f.Present();
            Require(!f.frame.Hudless(f.desc), "completed presentation cannot supply stale FG inputs");
        }
        // Entry hooks are shared by devices. An unrelated context still forwards
        // its work, without admitting it as the selected producer's frame.
        Fixture unrelated;
        unrelated.Begin(100); unrelated.FinishUI();
        dispatchCalls = copyCalls = transforms = 0;
        unrelated.Composite(true);
        Require(dispatchCalls == 0 && copyCalls == 0 && transforms == 0,
            "unrelated device work does not enter the selected producer");
        fixture = nullptr;
        std::puts("Live HDR entry observers: 64/64 frames ready after flush/query/state changes; fresh HUDless and visible UI pixels verified.");
    }
}

static void UILayers()
{
    Fixture f;
    for (unsigned number = 1; number <= 4; ++number) {
        if (number == 3) { f.Resize(12, 6); }
        f.Begin(number); f.FinishUI();
        // Empty, opaque green, half-alpha red, opaque black, low-coverage AA.
        // A changed-pixel mask incorrectly promoted these fractions to opaque.
        UINT pixels[72]{};
        if (number != 4) {
            pixels[1] = 0xFF00FF00; pixels[2] = 0x800000FF;
            pixels[3] = 0xFF000000; pixels[4] = 0x200000FF;
        }
        f.context->UpdateSubresource(f.ui.Get(), 0, nullptr, pixels, f.width * 4, 0);
        if (number == 2) {
            // Later producer scene changes must never become UI coverage.
            const float changed[]{.1f,.2f,.3f,1};
            f.context->ClearRenderTargetView(f.sceneRTV.Get(), changed);
        }
        Require(!f.frame.UI(f.desc), "no UI alpha before the current compositor");
        f.Composite(true, true);
        auto* captured = f.frame.UI(f.desc); Require(captured != nullptr, "current-frame alpha published");
        D3D11_TEXTURE2D_DESC uiDesc{}; captured->GetDesc(&uiDesc);
        Require(uiDesc.Format == DXGI_FORMAT_R8_UNORM && uiDesc.Width == f.width && uiDesc.Height == f.height,
            "single-channel alpha keeps eight-bit coverage at presentation extent");
        for (UINT y = 0; y < f.height; ++y) { for (UINT x = 0; x < f.width; ++x) {
            const auto alpha = Pixel(f.context.Get(), captured, x, y);
            Require(alpha == (pixels[y*f.width+x] >> 24),
                "empty, black, opaque, half-transparent and antialiased alpha copied exactly");
        } }
        // Independent motion/blending oracle: the translucent underlay must
        // contribute to a generated pixel. The failed opaque fallback gives 0.
        if (number != 4) {
            const float coverage = Pixel(f.context.Get(), captured, 2) / 255.f;
            const float underlayChange = (1.f-coverage) * (.8f-.2f);
            Require(underlayChange > .29f && underlayChange < .31f,
                "translucent coverage preserves moving underlay rather than freezing it opaque");
            const float edge = Pixel(f.context.Get(), captured, 4) / 255.f;
            Require((1.f-edge) > .87f, "low-coverage edge is not promoted to a solid outline");
        }
        const auto saved = Pixel(f.context.Get(), captured, 2);
        const float empty[4]{}; f.context->ClearRenderTargetView(f.uiRTV.Get(), empty);
        Require(Pixel(f.context.Get(), captured, 2) == saved, "producer clear cannot erase the tagged snapshot");
        f.Present(); Require(!f.frame.UI(f.desc), "consumed interval cannot expose stale alpha");
    }
    f.Begin(5); f.FinishUI(); f.Composite(true, false);
    Require(!f.frame.UI(f.desc) && f.frame.Hudless(f.desc), "disabled capture clears alpha without stopping FG");
    f.Present(); f.Begin(6, false);
    Require(!f.frame.UI(f.desc), "loading cannot reuse previous world alpha");
    f.frame.ResetAfterRetirement(); Require(!f.frame.UI(f.desc), "retired alpha unavailable");
    std::puts("CS UI alpha: fractional opacity/AA, moving-underlay oracle, black/empty coverage, scene changes, producer clear, disable, loading and resize passed.");
}

static void SDRUI()
{
    Fixture f;
    const UINT w=9,h=3;
    auto scene = Texture(f.device.Get(), w,h, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto guides = Texture(f.device.Get(), w,h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    CommunityShaderFrame frame;
    Check(frame.CaptureGuides(f.context.Get(), 1, guides.Get(), guides.Get(), {w,h}, {w,h}), "SDR guides");
    Check(frame.CaptureScene(f.context.Get(), scene.Get()), "SDR clean scene");
    frame.SetUIBoundary(scene.Get());
    D3D11_TEXTURE2D_DESC desc{}; scene->GetDesc(&desc);
    Require(frame.Hudless(desc) && !frame.UI(desc),
        "direct SDR UI has no proven alpha; keep ordinary FG without a guessed UI mask");
    std::puts("CS SDR: direct UI leaves recomposition unavailable while HUDless FG remains ready.");
}

int main(int argc, char** argv)
{
    const bool hardware = argc == 2 && std::string_view(argv[1]) == "--hardware";
    // Retain the old acceptance command; continuity is now mandatory by default.
    const bool requireStable = argc == 2 && std::string_view(argv[1]) == "--require-stable-observers";
    Require(argc == 1 || requireStable || hardware, "supported arguments");
    if (hardware) { driver = D3D_DRIVER_TYPE_HARDWARE; }
    Fixture f;
    // Old order: compositor cannot identify t1 and the late overlay is cleared.
    f.Begin(1); f.Composite(false);
    const auto sceneOnly = Pixel(f.context.Get(), f.presentation.Get());
    f.FinishUI();
    Require(f.frame.PresentationStatus(f.desc) == State::AwaitingTransform && !f.frame.Hudless(f.desc), "late boundary reproduces missing FG input");
    Require(Pixel(f.context.Get(), f.presentation.Get()) == sceneOnly, "late overlay missing from presentation");
    f.Present();

    for (std::uint64_t number = 2; number <= 4; ++number) {
        if (number == 4) { f.Resize(12, 6); }
        f.Begin(number);
        const auto before = f.overlays;
        bool engineFinished = false;
        f.boundary.DrawInterface([&] {
            // A nested engine interface call must not finish UI prematurely.
            f.boundary.DrawInterface([] {}, [&] { Require(false, "nested completion"); return false; });
            engineFinished = true;
        }, [&] { Require(engineFinished, "engine UI precedes overlay"); return f.FinishUI(); });
        f.boundary.DrawInterface([] {}, [&] { return f.FinishUI(); });
        f.Present(true);
        f.boundary.BeforePresent(true, true, [&] { return f.FinishUI(); });
        Require(f.overlays == before + 1, "one overlay despite duplicate interface and test Present");
        Require(f.frame.PresentationStatus(f.desc) == State::AwaitingTransform, "UI identified before compositor");
        // Suppression may skip our Present hook entirely. It must not be needed
        // to publish UI identity or pixels, nor consume the captured world.
        f.Composite(true);
        Require(f.frame.PresentationStatus(f.desc) == State::Ready, "HDR frame ready");
        const auto clean = Pixel(f.context.Get(), f.frame.Hudless(f.desc));
        Require(clean == sceneOnly, "FP16 scene converted to matching PQ without overlay");
        const auto visible = Pixel(f.context.Get(), f.presentation.Get());
        Require(visible != clean && ((visible >> 10) & 1023) > (visible & 1023), "green overlay visible in HDR output");
        auto mismatch = f.desc; mismatch.Width += 4;
        Require(f.frame.PresentationStatus(mismatch) == State::ExtentMismatch, "extent diagnostic");
        mismatch = f.desc; mismatch.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        Require(f.frame.PresentationStatus(mismatch) == State::FormatMismatch, "format diagnostic");
        f.Present();
        Require(f.overlays == before + 1 && !f.frame.Hudless(f.desc), "actual Present consumes once without redrawing");
        const float empty[4]{}; f.context->ClearRenderTargetView(f.uiRTV.Get(), empty);
        Require(Pixel(f.context.Get(), f.presentation.Get()) == visible, "producer UI clear cannot erase presented overlay");
    }
    Require(replayCount == 3, "one replay per HDR world frame");
    f.Begin(5, false);
    const auto beforeLoading = f.overlays;
    f.boundary.DrawInterface([] {}, [&] { return f.FinishUI(); });
    f.Composite(false);
    Require(f.overlays == beforeLoading + 1 && !f.frame.Hudless(f.desc), "loading UI works without admitting stale FG scene");
    f.Present();

    unsigned fallback{};
    f.boundary.BeforePresent(true, true, [&] { ++fallback; return true; });
    f.boundary.BeforePresent(false, false, [&] { ++fallback; return true; });
    Require(fallback == 0, "test Present and separate UI never use late fallback");
    f.boundary.BeforePresent(false, true, [&] { ++fallback; return true; });
    f.boundary.BeforePresent(false, true, [&] { ++fallback; return true; });
    Require(fallback == 1, "direct framebuffer fallback once when interface absent");
    f.boundary.AfterPresent(false);
    f.boundary.BeforePresent(false, true, [&] { ++fallback; return true; });
    Require(fallback == 2, "fallback rearmed for next presentation");

    // The producer may hold the same immediate context through another
    // interface pointer. Accept it only when declared, never during isolation.
    ComPtr<ID3D11Device> otherDevice; ComPtr<ID3D11DeviceContext> alias;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &otherDevice, nullptr, &alias), "alias context");
    auto& isolation = f.frame.Isolation();
    Require(!isolation.Accepts(alias.Get()) && isolation.Accepts(alias.Get(), alias.Get()) &&
        isolation.Accepts(f.context.Get(), alias.Get()), "producer context alias accepted only when declared");
    {
        D3D11ContextIsolation::Scope scope{isolation, f.context.Get()};
        Require(scope && !isolation.Accepts(alias.Get(), alias.Get()), "no producer capture during our isolation");
    }
    std::puts("CS HDR: late-boundary failure reproduced; early UI, FP16/PQ scene capture, producer state, suppression, test Present, loading, resize and context alias passed.");
    LiveObservers::Handoff();
    UILayers();
    SDRUI();
}
