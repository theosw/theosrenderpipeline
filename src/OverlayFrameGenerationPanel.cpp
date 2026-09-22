#include "OverlayUI.h"
#include "OverlayUIStyle.h"

#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"
#include "FrameGen/SourceFrameGeneration.h"
#include <PCH.h>

using namespace TheosRenderPipeline::Overlay;

void OverlayUI::DrawFrameGenerationPanel(float tabCardHeight, const FrameGenerationView& view)
{
    auto* frameGen = SourceFrameGeneration::GetSingleton();
    auto* nvidiaHost = NvidiaHost::GetSingleton();
    const auto& sourceDLSSGActive = view.sourceDLSSGActive;
    const auto& frameGenerationRuntimeActive = view.frameGenerationRuntimeActive;
    const auto& activeDisplayMultiplier = view.activeDisplayMultiplier;
    const auto& outputLabel = view.outputLabel;
    const auto& outputText = view.outputText;

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
        ImGui::BeginChild("##frameGenerationPage", ImVec2(0.0f, tabCardHeight), false);
        DrawSettingsHelp("Generated frames, latency and output rate");
        ImGui::PushTextWrapPos(0.0f);
        bool runtimeInterpolationRequested = frameGen->RuntimeInterpolationRequested();
        if (ImGui::Checkbox("Frame generation now##runtime", &runtimeInterpolationRequested))
        {
            frameGen->RequestRuntimeInterpolation(runtimeInterpolationRequested);
        }
        ImGui::SameLine();
        DrawBadge("LIVE", kSage);
        DrawSettingsHelp("Takes effect immediately. Save as default to keep this choice for the next launch.");
        if (runtimeInterpolationRequested != nvidiaHost->FrameGenerationEnabled())
        {
            ImGui::TextDisabled("Waiting for the current GPU frame to retire...");
        }

#if !defined(TRP_NO_NEURAL_RENDERING)
        ImGui::TextWrapped("Multiplier and Neural Rendering are independent settings.");
#endif
        DrawStatusLabel(frameGenerationRuntimeActive ? "ACTIVE" : "INACTIVE",
                        frameGenerationRuntimeActive ? UIHealth::kHealthy : UIHealth::kIdle);
        ImGui::SameLine();
        ImGui::Text("x%u | %.1f rendered FPS | %s: %s", activeDisplayMultiplier, renderedFps, outputLabel,
                    outputText.c_str());
        if (!sourceDLSSGActive)
        {
            ImGui::TextWrapped("NVIDIA host is unavailable. Check runtime status and restart Skyrim.");
        }
        if (sourceDLSSGActive)
        {
            DrawSettingsHeading("Multiplier");
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
            ImGui::Text("Selected x%u | active x%u", request.generatedFrames + 1, activeDisplayMultiplier);
            if (request.generatedFrames > sourceState.state.numFramesToGenerateMax || sourceState.generationLimited)
            {
                ImGui::TextColored(kOchre, "NVIDIA runtime maximum: x%u", sourceState.state.numFramesToGenerateMax + 1);
            }
            if (sourceState.generationLimited)
            {
                ImGui::TextWrapped("Saved/requested MFG is unsupported here. The runtime uses the supported count; "
                                   "your requested preference is retained.");
            }
            DrawSettingsHeading("Latency and frame limit");
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
            ImGui::TextDisabled("Last submitted: %s", TheosRenderPipeline::SourceDLSSG::ReflexModeName(
                                                          sourceBackend.Snapshot().reflexSubmitted));
            ImGui::TextUnformatted("Output FPS limit");
            TheosRenderPipeline::Overlay::FPSInput("##sourceReflexFPS", settingsDraft.sourceDLSSG.outputFPSLimit);
            settingsDraft.sourceDLSSG.outputFPSLimit = std::clamp(settingsDraft.sourceDLSSG.outputFPSLimit, 0, 1000);
            ImGui::TextDisabled("0 = no explicit cap");
            ImGui::TextWrapped("The cap includes generated frames. At x2, a 60 FPS output cap permits about 30 "
                               "raster FPS. Avoid stacking it with another limiter.");
        }

        if (ImGui::CollapsingHeader("Runtime status"))
        {
            ImGui::Text("Display refresh: %.0f Hz", frameGen->refreshRate);
            DrawSettingsHelp("Output FPS uses NVIDIA presentation counts, including passthrough; it does not measure "
                             "physical screen refreshes.");
            if (sourceDLSSGActive)
            {
                ImGui::SeparatorText("SUPPORT");
                ImGui::Text("Path: %s", unlock.UsesAmpereUnlock() ? "Ampere MFG (experimental)"
                                        : unlock.UsesAdaUnlock()  ? "Ada MFG unlock"
                                                                  : "Native NVIDIA runtime");
                if (usableMaximum > 1)
                {
                    ImGui::Text("Available multipliers: x2 to x%u", usableMaximum + 1);
                }
                else if (usableMaximum == 1)
                {
                    ImGui::TextUnformatted("Available multiplier: x2");
                }
                else
                {
                    ImGui::TextDisabled("Available multipliers: waiting for runtime support");
                }
                ImGui::Text("Dynamic multiplier: %s", supportsDynamic ? "supported" : "unavailable");
                if (unlock.UsesCompatibilityUnlock() && !unlock.Ready())
                {
                    ImGui::TextWrapped("%s", unlock.status);
                }
                if (sourceState.stateQueryResult == sl::Result::eWarnOutOfVRAM)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, kOchre);
                    ImGui::TextWrapped("NVIDIA VRAM budget warning; settings unchanged");
                    ImGui::PopStyleColor();
                }
            }
            else
            {
                ImGui::TextWrapped("%s", nvidiaHost->Status().c_str());
            }
            if (nvidiaHost->WarmupPresentsRemaining() > 0)
            {
                ImGui::TextWrapped("Warming up: %d frames remaining", nvidiaHost->WarmupPresentsRemaining());
            }
        }
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
}
