#include "OverlayUI.h"
#include "Localization.h"
#include "OverlayUIStyle.h"
#include "OverlayFrameView.h"
#include <cstdio>
#include <PCH.h>

using namespace TheosRenderPipeline::Overlay;

bool OverlayUI::BeginSettingsColumns(const char* id, float height, const FrameView& view)
{
    ImGui::PushID(id);
    const auto columns = DrawColumnSplitter(ImGui::GetContentRegionAvail().x, height, layout.leftFraction, UIScale());
    ImGui::BeginChild("##left", ImVec2(columns.left, height), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawFrameMeasurements(view, height);
    ImGui::BeginChild("##details", ImVec2(0, 0), false);
    ImGui::PushTextWrapPos(0);
    return true;
}

void OverlayUI::NextSettingsColumn(float height)
{
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::SameLine(0, Px(ColumnGap));
    ImGui::BeginChild("##right", ImVec2(0, height), false);
    ImGui::PushTextWrapPos(0);
    ImGui::PushItemWidth(-Px(180.0f));
}

void OverlayUI::EndSettingsColumns()
{
    ImGui::PopItemWidth();
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopID();
}

void OverlayUI::DrawFrameMeasurements(const FrameView& view, float columnHeight)
{
    if (ImGui::BeginTable("##rates", 2, ImGuiTableFlags_SizingStretchSame))
    {
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", TR("Raster FPS"));
        ImGui::Text("%.1f", renderedFps);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", view.outputLabel);
        ImGui::TextUnformatted(view.outputText.c_str());
        DrawSettingsHelp(
            TR("NVIDIA's presentation count; physical screen refreshes and scanout spacing are not measured."));
        ImGui::EndTable();
    }
    ImGui::TextDisabled(TR("Frame time: %.2f ms"), view.avgMs);
    DrawSettingsHelp(TR("Game-facing Present cadence, before generated frames. This is not GPU execution time."));
    float graphMaxMs = 50.0f;
    for (int i = 0; i < frameTimeCount; ++i)
        graphMaxMs = (std::max)(graphMaxMs, frameTimesMs[i] * 1.1f);
    char graphScale[32]{};
    std::snprintf(graphScale, sizeof(graphScale), TR("0-%.0f ms"), graphMaxMs);
    ImGui::PlotLines("##frameTimes", frameTimesMs, frameTimeCount, frameTimeIndex, graphScale, 0, graphMaxMs,
                     ImVec2(-1, GraphHeight(columnHeight, UIScale())));
    ImGui::Spacing();
}

namespace
{
// A timing for the menu: the shared formatter's value with its unit.
std::string MenuMilliseconds(float value, bool available)
{
    auto text = TheosRenderPipeline::Telemetry::Milliseconds(value, available);
    // The shared formatter is English (logs use it too); translate for display.
    return text == "unavailable" ? std::string(TR("unavailable")) : text + " ms";
}
}

void OverlayUI::DrawStageMeasurements(const FrameView& view, SettingsPage page)
{
    const auto& timings = view.timings;
    using Stage12 = PerformanceTuning::D3D12Stage;
    // D3D12 passes appear only while they run: NR placement and the frame
    // generation provider decide which lists exist.
    auto row12 = [&](const char* name, Stage12 stage, const char* help = nullptr) {
        const auto i = static_cast<std::size_t>(stage);
        if (!view.timingEnabled || !timings.d3d12Available[i]) { return false; }
        DrawSettingsValue(name, MenuMilliseconds(timings.d3d12Ms[i], true).c_str());
        if (help) { DrawSettingsHelp(help); }
        return true;
    };
    if (page == SettingsPage::NeuralRendering)
    {
        row12(TR("Preparation"), Stage12::kNeuralPrepare, TR("Colour, guide and UI preparation before inference."));
        row12(TR("Reconstruction + UI"), Stage12::kNeuralFinish,
              TR("Pass 2 restoration, colour reconstruction, native UI composition and the HUD-less copy."));
        using Stage11 = PerformanceTuning::D3D11Stage;
        const auto roundTrip = static_cast<std::size_t>(Stage11::kNeuralEarlyRoundTrip);
        const auto earlyList = static_cast<std::size_t>(Stage12::kEarlyNeuralList);
        if (view.timingEnabled && timings.d3d11Available[roundTrip] && timings.d3d12Available[earlyList])
        {
            const auto handoff = (std::max)(0.0f, timings.d3d11Ms[roundTrip] - timings.d3d12Ms[earlyList]);
            DrawSettingsValue(TR("D3D11/D3D12 handoff"),
                              MenuMilliseconds(handoff, true).c_str());
            DrawSettingsHelp(TR("Before-DLSS NR only: the input and result copies plus the waits between the two APIs."));
        }
        return;
    }
    if (view.timingEnabled && view.timingQuarantined)
    {
        ImGui::TextDisabled("%s", TR("GPU timings unavailable (query failure)"));
        DrawSettingsHelp(TR("GPU timing queries failed. Restarting the game retries timings; rendering continues."));
        return;
    }
    if (!view.timingEnabled || !timings.lastCompletedGeneration)
    {
        ImGui::TextDisabled("%s", view.timingEnabled ? TR("GPU timings: waiting") : TR("GPU timings: off"));
        DrawSettingsHelp(TR("Enable stage timings in Advanced, then Apply."));
        return;
    }
    using Stage = PerformanceTuning::D3D11Stage;
    auto row = [&](const char* name, Stage stage) {
        const auto i = static_cast<std::size_t>(stage);
        if (timings.d3d11ScopeInvalid[i]) { DrawSettingsValue(name, TR("invalid scope")); return; }
        DrawSettingsValue(
            name, MenuMilliseconds(timings.d3d11Ms[i], timings.d3d11Available[i]).c_str());
    };
    if (page == SettingsPage::Image)
    {
        if (!view.communityShaders)
        {
            row(view.xessActive ? TR("XeSS + transfers/sharpening") : "DLSS", Stage::kDLSS);
            row(TR("Sharpening"), Stage::kRCAS);
        }

        row(TR("D3D11 frame"), Stage::kFrame);
        row(TR("Input copy"), Stage::kInputColorCopy);
        row(TR("Mask"), Stage::kMaskEncode);
        row(TR("Upscaler output copy"), Stage::kOutputCopy);
        row(TR("Presentation copy"), Stage::kPresentationCopy);
        row12(TR("Output (D3D12)"), Stage12::kOutput, TR("Copy or HDR encoding into the swapchain."));
        row12(TR("Present work (D3D12)"), Stage12::kOutputList,
              TR("Everything TRP records before Present: after-DLSS NR, output and XeFG input copies."));
        const auto& c = timings.gameFrameCadence;
        const auto& g = timings.d3d11Frame;
        if (c.samples == 0)
        {
            ImGui::TextDisabled("%s", TR("Collecting percentile window"));
            return;
        }
        ImGui::TextWrapped(TR("Raster ms: p50 %.2f | p95 %.2f | p99 %.2f"), c.p50Ms, c.p95Ms, c.p99Ms);
        ImGui::TextWrapped(TR("GPU ms: p50 %.2f | p95 %.2f | p99 %.2f"), g.p50Ms, g.p95Ms, g.p99Ms);
        auto fps = [](float ms) { return ms > 0 ? 1000.0f / ms : 0.0f; };
        ImGui::TextWrapped(TR("FPS thresholds: p50 %.1f | 5%% %.1f | 1%% %.1f"), fps(c.p50Ms), fps(c.p95Ms), fps(c.p99Ms));
        DrawSettingsHelp(TR("Rolling raster thresholds, not slow-frame averages or generated-frame cadence."));
        ImGui::TextDisabled(TR("%u / 1024 frames; %llu GPU samples"), c.samples,
                            static_cast<unsigned long long>(timings.d3d11Samples));
    }
    else if (page == SettingsPage::FrameGeneration)
    {
        row(TR("FG inputs"), Stage::kFrameGenInputs);
        row(TR("HUD-less copy"), Stage::kHUDLessCopy);
        row12(TR("Input tags (D3D12)"), Stage12::kFrameGenTagList, TR("DLSS-G's copies of the tagged inputs."));
        row12(TR("XeFG input copies"), Stage12::kXeFGInputs);
        if (row12(TR("Frame generation"), Stage12::kFrameGenerationHold,
                  TR("GPU time from the end of TRP's output to DLSS-G releasing this frame's inputs. It covers generating "
                     "this frame's generated frames; other GPU work running at the same time can add to it.")))
        {
            const auto& hold = timings.frameGenerationHold;
            ImGui::TextWrapped(TR("Frame generation ms: p50 %.2f | p95 %.2f | p99 %.2f"), hold.p50Ms, hold.p95Ms,
                               hold.p99Ms);
            ImGui::TextDisabled(TR("%llu measured, %llu upper bounds only"), static_cast<unsigned long long>(timings.holdExact),
                                static_cast<unsigned long long>(timings.holdBounded));
            DrawSettingsHelp(TR("An upper bound is a frame where DLSS-G had already finished before TRP could measure it."));
        }
        const auto& present = timings.sourcePresentCpu;
        if (present.samples)
        {
            ImGui::TextWrapped(TR("CPU Present ms: p50 %.2f | p95 %.2f | p99 %.2f"), present.p50Ms, present.p95Ms,
                               present.p99Ms);
            DrawSettingsHelp(TR("CPU Present includes waits. Physical display cadence is not measured."));
        }
    }
    else if (page == SettingsPage::Advanced)
    {
        row(TR("UI composition"), Stage::kNativeUIComposition);
        row(TR("Startup overlays"), Stage::kStartupOverlayComposition);
    }
}

void OverlayUI::DrawOutputOptimizations()
{
    ImGui::Separator();
    ImGui::Checkbox(TR("Direct RCAS output"), &settingsDraft.directRCASOutput);
    DrawSettingsHelp(TR("On by default. Writes sharpened output directly, avoiding its final copy. Apply required."));
    ImGui::Checkbox(TR("Direct DLSS output"), &settingsDraft.directDLSSOutput);
    DrawSettingsHelp(TR("On by default. Avoids the DLSS copy when sharpening is off; does not disable sharpening. Apply required."));
}

void OverlayUI::DrawMeasurementControls(const FrameView& view)
{
    ImGui::Checkbox(TR("Stage timings"), &settingsDraft.enableGPUTimings);
    DrawSettingsHelp(TR("Non-blocking GPU/CPU measurements; Apply required."));
    ImGui::Checkbox(TR("Record frame trace"), &settingsDraft.enableFrameTrace);
    DrawSettingsHelp(TR("Writes a .sfgtrace sidecar on a background thread; Apply required."));
    const auto& trace = view.trace;
    if (trace.enabled || trace.written || trace.dropped || trace.writerFailed)
    {
        ImGui::TextWrapped(TR("Trace: %s | written %llu | dropped %llu"),
                           trace.writerFailed ? TR("FAILED")
                           : trace.enabled    ? TR("recording")
                                              : TR("stopped"),
                           static_cast<unsigned long long>(trace.written),
                           static_cast<unsigned long long>(trace.dropped));
        ImGui::TextWrapped("%s", trace.path.c_str());
    }
    if (showDeveloperControls)
    {
        if (ImGui::Button(TR("Reset session fallbacks")))
        {
            ResetSessionFallbacks();
            actionMessage = TR("Performance fallback latches reset.");
            actionMessageIsError = false;
        }
        if (ImGui::Button(TR("Clear timing window")))
        {
            ClearTimingWindow();
            actionMessage = TR("Rolling timing window cleared.");
            actionMessageIsError = false;
        }
    }
}
