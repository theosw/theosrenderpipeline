#include "CommunityShaderIntegration.h"
#include "DLSSBackend.h"
#include "DLSSPreset.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"
#include "OverlayFrameView.h"
#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "PerformanceTuning.h"
#include "ReShadeIntegration.h"
#include "RenderPipeline.h"
#include <PCH.h>
using namespace TheosRenderPipeline::Overlay;

void OverlayUI::DrawImageMeasurements(const FrameView& view)
{
    auto* upscaler = RenderPipeline::GetSingleton();
    auto* backend = DLSSBackend::GetSingleton();
    auto* host = NvidiaHost::GetSingleton();
    const bool cs = TheosRenderPipeline::CommunityShaders::Active();
    DrawStatusLabel(cs ? "Community Shaders" : ModeName(host->SourceUpscalerSettings().Effective().mode),
                    view.upscaleHealth);
    DrawSettingsValue("Render", std::format("{} x {}", host->RenderWidth(), host->RenderHeight()).c_str());
    DrawSettingsValue("Output", std::format("{} x {}", view.nativeWidth, view.nativeHeight).c_str());
    if (!cs)
        DrawSettingsValue("Preset request", TheosRenderPipeline::DLSSPreset::ShortName(
                                                host->SourceUpscalerSettings().Effective().preset));
    ImGui::Separator();
    DrawStageMeasurements(SettingsPage::Image);
    if (view.memorySnapshot.available)
    {
        constexpr double gib = 1024.0 * 1024.0 * 1024.0;
        DrawSettingsValue("GPU memory", std::format("{:.2f} / {:.2f} GiB", view.memorySnapshot.currentUsage / gib,
                                                    view.memorySnapshot.budget / gib)
                                            .c_str());
    }
    DrawMemoryMeasurements(view);
    if (view.textureProviderAvailable)
    {
        if (ImGui::CollapsingHeader("Texture residency"))
        {
            DrawStatusLabel(view.textureProviderAvailable
                                ? (view.textureTelemetry.hooksInstalled ? "PROVIDER ACTIVE" : "RELAUNCH REQUIRED")
                                : "PROVIDER UNAVAILABLE",
                            view.textureProviderAvailable
                                ? (view.textureTelemetry.hooksInstalled ? UIHealth::kHealthy : UIHealth::kWarning)
                                : UIHealth::kIdle);
            ImGui::Spacing();
            ImGui::Text("Textures reduced: %llu",
                        static_cast<unsigned long long>(view.textureTelemetry.reducedTextures));
            ImGui::Text("Estimated allocation avoided: %.2f GiB",
                        static_cast<double>(view.textureTelemetry.estimatedBytesAvoided) / (1024.0 * 1024.0 * 1024.0));
            const double nameCoverage = view.textureTelemetry.nameLookups > 0
                                            ? static_cast<double>(view.textureTelemetry.namesResolved) * 100.0 /
                                                  static_cast<double>(view.textureTelemetry.nameLookups)
                                            : 0.0;
            ImGui::Text("Texture names resolved: %llu / %llu (%.1f%%)",
                        static_cast<unsigned long long>(view.textureTelemetry.namesResolved),
                        static_cast<unsigned long long>(view.textureTelemetry.nameLookups), nameCoverage);
            ImGui::Spacing();
            ImGui::TextWrapped("The provider lowers the authored top mip before Skyrim allocates eligible file-backed "
                               "textures. Avoided allocation is an independent estimate; actual GPU memory usage is "
                               "shown in the left column.");
            ImGui::Spacing();
            ImGui::TextDisabled("%s", TextureProviderBridge::GetSingleton()->Status());
        }
    }
    if (ImGui::CollapsingHeader("Runtime details"))
    {
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
                                   route.reason.c_str());
                ImGui::TextWrapped("Frames %llu | fallbacks %llu", static_cast<unsigned long long>(route.activeFrames),
                                   static_cast<unsigned long long>(route.fallbackCount));
            }
        }
    }
}

void OverlayUI::DrawMemoryMeasurements(const FrameView& view)
{
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
            ImGui::TextDisabled("%s", videoMemory->Status());
        }
    }
}

void OverlayUI::DrawCompatibilityPanel(float height, const FrameView& view)
{
    if (!ImGui::BeginTabItem("Compatibility", nullptr,
                             requestedPage == SettingsPage::Compatibility ? ImGuiTabItemFlags_SetSelected : 0))
        return;
    if (BeginSettingsColumns("compatibility", height, view))
    {
        auto* pipeline = RenderPipeline::GetSingleton();
        const bool cs = TheosRenderPipeline::CommunityShaders::Active();
        DrawStatusLabel(cs ? "Community Shaders" : "Skyrim / ENB", view.nativeUIHealth);
        DrawSettingsValue("ReShade request",
                          pipeline->mReShadeBeforeUpscaling ? "Before upscaling" : "After upscaling");
        ImGui::TextWrapped("%s", TheosRenderPipeline::ReShadeIntegration::Get().Status().c_str());
        DrawUIStatusPanel();
        DrawStageMeasurements(SettingsPage::Compatibility);
        NextSettingsColumn(height);
        int placement = settingsDraft.reShadeBeforeUpscaling ? 0 : 1;
        const char* placements[]{"Before upscaling", "After upscaling"};
        if (ImGui::Combo("ReShade", &placement, placements, 2))
            settingsDraft.reShadeBeforeUpscaling = placement == 0;
        DrawSettingsHelp("Processes world colour/depth before Skyrim UI. Changing placement reloads effects.");
        ImGui::Checkbox("Request loading artwork", &settingsDraft.requestLoadingArtwork);
        DrawSettingsHelp("Requests artwork at the next eligible cell transition; Skyrim selects the image.");
        if (showDeveloperControls && !cs && ImGui::CollapsingHeader("UI integration (Lab)"))
        {
            ImGui::TextDisabled("Save and restart");
            ImGui::Checkbox("Native-resolution Skyrim UI", &settingsDraft.nativeUI);
            ImGui::Checkbox("Startup overlays at native resolution", &settingsDraft.lateOverlayBridge);
        }
        ImGui::Separator();
        const auto key = pipeline->mToggleOverlayHotkey;
        char name[64]{};
        const UINT scan = MapVirtualKeyA(static_cast<UINT>(key), MAPVK_VK_TO_VSC_EX);
        const LONG code = static_cast<LONG>(((scan & 0xff) << 16) | ((scan & 0xff00) ? (1 << 24) : 0));
        if (GetKeyNameTextA(code, name, sizeof(name)))
            ImGui::Text("Settings key: %s", name);
        else
            ImGui::Text("Settings key: 0x%02X", key);
        DrawSettingsHelp("Configured in the INI. Use separate keys for TRP, Community Shaders and KreatE.");
        ImGui::TextUnformatted("HDR unsupported");
        if (cs)
            ImGui::TextWrapped("Keep CS HDR, frame generation and Reflex off.");
        if (showDeveloperControls && !cs && ImGui::CollapsingHeader("Menu diagnostics (Lab)"))
        {
            auto* upscaler = pipeline;
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
        EndSettingsColumns();
    }
    ImGui::EndTabItem();
}

void OverlayUI::DrawUIStatusPanel()
{
    auto* pipeline = RenderPipeline::GetSingleton();
    auto* host = NvidiaHost::GetSingleton();
    const bool cs = TheosRenderPipeline::CommunityShaders::Active();
    DrawSettingsValue("UI composition", cs                    ? "Community Shaders"
                                        : pipeline->mNativeUI ? "Native resolution"
                                                              : "Render resolution");
    if (ImGui::CollapsingHeader("UI details"))
    {
        DrawSettingsValue("External overlays", cs                          ? "CS UI path"
                                               : host->StartupConfigured() ? "Native host"
                                                                           : "Unavailable");
        ImGui::TextWrapped("%s", cs ? "Community Shaders owns the UI render targets."
                                    : "Supported startup overlays use the native foreground target.");
    }
}

void OverlayUI::DrawSupport(const FrameView& view)
{
    if (supportRequested)
    {
        ImGui::OpenPopup("Support##TRP");
        supportRequested = false;
    }
    const auto display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSizeConstraints(ImVec2(300, 0),
                                        ImVec2((std::max)(300.0f, display.x - 40), (std::max)(200.0f, display.y - 40)));
    ImGui::SetNextWindowSize(ImVec2(650, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("Support##TRP"))
        return;
    ImGui::PushTextWrapPos(0);
    ImGui::Checkbox("Lab mode", &showDeveloperControls);
    DrawMeasurementControls();
    if (ImGui::CollapsingHeader("Rendering path"))
    {
        DrawPipelineSummary(view);
        if (requestedPage != SettingsPage::None)
            ImGui::CloseCurrentPopup();
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
        ImGui::Text("NR Pass 1: %.1f%% | network %s", neural.reconstruction.inputScale * 100,
                    neural.reconstruction.preset == 1 ? "Shipping" : "Default");
        if (neural.passes == 2)
        {
            const auto second = neural.EffectiveSecond();
            ImGui::Text("NR Pass 2: %s | %.1f%% | network %s", second.linked ? "linked" : "custom",
                        second.inputScale * 100, second.preset == 1 ? "Shipping" : "Default");
        }
        ImGui::TextWrapped(
            "Include your GPU, driver, game and TRP versions, the active renderer, settings and reproduction steps.");
        ImGui::TextWrapped("Attach TheosRenderPipeline.log from Documents / My Games / Skyrim Special Edition / SKSE.");
    }

    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}
