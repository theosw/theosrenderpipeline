#include "OverlayUI.h"
#include "OverlayUIStyle.h"

#include <PCH.h>
#include "FrameGen/SourceDLSSGBackend.h"

using namespace TheosRenderPipeline::Overlay;

#include "PerformanceTuning.h"
#include "FrameTrace.h"

void OverlayUI::DrawPerformancePanel(float advancedCardHeight, const TheosRenderPipeline::SourceDLSSG::NeuralSnapshot& sourceNeural)
{
	auto* performance = PerformanceTuning::GetSingleton();
	if (showDeveloperControls && ImGui::BeginTabItem("Experiments")) {
		if (ImGui::BeginTable("##performanceLayout", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
			ImGui::TableNextColumn();
			ImGui::BeginChild("##performanceControls", ImVec2(0.0f, advancedCardHeight), true);
			ImGui::TextUnformatted("MODULAR EXPERIMENTS");
			ImGui::Separator();
			ImGui::Checkbox("GPU/CPU stage timings", &settingsDraft.enableGPUTimings);
			ImGui::TextDisabled("Measurement only; uses non-blocking query rings.");
			ImGui::Checkbox("Record offline frame trace", &settingsDraft.enableFrameTrace);
			ImGui::TextDisabled("Writes a frame-aligned .sfgtrace sidecar on a background thread.");
			ImGui::Checkbox("Direct RCAS output", &settingsDraft.directRCASOutput);
			ImGui::TextDisabled("Skips the native post-sharpen copy.");
			ImGui::Checkbox("Direct DLSS output", &settingsDraft.directDLSSOutput);
			ImGui::TextDisabled("Skips the native output copy when sharpening is off.");
			ImGui::Spacing();
			ImGui::TextWrapped("All routes validate resource contracts independently. NGX failures retry through the original intermediate path and latch only that experiment off for the session.");
			if (ImGui::Button("RESET SESSION FALLBACKS", ImVec2(-1.0f, 0.0f))) {
				performance->ResetSessionFallbacks();
				actionMessage = "Performance fallback latches reset.";
				actionMessageIsError = false;
			}
			if (ImGui::Button("CLEAR TIMING WINDOW", ImVec2(-1.0f, 0.0f))) {
				performance->ResetTimingWindow();
				actionMessage = "Rolling timing window cleared.";
				actionMessageIsError = false;
			}
			ImGui::EndChild();

			ImGui::TableNextColumn();
			ImGui::BeginChild("##performanceStatus", ImVec2(0.0f, advancedCardHeight), true);
			ImGui::TextUnformatted("ROUTE STATUS");
			ImGui::Separator();
			auto drawRouteStatus = [&](const char* a_name, PerformanceTuning::Optimization a_optimization) {
				const auto& status = performance->GetRouteStatus(a_optimization);
				const UIHealth health = status.sessionRejected ? UIHealth::kError :
					status.activeLastFrame ? UIHealth::kHealthy :
					status.requested ? UIHealth::kWarning : UIHealth::kIdle;
				ImGui::TextUnformatted(a_name);
				ImGui::SameLine(180.0f);
				DrawStatusLabel(
					status.sessionRejected ? "FALLBACK LATCHED" :
					status.activeLastFrame ? "ACTIVE" :
					status.requested ? "ARMED" : "OFF",
					health);
				ImGui::TextDisabled("%s | frames %llu | fallbacks %llu",
					status.reason.c_str(),
					static_cast<unsigned long long>(status.activeFrames),
					static_cast<unsigned long long>(status.fallbackCount));
			};
			drawRouteStatus("RCAS output", PerformanceTuning::Optimization::kDirectRCASOutput);
			drawRouteStatus("DLSS output", PerformanceTuning::Optimization::kDirectDLSSOutput);

			ImGui::Spacing();
			ImGui::SeparatorText("TIMINGS");
			const auto& timings = performance->GetTimingSnapshot();

#if !defined(TRP_NO_NEURAL_RENDERING)
				ImGui::TextWrapped("NVIDIA generation GPU cost is unavailable. NR timing covers its evaluation sequence, including inter-pass work. It excludes input downsampling, final resolve/copies, UI composition and the D3D11/D3D12 handoff.");
				const auto& nrTiming = sourceNeural.telemetry;
				if (nrTiming.gpuSamples) {
					ImGui::Text("NR inference GPU avg %.3f / max %.3f ms (%llu samples, %llu query failures; feature %s)",
						nrTiming.AverageGPUMicroseconds() / 1000.0, nrTiming.MaximumGPUMicroseconds() / 1000.0,
						nrTiming.gpuSamples, nrTiming.gpuQueryFailures, sourceNeural.active ? "active" : "inactive");
				} else {
					ImGui::TextDisabled("NR inference GPU: no retired samples for this feature.");
				}
#else
				(void)sourceNeural;
				ImGui::TextWrapped("NVIDIA generation GPU cost is unavailable.");
#endif

			const auto traceStatus = FrameTrace::GetSingleton()->GetStatus();

			if (traceStatus.enabled || traceStatus.written > 0 || traceStatus.dropped > 0) {
				ImGui::Text("Trace: %s | written %llu | dropped %llu",
					traceStatus.writerFailed ? "WRITE FAILED" : traceStatus.enabled ? "RECORDING" : "stopped",
					static_cast<unsigned long long>(traceStatus.written),
					static_cast<unsigned long long>(traceStatus.dropped));
				if (!traceStatus.path.empty()) {
					ImGui::TextDisabled("%s", traceStatus.path.c_str());
				}
			}
			if (!performance->TimingEnabled() || timings.d3d11Samples == 0) {
				ImGui::TextDisabled(performance->TimingEnabled() ? "Waiting for GPU query results..." : "Enable timings or tracing and apply for session.");
			} else {
				const auto& d11 = timings.d3d11Ms;
				auto d11ms = [&](PerformanceTuning::D3D11Stage a_stage) {
					const auto i = static_cast<std::size_t>(a_stage);
					return TheosRenderPipeline::Telemetry::Milliseconds(d11[i], timings.d3d11Available[i]);
				};
				ImGui::Text("D3D11 frame %s | DLSS %s | RCAS %s", d11ms(PerformanceTuning::D3D11Stage::kFrame).c_str(), d11ms(PerformanceTuning::D3D11Stage::kDLSS).c_str(), d11ms(PerformanceTuning::D3D11Stage::kRCAS).c_str());
				ImGui::Text("Input %s | mask %s | output %s", d11ms(PerformanceTuning::D3D11Stage::kInputColorCopy).c_str(), d11ms(PerformanceTuning::D3D11Stage::kMaskEncode).c_str(), d11ms(PerformanceTuning::D3D11Stage::kOutputCopy).c_str());
				ImGui::Text("FG inputs %s | HUD-less %s", d11ms(PerformanceTuning::D3D11Stage::kFrameGenInputs).c_str(), d11ms(PerformanceTuning::D3D11Stage::kHUDLessCopy).c_str());
				ImGui::Text("Native UI composition %s | startup foreground %s", d11ms(PerformanceTuning::D3D11Stage::kNativeUIComposition).c_str(), d11ms(PerformanceTuning::D3D11Stage::kStartupOverlayComposition).c_str());
				const auto& sourcePresent = timings.sourcePresentCpu;
				if (sourcePresent.samples) {
					ImGui::Text("Source CPU Present ms p50 %.2f | p95 %.2f | p99 %.2f (%u samples)", sourcePresent.p50Ms, sourcePresent.p95Ms, sourcePresent.p99Ms, sourcePresent.samples);
				}
				ImGui::TextDisabled("D3D11 ends before Present. CPU Present includes waits; neither measures physical scanout or NVIDIA generation GPU cost.");

				ImGui::SeparatorText("RASTER FRAME-TIME PERCENTILES");
				const auto& cadence = timings.gameFrameCadence;
				const auto& gpuFrame = timings.d3d11Frame;
				if (cadence.samples == 0) {
					ImGui::TextDisabled("Collecting percentile window...");
				} else {
					auto fpsFromMs = [](float a_ms) { return a_ms > 0.0f ? 1000.0f / a_ms : 0.0f; };
					ImGui::Text("Raster cadence ms  p50 %.2f | p95 %.2f | p99 %.2f", cadence.p50Ms, cadence.p95Ms, cadence.p99Ms);
					ImGui::Text("FPS threshold   p50 %.1f | 5%% %.1f | 1%% %.1f", fpsFromMs(cadence.p50Ms), fpsFromMs(cadence.p95Ms), fpsFromMs(cadence.p99Ms));
					ImGui::Text("D3D11 GPU ms    p50 %.2f | p95 %.2f | p99 %.2f", gpuFrame.p50Ms, gpuFrame.p95Ms, gpuFrame.p99Ms);

					ImGui::TextDisabled("Rolling %u/1024 frames. P95/P99 are thresholds exceeded by the slowest 5%%/1%%; they are not slow-frame averages.", cadence.samples);
				}
				ImGui::TextDisabled("D3D11 samples: %llu.",
					static_cast<unsigned long long>(timings.d3d11Samples));
				ImGui::TextWrapped("These are raster timings, not generated-frame cadence or end-to-end latency.");
			}
			ImGui::EndChild();
			ImGui::EndTable();
		}
		ImGui::EndTabItem();
	}
}
