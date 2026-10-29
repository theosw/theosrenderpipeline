#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

// Opt-in ReShade route ([Compatibility] ReShadeGameDevice). ReShade wraps an
// owned, non-presenting surface through its ordinary factory, so its runtime
// and every add-on use the game's own D3D11 device, context and draw events,
// as they do without TRP. Needs stock ReShade 6.8's IID_UnwrappedObject.
namespace TheosRenderPipeline::ReShadeGameDevice
{
    // Hands `surface` to ReShade's factory with the game's (ReShade-wrapped)
    // device. The returned chain owns ReShade's runtime: release the chain to
    // destroy it, never ReShadeDestroyEffectRuntime. Rendering thread only.
    HRESULT Wrap(ID3D11Device* device, IDXGISwapChain* surface, Microsoft::WRL::ComPtr<IDXGISwapChain>& chain);

    // ReShade draws its GUI on its own native context, bypassing the game's
    // wrapped one, so the route patches the native OMSetRenderTargets entries
    // (both multithread-protection modes) for the process lifetime. Calls
    // outside a GUIRoute pass straight through. Install outside any bind.
    bool InstallGUIRoute(ID3D11DeviceContext* context);

    // While alive on this thread, a single-target bind of `image` on this
    // context's native object draws into `target` instead, except inside
    // ReShade's effect passes (BeginEffects/FinishEffects).
    class GUIRoute final
    {
    public:
        GUIRoute(ID3D11DeviceContext* context, ID3D11Resource* image, ID3D11RenderTargetView* target);
        ~GUIRoute();
        GUIRoute(const GUIRoute&) = delete;
        GUIRoute& operator=(const GUIRoute&) = delete;
        explicit operator bool() const { return active_ == this; }
    private:
        friend struct Router;
        friend void BeginEffects();
        friend void FinishEffects();
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
        ID3D11Resource* image_{};
        ID3D11RenderTargetView* target_{};
        unsigned effects_{};
        static thread_local GUIRoute* active_;
    };
    // reshade_begin_effects / reshade_finish_effects.
    void BeginEffects();
    void FinishEffects();

    // The back buffer ReShade and add-ons see: the full frame, scene with the
    // UI composited over it, without ReShade's GUI (that goes to the UI layer).
    class FrameImage final
    {
    public:
        HRESULT Initialize(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT storage, DXGI_FORMAT typed);
        // Caller owns context isolation. Without a separate UI layer the
        // scene already holds the UI and is copied as it is.
        HRESULT Capture(ID3D11DeviceContext* context, ID3D11Texture2D* scene, ID3D11Texture2D* ui, bool uiLayer);
        ID3D11Texture2D* Texture() const { return image_.Get(); }
        void Reset() { *this = {}; }
    private:
        Microsoft::WRL::ComPtr<ID3D11Texture2D> image_, scene_, ui_;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sceneSRV_, uiSRV_;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    };
}
