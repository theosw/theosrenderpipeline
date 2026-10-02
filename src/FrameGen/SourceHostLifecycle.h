#pragma once

#include <d3d11.h>

namespace TheosRenderPipeline
{
// This sequence owns no GPU resource. The same host owners are borrowed by
// evaluation, UI, resize and destruction; failed retirement retains them.
class SourceHostLifecycle
{
  public:
    template <class Operations> static HRESULT AfterResize(Operations& operations, HRESULT result)
    {
        // Old allocations have retired and the inner transport latches its own
        // resize failures. Retain partial reconstruction on failure; teardown
        // must prove retirement again before releasing newly created resources.
        if (FAILED(result)) { return operations.Fail(result, "NVIDIA inner resize"); }
        if (!operations.RebuildGameFacing()) {
            return operations.Fail(E_FAIL, "NVIDIA game-facing resize reconstruction");
        }
        if (!operations.RebuildUpscaler()) {
            return operations.Fail(E_FAIL, "NVIDIA source upscaler resize reconstruction");
        }
        operations.RequestHistoryReset();
        return result;
    }

    template <class Operations> static HRESULT BeforeResize(Operations& operations)
    {
        operations.DisableGeneration();
        if (!operations.Retire())
        {
            return DXGI_ERROR_WAS_STILL_DRAWING;
        }
        operations.EndUI();
        operations.ClearAndFlush();
        const auto released = operations.ReleaseUpscaler();
        if (FAILED(released)) { return operations.Fail(released, "source upscaler release before resize"); }
        operations.ReleaseGameFacing();
        operations.ReleasePresentation();
        return S_OK;
    }

    template <class Operations> static bool Destroy(Operations& operations)
    {
        operations.UnpublishInput();
        if (!operations.Retire())
        {
            operations.DetachFailedHost();
            return false;
        }
        operations.BeginDestruction();
        operations.DisableGeneration();
        const auto released = operations.ReleaseUpscaler();
        if (FAILED(released)) {
            operations.Fail(released, "source upscaler release during destruction");
            operations.DetachFailedHost();
            return false;
        }
        operations.DetachSwapchains();
        operations.ReleaseGameFacing();
        operations.ReleasePresentation();
        operations.ResetSession();
        return true;
    }
};
} // namespace TheosRenderPipeline
