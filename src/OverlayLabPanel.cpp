#include <PCH.h>
#include "Localization.h"
#include "OverlayUI.h"
#include "OverlayFrameView.h"
#include "OverlayUIStyle.h"
#include "DLSSBackend.h"
#include "RenderPipeline.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"
#include "FrameGen/FGInputDebugView.h"
#include "FrameGen/FGMotionRepair.h"

using namespace TheosRenderPipeline::Overlay;

// Lab mode details that read or change renderer internals directly. They stay
// out of the shared menu code; the offline preview draws a note instead.

void OverlayUI::DrawLabImageDetails(const FrameView& view)
{
    auto* upscaler = RenderPipeline::GetSingleton();
    auto* backend = DLSSBackend::GetSingleton();
    const bool cs = view.communityShaders;
    ImGui::Separator();
    ImGui::Text(TR("Host Present calls: %.1f FPS | %s: %s"), presentedFps, view.outputLabel, view.outputText.c_str());
    ImGui::Text(TR("Active path: %s%s"), view.activeUpscaleStage, cs || upscaler->IsEnabled() ? "" : " (inactive)");
    ImGui::Text(TR("Render %d x %d -> Native %d x %d (%.1f%%)"), upscaler->mRenderSizeX, upscaler->mRenderSizeY,
                view.nativeWidth, view.nativeHeight,
                view.nativeWidth > 0
                    ? static_cast<float>(upscaler->mRenderSizeX) / static_cast<float>(view.nativeWidth) * 100.0f
                    : 100.0f);
    if (!cs)
    {
        ImGui::Text(TR("Jitter: (%.4f, %.4f) | phases: %d"), upscaler->mJitterOffsets[0], upscaler->mJitterOffsets[1],
                    upscaler->JitterPhaseCount());
        ImGui::Text(TR("Mip LOD bias: %.3f"), upscaler->mMipLodBias);
        ImGui::Text(TR("NGX evals ok/failed: %llu / %llu | last 0x%08X"),
                    static_cast<unsigned long long>(backend->EvalSuccessCount()),
                    static_cast<unsigned long long>(backend->EvalFailCount()), backend->LastEvalResult());
        ImGui::Text(TR("Feature: %s | formats in/out: %d / %d | Linear HDR input: %s"),
                    backend->HasFeature() ? TR("created") : TR("none"), backend->InputFormat(), backend->OutputFormat(),
                    backend->IsHDRInput() ? TR("yes") : TR("no"));
    }
    if (upscaler->mGraphicsState)
    {
        auto& runtimeData = upscaler->mGraphicsState->GetRuntimeData();
        ImGui::Text(TR("Engine resolution ratio at present: %.3f x %.3f"), runtimeData.dynamicResolutionWidthRatio,
                    runtimeData.dynamicResolutionHeightRatio);
    }

    if (!cs)
    {
        auto* performance = PerformanceTuning::GetSingleton();
        for (auto item : {PerformanceTuning::Optimization::kDirectRCASOutput,
                          PerformanceTuning::Optimization::kDirectDLSSOutput})
        {
            const auto& route = performance->GetRouteStatus(item);
            ImGui::SeparatorText(item == PerformanceTuning::Optimization::kDirectRCASOutput ? TR("RCAS output")
                                                                                            : TR("DLSS output"));
            ImGui::TextWrapped(TR("%s: %s"),
                               route.sessionRejected   ? TR("Fallback latched")
                               : route.activeLastFrame ? TR("Active")
                               : route.requested       ? TR("Armed")
                                                       : TR("Off"),
                               !route.activeLastFrame && route.reason == "active" ? TR("not used this frame")
                                                                                : TR(route.reason.c_str()));
            ImGui::TextWrapped(TR("Frames %llu | fallbacks %llu"), static_cast<unsigned long long>(route.activeFrames),
                               static_cast<unsigned long long>(route.fallbackCount));
        }
    }
}

void OverlayUI::DrawLabXeFGControls()
{
    auto& sourceBackend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
    // Session-only diagnostics: separate generated-frame artifacts from upscaler ones.
    bool onlyGenerated = sourceBackend.XeFGOnlyGenerated(), tagGenerated = sourceBackend.XeFGTagGenerated();
    bool changed = ImGui::Checkbox(TR("Show only generated frames (Lab)"), &onlyGenerated);
    changed |= ImGui::Checkbox(TR("Tag generated frames (Lab)"), &tagGenerated);
    if (changed)
        sourceBackend.ConfigureXeFGDebugView(onlyGenerated, tagGenerated);
    DrawSettingsHelp(TR("Intel debug views, not saved. Only generated frames hides every real frame; "
                     "tagging marks generated frames with corner boxes."));
    bool gpuInputWait = sourceBackend.XeFGGpuInputWait();
    if (ImGui::Checkbox(TR("Input reuse: GPU fence wait (Lab)"), &gpuInputWait))
        sourceBackend.ConfigureXeFGGpuInputWait(gpuInputWait);
    DrawSettingsHelp(TR("A/B for the post-Present wait, not saved. Off drains every host lane on the CPU; "
                     "on queues a GPU wait for Intel's input copies. The log reports both waits every 600 frames. "
                     "With heavy NR, on can make rendered frames alternate long and short."));
}

void OverlayUI::DrawLabGenerationControls(const FrameView& view)
{
    auto& sourceBackend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
    const auto* beatReason = sourceBackend.XeFGModelBeatUnavailable();
    bool beat = sourceBackend.XeFGModelBeatRequested();
    ImGui::BeginDisabled(beatReason != nullptr);
    if (ImGui::Checkbox(TR("Even NR pacing (plain XeFG x2, Lab)"), &beat))
        sourceBackend.ConfigureXeFGModelBeat(beat);
    ImGui::EndDisabled();
    if (beatReason) DrawSettingsHelp(TR(beatReason));
    DrawSettingsHelp(TR("Off by default, not saved. Learns 16 model/skip pairs, then holds each frame after its GPU work is queued. "
        "Only holds when a persistent timing difference follows the model phase. "
        "Adds delay to fast frames; screen pacing and FPS need testing. "
        "Turn off and on to learn a new scene. Frame limit must be Off."));
    if (view.provider != TheosRenderPipeline::FrameGenerationProvider::XeFG)
    {
        // Session-only diagnostic: an artifact only in generated frames comes from generation.
        bool onlyGenerated = sourceBackend.XeFGOnlyGenerated();
        if (ImGui::Checkbox(TR("Show only generated frames (Lab)"), &onlyGenerated))
            sourceBackend.ConfigureXeFGDebugView(onlyGenerated, sourceBackend.XeFGTagGenerated());
        DrawSettingsHelp(TR("NVIDIA debug view, not saved. Hides every rendered frame, so an artifact still visible here "
                         "comes from frame generation; one that disappears comes from rendering, the upscaler or NR."));
        bool overlap = sourceBackend.NvidiaOverlapNextFrame();
        if (ImGui::Checkbox(TR("Overlap the next frame with generation (Lab)"), &overlap))
            sourceBackend.ConfigureNvidiaOverlapNextFrame(overlap);
        DrawSettingsHelp(TR("A/B, not saved. Off: the game's next frame waits on the GPU until frame generation has read this "
                         "frame's inputs. On: it waits only where TRP first writes them, so the world can render meanwhile. "
                         "Compare FPS and the frame generation time."));
    }
    auto& repair = TheosRenderPipeline::FGMotionRepair::Get();
    bool repairMotion = repair.Enabled();
    if (ImGui::Checkbox(TR("Repair missing motion (Lab)"), &repairMotion))
        repair.Enable(repairMotion);
    DrawSettingsHelp(TR("On by default; turning it off lasts until the game restarts. Underwater, Skyrim writes no real motion, "
                     "and generated frames show an offset copy. When nearly the whole frame disagrees with the camera, "
                     "NR and generation get the camera's motion instead."));
    auto& inputs = TheosRenderPipeline::FGInputDebugView::Get();
    bool showInputs = inputs.Enabled();
    if (ImGui::Checkbox(TR("Show frame generation inputs (Lab)"), &showInputs))
        inputs.Enable(showInputs);
    DrawSettingsHelp(TR("Not saved. Shows what generation receives, after any repair. Left: motion vectors (grey = none). Middle: depth (dark = near). "
                     "Right: where the motion vectors disagree with the camera's own motion at that depth "
                     "(black = agree, red = 8+ pixels off). Static scenery should stay black while the camera moves."));
    if (showInputs)
    {
        if (auto* strip = inputs.Strip())
        {
            const float width = ImGui::GetContentRegionAvail().x;
            const float height = width * float(inputs.PanelHeight()) / float(inputs.PanelWidth() * 3);
            ImGui::Image(reinterpret_cast<ImTextureID>(strip), ImVec2(width, height));
        }
        else
            ImGui::TextWrapped(TR("Inputs: %s"), inputs.Status());
    }
}

void OverlayUI::DrawLabGenerationDetails(const FrameView& view)
{
    auto* nvidiaHost = NvidiaHost::GetSingleton();
    const auto& sourceState = view.session;
    const auto& unlock = view.mfg;
    const bool intel = view.provider == TheosRenderPipeline::FrameGenerationProvider::XeFG;
    const bool editIntel = settingsDraft.sourceDLSSG.provider == TheosRenderPipeline::FrameGenerationProvider::XeFG;
    const bool nvidiaReported = !intel && sourceState.stateQueries > 0;
    const bool supportsDynamic = !editIntel && nvidiaReported && TheosRenderPipeline::SourceDLSSG::MFGContract::Dynamic(
                                     unlock.UsesCompatibilityUnlock(), unlock.Ready(),
                                     sourceState.state.bIsDynamicMFGSupported == sl::eTrue) &&
                                 sourceState.state.numFramesToGenerateMax > 1;
    ImGui::Text(TR("Evaluations: %llu"), static_cast<unsigned long long>(nvidiaHost->EvaluationCount()));
    ImGui::Text(TR("Host Presents: %llu | failures %llu | last 0x%08X"),
                static_cast<unsigned long long>(nvidiaHost->PresentCount()),
                static_cast<unsigned long long>(nvidiaHost->FailedPresentCount()),
                static_cast<unsigned int>(nvidiaHost->LastPresentResult()));
    if (!intel && nvidiaHost->RuntimeStateObservationCount() > 0)
    {
        ImGui::Text(TR("Last query: %u outputs | max generated %u | min dimension %u"),
                    nvidiaHost->RuntimeFramesActuallyPresented(), nvidiaHost->RuntimeMaxGeneratedFrames(),
                    nvidiaHost->RuntimeMinWidthOrHeight());
        ImGui::Text(TR("DLSS-G status: %u | observations: %llu"), nvidiaHost->RuntimeDLSSGStatus(),
                    static_cast<unsigned long long>(nvidiaHost->RuntimeStateObservationCount()));
    }
    else if (!intel)
    {
        ImGui::TextDisabled("%s", TR("DLSS-G state: waiting"));
    }
    if (intel)
    {
        const auto& state = view.xefg;
        ImGui::Text(TR("XeFG frame: %u | result: %d | warnings: %llu"), state.frameId, state.interpolationResult,
                    static_cast<unsigned long long>(state.warnings));
        ImGui::Text(TR("XeLL output interval: %u us"), state.frameLimitUs);
    }
    ImGui::TextWrapped("%s", nvidiaHost->Status().c_str());
    if (view.sourceDLSSGActive && !intel)
    {
        ImGui::Text(TR("Configured multiplier: x%u"), sourceState.options.numFramesToGenerate + 1);
        ImGui::Text(TR("Output limit submitted interval: %u us"), sourceState.frameLimitSubmittedUs);
        ImGui::TextWrapped(TR("MFG: %s"), unlock.status);
    }

    ImGui::Text(TR("Dynamic multiplier: %s"), supportsDynamic ? TR("supported") : TR("unavailable"));
}

void OverlayUI::DrawLabMenuDiagnostics()
{
    auto* upscaler = RenderPipeline::GetSingleton();
    ImGui::Spacing();
    ImGui::TextColored(kRust, "%s", TR("These controls deliberately break or instrument the normal render path."));
    if (ImGui::Checkbox(TR("Log menu/Console metrics (debug)"), &upscaler->mLogMenuMetrics))
    {
        upscaler->mConsoleDiagnosticsActive.store(upscaler->mLogMenuMetrics &&
                                                      upscaler->mConsoleOpen.load(std::memory_order_relaxed),
                                                  std::memory_order_relaxed);
    }
    ImGui::Spacing();
    ImGui::SeparatorText(TR("MAGIC PREVIEW DRAW ISOLATION"));
    static const char* drawIsolationModes[] = {"Normal",      TR("Only draw 1"), TR("Only draw 2"),
                                               TR("Only draw 3"), TR("Only draw 4"), TR("Hide draw 1"),
                                               TR("Hide draw 2"), TR("Hide draw 3"), TR("Hide draw 4")};
    if (ImGui::Combo(TR("Inventory3D draw view"), &upscaler->mInventory3DDrawIsolationMode, drawIsolationModes,
                     static_cast<int>(std::size(drawIsolationModes))))
    {
        upscaler->mInventory3DLastObservedDraws.store(0, std::memory_order_relaxed);
        upscaler->mInventory3DLastSkippedDraws.store(0, std::memory_order_relaxed);
        logger::info("[Inventory3DDrawIsolation] mode {} ({})", upscaler->mInventory3DDrawIsolationMode,
                     drawIsolationModes[upscaler->mInventory3DDrawIsolationMode]);
    }
    ImGui::TextDisabled(TR("Runtime only. Last preview frame: %u draws observed, %u skipped."),
                        upscaler->mInventory3DLastObservedDraws.load(std::memory_order_relaxed),
                        upscaler->mInventory3DLastSkippedDraws.load(std::memory_order_relaxed));
}
