#include <PCH.h>
#include "OverlayUI.h"
#include "OverlayFrameView.h"
#include "OverlayUIStyle.h"
#include "RenderPipeline.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"
#include "DLSSBackend.h"
#include "CommunityShaderIntegration.h"

using namespace TheosRenderPipeline::Overlay;

void OverlayUI::DrawAdvancedPanel(float advancedCardHeight, const FrameView& view)
{
    if (ImGui::BeginTabItem("Advanced"))
    {
        ImGui::Checkbox("ReShade before upscaling", &settingsDraft.reShadeBeforeUpscaling);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Runs ReShade effects before upscaling when checked, after upscaling otherwise.\n"
                "Effects receive world color and depth before Skyrim UI. Changing placement reloads effects.\n"
                "ReShade's own effects toggle and preset selection remain available.");
        }
        ImGui::TextWrapped("%s", TheosRenderPipeline::ReShadeIntegration::Get().Status().c_str());
        ImGui::Checkbox("Request loading-screen artwork", &settingsDraft.requestLoadingArtwork);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Requests artwork during cell transitions that would otherwise omit it.\n"
                              "Skyrim still chooses the image. Startup loading is unchanged.\n"
                              "Apply takes effect on the next eligible transition.");
        }
        ImGui::Checkbox("Lab mode for this session", &showDeveloperControls);
        ImGui::SameLine();
        ImGui::TextDisabled("Shows experiments and destructive renderer diagnostics.");
        if (ImGui::BeginTabBar("##developerSections", ImGuiTabBarFlags_None))
        {
            DrawUIStatusPanel(advancedCardHeight);

            DrawPerformancePanel(advancedCardHeight, view.sourceNeural);

            DrawRuntimePanel(advancedCardHeight, view);
            ImGui::EndTabBar();
        }
        ImGui::EndTabItem();
    }
}

void OverlayUI::DrawUIStatusPanel(float advancedCardHeight)
{
    auto* upscaler = RenderPipeline::GetSingleton();
    auto* nvidiaHost = NvidiaHost::GetSingleton();
    if (ImGui::BeginTabItem("UI status"))
    {
        ImGui::BeginChild("##compatibilityCard", ImVec2(0.0f, advancedCardHeight), true);
        ImGui::TextUnformatted("UI COMPOSITION");
        ImGui::Separator();
        if (showDeveloperControls && !TheosRenderPipeline::CommunityShaders::Active())
        {
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
            DrawStatusLabel(TheosRenderPipeline::CommunityShaders::Active() ? "COMMUNITY SHADERS" : upscaler->mNativeUI ? "NATIVE COMPOSITION" : "RENDER-SPACE UI",
                            upscaler->mNativeUI ? UIHealth::kHealthy : UIHealth::kIdle);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("External ImGui overlays");
            ImGui::TableNextColumn();
            DrawStatusLabel(TheosRenderPipeline::CommunityShaders::Active() ? "COMMUNITY SHADERS UI" : nvidiaHost->StartupConfigured() ? "NATIVE HOST / STARTUP FOREGROUND" : "HOST UNAVAILABLE",
                            nvidiaHost->StartupConfigured() ? UIHealth::kHealthy : UIHealth::kIdle);
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextWrapped(TheosRenderPipeline::CommunityShaders::Active() ?
                           "Community Shaders owns the UI render targets. External menus retain their original draw paths." :
                           "Supported startup overlays use a native-resolution foreground target. "
                           "Their original render state is restored after each draw.");
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
}

void OverlayUI::DrawRuntimePanel(float advancedCardHeight, const FrameView& view)
{
    auto* upscaler = RenderPipeline::GetSingleton();
    auto* backend = DLSSBackend::GetSingleton();
    if (ImGui::BeginTabItem("Runtime"))
    {
        ImGui::BeginChild("##diagnosticsCard", ImVec2(0.0f, advancedCardHeight), true);
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
        if (!TheosRenderPipeline::CommunityShaders::Active()) {
            ImGui::Text("Jitter: (%.4f, %.4f) | phases: %d", upscaler->mJitterOffsets[0], upscaler->mJitterOffsets[1],
                        backend->GetJitterPhaseCount());
            ImGui::Text("Mip LOD bias: %.3f", upscaler->mMipLodBias);
            ImGui::Text("NGX evals ok/failed: %llu / %llu | last 0x%08X",
                        static_cast<unsigned long long>(backend->EvalSuccessCount()),
                        static_cast<unsigned long long>(backend->EvalFailCount()), backend->LastEvalResult());
            ImGui::Text("Feature: %s | formats in/out: %d / %d | HDR: %s", backend->HasFeature() ? "created" : "none",
                        backend->InputFormat(), backend->OutputFormat(), backend->IsHDRInput() ? "yes" : "no");
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
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
}
