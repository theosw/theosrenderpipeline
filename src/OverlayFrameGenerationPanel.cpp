#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "OverlayFrameView.h"

#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"
#include "FrameGen/SourceFrameGeneration.h"
#include <PCH.h>

using namespace TheosRenderPipeline::Overlay;

void OverlayUI::DrawFrameGenerationPanel(float tabCardHeight, const FrameView& view)
{
    auto* frameGen = SourceFrameGeneration::GetSingleton();
    auto* nvidiaHost = NvidiaHost::GetSingleton();
    const auto& sourceDLSSGActive = view.sourceDLSSGActive;
    const auto& frameGenerationRuntimeActive = view.frameGenerationRuntimeActive;
    const auto& activeDisplayMultiplier = view.activeDisplayMultiplier;

    if (ImGui::BeginTabItem("Frame generation", nullptr,
                            requestedPage == SettingsPage::FrameGeneration ? ImGuiTabItemFlags_SetSelected
                                                                           : ImGuiTabItemFlags_None))
    {
        auto& sourceBackend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
        const auto& sourceState = sourceBackend.Snapshot();
        const auto& unlock = sourceBackend.MFGState();
        const auto usableMaximum = TheosRenderPipeline::SourceDLSSG::MFGContract::Maximum(
            unlock.UsesCompatibilityUnlock(), unlock.Ready(), sourceState.state.numFramesToGenerateMax);
        const bool supportsDynamic = TheosRenderPipeline::SourceDLSSG::MFGContract::Dynamic(
                                         unlock.UsesCompatibilityUnlock(), unlock.Ready(),
                                         sourceState.state.bIsDynamicMFGSupported == sl::eTrue) &&
                                     sourceState.state.numFramesToGenerateMax > 1;
        if (!BeginSettingsColumns("generation", tabCardHeight, view))
        {
            ImGui::EndTabItem();
            return;
        }
        DrawStatusLabel(frameGenerationRuntimeActive ? "DLSS-G active" : "Frame generation inactive",
                        frameGenerationRuntimeActive ? UIHealth::kHealthy : UIHealth::kIdle);
        DrawSettingsValue("Multiplier", std::format("x{}", activeDisplayMultiplier).c_str());
        DrawSettingsValue("Display", std::format("{:.0f} Hz", frameGen->refreshRate).c_str());
        DrawSettingsValue("Reflex", TheosRenderPipeline::SourceDLSSG::ReflexModeName(sourceState.reflexSubmitted));
        DrawSettingsValue("Output cap",
                          sourceState.frameLimitSubmittedUs
                              ? std::format("{:.1f} FPS", 1000000.0 / sourceState.frameLimitSubmittedUs).c_str()
                              : "Off");
        DrawSettingsValue("Path", unlock.UsesAmpereUnlock() ? "Ampere MFG (experimental)"
                                  : unlock.UsesAdaUnlock()  ? "Ada MFG"
                                                            : "Native NVIDIA");
        if (usableMaximum > 1)
            ImGui::Text("Available: x2 to x%u", usableMaximum + 1);
        else
            ImGui::TextUnformatted(usableMaximum == 1 ? "Available: x2" : "Availability: waiting");
        if (unlock.UsesCompatibilityUnlock() && !unlock.Ready())
            ImGui::TextWrapped("%s", unlock.status);
        if (sourceState.stateQueryResult == sl::Result::eWarnOutOfVRAM)
            ImGui::TextColored(kOchre, "NVIDIA VRAM budget warning");
        if (nvidiaHost->WarmupPresentsRemaining() > 0)
            ImGui::Text("Warmup: %d frames", nvidiaHost->WarmupPresentsRemaining());
        if (ImGui::CollapsingHeader("Runtime details"))
        {
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
                const auto& sourceBackend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
                ImGui::Text("Configured multiplier: x%u", sourceBackend.Snapshot().options.numFramesToGenerate + 1);
                ImGui::Text("Output limit submitted interval: %u us", sourceBackend.Snapshot().frameLimitSubmittedUs);
                ImGui::TextWrapped("MFG: %s", sourceBackend.MFGState().status);
            }

            ImGui::Text("Dynamic multiplier: %s", supportsDynamic ? "supported" : "unavailable");
        }
        DrawStageMeasurements(SettingsPage::FrameGeneration);
        NextSettingsColumn(tabCardHeight);
        bool runtimeInterpolationRequested = frameGen->RuntimeInterpolationRequested();
        if (ImGui::Checkbox("Frame generation##runtime", &runtimeInterpolationRequested))
        {
            frameGen->RequestRuntimeInterpolation(runtimeInterpolationRequested);
        }
        DrawSettingsHelp("Takes effect immediately. Save as default to keep this choice for the next launch.");
        if (runtimeInterpolationRequested != nvidiaHost->FrameGenerationEnabled())
        {
            ImGui::TextDisabled("Waiting for the current GPU frame to retire...");
        }

        if (!sourceDLSSGActive)
        {
            ImGui::TextWrapped("NVIDIA host is unavailable. Check runtime status and restart Skyrim.");
        }
        if (sourceDLSSGActive)
        {
            ImGui::TextUnformatted("Multiplier");
            auto& request = settingsDraft.sourceDLSSG.generation;
            const char* multipliers[]{"x2", "x3", "x4", "x5", "x6"};
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##sourceMultiplier", multipliers[std::clamp(request.generatedFrames, 1u, 5u) - 1]))
            {
                for (unsigned count = 1; count <= 5; ++count)
                {
                    ImGui::BeginDisabled(count > usableMaximum);
                    if (ImGui::Selectable(multipliers[count - 1], request.generatedFrames == count))
                    {
                        request.generatedFrames = count;
                    }
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
            if (supportsDynamic)
            {
                ImGui::Checkbox("Dynamic multiplier##sourceMFG", &request.dynamic);
            }
            else if (request.dynamic)
            {
                ImGui::TextColored(kOchre, "A saved dynamic request is unsupported by this NVIDIA runtime.");
                if (ImGui::Button("Use fixed multiplier##sourceMFG"))
                {
                    request.dynamic = false;
                }
            }
            if (request.dynamic)
            {
                int target = static_cast<int>(request.dynamicTargetFPS);
                ImGui::TextUnformatted("Target output FPS");
                if (TheosRenderPipeline::Overlay::FPSInput("##sourceMFGTarget", target))
                {
                    // Keep intermediate digits while typing 120, rather than
                    // replacing 1 and 12 with zero. Validate on Apply.
                    request.dynamicTargetFPS = static_cast<unsigned>(std::clamp(target, 0, 1000));
                }
                ImGui::TextDisabled("0 = display refresh rate; explicit targets must exceed 60 FPS.");
            }
            if (request.generatedFrames > sourceState.state.numFramesToGenerateMax || sourceState.generationLimited)
            {
                ImGui::TextColored(kOchre, "NVIDIA runtime maximum: x%u", sourceState.state.numFramesToGenerateMax + 1);
            }
            if (sourceState.generationLimited)
            {
                ImGui::TextWrapped("Saved/requested MFG is unsupported here. The runtime uses the supported count; "
                                   "your requested preference is retained.");
            }
            ImGui::Separator();
            ImGui::TextUnformatted("NVIDIA Reflex");
            const auto requestedReflex = static_cast<sl::ReflexMode>(settingsDraft.sourceDLSSG.reflexMode);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##sourceReflex", TheosRenderPipeline::SourceDLSSG::ReflexModeName(requestedReflex)))
            {
                for (const auto mode :
                     {sl::ReflexMode::eOff, sl::ReflexMode::eLowLatency, sl::ReflexMode::eLowLatencyWithBoost})
                {
                    if (ImGui::Selectable(TheosRenderPipeline::SourceDLSSG::ReflexModeName(mode),
                                          mode == requestedReflex))
                    {
                        settingsDraft.sourceDLSSG.reflexMode = static_cast<int>(mode);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TextUnformatted("Output FPS limit");
            TheosRenderPipeline::Overlay::FPSInput("##sourceReflexFPS", settingsDraft.sourceDLSSG.outputFPSLimit);
            settingsDraft.sourceDLSSG.outputFPSLimit = std::clamp(settingsDraft.sourceDLSSG.outputFPSLimit, 0, 1000);
            ImGui::TextDisabled("0 = no explicit cap");
            DrawSettingsHelp("The cap includes generated frames. Avoid stacking it with another limiter.");
        }

        EndSettingsColumns();
        ImGui::EndTabItem();
    }
}
