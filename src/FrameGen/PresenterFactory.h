#pragma once
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <vector>

namespace TheosRenderPipeline
{
    // Observe the real HWND owner beneath Intel's COM proxy. Probing the proxy
    // itself prevents xefgSwapChainDestroy from succeeding. Forward factory
    // operations without changing descriptors or window association. During a
    // same-provider rebuild, supply the retired native transport on the SDK's
    // new queue instead of creating a second flip chain on the same HWND.
    class PresenterFactory final : public IDXGIFactory2
    {
    public:
        explicit PresenterFactory(IDXGIFactory2* factory, IDXGISwapChain* reuse = nullptr) : factory_(factory), native_(reuse), reuse_(reuse != nullptr) {}
        IDXGISwapChain* Native() const { return native_.Get(); }
        void ReleaseNative() { native_.Reset(); }
        static HRESULT NativeFallbackAfterRetirement(IDXGISwapChain* chain, ID3D12CommandQueue* queue)
        {
            if (!chain || !queue) { return E_POINTER; }
            DXGI_SWAP_CHAIN_DESC desc{};
            auto hr = chain->GetDesc(&desc);
            if (FAILED(hr)) { return hr; }
            Microsoft::WRL::ComPtr<IDXGISwapChain3> indexed;
            if (FAILED(hr = chain->QueryInterface(IID_PPV_ARGS(&indexed)))) { return hr; }
            // Preserve SDK-native immutable flags (not the proxy's descriptor)
            // and replace the retired Intel queue with the host DIRECT queue.
            UINT masks[2]{};
            IUnknown* queues[2]{queue, queue};
            return indexed->ResizeBuffers1(2, desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format, desc.Flags, masks, queues);
        }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
        {
            if (!out) { return E_POINTER; }
            if (iid == __uuidof(IUnknown) || iid == __uuidof(IDXGIObject) || iid == __uuidof(IDXGIFactory) ||
                iid == __uuidof(IDXGIFactory1) || iid == __uuidof(IDXGIFactory2)) {
                *out = static_cast<IDXGIFactory2*>(this); AddRef(); return S_OK;
            }
            return factory_->QueryInterface(iid, out);
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
        ULONG STDMETHODCALLTYPE Release() override { const auto n = --refs_; if (!n) { delete this; } return n; }
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT s, const void* d) override { return factory_->SetPrivateData(n,s,d); }
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown* d) override { return factory_->SetPrivateDataInterface(n,d); }
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT* s, void* d) override { return factory_->GetPrivateData(n,s,d); }
        HRESULT STDMETHODCALLTYPE GetParent(REFIID i, void** p) override { return factory_->GetParent(i,p); }
        HRESULT STDMETHODCALLTYPE EnumAdapters(UINT n, IDXGIAdapter** a) override { return factory_->EnumAdapters(n,a); }
        HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND w, UINT f) override { return factory_->MakeWindowAssociation(w,f); }
        HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* w) override { return factory_->GetWindowAssociation(w); }
        HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE m, IDXGIAdapter** a) override { return factory_->CreateSoftwareAdapter(m,a); }
        HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT n, IDXGIAdapter1** a) override { return factory_->EnumAdapters1(n,a); }
        BOOL STDMETHODCALLTYPE IsCurrent() override { return factory_->IsCurrent(); }
        BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return factory_->IsWindowedStereoEnabled(); }
        HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* d, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) override
        {
            if (reuse_) { if (out) { *out = nullptr; } return DXGI_ERROR_UNSUPPORTED; }
            const auto hr = factory_->CreateSwapChain(d, desc, out);
            if (SUCCEEDED(hr) && out) { native_ = *out; }
            return hr;
        }
        HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* d, HWND w, const DXGI_SWAP_CHAIN_DESC1* desc,
            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full, IDXGIOutput* restrict, IDXGISwapChain1** out) override
        {
            if (reuse_) {
                if (!out) { return E_POINTER; }
                *out = nullptr;
                if (!desc || !native_ || restrict || (full && !full->Windowed)) { return E_INVALIDARG; }
                DXGI_SWAP_CHAIN_DESC previous{};
                auto hr = native_->GetDesc(&previous);
                if (FAILED(hr)) { return hr; }
                // Only a same-HWND, same-device/layout SDK recreation may reuse
                // this transport. New layouts keep ordinary DXGI creation.
                if (previous.OutputWindow != w || previous.BufferDesc.Width != desc->Width || previous.BufferDesc.Height != desc->Height ||
                    previous.BufferDesc.Format != desc->Format || previous.SampleDesc.Count != desc->SampleDesc.Count ||
                    previous.SampleDesc.Quality != desc->SampleDesc.Quality || previous.SwapEffect != desc->SwapEffect || previous.Flags != desc->Flags) { return E_INVALIDARG; }
                Microsoft::WRL::ComPtr<IUnknown> chainDevice, requestedIdentity, chainIdentity;
                if (FAILED(hr = native_->GetDevice(IID_PPV_ARGS(&chainDevice)))) { return hr; }
                Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
                Microsoft::WRL::ComPtr<ID3D12Device> device;
                if (!d) { return E_POINTER; }
                if (FAILED(hr = d->QueryInterface(IID_PPV_ARGS(&queue))) || FAILED(hr = queue->GetDevice(IID_PPV_ARGS(&device))) ||
                    FAILED(hr = device.As(&requestedIdentity)) || FAILED(hr = chainDevice.As(&chainIdentity))) { return hr; }
                if (requestedIdentity.Get() != chainIdentity.Get()) { return E_INVALIDARG; }
                Microsoft::WRL::ComPtr<IDXGISwapChain3> indexed;
                if (FAILED(hr = native_.As(&indexed))) { return hr; }
                // Intel supplies a new internal presenting queue per context.
                // ResizeBuffers alone preserves the *old* queue association.
                std::vector<UINT> masks(desc->BufferCount, 0);
                std::vector<IUnknown*> queues(desc->BufferCount, queue.Get());
                hr = indexed->ResizeBuffers1(desc->BufferCount, desc->Width, desc->Height, desc->Format, desc->Flags, masks.data(), queues.data());
                return FAILED(hr) ? hr : native_->QueryInterface(IID_PPV_ARGS(out));
            }
            const auto hr = factory_->CreateSwapChainForHwnd(d,w,desc,full,restrict,out);
            if (SUCCEEDED(hr) && out) { native_ = *out; }
            return hr;
        }
        HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* d, IUnknown* w, const DXGI_SWAP_CHAIN_DESC1* desc,
            IDXGIOutput* restrict, IDXGISwapChain1** out) override { if (reuse_) { if (out) { *out = nullptr; } return DXGI_ERROR_UNSUPPORTED; } return factory_->CreateSwapChainForCoreWindow(d,w,desc,restrict,out); }
        HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE h, LUID* l) override { return factory_->GetSharedResourceAdapterLuid(h,l); }
        HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND w, UINT m, DWORD* c) override { return factory_->RegisterStereoStatusWindow(w,m,c); }
        HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE h, DWORD* c) override { return factory_->RegisterStereoStatusEvent(h,c); }
        void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD c) override { factory_->UnregisterStereoStatus(c); }
        HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND w, UINT m, DWORD* c) override { return factory_->RegisterOcclusionStatusWindow(w,m,c); }
        HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE h, DWORD* c) override { return factory_->RegisterOcclusionStatusEvent(h,c); }
        void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD c) override { factory_->UnregisterOcclusionStatus(c); }
        HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* d, const DXGI_SWAP_CHAIN_DESC1* desc,
            IDXGIOutput* restrict, IDXGISwapChain1** out) override { if (reuse_) { if (out) { *out = nullptr; } return DXGI_ERROR_UNSUPPORTED; } return factory_->CreateSwapChainForComposition(d,desc,restrict,out); }
    private:
        std::atomic<ULONG> refs_{1};
        Microsoft::WRL::ComPtr<IDXGIFactory2> factory_;
        Microsoft::WRL::ComPtr<IDXGISwapChain> native_;
        bool reuse_{};
    };
}
