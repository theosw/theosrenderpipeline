#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

namespace TheosRenderPipeline
{
    // Owns composition resources only; it does not know Skyrim, Streamline, or
    // a presentation provider. The host must retire GPU work before Initialize
    // or ResetAfterRetirement. Invalidate retains every COM resource on failure.
    class NativeUIComposition
    {
    public:
        NativeUIComposition() = default;
        NativeUIComposition(const NativeUIComposition&) = delete;
        NativeUIComposition& operator=(const NativeUIComposition&) = delete;
        bool Initialize(ID3D11Device* a_device, ID3D11DeviceContext* a_context,
            ID3D11Texture2D* a_scene, const D3D11_TEXTURE2D_DESC& a_outputDesc, bool dedicated);
        bool Extract(ID3D11DeviceContext* a_context, ID3D11Texture2D* a_presentation);
        bool Compose(ID3D11DeviceContext* a_context, ID3D11Texture2D* a_presentation);
        // Compose an independent native foreground without changing the stable
        // game UI texture tagged for frame generation.
        bool ComposeOverlay(ID3D11DeviceContext* context, ID3D11Texture2D* presentation,
            ID3D11ShaderResourceView* overlay);
        void ResetAfterRetirement();
        void Invalidate() { nativeUIExtractionAvailable_ = nativeUITextureMode_ = false; }
        bool Available() const { return nativeUIExtractionAvailable_; }
        bool Dedicated() const { return nativeUITextureMode_; }
        ID3D11Texture2D* TaggedTexture() const { return nativeUIColorAndAlpha_.Get(); }
        ID3D11Texture2D* RenderTexture() const { return nativeUIRenderTexture_.Get(); }
        ID3D11RenderTargetView* RenderRTV() const { return nativeUIRenderRTV_.Get(); }
    private:
        bool ComposeLayer(ID3D11DeviceContext* context, ID3D11Texture2D* presentation,
            ID3D11ShaderResourceView* layer);
        bool Matches(ID3D11Texture2D* texture) const;
        UINT outputWidth_{}, outputHeight_{};
        DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
        bool nativeUIExtractionAvailable_{}, nativeUITextureMode_{};
        std::uint64_t nativeUIExtractionCount_{};
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> upscaleOutputSRV_;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> nativeComposedSnapshot_;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> nativeComposedSnapshotSRV_;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> nativeUIColorAndAlpha_;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> nativeUIColorAndAlphaSRV_;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> nativeUIColorAndAlphaRTV_;
	Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> nativeUIColorAndAlphaUAV_;
	Microsoft::WRL::ComPtr<ID3D11ComputeShader> nativeUIExtractionShader_;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> nativeUIRenderTexture_;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> nativeUIRenderRTV_;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> nativeUICompositionTexture_;
	Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> nativeUICompositionUAV_;
	Microsoft::WRL::ComPtr<ID3D11ComputeShader> nativeUICompositionShader_;
    };
}
