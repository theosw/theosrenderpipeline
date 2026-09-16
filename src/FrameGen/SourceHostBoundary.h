#pragma once

#include <d3d11.h>

namespace TheosRenderPipeline
{
// A partially constructed host cannot silently resume on the same swapchain.
// Latching an error changes eligibility only; resource release still requires
// the owner's existing GPU retirement sequence.
class SourceHostFailure
{
  public:
    HRESULT Result() const { return result_; }
    HRESULT Fail(HRESULT result)
    {
        if (SUCCEEDED(result_)) { result_ = FAILED(result) ? result : E_FAIL; }
        return result_;
    }

  private:
    HRESULT result_{S_OK};
};

// Native device creation may succeed even when a factory hook was bypassed.
// That is not a usable renderer: the NVIDIA host owns DLSS and native UI.
template<class Host>
HRESULT CompleteRequiredHostStartup(Host& host)
{
    if (!host.ProxyActive()) { return host.FailLifecycle(E_FAIL, "NVIDIA swapchain ownership was not acquired"); }
    if (!host.CompleteStartupAfterDeviceCreation()) { return host.FailLifecycle(E_FAIL, "Deferred source DLSS startup"); }
    return S_OK;
}

// Used immediately after the original D3D11 call, before publishing hooks or UI.
// Outputs must have been initialized to null before that call. Release only the
// returned COM references; retained host/backend owners handle GPU retirement.
template<class SwapChain, class Device, class Context, class CompleteHost>
HRESULT CompleteDeviceCreation(HRESULT result, SwapChain** swapChain, Device** device,
    Context** context, CompleteHost&& completeHost)
{
    if (SUCCEEDED(result)) {
        if (!swapChain || !*swapChain || !device || !*device || !context || !*context) {
            result = E_FAIL;
        } else {
            const HRESULT hostResult = completeHost();
            if (FAILED(hostResult)) { result = hostResult; }
        }
    }
    if (FAILED(result)) {
        const auto release = [](auto** output) {
            if (output && *output) {
                auto* owned = *output;
                *output = nullptr;
                owned->Release();
            }
        };
        release(swapChain);
        release(context);
        release(device);
    }
    return result;
}

// Both DXGI resize entry points must return reconstruction failures as well as
// native failures. A refused retirement must not call native resize at all.
template<class Host, class Resize>
HRESULT ResizeHostBuffers(Host& host, IDXGISwapChain* inner, Resize&& resize)
{
    const HRESULT prepared = host.BeforeResizeBuffers(inner);
    if (FAILED(prepared)) { return prepared; }
    return host.AfterResizeBuffers(inner, resize());
}
} // namespace TheosRenderPipeline
