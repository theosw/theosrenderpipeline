#include <PCH.h>
#include "OverlayUI.h"
#include "OverlayFrameView.h"
#include "OverlayUIStyle.h"
#include "DLSSBackend.h"
#include "RenderPipeline.h"
#include "FrameGen/NvidiaHost.h"

using namespace TheosRenderPipeline::Overlay;

// Lab mode details that read or change renderer internals directly. They stay
// out of the shared menu code; the offline preview draws a note instead.

void OverlayUI::DrawLabImageDetails(const FrameView& view)
{
    auto* upscaler = RenderPipeline::GetSingleton();
    auto* backend = DLSSBackend::GetSingleton();
    const bool cs = view.communityShaders;
    ImGui::Separator();
    ImGui::Text("Host Present calls: %.1f FPS | %s: %s", presentedFps, view.outputLabel, view.outputText.c_str());
    ImGui::Text("Active path: %s%s", view.activeUpscaleStage, cs || upscaler->IsEnabled() ? "" : " (inactive)");
    ImGui::Text("Render %d x %d -> Native %d x %d (%.1f%%)", upscaler->mRenderSizeX, upscaler->mRenderSizeY,
                view.nativeWidth, view.nativeHeight,
                view.nativeWidth > 0
                    ? static_cast<float>(upscaler->mRenderSizeX) / static_cast<float>(view.nativeWidth) * 100.0f
                    : 100.0f);
    if (!cs)
    {
        ImGui::Text("Jitter: (%.4f, %.4f) | phases: %d", upscaler->mJitterOffsets[0], upscaler->mJitterOffsets[1],
                    backend->GetJitterPhaseCount());
        ImGui::Text("Mip LOD bias: %.3f", upscaler->mMipLodBias);
        ImGui::Text("NGX evals ok/failed: %llu / %llu | last 0x%08X",
                    static_cast<unsigned long long>(backend->EvalSuccessCount()),
                    static_cast<unsigned long long>(backend->EvalFailCount()), backend->LastEvalResult());
        ImGui::Text("Feature: %s | formats in/out: %d / %d | Linear HDR input: %s",
                    backend->HasFeature() ? "created" : "none", backend->InputFormat(), backend->OutputFormat(),
                    backend->IsHDRInput() ? "yes" : "no");
    }
    if (upscaler->mGraphicsState)
    {
        auto& runtimeData = upscaler->mGraphicsState->GetRuntimeData();
        ImGui::Text("Engine resolution ratio at present: %.3f x %.3f", runtimeData.dynamicResolutionWidthRatio,
                    runtimeData.dynamicResolutionHeightRatio);
    }

    if (!cs)
    {
        auto* performance = PerformanceTuning::GetSingleton();
        for (auto item : {PerformanceTuning::Optimization::kDirectRCASOutput,
                          PerformanceTuning::Optimization::kDirectDLSSOutput})
        {
            const auto& route = performance->GetRouteStatus(item);
            ImGui::SeparatorText(item == PerformanceTuning::Optimization::kDirectRCASOutput ? "RCAS output"
                                                                                            : "DLSS output");
            ImGui::TextWrapped("%s: %s",
                               route.sessionRejected   ? "Fallback latched"
                               : route.activeLastFrame ? "Active"
                               : route.requested       ? "Armed"
                                                       : "Off",
                               !route.activeLastFrame && route.reason == "active" ? "not used this frame"
                                                                                : route.reason.c_str());
            ImGui::TextWrapped("Frames %llu | fallbacks %llu", static_cast<unsigned long long>(route.activeFrames),
                               static_cast<unsigned long long>(route.fallbackCount));
        }
    }
}

void OverlayUI::DrawLabGenerationDetails(const FrameView& view)
{
    auto* nvidiaHost = NvidiaHost::GetSingleton();
    const auto& sourceState = view.session;
    const auto& unlock = view.mfg;
    const bool supportsDynamic = TheosRenderPipeline::SourceDLSSG::MFGContract::Dynamic(
                                     unlock.UsesCompatibilityUnlock(), unlock.Ready(),
                                     sourceState.state.bIsDynamicMFGSupported == sl::eTrue) &&
                                 sourceState.state.numFramesToGenerateMax > 1;
    ImGui::Text("Evaluations: %llu", static_cast<unsigned long long>(nvidiaHost->EvaluationCount()));
    ImGui::Text("Host Presents: %llu | failures %llu | last 0x%08X",
                static_cast<unsigned long long>(nvidiaHost->PresentCount()),
                static_cast<unsigned long long>(nvidiaHost->FailedPresentCount()),
                static_cast<unsigned int>(nvidiaHost->LastPresentResult()));
    if (nvidiaHost->RuntimeStateObservationCount() > 0)
    {
        ImGui::Text("Last query: %u outputs | max generated %u | min dimension %u",
                    nvidiaHost->RuntimeFramesActuallyPresented(), nvidiaHost->RuntimeMaxGeneratedFrames(),
                    nvidiaHost->RuntimeMinWidthOrHeight());
        ImGui::Text("DLSS-G status: %u | observations: %llu", nvidiaHost->RuntimeDLSSGStatus(),
                    static_cast<unsigned long long>(nvidiaHost->RuntimeStateObservationCount()));
    }
    else
    {
        ImGui::TextDisabled("DLSS-G state: waiting");
    }
    ImGui::TextWrapped("%s", nvidiaHost->Status().c_str());
    if (view.sourceDLSSGActive)
    {
        ImGui::Text("Configured multiplier: x%u", sourceState.options.numFramesToGenerate + 1);
        ImGui::Text("Output limit submitted interval: %u us", sourceState.frameLimitSubmittedUs);
        ImGui::TextWrapped("MFG: %s", unlock.status);
    }

    ImGui::Text("Dynamic multiplier: %s", supportsDynamic ? "supported" : "unavailable");
}

void OverlayUI::DrawLabMenuDiagnostics()
{
    auto* upscaler = RenderPipeline::GetSingleton();
    ImGui::Spacing();
    ImGui::TextColored(kRust, "These controls deliberately break or instrument the normal render path.");
    if (ImGui::Checkbox("Log menu/Console metrics (debug)", &upscaler->mLogMenuMetrics))
    {
        upscaler->mConsoleDiagnosticsActive.store(upscaler->mLogMenuMetrics &&
                                                      upscaler->mConsoleOpen.load(std::memory_order_relaxed),
                                                  std::memory_order_relaxed);
    }
    ImGui::Spacing();
    ImGui::SeparatorText("MAGIC PREVIEW DRAW ISOLATION");
    static const char* drawIsolationModes[] = {"Normal",      "Only draw 1", "Only draw 2",
                                               "Only draw 3", "Only draw 4", "Hide draw 1",
                                               "Hide draw 2", "Hide draw 3", "Hide draw 4"};
    if (ImGui::Combo("Inventory3D draw view", &upscaler->mInventory3DDrawIsolationMode, drawIsolationModes,
                     static_cast<int>(std::size(drawIsolationModes))))
    {
        upscaler->mInventory3DLastObservedDraws.store(0, std::memory_order_relaxed);
        upscaler->mInventory3DLastSkippedDraws.store(0, std::memory_order_relaxed);
        logger::info("[Inventory3DDrawIsolation] mode {} ({})", upscaler->mInventory3DDrawIsolationMode,
                     drawIsolationModes[upscaler->mInventory3DDrawIsolationMode]);
    }
    ImGui::TextDisabled("Runtime only. Last preview frame: %u draws observed, %u skipped.",
                        upscaler->mInventory3DLastObservedDraws.load(std::memory_order_relaxed),
                        upscaler->mInventory3DLastSkippedDraws.load(std::memory_order_relaxed));
}
