#include "FrameGen/NativeUIBlend.h"
#include "FrameGen/NativeUIBlendCoverage.h"
#include "FrameGen/NativeUIPass.h"
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <array>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace TheosRenderPipeline;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Check(HRESULT hr, const char* why) { Require(SUCCEEDED(hr), why); }

static D3D11_BLEND_DESC Blend(D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND srcA, D3D11_BLEND dstA)
{
    D3D11_BLEND_DESC d{}; auto& rt = d.RenderTarget[0];
    rt.BlendEnable = TRUE; rt.SrcBlend = src; rt.DestBlend = dst; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = srcA; rt.DestBlendAlpha = dstA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return d;
}
// Scaleform-style source-over whose stored alpha is a*a on a transparent layer.
static const auto kSquared = Blend(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA);
// IED 1.7.4 ImGui backend: stored alpha a*(1-a).
static const auto kImGui = Blend(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ZERO);
static const auto kCoverage = Blend(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA);
static const auto kAdditive = Blend(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE);

// UI pass entry through the production render-target gate: OMSetRenderTargets
// is replaced in this context's vtable by the same check UpscalerHooks makes.
namespace EntryHook
{
    using Setter = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
    Setter original{};
    NativeUIPass* pass{};
    NativeUIBlendCoverage* coverage{};
    ID3D11RenderTargetView* layer{};
    void STDMETHODCALLTYPE SetTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* t, ID3D11DepthStencilView* d)
    {
        original(c, n, t, d);
        if (!coverage->NeedsSync(pass->Active())) { return; }
        coverage->Sync(c, layer, [c](ID3D11BlendState* s, const FLOAT* f, UINT m) { c->OMSetBlendState(s, f, m); });
    }
}

static void RuleCases()
{
    auto d = kSquared;
    Require(CorrectCoverage(d, false) == UIBlendCoverage::SourceOver, "a*a source-over is corrected");
    Require(d.RenderTarget[0].SrcBlendAlpha == D3D11_BLEND_ONE && d.RenderTarget[0].DestBlendAlpha == D3D11_BLEND_INV_SRC_ALPHA,
        "source-over alpha becomes a + prior*(1-a)");
    Require(d.RenderTarget[0].SrcBlend == D3D11_BLEND_SRC_ALPHA && d.RenderTarget[0].DestBlend == D3D11_BLEND_INV_SRC_ALPHA,
        "RGB factors unchanged");
    d = kImGui;
    Require(CorrectCoverage(d, false) == UIBlendCoverage::SourceOver && d.RenderTarget[0].DestBlendAlpha == D3D11_BLEND_INV_SRC_ALPHA,
        "legacy ImGui a*(1-a) is corrected");
    d = Blend(D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA);
    Require(CorrectCoverage(d, false) == UIBlendCoverage::SourceOver && d.RenderTarget[0].SrcBlend == D3D11_BLEND_ONE,
        "premultiplied source-over is corrected with RGB kept");
    d = kCoverage;
    Require(CorrectCoverage(d, false) == UIBlendCoverage::Unchanged, "already coverage stays unchanged");
    d = kAdditive;
    Require(CorrectCoverage(d, false) == UIBlendCoverage::Additive &&
        d.RenderTarget[0].SrcBlendAlpha == D3D11_BLEND_ZERO && d.RenderTarget[0].DestBlendAlpha == D3D11_BLEND_ONE,
        "additive keeps prior coverage");
    auto reject = [](D3D11_BLEND_DESC v, bool logicOp, const char* why) {
        const auto before = v;
        Require(CorrectCoverage(v, logicOp) == UIBlendCoverage::Unchanged && std::memcmp(&v, &before, sizeof(v)) == 0, why);
    };
    reject(kSquared, true, "logic operation untouched");
    auto v = kSquared; v.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_ALPHA;
    reject(v, false, "partial write mask untouched");
    v = kSquared; v.AlphaToCoverageEnable = TRUE; reject(v, false, "alpha-to-coverage untouched");
    v = kSquared; v.RenderTarget[0].BlendEnable = FALSE; reject(v, false, "unblended (opaque) untouched");
    v = kSquared; v.RenderTarget[0].BlendOp = D3D11_BLEND_OP_REV_SUBTRACT; reject(v, false, "other RGB operations untouched");
    v = Blend(D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_ZERO); reject(v, false, "multiply untouched");
}

int main()
{
    RuleCases();
    ComPtr<ID3D11Device> d; ComPtr<ID3D11DeviceContext> c;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &d, nullptr, &c), "device");
    ComPtr<ID3D11InfoQueue> info; Check(d.As(&info), "info");
    constexpr UINT W = 8, H = 4;
    D3D11_TEXTURE2D_DESC td{}; td.Width = W; td.Height = H; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    auto make = [&](ComPtr<ID3D11Texture2D>& t, ComPtr<ID3D11RenderTargetView>& rtv, ComPtr<ID3D11ShaderResourceView>* srv) {
        Check(d->CreateTexture2D(&td, nullptr, &t), "texture"); Check(d->CreateRenderTargetView(t.Get(), nullptr, &rtv), "RTV");
        if (srv) { Check(d->CreateShaderResourceView(t.Get(), nullptr, srv->GetAddressOf()), "SRV"); }
    };
    ComPtr<ID3D11Texture2D> layer, scene, direct, other; ComPtr<ID3D11RenderTargetView> layerRTV, sceneRTV, directRTV, otherRTV;
    ComPtr<ID3D11ShaderResourceView> layerSRV;
    make(layer, layerRTV, &layerSRV); make(scene, sceneRTV, nullptr); make(direct, directRTV, nullptr); make(other, otherRTV, nullptr);

    constexpr char shader[] = R"(
cbuffer C : register(b0) { float4 colour; };
float4 VS(uint i : SV_VertexID) : SV_Position { float2 p = float2((i << 1) & 2, i & 2); return float4(p * float2(2,-2) + float2(-1,1), 0, 1); }
float4 PS(float4 p : SV_Position) : SV_Target { return colour; }
)";
    ComPtr<ID3DBlob> vs, ps; ComPtr<ID3D11VertexShader> vsh; ComPtr<ID3D11PixelShader> psh;
    Check(D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, nullptr), "VS");
    Check(D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, nullptr), "PS");
    Check(d->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vsh), "vertex shader");
    Check(d->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &psh), "pixel shader");
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; ComPtr<ID3D11Buffer> cb;
    Check(d->CreateBuffer(&bd, nullptr, &cb), "constants");
    auto state = [&](const D3D11_BLEND_DESC& desc) { ComPtr<ID3D11BlendState> s; Check(d->CreateBlendState(&desc, &s), "blend state"); return s; };
    auto draw = [&](ID3D11RenderTargetView* target, ID3D11BlendState* blend, std::array<float, 4> colour) {
        c->UpdateSubresource(cb.Get(), 0, nullptr, colour.data(), 0, 0);
        c->OMSetRenderTargets(1, &target, nullptr); c->OMSetBlendState(blend, nullptr, ~0u);
        const D3D11_VIEWPORT vp{0, 0, float(W), float(H), 0, 1}; c->RSSetViewports(1, &vp);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); c->VSSetShader(vsh.Get(), nullptr, 0);
        c->PSSetShader(psh.Get(), nullptr, 0); auto* b = cb.Get(); c->PSSetConstantBuffers(0, 1, &b); c->Draw(3, 0);
    };
    auto read = [&](ID3D11Texture2D* t) {
        auto sd = td; sd.BindFlags = 0; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> s; Check(d->CreateTexture2D(&sd, nullptr, &s), "staging"); c->CopyResource(s.Get(), t);
        D3D11_MAPPED_SUBRESOURCE m{}; Check(c->Map(s.Get(), 0, D3D11_MAP_READ, 0, &m), "map");
        std::array<float, 4> p{}; std::memcpy(p.data(), m.pData, sizeof(p)); c->Unmap(s.Get(), 0); return p;
    };
    NativeUIBlend compose; Require(compose.Initialize(d.Get(), c.Get(), td.Format), "compositor");
    const std::array<float, 4> world{0.2f, 0.6f, 0.4f, 1.0f};
    const std::array<float, 4> panel{0.9f, 0.3f, 0.1f, 0.7f}, text{0.1f, 0.8f, 0.5f, 0.5f}, glow{0.3f, 0.2f, 0.6f, 0.4f};
    NativeUIBlendCoverage coverage;
    // Reference: the producer draws onto an opaque frame, as without a UI layer.
    // Layer: the same draws into a transparent layer, then the production compositor.
    auto run = [&](const D3D11_BLEND_DESC& producer, bool corrected) {
        const float clear[4]{}; c->ClearRenderTargetView(directRTV.Get(), world.data()); c->ClearRenderTargetView(sceneRTV.Get(), world.data());
        c->ClearRenderTargetView(layerRTV.Get(), clear);
        auto p = state(producer), a = state(kAdditive);
        ID3D11BlendState* sp = corrected ? coverage.Corrected(d.Get(), p.Get()).state : p.Get();
        ID3D11BlendState* sa = corrected ? coverage.Corrected(d.Get(), a.Get()).state : a.Get();
        for (auto* t : {directRTV.Get(), layerRTV.Get()}) {
            const bool ui = t == layerRTV.Get();
            draw(t, ui ? sp : p.Get(), panel); draw(t, ui ? sp : p.Get(), text); draw(t, ui ? sa : a.Get(), glow);
        }
        c->OMSetRenderTargets(0, nullptr, nullptr);
        Require(compose.Compose(c.Get(), scene.Get(), layerSRV.Get(), W, H), "compose");
        return std::pair{read(direct.Get()), read(scene.Get())};
    };
    auto close = [](const std::array<float, 4>& x, const std::array<float, 4>& y) {
        for (int k = 0; k < 3; ++k) { if (std::abs(x[k] - y[k]) > 1e-5f) { return false; } } return true;
    };
    for (const auto* producer : {&kSquared, &kImGui}) {
        const auto [reference, uncorrected] = run(*producer, false);
        Require(!close(reference, uncorrected), "uncorrected producer alpha lets the world show through");
        const auto [reference2, fixed] = run(*producer, true);
        Require(close(reference2, fixed), "corrected layer matches drawing onto the frame (source-over and additive)");
    }
    {
        // A producer already storing coverage is bound unchanged and still matches.
        auto already = state(kCoverage);
        Require(coverage.Corrected(d.Get(), already.Get()).state == already.Get(), "coverage producer bound unchanged");
        const auto [reference, same] = run(kCoverage, true);
        Require(close(reference, same), "coverage producers match");
    }

    // Binding lifetime: corrected only while the UI layer is target 0.
    auto setRaw = [&](ID3D11BlendState* s, const FLOAT* f, UINT m) { c->OMSetBlendState(s, f, m); };
    auto bound = [&] { ComPtr<ID3D11BlendState> s; FLOAT f[4]; UINT m; c->OMGetBlendState(&s, f, &m); return s; };
    coverage.Reset();
    auto squared = state(kSquared), already = state(kCoverage);
    ID3D11RenderTargetView* targets[]{layerRTV.Get()};
    c->OMSetRenderTargets(1, targets, nullptr);
    auto r = coverage.Producer(d.Get(), squared.Get(), true); c->OMSetBlendState(r.state, nullptr, ~0u);
    Require(r.state != squared.Get() && r.firstSeen && coverage.CorrectedBound(), "producer state corrected on the UI layer");
    Require(r.state == already.Get(), "D3D11 returns one object for the identical corrected description");
    targets[0] = otherRTV.Get(); c->OMSetRenderTargets(1, targets, nullptr); coverage.Sync(c.Get(), layerRTV.Get(), setRaw);
    Require(bound().Get() == squared.Get() && !coverage.CorrectedBound(), "leaving the layer restores the producer's state");
    targets[0] = layerRTV.Get(); c->OMSetRenderTargets(1, targets, nullptr); coverage.Sync(c.Get(), layerRTV.Get(), setRaw);
    Require(bound().Get() == already.Get(), "a state set before binding the layer is corrected at bind time");
    // A producer that genuinely uses the already-coverage state keeps it everywhere.
    targets[0] = otherRTV.Get(); c->OMSetRenderTargets(1, targets, nullptr);
    r = coverage.Producer(d.Get(), already.Get(), false); c->OMSetBlendState(r.state, nullptr, ~0u);
    coverage.Sync(c.Get(), layerRTV.Get(), setRaw);
    Require(bound().Get() == already.Get(), "shared description is not mistaken for a corrected state");
    // Host passes that set and restore their own states never become the producer.
    r = coverage.Producer(d.Get(), squared.Get(), false); c->OMSetBlendState(r.state, nullptr, ~0u);
    auto hostState = state(Blend(D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_ZERO));
    coverage.Internal(hostState.Get()); coverage.Internal(already.Get());
    c->OMSetBlendState(already.Get(), nullptr, ~0u); coverage.Sync(c.Get(), layerRTV.Get(), setRaw);
    Require(bound().Get() == squared.Get(), "a restored corrected state outside the layer reverts to the producer's");
    c->ClearState(); coverage.Sync(c.Get(), layerRTV.Get(), setRaw);
    Require(!bound() && !coverage.CorrectedBound(), "ClearState is adopted, nothing is re-bound");

    {
        // A producer state bound before the UI pass, with no later setter (Skyrim's
        // state cache skips redundant calls): entry binds its correction, and
        // leaving the pass restores the producer's own state.
        c->ClearState(); coverage.Reset();
        auto producer = state(kSquared);
        targets[0] = otherRTV.Get(); c->OMSetRenderTargets(1, targets, nullptr);
        coverage.Producer(d.Get(), producer.Get(), false); c->OMSetBlendState(producer.Get(), nullptr, ~0u);
        NativeUIPass pass;
        EntryHook::pass = &pass; EntryHook::coverage = &coverage; EntryHook::layer = layerRTV.Get();
        constexpr std::size_t kOMSetRenderTargets = 33;
        auto** object = reinterpret_cast<void***>(c.Get()); void** saved = *object;
        std::array<void*, 115> table{}; std::copy_n(saved, table.size(), table.begin()); // ID3D11DeviceContext methods
        EntryHook::original = reinterpret_cast<EntryHook::Setter>(table[kOMSetRenderTargets]);
        table[kOMSetRenderTargets] = reinterpret_cast<void*>(&EntryHook::SetTargets); *object = table.data();
        const bool entered = pass.Enter(c.Get(), {layerRTV.Get(), nullptr, W, H, true});
        const auto atEntry = bound();
        pass.End(c.Get());
        const auto afterEnd = bound();
        *object = saved;
        D3D11_BLEND_DESC desc{}; if (atEntry) { atEntry->GetDesc(&desc); }
        Require(entered && atEntry && atEntry.Get() != producer.Get() && desc.RenderTarget[0].SrcBlendAlpha == D3D11_BLEND_ONE,
            "UI pass entry corrects a blend state bound before it");
        Require(afterEnd.Get() == producer.Get(), "leaving the UI pass restores the producer's state");
    }

    c->ClearState(); c->Flush();
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T size{}; info->GetMessage(i, nullptr, &size); std::vector<char> bytes(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data()); info->GetMessage(i, message, &size);
        Require(message->Severity > D3D11_MESSAGE_SEVERITY_ERROR, message->pDescription);
    }
    std::puts("UI layer blend coverage rule/source-over a*a/ImGui a*(1-a)/additive/binding lifetime/pass entry PASS");
}
