#include "CommunityShaderIntegration.h"
#include "DLSSBackend.h"
#include "DLSSPreset.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"
#include "OverlayFrameView.h"
#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "ReShadeIntegration.h"
#include "RenderPipeline.h"
#include <PCH.h>

using namespace TheosRenderPipeline::Overlay;

void OverlayUI::DrawCompatibilityPanel(float tabCardHeight)
{
    if (!ImGui::BeginTabItem("Compatibility", nullptr,
                             requestedPage == SettingsPage::Compatibility ? ImGuiTabItemFlags_SetSelected : 0))
    {
        return;
    }
    ImGui::BeginChild("##compatibilityPage", ImVec2(0, tabCardHeight), false);
    ImGui::PushTextWrapPos(0.0f);
    DrawSettingsHelp("Post-processing, menus and other mods");
    DrawSettingsHeading("ReShade");
    ImGui::TextUnformatted("Effect placement");
    int placement = settingsDraft.reShadeBeforeUpscaling ? 0 : 1;
    const char* placements[]{"Before upscaling", "After upscaling"};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##reshadePlacement", &placement, placements, 2))
    {
        settingsDraft.reShadeBeforeUpscaling = placement == 0;
    }
    DrawSettingsHelp("Processes world colour and depth before Skyrim UI. Changing placement reloads effects; presets "
                     "and the effects toggle remain in ReShade.");
    ImGui::Text("Session request: %s upscaling",
                RenderPipeline::GetSingleton()->mReShadeBeforeUpscaling ? "before" : "after");
    ImGui::TextWrapped("%s", TheosRenderPipeline::ReShadeIntegration::Get().Status().c_str());
    DrawSettingsHeading("Menus and loading screens");
    ImGui::Checkbox("Request loading-screen artwork", &settingsDraft.requestLoadingArtwork);
    DrawSettingsHelp("Requests artwork on the next eligible cell transition. Skyrim chooses the image; startup loading "
                     "is unchanged.");
    DrawUIStatusPanel();
    DrawSettingsHeading("Display and shared controls", "");
    ImGui::TextUnformatted("HDR is not supported.");
    const auto key = RenderPipeline::GetSingleton()->mToggleOverlayHotkey;
    char keyName[64]{};
    const UINT scan = MapVirtualKeyA(static_cast<UINT>(key), MAPVK_VK_TO_VSC_EX);
    const LONG keyNameCode = static_cast<LONG>(((scan & 0xff) << 16) | ((scan & 0xff00) ? (1 << 24) : 0));
    if (GetKeyNameTextA(keyNameCode, keyName, sizeof(keyName)))
    {
        ImGui::Text("TRP settings key: %s (configured in the INI)", keyName);
    }
    else
    {
        ImGui::Text("TRP settings key: virtual-key 0x%02X (configured in the INI)", key);
    }
    DrawSettingsHelp("Use separate menu keys for TRP, Community Shaders, KreatE and other overlays.");
    if (TheosRenderPipeline::CommunityShaders::Active())
    {
        ImGui::TextWrapped("Keep Community Shaders' HDR, frame generation and Reflex disabled. TRP manages frame "
                           "generation and Reflex.");
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::EndTabItem();
}

void OverlayUI::DrawDiagnosticsPanel(float tabCardHeight, const FrameView& view)
{
    if (!ImGui::BeginTabItem("Diagnostics", nullptr,
                             requestedPage == SettingsPage::Diagnostics ? ImGuiTabItemFlags_SetSelected : 0))
    {
        return;
    }
    ImGui::BeginChild("##diagnosticsPage", ImVec2(0, tabCardHeight), false);
    ImGui::PushTextWrapPos(0.0f);
    DrawSettingsHelp("Active state, measurements and troubleshooting");
    ImGui::Text("World renderer: %s",
                TheosRenderPipeline::CommunityShaders::Active() ? "Community Shaders" : "Skyrim / ENB");
    ImGui::Text("Raster-rendered: %.1f FPS | %s: %s", renderedFps, view.outputLabel, view.outputText.c_str());
    ImGui::Text("Raster frame time: %.2f ms", view.avgMs);
    ImGui::PlotLines("##frametimes", frameTimesMs, frameTimeCount, frameTimeIndex, nullptr, 0, 50, ImVec2(-1, 80));
    DrawSettingsHelp(
        "Output counts runtime presentations, not physical screen refreshes. Scanout cadence is not measured here.");
    ImGui::Checkbox("Lab mode for this session", &showDeveloperControls);
    DrawSettingsHelp(
        "Shows output experiments in Image, UI-routing controls in Compatibility and diagnostic tools below.");
    DrawRuntimePanel(view);
    DrawPerformancePanel(view.sourceNeural);
    if (ImGui::CollapsingHeader("GPU memory"))
    {
        auto* videoMemory = VideoMemoryTelemetry::GetSingleton();
        if (view.memorySnapshot.available && view.memorySnapshot.budget > 0)
        {
            constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
            const auto headroomBytes = view.memorySnapshot.currentUsage < view.memorySnapshot.budget
                                           ? view.memorySnapshot.budget - view.memorySnapshot.currentUsage
                                           : 0;
            const float pressure = static_cast<float>(static_cast<double>(view.memorySnapshot.currentUsage) /
                                                      static_cast<double>(view.memorySnapshot.budget));
            const UIHealth memoryHealth = pressure >= 0.92f   ? UIHealth::kError
                                          : pressure >= 0.80f ? UIHealth::kWarning
                                                              : UIHealth::kHealthy;
            ImGui::Spacing();
            ImGui::SeparatorText("LOCAL GPU MEMORY");
            DrawStatusLabel(pressure >= 0.92f   ? "BUDGET CRITICAL"
                            : pressure >= 0.80f ? "BUDGET PRESSURE"
                                                : "BUDGET HEALTHY",
                            memoryHealth);
            ImGui::Text("Current usage: %.2f GiB", static_cast<double>(view.memorySnapshot.currentUsage) / kGiB);
            ImGui::Text("Driver budget: %.2f GiB", static_cast<double>(view.memorySnapshot.budget) / kGiB);
            ImGui::Text("Budget headroom: %.2f GiB", static_cast<double>(headroomBytes) / kGiB);
            if (view.memorySnapshot.dedicatedCapacity > 0)
            {
                ImGui::TextDisabled("Physical dedicated memory: %.2f GiB",
                                    static_cast<double>(view.memorySnapshot.dedicatedCapacity) / kGiB);
            }
            char pressureLabel[32]{};
            std::snprintf(pressureLabel, sizeof(pressureLabel), "%.1f%% of budget", pressure * 100.0f);
            ImGui::ProgressBar(std::clamp(pressure, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), pressureLabel);
        }
        else
        {
            ImGui::Spacing();
            ImGui::SeparatorText("LOCAL GPU MEMORY");
            ImGui::TextDisabled("%s", videoMemory->Status());
        }
    }
    if (ImGui::CollapsingHeader("Reporting a problem"))
    {
        const auto& upscaler = NvidiaHost::GetSingleton()->SourceUpscalerSettings().Effective();
        const auto neural = TheosRenderPipeline::SourceDLSSG::Backend::Get().NeuralConfiguration();
        if (!TheosRenderPipeline::CommunityShaders::Active())
        {
            ImGui::Text("Upscaling: %s | requested preset %s", ModeName(upscaler.mode),
                        TheosRenderPipeline::DLSSPreset::ShortName(upscaler.preset));
        }
        ImGui::Text("Frame generation: %s | active x%u", view.frameGenerationRuntimeActive ? "active" : "inactive",
                    view.activeDisplayMultiplier);
        ImGui::Text("NR session request: %s | %s upscaling | %d %s", neural.enabled ? "on" : "off",
                    neural.beforeUpscaling ? "before" : "after", neural.passes, neural.passes == 1 ? "pass" : "passes");
        ImGui::TextWrapped(
            "Include your GPU, driver, game and TRP versions, the active renderer, settings and reproduction steps.");
        DrawSettingsHelp("Attach TheosRenderPipeline.log from Documents / My Games / Skyrim Special Edition / SKSE.");
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::EndTabItem();
}

void OverlayUI::DrawUIStatusPanel()
{
    auto* upscaler = RenderPipeline::GetSingleton();
    auto* nvidiaHost = NvidiaHost::GetSingleton();
    if (ImGui::CollapsingHeader("UI integration"))
    {
        ImGui::TextUnformatted("UI COMPOSITION");
        ImGui::Separator();
        if (showDeveloperControls && !TheosRenderPipeline::CommunityShaders::Active())
        {
            DrawSettingsHelp("Save and restart after changing UI routing.");
            ImGui::Checkbox("Native-resolution Skyrim UI", &settingsDraft.nativeUI);
            ImGui::Checkbox("Startup overlays at native resolution", &settingsDraft.lateOverlayBridge);
            ImGui::Spacing();
        }

        if (ImGui::BeginTable("##compatibilityStatus", 2,
                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Producer", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 330.0f);
            ImGui::TableHeadersRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Skyrim / Scaleform");
            ImGui::TableNextColumn();
            DrawStatusLabel(TheosRenderPipeline::CommunityShaders::Active() ? "COMMUNITY SHADERS"
                            : upscaler->mNativeUI                           ? "NATIVE COMPOSITION"
                                                                            : "RENDER-SPACE UI",
                            upscaler->mNativeUI ? UIHealth::kHealthy : UIHealth::kIdle);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("External ImGui overlays");
            ImGui::TableNextColumn();
            DrawStatusLabel(TheosRenderPipeline::CommunityShaders::Active() ? "COMMUNITY SHADERS UI"
                            : nvidiaHost->StartupConfigured()               ? "NATIVE HOST / STARTUP FOREGROUND"
                                                                            : "HOST UNAVAILABLE",
                            nvidiaHost->StartupConfigured() ? UIHealth::kHealthy : UIHealth::kIdle);
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextWrapped(
            TheosRenderPipeline::CommunityShaders::Active()
                ? "Community Shaders owns the UI render targets. External menus retain their original draw paths."
                : "Supported startup overlays use a native-resolution foreground target. "
                  "Their original render state is restored after each draw.");
    }
}

void OverlayUI::DrawRuntimePanel(const FrameView& view)
{
    auto* upscaler = RenderPipeline::GetSingleton();
    auto* backend = DLSSBackend::GetSingleton();
    if (ImGui::CollapsingHeader("Runtime details"))
    {
        ImGui::TextUnformatted("RENDERER TELEMETRY");
        ImGui::Separator();
        ImGui::Text("Host Present calls: %.1f FPS | %s: %s", presentedFps, view.outputLabel, view.outputText.c_str());
        ImGui::Text("Active path: %s%s", view.activeUpscaleStage,
                    TheosRenderPipeline::CommunityShaders::Active() || upscaler->IsEnabled() ? "" : " (inactive)");
        ImGui::Text("Render %d x %d -> Native %d x %d (%.1f%%)", upscaler->mRenderSizeX, upscaler->mRenderSizeY,
                    view.nativeWidth, view.nativeHeight,
                    view.nativeWidth > 0
                        ? static_cast<float>(upscaler->mRenderSizeX) / static_cast<float>(view.nativeWidth) * 100.0f
                        : 100.0f);
        if (!TheosRenderPipeline::CommunityShaders::Active())
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
        ImGui::SeparatorText("FRAME GENERATION RUNTIME");
        auto* nvidiaHost = NvidiaHost::GetSingleton();
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
        if (showDeveloperControls && !TheosRenderPipeline::CommunityShaders::Active())
        {
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
    }
}
