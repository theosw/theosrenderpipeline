#include "OverlayUI.h"
#include "OverlayUIStyle.h"

#include <PCH.h>
#include "FrameGen/SourceFrameGeneration.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"

using namespace TheosRenderPipeline::Overlay;

#include "RenderPipeline.h"

namespace
{
	void DrawNRReconstructionControls(TheosRenderPipeline::NeuralRendering::Reconstruction& value)
	{
		const char* presets[]{ "Default", "Shipping" };
		ImGui::Combo("NR network preset", &value.preset, presets, IM_ARRAYSIZE(presets));
		const char* methods[]{ "Auto | Direct at 100%, residual below", "Residual | Add NR changes to native scene", "Ratio | Transfer lighting and colour" };
		int method = static_cast<int>(value.method);
		if (ImGui::Combo("Reconstruction", &method, methods, IM_ARRAYSIZE(methods))) {
			value.method = static_cast<TheosRenderPipeline::NeuralRendering::ResolveMethod>(method);
		}
		float percent = value.inputScale * 100;
		if (ImGui::SliderFloat("NR input resolution", &percent, 25, 100, "%.1f%%")) { value.inputScale = percent / 100; }
		ImGui::TextWrapped("Relative to the selected stage: render resolution before DLSS, output resolution after DLSS. Lower values reduce NR's working resolution; game and UI sizes stay unchanged.");
		ImGui::TextWrapped("Residual clamps colour to 0..1 like the reference. Use Ratio for linear HDR input.");
		if (ImGui::TreeNode("Advanced reconstruction")) {
			ImGui::BeginDisabled(value.method != TheosRenderPipeline::NeuralRendering::ResolveMethod::Ratio);
			ImGui::SliderFloat("Ratio effect strength", &value.transferStrength, 0, 2);
			ImGui::SliderFloat("Ratio colour strength", &value.colourStrength, 0, 2);
			ImGui::SliderFloat("Maximum luma ratio", &value.maxRatio, 0.01f, 16);
			ImGui::InputFloat("HDR encode white point", &value.whitePoint, 0, 0, "%.4f");
			ImGui::Checkbox("Input colour is linear HDR", &value.colorIsHDR);
			ImGui::EndDisabled();
			ImGui::TextWrapped("HDR input is used by Ratio reconstruction; it does not change the display HDR mode. Preset or resolution changes retire and recreate NR and may stall briefly. Ctrl-click sliders to type.");
			ImGui::TreePop();
		}
	}

	void DrawSourceNeuralControls(TheosRenderPipeline::SourceDLSSG::Preferences& draft, int upscaleType)
	{
		auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
		const auto state = backend.NeuralState();
		const auto* frameGen = SourceFrameGeneration::GetSingleton();
		ImGui::TextUnformatted("Output mode");
		const char* unavailableReason = nullptr;
		if (frameGen->settings.neuralRenderingRuntimePath.empty()) {
			unavailableReason = "NR runtime path is empty. Configure NeuralRenderingRuntimePath in TheosRenderPipeline.ini and restart Skyrim.";
		} else if (!backend.Ready()) {
			unavailableReason = "The NVIDIA host is not ready. Check runtime status and restart Skyrim.";
		} else if (state.failed) {
			unavailableReason = "NR failed. Open NR runtime details below for the reported error.";
		} else if (upscaleType != DLSS) {
			unavailableReason = "NR currently requires DLSS mode. Select DLSS in startup settings and restart Skyrim.";
		} else if (!NvidiaHost::GetSingleton()->DedicatedUITextureMode()) {
			unavailableReason = "NR requires dedicated native UI composition. Enable Native UI in startup settings and restart Skyrim.";
		}
		const bool unavailable = unavailableReason != nullptr;
		ImGui::BeginDisabled(unavailable);
		ImGui::Checkbox("Neural Rendering##sourceNR", &draft.neuralEnabled);
		int placement = draft.neuralBeforeUpscaling ? 0 : 1;
		const char* placements[]{ "Before DLSS", "After DLSS" };
		if (ImGui::Combo("Placement##sourceNR", &placement, placements, IM_ARRAYSIZE(placements))) {
			draft.neuralBeforeUpscaling = placement == 0;
		}
		if (draft.neuralBeforeUpscaling) { ImGui::TextWrapped("NR processes the world before DLSS. UI correction is unused; native UI is added later."); }
		int passChoice = draft.neuralPasses - 1;
		const char* passes[]{ "One", "Two" };
		if (ImGui::Combo("Passes##sourceNR", &passChoice, passes, IM_ARRAYSIZE(passes))) { draft.neuralPasses = passChoice + 1; }
		if (draft.neuralPasses == 2) { ImGui::TextWrapped("The second pass processes the first result with separate history. It adds GPU time and memory; stronger processing may also amplify artifacts."); }
		DrawNRReconstructionControls(draft.neuralReconstruction);
		if (ImGui::TreeNode("Image tuning##sourceNR")) {
			const char* styles[]{ "Style 0", "Style 1", "Style 2", "Style 3", "Style 4", "Style 5", "Style 6", "Style 7" };
			ImGui::Combo("Style##sourceNR", &draft.neuralTuning.style, styles, IM_ARRAYSIZE(styles));
			ImGui::SliderFloat("Intensity##sourceNR", &draft.neuralTuning.intensity, 0, 2);
			ImGui::SliderFloat("Local tone##sourceNR", &draft.neuralTuning.localToneStrength, 0, 2);
			ImGui::SliderFloat("Local structure##sourceNR", &draft.neuralTuning.localStructureStrength, 0, 2);
			ImGui::SliderFloat("Skin structure##sourceNR", &draft.neuralTuning.skinStructureStrength, -1, 2);
			ImGui::Checkbox("Automatic skin mask##sourceNR", &draft.neuralTuning.useAutoSkinMask);
			ImGui::BeginDisabled(draft.neuralBeforeUpscaling);
			ImGui::Checkbox("UI correction##sourceNR", &draft.neuralTuning.uiCorrection);
			ImGui::EndDisabled();
			ImGui::TextDisabled("Skin -1 follows local structure. Ctrl-click a slider to type.");
			ImGui::TreePop();
		}
		ImGui::EndDisabled();
		ImGui::TextColored(state.failed ? kRust : state.active ? kSage : kMuted, "%s",
			state.failed ? "NR failed" : state.active ? "NR active" : draft.neuralEnabled ? "NR waiting for a world frame" : "Standard DLSS active");
		if (unavailable) {
			ImGui::TextWrapped("%s", unavailableReason);
		}
		if (ImGui::CollapsingHeader("NR runtime details##sourceNR")) {
			ImGui::TextWrapped("%s", state.status.c_str());
			ImGui::Text("Submitted frames %llu | history resets %llu",
				static_cast<unsigned long long>(state.evaluations), static_cast<unsigned long long>(state.resets));
			ImGui::TextWrapped("NR feeds both real and generated scene frames. Native UI is added afterwards. Frame generation can be off while NR remains on.");
		}
	}
}

void OverlayUI::DrawNeuralRenderingPanel(float tabCardHeight)
{
	if (ImGui::BeginTabItem("Neural Rendering", nullptr, requestedPage == SettingsPage::NeuralRendering ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
		ImGui::BeginChild("##neuralRenderingPage", ImVec2(0.0f, tabCardHeight), false);
		DrawSourceNeuralControls(settingsDraft.sourceDLSSG, settingsDraft.upscaleType);
		ImGui::EndChild();
		ImGui::EndTabItem();
	}
}
