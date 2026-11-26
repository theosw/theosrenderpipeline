#include "SupportDetails.h"

#include "DLSSPreset.h"
#include "OverlayFrameView.h"
#include "OverlayUIStyle.h"
#include <PCH.h>

// The menu state part of the Copy details text. English on purpose, like the
// log: the developer has to read every player's paste, so nothing here is
// translated.
namespace TheosRenderPipeline::Support
{
    void AddMenuState(Summary& summary, const OverlayUI::FrameView& view)
    {
        using Overlay::ModeName;
        const bool cs = view.communityShaders;
        if (view.steppedAside) {
            summary.upscaling = summary.frameGeneration = summary.neuralRendering = "off this session (TRP stepped aside)";
        } else {
            summary.upscaling = std::format("{} | {}x{} -> {}x{} | {}", cs ? "Community Shaders" : ModeName(view.upscaler.mode),
                view.renderWidth, view.renderHeight, view.nativeWidth, view.nativeHeight,
                !view.nvidiaHostActive ? "host unavailable" : !view.upscalerReady ? "not ready" : "ready");
            if (!cs && view.upscaler.mode != XeSS && !IsFSR(view.upscaler.mode)) {
                summary.upscaling += std::format(" | preset {}", DLSSPreset::ShortName(view.upscaler.preset));
            }
            if (!view.fsrRunning.empty()) { summary.upscaling += " | running " + view.fsrRunning; }
            if (!view.fsrFallback.empty()) { summary.upscaling += " | " + view.fsrFallback; }

            summary.frameGeneration = std::format("{} | {} | requested {}", ProviderName(view.provider),
                view.frameGenerationRuntimeActive ? std::format("active x{}", view.activeDisplayMultiplier) : std::string("inactive"),
                view.frameGenerationRequested ? "on" : "off");
            if (view.requestedProvider != view.provider) {
                summary.frameGeneration += std::format(" | saved provider {}", ProviderName(view.requestedProvider));
            }
            if (view.provider == FrameGenerationProvider::NVIDIA) {
                const auto& mfg = view.mfg;
                summary.frameGeneration += std::format(" | path {}", mfg.UsesTuringUnlock() ? "RTX 20 compatibility" :
                    mfg.UsesAmpereUnlock() ? "RTX 30 compatibility" : mfg.UsesAdaUnlock() ? "RTX 40 unlock" : "native");
                if (!mfg.status.empty()) { summary.frameGeneration += std::format(" ({})", mfg.status); }
            }
            if (!view.providerStatus.empty()) { summary.frameGeneration += " | " + view.providerStatus; }

#if defined(TRP_NO_NEURAL_RENDERING)
            summary.neuralRendering = "not included in this build";
#else
            const auto& neural = view.neuralApplied;
            const auto& state = view.sourceNeural;
            summary.neuralRendering = std::format("{} | {}", neural.enabled ? "on" : "off",
                state.sessionDisabled ? "stopped this session" : state.failed ? "failed" : state.active ? "active" : "inactive");
            const auto& runtimePath = !view.appliedNeuralRuntimePath.empty() ? view.appliedNeuralRuntimePath : view.neuralRuntimePath;
            const auto& admissionReason = !view.appliedNeuralAdmissionReason.empty() ? view.appliedNeuralAdmissionReason : view.neuralAdmissionReason;
            if (neural.enabled) {
                summary.neuralRendering += std::format(" | {} upscaling | {} {}", neural.beforeUpscaling ? "before" : "after",
                    neural.passes, neural.passes == 1 ? "pass" : "passes");
                if (!runtimePath.empty()) { summary.neuralRendering += " | runtime " + runtimePath; }
            }
            if (!state.status.empty()) { summary.neuralRendering += " | " + state.status; }
            if (!admissionReason.empty() && admissionReason != state.status) {
                summary.neuralRendering += " | " + admissionReason;
            }
#endif
        }

        const auto& app = view.nvidiaApp;
        summary.nvidiaApp.clear();
        summary.nvidiaApp.push_back(view.steppedAside ? "driver profile not read (TRP stepped aside)" :
            !view.nvidiaAdapter ? "not an NVIDIA GPU; driver profile not read" :
            app.profileRead ? "Skyrim's driver profile was read" : "Skyrim's driver profile was not read (yet)");
        if (!view.steppedAside && view.nvidiaAdapter && app.profileRead) {
            summary.nvidiaApp.push_back(app.upscalingModelOverride ? "DLSS model override: " + app.upscalingModel : std::string("DLSS model override: none"));
            summary.nvidiaApp.push_back(std::string("DLSS Frame Generation override: ") + (app.frameGenerationOverride ? "yes" : "none") +
                (app.forcedMultiplier ? std::format(" (forces x{})", app.forcedMultiplier) : std::string()));
            summary.nvidiaApp.push_back(app.neuralModelOverride ? "DLSS NR model override: " + app.neuralModel : std::string("DLSS NR model override: none"));
        }
        summary.nvidiaApp.push_back(std::string("Smooth Motion: ") +
            (app.smoothMotion == 1 ? "on (NvPresent64.dll is loaded)" : app.smoothMotion == 0 ? "off" : "unknown"));
        summary.degraded = view.degraded;
        summary.steppedAside = view.steppedAside;
    }
}
