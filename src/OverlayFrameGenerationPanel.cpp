#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "OverlayFrameView.h"

#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"
#include "FrameGen/FGInputDebugView.h"
#include "FrameGen/FGMotionRepair.h"
#include "FrameGen/SourceFrameGeneration.h"
#include "FrameGen/XeFGUnlock.h"
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
        // NVIDIA's limits are known only once DLSS-G has run and answered a state
        // query; before that, leave the list open and let the runtime limit it.
        const bool nvidiaReported = !intel && sourceState.stateQueries > 0;
        // Standard offers Intel's official x2 only; the x3-x6 unlock is in Universal.
        const auto usableMaximum = editIntel ? (TheosRenderPipeline::XeFGUnlock::Available() ? TheosRenderPipeline::XeFGMaxGeneratedFrames : 1u) :
            !nvidiaReported ? 5u :
            TheosRenderPipeline::SourceDLSSG::MFGContract::Maximum(
            unlock.UsesCompatibilityUnlock(), unlock.Ready(), sourceState.state.numFramesToGenerateMax);
        const bool supportsDynamic = !editIntel && nvidiaReported && TheosRenderPipeline::SourceDLSSG::MFGContract::Dynamic(
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
        const auto availableMaximum = intel ? sourceBackend.XeFGState().maxGeneratedFrames : nvidiaReported ? usableMaximum : 0u;
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
        if (!intel && showDeveloperControls)
        {
            // Session-only diagnostic: an artifact only in generated frames comes from generation.
            bool onlyGenerated = sourceBackend.XeFGOnlyGenerated();
            if (ImGui::Checkbox("Show only generated frames (Lab)", &onlyGenerated))
                sourceBackend.ConfigureXeFGDebugView(onlyGenerated, sourceBackend.XeFGTagGenerated());
            DrawSettingsHelp("NVIDIA debug view, not saved. Hides every rendered frame, so an artifact still visible here "
                             "comes from frame generation; one that disappears comes from rendering, the upscaler or NR.");
        }
        if (showDeveloperControls)
        {
#if defined(TRP_ENABLE_FRAMEWARP_EXPERIMENT)
            auto& warp = sourceBackend.ExperimentalFrameWarp();
            bool warpEnabled = warp.Requested();
            if (ImGui::Checkbox("Experimental camera reprojection (Lab)", &warpEnabled)) warp.Enable(warpEnabled);
            DrawSettingsHelp("Session only. Requires frame generation off, SDR, and dedicated native UI. "
                             "Uses XPAR's own presenter without ReShade. Pauses while settings or a paused menu are open. "
                             "Close settings to start; disable here to return to native presentation. Game acceptance is pending.");
            bool original = warp.Original();
            if (ImGui::Checkbox("FrameWarp A/B: original image", &original)) warp.Original(original);
            const auto ws = warp.Snapshot();
            ImGui::TextWrapped("FrameWarp: %s", ws.message.c_str());
            ImGui::Text("Sources %llu | skipped %llu | source %.1f / output %.1f fps",
                static_cast<unsigned long long>(ws.published), static_cast<unsigned long long>(ws.dropped), ws.sourceFPS, ws.outputFPS);
            ImGui::Text("Warp GPU %.2f ms | source age %.2f ms | mouse fit %s/%s",
                ws.warpGPUMs, ws.sourceAgeMs, ws.calibratedX ? "ready" : "learning", ws.calibratedY ? "ready" : "learning");
            if (ws.copyGPUSamples)
                ImGui::Text("Native capture GPU %.3f ms average | %.3f ms max (%llu samples)",
                    ws.copyGPUAverageMs, ws.copyGPUMaxMs, static_cast<unsigned long long>(ws.copyGPUSamples));
            else ImGui::TextUnformatted("Native capture GPU timing: pending or unavailable");
            DrawSettingsHelp("Presenter telemetry is available only after the helper runs. Source age and learned mouse timing "
                             "are estimates; they do not measure physical input-to-photon latency. A capture fault requires host retirement or restart.");
#endif
            auto& repair = TheosRenderPipeline::FGMotionRepair::Get();
            bool repairMotion = repair.Enabled();
            if (ImGui::Checkbox("Repair missing motion (Lab)", &repairMotion))
                repair.Enable(repairMotion);
            DrawSettingsHelp("On by default; turning it off lasts until the game restarts. Underwater, Skyrim writes no real motion, "
                             "and generated frames show an offset copy. When nearly the whole frame disagrees with the camera, "
                             "NR and generation get the camera's motion instead.");
            auto& inputs = TheosRenderPipeline::FGInputDebugView::Get();
            bool showInputs = inputs.Enabled();
            if (ImGui::Checkbox("Show frame generation inputs (Lab)", &showInputs))
                inputs.Enable(showInputs);
            DrawSettingsHelp("Not saved. Shows what generation receives, after any repair. Left: motion vectors (grey = none). Middle: depth (dark = near). "
                             "Right: where the motion vectors disagree with the camera's own motion at that depth "
                             "(black = agree, red = 8+ pixels off). Static scenery should stay black while the camera moves.");
            if (showInputs)
            {
                if (auto* strip = inputs.Strip())
                {
                    const float width = ImGui::GetContentRegionAvail().x;
                    const float height = width * float(inputs.PanelHeight()) / float(inputs.PanelWidth() * 3);
                    ImGui::Image(reinterpret_cast<ImTextureID>(strip), ImVec2(width, height));
                }
                else
                    ImGui::TextWrapped("Inputs: %s", inputs.Status());
            }
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
        if (!editIntel && intel && sourceBackend.NvidiaSwitchBlocked()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kOchre);
            ImGui::TextWrapped("Switching to NVIDIA needs a restart this session: NR started before NVIDIA's runtime. "
                               "Save as default, then restart Skyrim.");
            ImGui::PopStyleColor();
        } else if (sourceBackend.RequestedProvider() != sourceBackend.Provider()) {
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
                if (TheosRenderPipeline::XeFGUnlock::Available())
                    DrawSettingsHelp("x2 is Intel's official mode. x3-x6 patch Intel's runtime in memory and are experimental. "
                                     "The first Apply to x3-x6 recreates the Intel presenter; later multiplier changes apply live.");
                else
                    DrawSettingsHelp("x2 is Intel's official mode. The experimental x3-x6 patch Intel's runtime in memory and "
                                     "need the Universal edition installed below Standard; a saved x3-x6 runs at x2.");
            }
            if (!editIntel)
            {
                if (supportsDynamic)
                {
                    ImGui::Checkbox("Dynamic multiplier##sourceMFG", &request.dynamic);
                }
                else if (request.dynamic && nvidiaReported)
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
                if (!nvidiaReported)
                {
                    ImGui::TextDisabled("NVIDIA reports its supported multipliers once DLSS-G is running.");
                }
                else if (request.generatedFrames > sourceState.state.numFramesToGenerateMax || sourceState.generationLimited)
                {
                    ImGui::TextColored(kOchre, "NVIDIA runtime maximum: x%u", sourceState.state.numFramesToGenerateMax + 1);
                }
                if (nvidiaReported && sourceState.generationLimited)
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
