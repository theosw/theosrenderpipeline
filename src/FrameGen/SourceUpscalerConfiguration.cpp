#include <PCH.h>
#include "NvidiaHost.h"
#include "SourceDLSSGBackend.h"
#include "DLSSBackend.h"
#include "RenderPipeline.h"
#include "CommunityShaderIntegration.h"
#include "../PluginPaths.h"
#include <chrono>

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
    if (FAILED(FailureResult()) || nativeUIPass_.Active() || nativeUIPass_.HasEarlyEvaluation()) { return; }
    auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
    if (backend.PresenterRecoveryPending()) { return; }
    sourceUpscalerSettings_.SetNvidiaServicesReady(backend.NvidiaServicesReady());
    if (!sourceUpscalerSettings_.NeedsLiveChange()) { return; }
    auto* dlss = DLSSBackend::GetSingleton();
    const auto candidate = sourceUpscalerSettings_.LiveCandidate();
    const auto transitionBegin = std::chrono::steady_clock::now();
    const int fromMode = sourceUpscalerSettings_.Effective().mode;
    auto measure = [&](const char* stage, auto operation) {
        const auto begin = std::chrono::steady_clock::now();
        const auto result = operation();
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        logger::info("[SourceUpscalerTiming] from={} to={} stage={} cpuMs={:.3f} result={}",
            fromMode, candidate.mode, stage, elapsed, static_cast<std::int64_t>(result));
        return result;
    };
    if (candidate.mode == XeSS && !xeSSActive_) {
        auto& sr = backend.XeSS();
        auto hr = measure("preflight-open", [&] { return sr.Open(backend.Device12(), TheosRenderPipeline::PluginPaths::IntelDirectory()); });
        TheosRenderPipeline::UpscalerInputResolution range;
        if (SUCCEEDED(hr)) { hr = measure("preflight-range", [&] { return sr.QueryInputRange(outputWidth_, outputHeight_, candidate.AllocationQuality(), range); }); }
        if (FAILED(hr) || !range.Accepts({renderWidth_, renderHeight_}, {outputWidth_, outputHeight_})) {
            // Refuse before retiring the working method or touching its targets.
            sourceUpscalerSettings_.RejectLiveMode();
            status_ = std::format("Fixed-size XeSS switch refused: {}x{} input; {}. Saved mode requires restart.",
                renderWidth_, renderHeight_, FAILED(hr) ? sr.Status() : "unsupported input extent");
            logger::warn("[SourceUpscaler] {} hr=0x{:08X}", status_, static_cast<unsigned>(hr));
            AdoptEffectiveSourceUpscalerSettings();
            return;
        }
    }
    if (candidate.mode == 0 && xeSSActive_ && sourceUpscalerSettings_.Startup().mode == XeSS) {
        dlss->SetupDevice(device_.Get(), context_.Get());
        TheosRenderPipeline::UpscalerInputResolution range;
        const auto hr=measure("dlss-preflight-range", [&] { return dlss->QueryInputRange(outputWidth_,outputHeight_,candidate.AllocationQuality(),range); });
        if (FAILED(hr) || !range.Accepts({renderWidth_,renderHeight_},{outputWidth_,outputHeight_})) {
            sourceUpscalerSettings_.RejectLiveMode();
            status_="Fixed-size DLSS return refused by SDK range; current XeSS retained, saved mode requires restart.";
            logger::warn("[SourceUpscaler] {} hr=0x{:08X}",status_,static_cast<unsigned>(hr));
            AdoptEffectiveSourceUpscalerSettings();return;
        }
    }
    D3D11_TEXTURE2D_DESC inputDesc{};
    gameTargets_.UpscaleInput()->GetDesc(&inputDesc);
    const bool applied = TheosRenderPipeline::Upscaler::ApplyLive(sourceUpscalerSettings_,
        [&] { return measure("retirement", [&] { return backend.Quiesce(TheosRenderPipeline::SourceDLSSG::RetirementPurpose::UpscalerReplacement); }); },
        [&](const TheosRenderPipeline::Upscaler::Creation& request) {
            if (request.mode == XeSS) {
                if (xeSSActive_) { return true; } // XeSS sharpening remains a post pass.
                const auto hr = measure("xess-create", [&] { return backend.XeSS().Initialize(backend.Transport(), device_.Get(),
                    {outputWidth_, outputHeight_}, request.AllocationQuality(), inputDesc.Format, true,
                    backend.XeSSPipelines(), {renderWidth_, renderHeight_}); });
                if (FAILED(hr)) {
                    logger::error("[SourceUpscaler] XeSS replacement failed {} hr=0x{:08X}", backend.XeSS().Status(), static_cast<unsigned>(hr));
                    return false;
                }
                return SUCCEEDED(measure("dlss-release", [&] { return dlss->ReleaseFeature(); }));
            }
            dlss->SetupDevice(device_.Get(), context_.Get());
            if (!measure("dlss-create", [&] { return dlss->InitUpscale(renderWidth_, renderHeight_, outputWidth_, outputHeight_, inputDesc.Format,
                request.sharpening, request.autoExposure, request.preset, request.AllocationQuality()); })) { return false; }
            return !xeSSActive_ || SUCCEEDED(measure("xess-release", [&] { return backend.XeSS().ReleaseAfterRetirement(); }));
        },
        [&] { return measure("resume", [&] { return backend.ResumeAfterUpscalerReplacement(); }); });
    logger::info("[SourceUpscalerTiming] from={} to={} stage=total cpuMs={:.3f} result={}",
        fromMode, candidate.mode, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - transitionBegin).count(), applied);
    if (!applied) {
        FailLifecycle(E_FAIL, "Source DLSS live settings");
        return;
    }
    // Reader retirement retains compatible guides/NR/HDR owners at fixed extent.
    // The next frame republishes guides and resets temporal history.
    frameGenerationStateKnown_ = false;
    resetNextEvaluation_ = true;
    xeSSActive_ = sourceUpscalerSettings_.Effective().mode == XeSS;
    splitSourceDLSSActive_ = !xeSSActive_;
    AdoptEffectiveSourceUpscalerSettings();
    const auto& effective = sourceUpscalerSettings_.Effective();
    logger::info("[SourceUpscaler] applied after GPU retirement: mode={} quality={} preset={} autoExposure={} sharpening={}; restartPending={} unsaved={}",
        effective.mode, effective.quality, effective.preset, effective.autoExposure, effective.sharpening,
        sourceUpscalerSettings_.NeedsRestart(), sourceUpscalerSettings_.Unsaved());
}
