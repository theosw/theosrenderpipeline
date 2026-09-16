#include "GameSwapChain.h"

#include "NvidiaHost.h"
#include "UpscalerHooks.h"
#include "PerformanceTuning.h"
#include <chrono>

#include <intrin.h>

namespace
{
    template<class Present> HRESULT MeasureSourcePresent(Present&& present)
    {
        auto* timing = PerformanceTuning::GetSingleton();
        if (!timing->TimingEnabled()) { return present(); }
        const auto start = std::chrono::steady_clock::now();
        const HRESULT result = present();
        const auto elapsed = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
        // CPU API duration includes waits. It is neither GPU generation cost
        // nor time to physical display. Failed calls are not timing samples.
        if (SUCCEEDED(result)) { timing->RecordSourcePresentCpuMs(elapsed); }
        return result;
    }
}

GameSwapChain::GameSwapChain(IDXGISwapChain* a_inner, NvidiaHost* a_host) : inner_(a_inner), host_(a_host)
{
    if (inner_)
    {
        inner_.As(&inner1_);
        inner_.As(&inner2_);
        inner_.As(&inner3_);
        inner_.As(&inner4_);
    }
}

GameSwapChain::~GameSwapChain()
{
    if (host_)
    {
        host_->OnGameFacingSwapChainDestroyed(this);
    }
}

HRESULT STDMETHODCALLTYPE GameSwapChain::QueryInterface(REFIID a_iid, void** a_object)
{
    if (!a_object)
    {
        return E_POINTER;
    }
    *a_object = nullptr;

    const bool base =
        a_iid == __uuidof(IUnknown) || a_iid == __uuidof(IDXGIObject) || a_iid == __uuidof(IDXGIDeviceSubObject) || a_iid == __uuidof(IDXGISwapChain);
    const bool supportedSwapChain = base || (a_iid == __uuidof(IDXGISwapChain1) && inner1_) || (a_iid == __uuidof(IDXGISwapChain2) && inner2_) ||
                                    (a_iid == __uuidof(IDXGISwapChain3) && inner3_) || (a_iid == __uuidof(IDXGISwapChain4) && inner4_);
    if (supportedSwapChain)
    {
        *a_object = static_cast<IDXGISwapChain4*>(this);
        AddRef();
        return S_OK;
    }
    return inner_ ? inner_->QueryInterface(a_iid, a_object) : E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE GameSwapChain::AddRef() { return references_.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG STDMETHODCALLTYPE GameSwapChain::Release()
{
    const auto remaining = references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (!remaining)
    {
        delete this;
    }
    return remaining;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::SetPrivateData(REFGUID a_name, UINT a_size, const void* a_data)
{
    return inner_->SetPrivateData(a_name, a_size, a_data);
}

HRESULT STDMETHODCALLTYPE GameSwapChain::SetPrivateDataInterface(REFGUID a_name, const IUnknown* a_unknown)
{
    return inner_->SetPrivateDataInterface(a_name, a_unknown);
}

HRESULT STDMETHODCALLTYPE GameSwapChain::GetPrivateData(REFGUID a_name, UINT* a_size, void* a_data)
{
    return inner_->GetPrivateData(a_name, a_size, a_data);
}

HRESULT STDMETHODCALLTYPE GameSwapChain::GetParent(REFIID a_iid, void** a_parent) { return inner_->GetParent(a_iid, a_parent); }

HRESULT STDMETHODCALLTYPE GameSwapChain::GetDevice(REFIID a_iid, void** a_device) { return inner_->GetDevice(a_iid, a_device); }

HRESULT STDMETHODCALLTYPE GameSwapChain::Present(UINT a_syncInterval, UINT a_flags)
{
    if (host_ && FAILED(host_->FailureResult())) { return host_->FailureResult(); }
    if ((a_flags & DXGI_PRESENT_TEST) != 0)
    {
        return inner_->Present(a_syncInterval, a_flags);
    }
    BeforeGameSwapChainPresent(this);
    const auto result = MeasureSourcePresent([&] { return inner_->Present(a_syncInterval, a_flags); });
    if (host_)
    {
        host_->OnPresentCompleted(result);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::GetBuffer(UINT a_buffer, REFIID a_iid, void** a_surface)
{
    return host_ ? host_->GetGameFacingBuffer(this, a_buffer, a_iid, a_surface) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::SetFullscreenState(BOOL a_fullscreen, IDXGIOutput* a_target)
{
    return inner_->SetFullscreenState(a_fullscreen, a_target);
}

HRESULT STDMETHODCALLTYPE GameSwapChain::GetFullscreenState(BOOL* a_fullscreen, IDXGIOutput** a_target)
{
    return inner_->GetFullscreenState(a_fullscreen, a_target);
}

HRESULT STDMETHODCALLTYPE GameSwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* a_desc)
{
    const auto result = inner_->GetDesc(a_desc);
    if (SUCCEEDED(result) && host_)
    {
        // Only D3D11's internal
        // caller sees the render extent. Game/plugin callers continue to see the
        // native presentation contract; GetDesc1 remains native as well.
        host_->AdjustLegacyDescForCaller(a_desc, _ReturnAddress());
    }
    return result;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::ResizeBuffers(UINT a_bufferCount, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags)
{
    const auto resize = [&] { return inner_->ResizeBuffers(a_bufferCount, a_width, a_height, a_format, a_flags); };
    return host_ ? TheosRenderPipeline::ResizeHostBuffers(*host_, inner_.Get(), resize) : resize();
}

HRESULT STDMETHODCALLTYPE GameSwapChain::ResizeTarget(const DXGI_MODE_DESC* a_desc) { return inner_->ResizeTarget(a_desc); }

HRESULT STDMETHODCALLTYPE GameSwapChain::GetContainingOutput(IDXGIOutput** a_output) { return inner_->GetContainingOutput(a_output); }

HRESULT STDMETHODCALLTYPE GameSwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* a_stats) { return inner_->GetFrameStatistics(a_stats); }

HRESULT STDMETHODCALLTYPE GameSwapChain::GetLastPresentCount(UINT* a_count) { return inner_->GetLastPresentCount(a_count); }

HRESULT STDMETHODCALLTYPE GameSwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* a_desc) { return inner1_ ? inner1_->GetDesc1(a_desc) : E_NOINTERFACE; }

HRESULT STDMETHODCALLTYPE GameSwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* a_desc)
{
    return inner1_ ? inner1_->GetFullscreenDesc(a_desc) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::GetHwnd(HWND* a_window) { return inner1_ ? inner1_->GetHwnd(a_window) : E_NOINTERFACE; }

HRESULT STDMETHODCALLTYPE GameSwapChain::GetCoreWindow(REFIID a_iid, void** a_window)
{
    return inner1_ ? inner1_->GetCoreWindow(a_iid, a_window) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::Present1(UINT a_syncInterval, UINT a_flags, const DXGI_PRESENT_PARAMETERS* a_parameters)
{
    if (host_ && FAILED(host_->FailureResult())) { return host_->FailureResult(); }
    if (!inner1_)
    {
        return E_NOINTERFACE;
    }
    if ((a_flags & DXGI_PRESENT_TEST) != 0)
    {
        return inner1_->Present1(a_syncInterval, a_flags, a_parameters);
    }
    BeforeGameSwapChainPresent(this);
    const auto result = MeasureSourcePresent([&] { return inner1_->Present1(a_syncInterval, a_flags, a_parameters); });
    if (host_)
    {
        host_->OnPresentCompleted(result);
    }
    return result;
}

BOOL STDMETHODCALLTYPE GameSwapChain::IsTemporaryMonoSupported() { return inner1_ ? inner1_->IsTemporaryMonoSupported() : FALSE; }

HRESULT STDMETHODCALLTYPE GameSwapChain::GetRestrictToOutput(IDXGIOutput** a_output)
{
    return inner1_ ? inner1_->GetRestrictToOutput(a_output) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::SetBackgroundColor(const DXGI_RGBA* a_color)
{
    return inner1_ ? inner1_->SetBackgroundColor(a_color) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::GetBackgroundColor(DXGI_RGBA* a_color)
{
    return inner1_ ? inner1_->GetBackgroundColor(a_color) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::SetRotation(DXGI_MODE_ROTATION a_rotation)
{
    return inner1_ ? inner1_->SetRotation(a_rotation) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::GetRotation(DXGI_MODE_ROTATION* a_rotation)
{
    return inner1_ ? inner1_->GetRotation(a_rotation) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::SetSourceSize(UINT a_width, UINT a_height)
{
    return inner2_ ? inner2_->SetSourceSize(a_width, a_height) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::GetSourceSize(UINT* a_width, UINT* a_height)
{
    return inner2_ ? inner2_->GetSourceSize(a_width, a_height) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::SetMaximumFrameLatency(UINT a_latency)
{
    return inner2_ ? inner2_->SetMaximumFrameLatency(a_latency) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::GetMaximumFrameLatency(UINT* a_latency)
{
    return inner2_ ? inner2_->GetMaximumFrameLatency(a_latency) : E_NOINTERFACE;
}

HANDLE STDMETHODCALLTYPE GameSwapChain::GetFrameLatencyWaitableObject() { return inner2_ ? inner2_->GetFrameLatencyWaitableObject() : nullptr; }

HRESULT STDMETHODCALLTYPE GameSwapChain::SetMatrixTransform(const DXGI_MATRIX_3X2_F* a_matrix)
{
    return inner2_ ? inner2_->SetMatrixTransform(a_matrix) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::GetMatrixTransform(DXGI_MATRIX_3X2_F* a_matrix)
{
    return inner2_ ? inner2_->GetMatrixTransform(a_matrix) : E_NOINTERFACE;
}

UINT STDMETHODCALLTYPE GameSwapChain::GetCurrentBackBufferIndex() { return inner3_ ? inner3_->GetCurrentBackBufferIndex() : 0; }

HRESULT STDMETHODCALLTYPE GameSwapChain::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE a_colorSpace, UINT* a_support)
{
    return inner3_ ? inner3_->CheckColorSpaceSupport(a_colorSpace, a_support) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE a_colorSpace)
{
    return inner3_ ? inner3_->SetColorSpace1(a_colorSpace) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE GameSwapChain::ResizeBuffers1(UINT a_bufferCount, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags,
                                                        const UINT* a_creationNodeMask, IUnknown* const* a_presentQueue)
{
    if (!inner3_)
    {
        return E_NOINTERFACE;
    }
    const auto resize = [&] { return inner3_->ResizeBuffers1(a_bufferCount, a_width, a_height, a_format, a_flags, a_creationNodeMask, a_presentQueue); };
    return host_ ? TheosRenderPipeline::ResizeHostBuffers(*host_, inner_.Get(), resize) : resize();
}

HRESULT STDMETHODCALLTYPE GameSwapChain::SetHDRMetaData(DXGI_HDR_METADATA_TYPE a_type, UINT a_size, void* a_metadata)
{
    return inner4_ ? inner4_->SetHDRMetaData(a_type, a_size, a_metadata) : E_NOINTERFACE;
}
