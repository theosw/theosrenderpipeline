#pragma once
#include <winerror.h>
#include <dxgi.h>

namespace TheosRenderPipeline::SourceDLSSG
{
    // Only before the first presenter creation attempt: no HWND owner and no
    // caller frame lists have been submitted. SDK/module owners stay resident.
    constexpr bool CanUseStartupTransport(HRESULT failure, HRESULT deviceStatus,
        bool presenterAttempted, bool queueWorkSubmitted = false)
    {
        return FAILED(failure) && SUCCEEDED(deviceStatus) && !presenterAttempted && !queueWorkSubmitted &&
            failure != E_UNEXPECTED && failure != E_INVALIDARG && failure != E_POINTER &&
            failure != DXGI_ERROR_DEVICE_REMOVED && failure != DXGI_ERROR_DEVICE_RESET &&
            failure != DXGI_ERROR_DEVICE_HUNG && failure != DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }

    // XeFG's startup check (XeFGPresenter::Probe) loads Intel's SDK and creates
    // and destroys a probe context; it makes no presenter factory call. A refused
    // check may use the startup transport under the same rule, except when the
    // probe context could not be destroyed and is retained. Unsafe patch state
    // is reported as E_UNEXPECTED, which the rule already keeps terminal.
    constexpr bool CanDegradeXeFGProbe(HRESULT probe, HRESULT deviceStatus, bool probeContextRetained, bool presenterAttempted)
    {
        return !probeContextRetained && CanUseStartupTransport(probe, deviceStatus, presenterAttempted);
    }
}
