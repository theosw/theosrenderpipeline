#pragma once

#include "CommunityLensMode.h"
#include "D3D11ComputeBindings.h"
#include "D3D11FrameCopy.h"
#include <d3dcompiler.h>

namespace TheosRenderPipeline
{
    // Where Bottled Shaders' Camera FX lens overlay goes this frame.
    enum class LensRoute
    {
        SceneChosen,          // The user keeps it on the scene.
        SceneNotGenerating,   // Frame generation is off: nothing to separate it for.
        SceneSuspended,       // An earlier lens could only be laid over the HUD.
        SceneOutsideBoundary, // Not inside TRP's interface call before the menus.
        SceneNoWorld,         // No completed scene is open this frame.
        SceneForeignTarget,   // The producer's target is not the scene TRP captured.
        SceneUnknownSurface,  // A separate interface surface TRP has not seen at a boundary yet.
        ProducerSurface,      // With the HUD on the producer's own interface surface (CS HDR Display).
        Layer,                // On TRP's layer, laid over the scene before the menus.
    };

    // open: the completed scene while it can still be read and drawn on before
    // any menu. target: what the producer would draw the lens on. surface: what
    // the interface draws on now. knownSurface: the producer's separate
    // interface surface as last seen at an interface boundary.
    inline LensRoute ChooseLensRoute(CommunityLensMode mode, bool generating, bool suspended, bool beforeMenus,
        ID3D11Texture2D* open, ID3D11Texture2D* target, ID3D11Texture2D* surface, ID3D11Texture2D* knownSurface)
    {
        if (mode == CommunityLensMode::Scene) { return LensRoute::SceneChosen; }
        if (!generating) { return LensRoute::SceneNotGenerating; }
        if (suspended) { return LensRoute::SceneSuspended; }
        if (!beforeMenus) { return LensRoute::SceneOutsideBoundary; }
        if (!open) { return LensRoute::SceneNoWorld; }
        if (!D3D11FrameCopy::SameObject(target, open)) { return LensRoute::SceneForeignTarget; }
        if (surface && !D3D11FrameCopy::SameObject(surface, open)) {
            // CS HDR Display draws the interface on its own surface and composes
            // it over the scene itself. Only a surface already seen at an
            // interface boundary is trusted to be that one.
            return D3D11FrameCopy::SameObject(surface, knownSurface) ? LensRoute::ProducerSurface : LensRoute::SceneUnknownSurface;
        }
        return LensRoute::Layer;
    }

    inline const char* Describe(LensRoute route)
    {
        switch (route) {
        case LensRoute::SceneChosen: return "on the scene (Bottled lens effects: On the scene)";
        case LensRoute::SceneNotGenerating: return "on the scene; frame generation is off";
        case LensRoute::SceneSuspended: return "on the scene; an earlier lens could only be laid over the HUD, so the layer is off until restart";
        case LensRoute::SceneOutsideBoundary: return "on the scene; it was drawn outside TRP's interface boundary or the world-completion hook is missing";
        case LensRoute::SceneNoWorld: return "on the scene; no completed scene is open this frame";
        case LensRoute::SceneForeignTarget: return "on the scene; the producer's target is not the scene TRP captured";
        case LensRoute::SceneUnknownSurface: return "on the scene; the producer's interface surface is not known yet";
        case LensRoute::ProducerSurface: return "with the HUD, on the producer's interface surface";
        default: return "on TRP's lens layer, laid over the scene before the menus";
        }
    }

    // Bottled Shaders' Camera FX draws its lens overlay (rain, frost, blood on
    // the lens) premultiplied over the completed scene. Offered a transparent
    // layer of the scene's size, it draws the same overlay there with its
    // coverage in alpha, as it does for its own frame generation. This owner
    // supplies that layer and lays it over the scene before the first menu:
    // the presented frame keeps the lens under the HUD, while the HUD-less
    // scene taken just before does not hold it.
    // The result is what Bottled's own frame generation path shows: its passes
    // collect in the layer and meet the scene once. That equals its direct
    // draws except where a pass adds light without coverage onto a scene that
    // saturates, which then clips once and not per pass.
    // Render thread only. Only compute bindings are touched, and restored.
    class CommunityShaderLens
    {
    public:
        enum class Refusal { None, Scene, Busy, Format, Resources };

        // Compile the composition shader ahead of the first lens, off the frame
        // that starts the rain. Once per process; the bytecode is shared.
        static bool Precompile() { return Code() != nullptr; }

        // Before the producer draws its lens on scene. Null leaves it there.
        ID3D11RenderTargetView* Begin(ID3D11DeviceContext* context, ID3D11Texture2D* scene)
        {
            refusal_ = Refusal::Scene;
            if (!context || !scene || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { return nullptr; }
            if (drawn_) { refusal_ = Refusal::Busy; return nullptr; }
            D3D11_TEXTURE2D_DESC desc{}; scene->GetDesc(&desc);
            if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.MipLevels != 1 ||
                !(desc.BindFlags & D3D11_BIND_RENDER_TARGET)) { return nullptr; }
            if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM) {
                refusal_ = Refusal::Format; return nullptr;
            }
            if (!Ensure(context, scene, desc)) { refusal_ = Refusal::Resources; return nullptr; }
            constexpr float transparent[4]{};
            context->ClearRenderTargetView(layerRTV_.Get(), transparent);
            scene_ = scene;
            drawn_ = true; composed_ = false;
            refusal_ = Refusal::None;
            return layerRTV_.Get();
        }

        // The producer drew into the layer and it is not on the scene yet.
        bool Pending() const { return drawn_ && !composed_; }

        // The layer over the scene, in place: premultiplied colour over it,
        // scene alpha kept.
        HRESULT Compose(ID3D11DeviceContext* context)
        {
            if (!Pending()) { return S_FALSE; }
            // One attempt: a second would lay the lens over itself.
            composed_ = true;
            if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !scene_) { return E_INVALIDARG; }
            context->CopyResource(background_.Get(), scene_.Get());
            // If the producer left layerRTV_ bound on the OM, unbind it while
            // layerSRV_ is read in CS so D3D11 does not force-null the SRV.
            ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            ID3D11DepthStencilView* savedDSV{};
            context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
            bool unbindLayerRTV = false;
            for (auto* rtv : savedRTVs) {
                if (!rtv) { continue; }
                Microsoft::WRL::ComPtr<ID3D11Resource> resource;
                rtv->GetResource(&resource);
                if (D3D11FrameCopy::SameObject(resource.Get(), layer_.Get())) { unbindLayerRTV = true; break; }
            }
            if (unbindLayerRTV) {
                ID3D11RenderTargetView* noRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
                context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, noRTVs, savedDSV);
            }
            {
                D3D11ComputeBindings saved{context, 0, 2, 1};
                ID3D11ShaderResourceView* inputs[]{backgroundSRV_.Get(), layerSRV_.Get()};
                ID3D11UnorderedAccessView* outputs[]{resultUAV_.Get()};
                context->CSSetShader(shader_.Get(), nullptr, 0);
                context->CSSetShaderResources(0, 2, inputs);
                context->CSSetUnorderedAccessViews(0, 1, outputs, nullptr);
                context->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
            }
            if (unbindLayerRTV) {
                context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
            }
            for (auto* rtv : savedRTVs) { if (rtv) { rtv->Release(); } }
            if (savedDSV) { savedDSV->Release(); }
            context->CopyResource(scene_.Get(), result_.Get());
            return S_OK;
        }

        // The composed layer of this frame: premultiplied colour and coverage.
        // A ten-bit scene gets a 16-bit layer; two bits of alpha cannot hold
        // coverage. UNORM keeps the producer's per-draw clamp either way.
        ID3D11Texture2D* Layer() const { return drawn_ && composed_ ? layer_.Get() : nullptr; }
        Refusal LastRefusal() const { return refusal_; }

        void NextFrame() { drawn_ = composed_ = false; scene_.Reset(); }

        void ResetAfterRetirement()
        {
            NextFrame();
            layer_.Reset(); background_.Reset(); result_.Reset();
            layerRTV_.Reset(); layerSRV_.Reset(); backgroundSRV_.Reset(); resultUAV_.Reset(); shader_.Reset(); device_.Reset();
            width_ = height_ = 0; format_ = DXGI_FORMAT_UNKNOWN;
            refusal_ = Refusal::None;
        }

    private:
        static ID3DBlob* Code()
        {
            static constexpr char source[] = R"(
Texture2D<float4> Scene : register(t0);
Texture2D<float4> Lens : register(t1);
RWTexture2D<float4> Result : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    Scene.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const float4 scene = Scene.Load(int3(id.xy, 0));
    const float4 lens = Lens.Load(int3(id.xy, 0));
    Result[id.xy] = float4(lens.rgb + scene.rgb * (1.0 - lens.a), scene.a);
}
)";
            static const Microsoft::WRL::ComPtr<ID3DBlob> code = [] {
                Microsoft::WRL::ComPtr<ID3DBlob> compiled;
                if (FAILED(D3DCompile(source, sizeof(source) - 1, "TheosRenderPipelineLensCompositionCS", nullptr, nullptr, "main", "cs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &compiled, nullptr))) { compiled.Reset(); }
                return compiled;
            }();
            return code.Get();
        }

        bool Ensure(ID3D11DeviceContext* context, ID3D11Texture2D* scene, D3D11_TEXTURE2D_DESC desc)
        {
            Microsoft::WRL::ComPtr<ID3D11Device> device, sceneDevice;
            context->GetDevice(&device); scene->GetDevice(&sceneDevice);
            if (!D3D11FrameCopy::SameObject(device.Get(), sceneDevice.Get())) { return false; }
            if (layer_ && shader_ && desc.Width == width_ && desc.Height == height_ && desc.Format == format_ &&
                D3D11FrameCopy::SameObject(device.Get(), device_.Get())) { return true; }
            ResetAfterRetirement();
            auto* code = Code();
            if (!code) { return false; }
            const auto sceneFormat = desc.Format;
            desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = desc.MiscFlags = 0;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> layer, background, result;
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> layerRTV;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> layerSRV, backgroundSRV;
            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> resultUAV;
            Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (sceneFormat == DXGI_FORMAT_R10G10B10A2_UNORM) { desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM; }
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &layer)) ||
                FAILED(device->CreateRenderTargetView(layer.Get(), nullptr, &layerRTV)) ||
                FAILED(device->CreateShaderResourceView(layer.Get(), nullptr, &layerSRV))) { return false; }
            desc.Format = sceneFormat;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &background)) ||
                FAILED(device->CreateShaderResourceView(background.Get(), nullptr, &backgroundSRV))) { return false; }
            desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &result)) ||
                FAILED(device->CreateUnorderedAccessView(result.Get(), nullptr, &resultUAV)) ||
                FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader))) { return false; }
            layer_ = std::move(layer); background_ = std::move(background); result_ = std::move(result);
            layerRTV_ = std::move(layerRTV); layerSRV_ = std::move(layerSRV); backgroundSRV_ = std::move(backgroundSRV);
            resultUAV_ = std::move(resultUAV); shader_ = std::move(shader); device_ = device;
            width_ = desc.Width; height_ = desc.Height; format_ = desc.Format;
            return true;
        }

        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> layer_, background_, result_, scene_;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> layerRTV_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> layerSRV_, backgroundSRV_;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> resultUAV_;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
        UINT width_{}, height_{};
        DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
        Refusal refusal_{Refusal::None};
        bool drawn_{}, composed_{};
    };
}
