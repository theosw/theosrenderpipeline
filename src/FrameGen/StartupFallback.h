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
}
