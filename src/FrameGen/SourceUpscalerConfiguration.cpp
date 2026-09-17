#include <PCH.h>
#include "NvidiaHost.h"
#include "SourceDLSSGBackend.h"
#include "DLSSBackend.h"
#include "RenderPipeline.h"
#include "CommunityShaderIntegration.h"

void NvidiaHost::AdoptEffectiveSourceUpscalerSettings() const
{
    if (!StartupConfigured() || TheosRenderPipeline::CommunityShaders::Active()) { return; }
    const auto& effective = sourceUpscalerSettings_.Effective();
    auto* settings = RenderPipeline::GetSingleton();
    settings->mUpscaleType = effective.mode;
    settings->mQualityLevel = effective.quality;
    settings->mDLSSPreset = effective.preset;
    settings->mAutoExposure = effective.autoExposure;
    settings->mSharpening = effective.sharpening;
}

void NvidiaHost::RequestSourceUpscalerSettings(TheosRenderPipeline::Upscaler::Creation request)
{
    if (!StartupConfigured() || TheosRenderPipeline::CommunityShaders::Active()) { return; }
    sourceUpscalerSettings_.Request(request);
    // Pipeline fields describe this session, never a pending allocation.
    AdoptEffectiveSourceUpscalerSettings();
}

void NvidiaHost::ApplySourceUpscalerSettingsAfterPresent()
{
    if (FAILED(FailureResult()) || !sourceUpscalerSettings_.NeedsLiveChange() || nativeUIPass_.Active() || nativeUIPass_.HasEarlyEvaluation()) { return; }
    auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
    auto* dlss = DLSSBackend::GetSingleton();
    D3D11_TEXTURE2D_DESC inputDesc{};
    gameTargets_.UpscaleInput()->GetDesc(&inputDesc);
    const bool applied = TheosRenderPipeline::Upscaler::ApplyLive(sourceUpscalerSettings_,
        [&] { return backend.Quiesce(); },
        [&](const TheosRenderPipeline::Upscaler::Creation& request) {
            return dlss->InitUpscale(renderWidth_, renderHeight_, outputWidth_, outputHeight_, inputDesc.Format,
                request.sharpening, request.autoExposure, request.preset, request.AllocationQuality());
        },
        [&] { return backend.ResumeAfterResize(); });
    if (!applied) {
        FailLifecycle(E_FAIL, "Source DLSS live settings");
        return;
    }
    // Quiesce cleared guides and retired the last D3D11 work before replacing
    // the feature. The next frame must republish guides and reset history.
    frameGenerationStateKnown_ = false;
    resetNextEvaluation_ = true;
    AdoptEffectiveSourceUpscalerSettings();
    const auto& effective = sourceUpscalerSettings_.Effective();
    logger::info("[SourceUpscaler] applied after GPU retirement: mode={} quality={} preset={} autoExposure={} sharpening={}; restartPending={} unsaved={}",
        effective.mode, effective.quality, effective.preset, effective.autoExposure, effective.sharpening,
        sourceUpscalerSettings_.NeedsRestart(), sourceUpscalerSettings_.Unsaved());
}
