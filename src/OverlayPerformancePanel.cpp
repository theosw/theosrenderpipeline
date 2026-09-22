#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "OverlayFrameView.h"
#include "PerformanceTuning.h"
#include "FrameTrace.h"
#include "CommunityShaderIntegration.h"
#include <PCH.h>

using namespace TheosRenderPipeline::Overlay;

bool OverlayUI::BeginSettingsColumns(const char* id, float height, const FrameView& view)
{
    ImGui::PushID(id);
    if (!ImGui::BeginTable("##columns", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV))
    {
        ImGui::PopID();
        return false;
    }
    ImGui::TableSetupColumn("##readings", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("##controls", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableNextColumn();
    ImGui::BeginChild("##left", ImVec2(0, height), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawFrameMeasurements(view);
    ImGui::BeginChild("##details", ImVec2(0, 0), false);
    ImGui::PushTextWrapPos(0);
    return true;
}

void OverlayUI::NextSettingsColumn(float height)
{
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::TableNextColumn();
    ImGui::BeginChild("##right", ImVec2(0, height), false);
    ImGui::PushTextWrapPos(0);
    ImGui::PushItemWidth(-180.0f);
}

void OverlayUI::EndSettingsColumns()
{
    ImGui::PopItemWidth();
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::EndTable();
    ImGui::PopID();
}

void OverlayUI::DrawFrameMeasurements(const FrameView& view)
{
    if (ImGui::BeginTable("##rates", 2, ImGuiTableFlags_SizingStretchSame))
    {
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Raster FPS");
        ImGui::Text("%.1f", renderedFps);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", view.outputLabel);
        ImGui::TextUnformatted(view.outputText.c_str());
        DrawSettingsHelp(
            "NVIDIA's presentation count; physical screen refreshes and scanout spacing are not measured.");
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Frame time: %.2f ms", view.avgMs);
    DrawSettingsHelp("Game-facing Present cadence, before generated frames. This is not GPU execution time.");
    ImGui::PlotLines("##frameTimes", frameTimesMs, frameTimeCount, frameTimeIndex, nullptr, 0, 50, ImVec2(-1, 65));
    ImGui::Spacing();
}

void OverlayUI::DrawStageMeasurements(SettingsPage page)
{
    const auto* performance = PerformanceTuning::GetSingleton();
    const auto& timings = performance->GetTimingSnapshot();
    if (!performance->TimingEnabled() || !timings.d3d11Samples)
    {
        ImGui::TextDisabled("%s", performance->TimingEnabled() ? "GPU timings: waiting" : "GPU timings: off");
        DrawSettingsHelp("Enable stage timings in Advanced, then Apply.");
        return;
    }
    using Stage = PerformanceTuning::D3D11Stage;
    auto row = [&](const char* name, Stage stage) {
        const auto i = static_cast<std::size_t>(stage);
        DrawSettingsValue(
            name, TheosRenderPipeline::Telemetry::Milliseconds(timings.d3d11Ms[i], timings.d3d11Available[i]).c_str());
    };
    if (page == SettingsPage::Image)
    {
        if (!TheosRenderPipeline::CommunityShaders::Active())
        {
            row("DLSS", Stage::kDLSS);
            row("Sharpening", Stage::kRCAS);
        }

        row("D3D11 frame", Stage::kFrame);
        row("Input copy", Stage::kInputColorCopy);
        row("Mask", Stage::kMaskEncode);
        row("Output copy", Stage::kOutputCopy);
        const auto& c = timings.gameFrameCadence;
        const auto& g = timings.d3d11Frame;
        if (c.samples == 0)
        {
            ImGui::TextDisabled("Collecting percentile window");
            return;
        }
        ImGui::TextWrapped("Raster ms: p50 %.2f | p95 %.2f | p99 %.2f", c.p50Ms, c.p95Ms, c.p99Ms);
        ImGui::TextWrapped("GPU ms: p50 %.2f | p95 %.2f | p99 %.2f", g.p50Ms, g.p95Ms, g.p99Ms);
        auto fps = [](float ms) { return ms > 0 ? 1000.0f / ms : 0.0f; };
        ImGui::TextWrapped("FPS thresholds: p50 %.1f | 5%% %.1f | 1%% %.1f", fps(c.p50Ms), fps(c.p95Ms), fps(c.p99Ms));
        DrawSettingsHelp("Rolling raster thresholds, not slow-frame averages or generated-frame cadence.");
        ImGui::TextDisabled("%u / 1024 frames; %llu GPU samples", c.samples,
                            static_cast<unsigned long long>(timings.d3d11Samples));
    }
    else if (page == SettingsPage::FrameGeneration)
    {
        row("FG inputs", Stage::kFrameGenInputs);
        row("HUD-less copy", Stage::kHUDLessCopy);
        const auto& present = timings.sourcePresentCpu;
        if (present.samples)
        {
            ImGui::TextWrapped("CPU Present ms: p50 %.2f | p95 %.2f | p99 %.2f", present.p50Ms, present.p95Ms,
                               present.p99Ms);
            DrawSettingsHelp(
                "CPU Present includes waits. NVIDIA generation GPU cost and physical cadence are not measured.");
        }
    }
    else if (page == SettingsPage::Advanced)
    {
        row("UI composition", Stage::kNativeUIComposition);
        row("Startup overlays", Stage::kStartupOverlayComposition);
    }
}

void OverlayUI::DrawOutputOptimizations()
{
    ImGui::Separator();
    ImGui::Checkbox("Direct RCAS output", &settingsDraft.directRCASOutput);
    DrawSettingsHelp("Writes sharpened output directly, avoiding its final copy. Apply required.");
    ImGui::Checkbox("Direct DLSS output", &settingsDraft.directDLSSOutput);
    DrawSettingsHelp("Avoids the DLSS copy when sharpening is off; does not disable sharpening. Apply required.");
}

void OverlayUI::DrawMeasurementControls()
{
    ImGui::Checkbox("Stage timings", &settingsDraft.enableGPUTimings);
    DrawSettingsHelp("Non-blocking GPU/CPU measurements; Apply required.");
    ImGui::Checkbox("Record frame trace", &settingsDraft.enableFrameTrace);
    DrawSettingsHelp("Writes a .sfgtrace sidecar on a background thread; Apply required.");
    const auto trace = FrameTrace::GetSingleton()->GetStatus();
    if (trace.enabled || trace.written || trace.dropped || trace.writerFailed)
    {
        ImGui::TextWrapped("Trace: %s | written %llu | dropped %llu",
                           trace.writerFailed ? "FAILED"
                           : trace.enabled    ? "recording"
                                              : "stopped",
                           static_cast<unsigned long long>(trace.written),
                           static_cast<unsigned long long>(trace.dropped));
        ImGui::TextWrapped("%s", trace.path.c_str());
    }
    if (showDeveloperControls)
    {
        auto* performance = PerformanceTuning::GetSingleton();
        if (ImGui::Button("Reset session fallbacks"))
        {
            performance->ResetSessionFallbacks();
            actionMessage = "Performance fallback latches reset.";
            actionMessageIsError = false;
        }
        if (ImGui::Button("Clear timing window"))
        {
            performance->ResetTimingWindow();
            actionMessage = "Rolling timing window cleared.";
            actionMessageIsError = false;
        }
    }
}
