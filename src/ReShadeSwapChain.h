#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <atomic>

namespace TheosRenderPipeline
{
    // An explicitly managed ReShade runtime needs a swapchain-shaped resource
    // provider. This object has no presentation/resize capability and never
    // enters DXGI factory hooks. The runtime retains views of this stable image.
    class ReShadeSwapChain final : public IDXGISwapChain
    {
    public:
        ReShadeSwapChain(ID3D11Device* device, ID3D11Texture2D* color, HWND window) :
            device_(device), color_(color), window_(window) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
        {
            if (!out) { return E_POINTER; }
            *out = nullptr;
            if (iid != __uuidof(IUnknown) && iid != __uuidof(IDXGIObject) &&
                iid != __uuidof(IDXGIDeviceSubObject) && iid != __uuidof(IDXGISwapChain)) { return E_NOINTERFACE; }
            *out = static_cast<IDXGISwapChain*>(this); AddRef(); return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
        ULONG STDMETHODCALLTYPE Release() override
        { const auto count = --references_; if (!count) { delete this; } return count; }
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return DXGI_ERROR_NOT_FOUND; }
        HRESULT STDMETHODCALLTYPE GetParent(REFIID, void** out) override
        { if (out) { *out = nullptr; } return E_NOINTERFACE; }
        HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** out) override { return device_->QueryInterface(iid, out); }
        HRESULT STDMETHODCALLTYPE Present(UINT, UINT) override { return DXGI_ERROR_INVALID_CALL; }
        HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID iid, void** out) override
        { if (!out) { return E_POINTER; } *out = nullptr; return index ? DXGI_ERROR_INVALID_CALL : color_->QueryInterface(iid, out); }
        HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput*) override { return DXGI_ERROR_INVALID_CALL; }
        HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** output) override
        { if (fullscreen) { *fullscreen = FALSE; } if (output) { *output = nullptr; } return S_OK; }
        HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* out) override
        {
            if (!out) { return E_POINTER; }
            D3D11_TEXTURE2D_DESC desc{}; color_->GetDesc(&desc);
            *out = {}; out->BufferDesc.Width = desc.Width; out->BufferDesc.Height = desc.Height;
            out->BufferDesc.Format = desc.Format; out->SampleDesc = desc.SampleDesc;
            out->BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; out->BufferCount = 1;
            out->OutputWindow = window_; out->Windowed = TRUE; out->SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT, UINT, UINT, DXGI_FORMAT, UINT) override { return DXGI_ERROR_INVALID_CALL; }
        HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return DXGI_ERROR_INVALID_CALL; }
        HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** out) override
        { if (out) { *out = nullptr; } return DXGI_ERROR_NOT_FOUND; }
        HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*) override { return DXGI_ERROR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* out) override { if (out) { *out = 0; } return S_OK; }
    private:
        std::atomic_ulong references_{1};
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> color_;
        HWND window_{};
    };
}
