#pragma once

#include <d3d11.h>

namespace TheosRenderPipeline
{
    // Borrowed resources and camera inputs shared by frame producers. The host
    // retains resources through presentation and GPU retirement; this view does
    // not transfer ownership. UI contents may be filled later, before Present.
    struct SourceNvidiaFrameGuides
    {
        ID3D11Texture2D* motion{};
        ID3D11Texture2D* depth{};
        ID3D11Texture2D* uiColorAndAlpha{};
        ID3D11Texture2D* hudLessColor{};
        UINT renderWidth{}, renderHeight{}, outputWidth{}, outputHeight{};
        float jitterX{}, jitterY{};
        bool reset{}, jitterEnabled{};
    };

    struct SourceNvidiaFramePreparationResult
    {
        bool cameraValid{}, prepared{};
    };

    class SourceNvidiaFramePreparation
    {
    public:
        // Call once after the producer succeeds and orders its completed world
        // image for consumption. The caller validates guide extents/readiness
        // and handles any required interop synchronization. Failed producers
        // must skip this step so camera history does not advance.
        template<class Operations>
        static SourceNvidiaFramePreparationResult PrepareCompletedFrame(
            const SourceNvidiaFrameGuides& frame, Operations& operations)
        {
            // Preparation also serves after-upscale NR when FG is disabled.
            const bool cameraValid = operations.CaptureCamera(frame);
            const bool prepared = cameraValid && operations.Prepare(frame);
            operations.PublishGeneration(prepared);
            return {cameraValid, prepared};
        }
    };
}
