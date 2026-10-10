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
        const bool intel = sourceBackend.Provider() == TheosRenderPipeline::FrameGenerationProvider::XeFG;
        const bool editIntel = settingsDraft.sourceDLSSG.provider == TheosRenderPipeline::FrameGenerationProvider::XeFG;
        const auto& unlock = sourceBackend.MFGState();
        const auto usableMaximum = editIntel ? TheosRenderPipeline::XeFGMaxGeneratedFrames : TheosRenderPipeline::SourceDLSSG::MFGContract::Maximum(
            unlock.UsesCompatibilityUnlock(), unlock.Ready(), sourceState.state.numFramesToGenerateMax);
        const bool supportsDynamic = !editIntel && TheosRenderPipeline::SourceDLSSG::MFGContract::Dynamic(
                                         unlock.UsesCompatibilityUnlock(), unlock.Ready(),
                                         sourceState.state.bIsDynamicMFGSupported == sl::eTrue) &&
                                     sourceState.state.numFramesToGenerateMax > 1;
        if (!BeginSettingsColumns("generation", tabCardHeight, view))
        {
            ImGui::EndTabItem();
            return;
        }
        DrawStatusLabel(frameGenerationRuntimeActive ? (intel ? "XeFG active" : "DLSS-G active") : "Frame generation inactive",
                        frameGenerationRuntimeActive ? UIHealth::kHealthy : UIHealth::kIdle);
        DrawSettingsValue("Multiplier", std::format("x{}", activeDisplayMultiplier).c_str());
        DrawSettingsValue("Display", std::format("{:.0f} Hz", frameGen->refreshRate).c_str());
        DrawSettingsValue("Latency", intel ? "XeLL" : TheosRenderPipeline::SourceDLSSG::ReflexModeName(sourceState.reflexSubmitted));
        DrawSettingsValue("Output cap", intel ? (sourceBackend.XeFGState().frameLimitUs ?
            std::format("{:.1f} FPS", 1000000.0 / sourceBackend.XeFGState().frameLimitUs).c_str() : "Off") :
                          sourceState.frameLimitSubmittedUs
                              ? std::format("{:.1f} FPS", 1000000.0 / sourceState.frameLimitSubmittedUs).c_str()
                              : "Off");
        const char* intelPath = sourceBackend.XeFGState().unlockReady && sourceBackend.XeFGState().generatedFrames > 1
            ? "Experimental XeFG MFG" : "Intel XeFG x2";
        DrawSettingsValue("Path", intel                       ? intelPath
                                  : unlock.UsesTuringUnlock() ? "Turing MFG (experimental)"
                                  : unlock.UsesAmpereUnlock() ? "Ampere MFG (experimental)"
                                  : unlock.UsesAdaUnlock()    ? "Ada MFG"
                                                              : "Native NVIDIA");
        const auto availableMaximum = intel ? sourceBackend.XeFGState().maxGeneratedFrames : usableMaximum;
        if (availableMaximum > 1)
            ImGui::Text("Available: x2 to x%u", availableMaximum + 1);
        else
            ImGui::TextUnformatted(availableMaximum == 1 ? "Available: x2" : "Availability: waiting");
        ImGui::TextWrapped("%s", sourceBackend.ProviderStatus().c_str());
        if (intel)
        {
            const auto& state = sourceBackend.XeFGState();
            ImGui::Text("Runtime outputs: %u | generated presents: %llu", state.framesPresented,
                static_cast<unsigned long long>(state.generatedPresents));
            DrawSettingsValue("UI composition", !sourceBackend.UIRecompositionConfiguration() ? "Off (UI interpolated)"
                                                : state.uiTexture ? "UI layer + HUD-less"
                                                                  : "Extracted from frame");
            if (state.frameTimeMs > 0)
                ImGui::Text("Frame time sent: %.2f ms", state.frameTimeMs);
            if (showDeveloperControls)
            {
                // Session-only diagnostics: separate generated-frame artifacts from upscaler ones.
                bool onlyGenerated = sourceBackend.XeFGOnlyGenerated(), tagGenerated = sourceBackend.XeFGTagGenerated();
                bool changed = ImGui::Checkbox("Show only generated frames (Lab)", &onlyGenerated);
                changed |= ImGui::Checkbox("Tag generated frames (Lab)", &tagGenerated);
                if (changed)
                    sourceBackend.ConfigureXeFGDebugView(onlyGenerated, tagGenerated);
                DrawSettingsHelp("Intel debug views, not saved. Only generated frames hides every real frame; "
                                 "tagging marks generated frames with corner boxes.");
                bool gpuInputWait = sourceBackend.XeFGGpuInputWait();
                if (ImGui::Checkbox("Input reuse: GPU fence wait (Lab)", &gpuInputWait))
                    sourceBackend.ConfigureXeFGGpuInputWait(gpuInputWait);
                DrawSettingsHelp("A/B for the post-Present wait, not saved. Off drains every host lane on the CPU; "
                                 "on queues a GPU wait for Intel's input copies. The log reports both waits every 600 frames. "
                                 "With heavy NR, on can make rendered frames alternate long and short.");
            }
            ImGui::TextWrapped("%s", sourceBackend.XeFGUnlockStatus().c_str());
        }
        if (!intel && unlock.UsesCompatibilityUnlock() && !unlock.Ready())
            ImGui::TextWrapped("%s", unlock.status);
        if (!intel && sourceState.stateQueryResult == sl::Result::eWarnOutOfVRAM)
            ImGui::TextColored(kOchre, "NVIDIA VRAM budget warning");
        if (nvidiaHost->WarmupPresentsRemaining() > 0)
            ImGui::Text("Warmup: %d frames", nvidiaHost->WarmupPresentsRemaining());
        if (showDeveloperControls && ImGui::CollapsingHeader("Runtime details"))
        {
            ImGui::Text("Evaluations: %llu", static_cast<unsigned long long>(nvidiaHost->EvaluationCount()));
            ImGui::Text("Host Presents: %llu | failures %llu | last 0x%08X",
                        static_cast<unsigned long long>(nvidiaHost->PresentCount()),
                        static_cast<unsigned long long>(nvidiaHost->FailedPresentCount()),
                        static_cast<unsigned int>(nvidiaHost->LastPresentResult()));
            if (!intel && nvidiaHost->RuntimeStateObservationCount() > 0)
            {
                ImGui::Text("Last query: %u outputs | max generated %u | min dimension %u",
                            nvidiaHost->RuntimeFramesActuallyPresented(), nvidiaHost->RuntimeMaxGeneratedFrames(),
                            nvidiaHost->RuntimeMinWidthOrHeight());
                ImGui::Text("DLSS-G status: %u | observations: %llu", nvidiaHost->RuntimeDLSSGStatus(),
                            static_cast<unsigned long long>(nvidiaHost->RuntimeStateObservationCount()));
            }
            else if (!intel)
            {
                ImGui::TextDisabled("DLSS-G state: waiting");
            }
            if (intel) {
                const auto& state = sourceBackend.XeFGState();
                ImGui::Text("XeFG frame: %u | result: %d | warnings: %llu", state.frameId, state.interpolationResult,
                    static_cast<unsigned long long>(state.warnings));
                ImGui::Text("XeLL output interval: %u us", state.frameLimitUs);
            }
            ImGui::TextWrapped("%s", nvidiaHost->Status().c_str());
            if (view.sourceDLSSGActive && !intel)
            {
                ImGui::Text("Configured multiplier: x%u", sourceBackend.Snapshot().options.numFramesToGenerate + 1);
                ImGui::Text("Output limit submitted interval: %u us", sourceBackend.Snapshot().frameLimitSubmittedUs);
                ImGui::TextWrapped("MFG: %s", sourceBackend.MFGState().status);
            }

            ImGui::Text("Dynamic multiplier: %s", supportsDynamic ? "supported" : "unavailable");
        }
        DrawStageMeasurements(SettingsPage::FrameGeneration);
        NextSettingsColumn(tabCardHeight);
        int provider = static_cast<int>(settingsDraft.sourceDLSSG.provider);
        const char* providers[]{"NVIDIA DLSS-G", "Intel XeFG (experimental)"};
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("Provider", providers[std::clamp(provider, 0, 1)])) {
            for (int i = 0; i < 2; ++i) {
                ImGui::BeginDisabled(i == 0 && !sourceBackend.NvidiaAdapter());
                if (ImGui::Selectable(providers[i], provider == i)) { settingsDraft.sourceDLSSG.provider = static_cast<TheosRenderPipeline::FrameGenerationProvider>(i); }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        DrawSettingsHelp("Apply switches after a completed world frame, with a pause of under a second. Upscaling and NR keep "
                         "their settings. NVIDIA DLSS-G requires NVIDIA hardware; XeFG uses XeLL latency reduction. On NVIDIA "
                         "cards XeFG costs noticeably more GPU time than DLSS-G; it is meant for other GPUs.");
        if (sourceBackend.RequestedProvider() != sourceBackend.Provider()) {
            ImGui::TextDisabled("Provider change pending; waiting for world inputs...");
        }
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
            auto& requestedCount = editIntel ? settingsDraft.sourceDLSSG.xefg.generatedFrames : request.generatedFrames;
            const char* multipliers[]{"x2", "x3", "x4", "x5", "x6"};
            const char* intelMultipliers[]{"x2", "x3 (experimental)", "x4 (experimental)", "x5 (experimental)",
                                           "x6 (experimental)"};
            static_assert(TheosRenderPipeline::XeFGMaxGeneratedFrames <= std::size(intelMultipliers));
            const auto& labels = editIntel ? intelMultipliers : multipliers;
            const auto listedMaximum = editIntel ? TheosRenderPipeline::XeFGMaxGeneratedFrames : 5u;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##sourceMultiplier", labels[std::clamp(requestedCount, 1u, listedMaximum) - 1]))
            {
                for (unsigned count = 1; count <= listedMaximum; ++count)
                {
                    ImGui::BeginDisabled(count > usableMaximum);
                    if (ImGui::Selectable(labels[count - 1], requestedCount == count))
                    {
                        requestedCount = count;
                        if (editIntel)
                            settingsDraft.sourceDLSSG.xefg.experimentalMFG = count > 1;
                    }
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
            if (editIntel)
            {
                DrawSettingsHelp("x2 is Intel's official mode. x3-x6 patch Intel's runtime in memory and are experimental. "
                                 "The first Apply to x3-x6 recreates the Intel presenter; later multiplier changes apply live.");
            }
            if (!editIntel)
            {
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
            }
            ImGui::Checkbox("UI recomposition##sourceDLSSG", &settingsDraft.sourceDLSSG.uiRecomposition);
            DrawSettingsHelp("Generates the scene and HUD separately to reduce HUD ghosting in motion. "
                             "Small GPU and VRAM cost. Apply to compare live.");
            if (!intel && frameGenerationRuntimeActive && sourceState.uiRecompositionRequested)
            {
                if (sourceState.options.enableUserInterfaceRecomposition == sl::eTrue)
                    ImGui::TextDisabled("Submitted to DLSS-G");
                else
                    ImGui::TextColored(kOchre, "Waiting for HUD-less and UI layers");
            }
            if (editIntel)
            {
                ImGui::Checkbox("Send frame time to XeFG##sourceDLSSG", &settingsDraft.sourceDLSSG.xefgFrameTime);
                DrawSettingsHelp("Gives Intel's frame pacing the measured frame time. On AMD and NVIDIA cards Intel "
                                 "uses it to sanity-check pacing. Apply to compare live.");
            }
            ImGui::Separator();
            ImGui::BeginDisabled(editIntel);
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
            ImGui::EndDisabled();
            TheosRenderPipeline::Overlay::FPSInput("##sourceReflexFPS", settingsDraft.sourceDLSSG.outputFPSLimit);
            settingsDraft.sourceDLSSG.outputFPSLimit = std::clamp(settingsDraft.sourceDLSSG.outputFPSLimit, 0, 1000);
            ImGui::TextDisabled("0 = no explicit cap");
            DrawSettingsHelp("The cap includes generated frames. Avoid stacking it with another limiter.");
            if (editIntel) { ImGui::TextDisabled("XeFG uses XeLL. NVIDIA multiplier and Reflex preferences are retained."); }
        }

        EndSettingsColumns();
        ImGui::EndTabItem();
    }
}
