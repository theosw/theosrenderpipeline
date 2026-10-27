#pragma once
#include <winerror.h>

namespace TheosRenderPipeline::SourceDLSSG
{
    // Only before the first presenter creation attempt: no HWND owner and no
    // caller frame lists have been submitted. SDK/module owners stay resident.
    constexpr bool CanUseStartupTransport(HRESULT failure, HRESULT deviceStatus,
        bool presenterAttempted, bool queueWorkSubmitted = false)
    {
        return FAILED(failure) && SUCCEEDED(deviceStatus) && !presenterAttempted && !queueWorkSubmitted &&
            failure != E_UNEXPECTED && failure != E_INVALIDARG && failure != E_POINTER;
    }
}
