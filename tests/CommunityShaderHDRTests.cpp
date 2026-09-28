#include "CommunityShaderUIBoundary.h"
#include "FrameGen/CommunityShaderFrame.h"
#include "FrameGen/D3D11LiveSlot.h"
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

static UINT Pixel(ID3D11DeviceContext* context, ID3D11Texture2D* source)
{
    Require(source != nullptr, "readback source");
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc, nullptr, &staging), "staging");
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE map{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "readback");
    UINT pixel{}; std::memcpy(&pixel, map.pData, sizeof(pixel)); context->Unmap(staging.Get(), 0);
    return pixel;
}

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
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
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
    void Composite(bool capture)
    {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CSSetShader(shader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* inputs[]{sceneSRV.Get(), uiSRV.Get()};
        context->CSSetShaderResources(0, 2, inputs);
        context->CSSetUnorderedAccessViews(0, 1, outputUAV.GetAddressOf(), nullptr);
        const auto result = frame.CaptureDisplayTransform(context.Get(), width / 4, height / 2, 1, Replay);
        Require(result == (capture ? S_OK : S_FALSE), "only an identified current-frame compositor is replayed");
        if (capture) { Require(frame.PresentationStatus(desc) == State::AwaitingCopy, "replay awaits actual presentation copy"); }
        ComPtr<ID3D11ShaderResourceView> restoredScene, restoredUI;
        ComPtr<ID3D11UnorderedAccessView> restoredOutput;
        context->CSGetShaderResources(0, 1, &restoredScene); context->CSGetShaderResources(1, 1, &restoredUI);
        context->CSGetUnorderedAccessViews(0, 1, &restoredOutput);
        Require(restoredScene == sceneSRV && restoredUI == uiSRV && restoredOutput == outputUAV, "HDR producer bindings restored");
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
    using Copy = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
    static Fixture* fixture{};
    static D3D11LiveSlot dispatchSlot, copySlot;
    static unsigned dispatchCalls{}, copyCalls{};

    static void STDMETHODCALLTYPE Dispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
    {
        ++dispatchCalls;
        const auto original = reinterpret_cast<CommunityShaderFrame::Dispatch>(dispatchSlot.Original());
        fixture->frame.CaptureDisplayTransform(context, x, y, z, original);
        original(context, x, y, z);
    }
    static void STDMETHODCALLTYPE CopyResource(ID3D11DeviceContext* context, ID3D11Resource* destination, ID3D11Resource* source)
    {
        ++copyCalls;
        if (D3D11FrameCopy::SameObject(destination, fixture->presentation.Get())) {
            fixture->frame.ConfirmPresentationCopy(source);
        }
        reinterpret_cast<Copy>(copySlot.Original())(context, destination, source);
    }
    static void Install()
    {
        Require(dispatchSlot.Ensure(fixture->context.Get(), 41, reinterpret_cast<std::uintptr_t>(&Dispatch)), "install live Dispatch observer");
        Require(copySlot.Ensure(fixture->context.Get(), 47, reinterpret_cast<std::uintptr_t>(&CopyResource)), "install live CopyResource observer");
        dispatchCalls = copyCalls = 0;
    }
    static void Restore(std::size_t index, std::uintptr_t hook, std::uintptr_t original)
    {
        auto* slot = *reinterpret_cast<std::uintptr_t**>(fixture->context.Get()) + index;
        DWORD previous{}, ignored{};
        Require(VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous), "unprotect test observer slot");
        // Do not overwrite a replacement installed by the runtime.
        InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),
            reinterpret_cast<void*>(original), reinterpret_cast<void*>(hook));
        Require(VirtualProtect(slot, sizeof(*slot), previous, &ignored), "restore test slot protection");
    }
    static void Composite()
    {
        auto& f = *fixture;
        f.context->OMSetRenderTargets(0, nullptr, nullptr);
        f.context->CSSetShader(f.shader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* inputs[]{f.sceneSRV.Get(), f.uiSRV.Get()};
        f.context->CSSetShaderResources(0, 2, inputs);
        f.context->CSSetUnorderedAccessViews(0, 1, f.outputUAV.GetAddressOf(), nullptr);
        // Unlike Fixture::Composite, these reach capture only through the real
        // context table. No direct helper calls can conceal a missing observer.
        f.context->Dispatch(f.width / 4, f.height / 2, 1);
        f.context->ClearState();
        f.context->CopyResource(f.presentation.Get(), f.converted.Get());
    }
    static bool Handoff()
    {
        Fixture f; fixture = &f;
        f.Begin(1); f.FinishUI(); Install(); Composite();
        Require(dispatchCalls && copyCalls && f.frame.PresentationStatus(f.desc) == State::Ready,
            "live callbacks prepare a complete HDR frame");
        const auto clean = Pixel(f.context.Get(), f.frame.Hudless(f.desc));
        const auto visible = Pixel(f.context.Get(), f.presentation.Get());
        Require(clean != visible, "live replay excludes the visible UI");
        f.Present();

        f.Begin(2); f.FinishUI(); Install();
        // This ordinary runtime call can rewrite the context table after the
        // last UI-boundary repair, without a SwapDeviceContextState call.
        f.context->Flush();
        Composite();
        const auto state = f.frame.PresentationStatus(f.desc);
        const bool continuous = dispatchCalls && copyCalls && state == State::Ready;
        if (continuous) {
            Require(Pixel(f.context.Get(), f.frame.Hudless(f.desc)) == clean, "continuous live capture stays HUDless");
        } else {
            Require((state == State::AwaitingTransform || state == State::AwaitingCopy) && !f.frame.Hudless(f.desc),
                "lost observer refuses stale frame-generation inputs");
        }
        std::printf("Live HDR observers after Flush: Dispatch=%u CopyResource=%u capture=%s\n",
            dispatchCalls, copyCalls, continuous ? "ready" : "missing (known reliability gap)");
        Require(Pixel(f.context.Get(), f.presentation.Get()) == visible, "visible HDR output survives a capture gap");
        f.Present();

        f.Begin(3); f.FinishUI(); Install(); Composite();
        Require(f.frame.PresentationStatus(f.desc) == State::Ready, "next-frame observer repair recovers capture");
        Require(Pixel(f.context.Get(), f.frame.Hudless(f.desc)) == clean, "recovered frame has fresh HUDless output");
        f.Present();
        Restore(41, reinterpret_cast<std::uintptr_t>(&Dispatch), dispatchSlot.Original());
        Restore(47, reinterpret_cast<std::uintptr_t>(&CopyResource), copySlot.Original());
        fixture = nullptr;
        return continuous;
    }
}

int main(int argc, char** argv)
{
    const bool requireStable = argc == 2 && std::string_view(argv[1]) == "--require-stable-observers";
    Require(argc == 1 || requireStable, "supported arguments");
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
    const bool continuous = LiveObservers::Handoff();
    // Default coverage checks the handoff, safe failure and recovery. This
    // explicit acceptance probe must also pass before claiming reliable HDR FG.
    if (requireStable) {
        Require(continuous, "HDR observers must survive ordinary post-UI runtime work");
    }
}
