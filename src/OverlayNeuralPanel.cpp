#include "OverlayUI.h"
#include "OverlayUIStyle.h"

#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"
#include "FrameGen/SourceFrameGeneration.h"
#include <PCH.h>

using namespace TheosRenderPipeline::Overlay;

#include "CommunityShaderIntegration.h"
#include "NeuralRenderingMode.h"
#include "RenderPipeline.h"

namespace
{
void DrawNRReconstructionControls(TheosRenderPipeline::NeuralRendering::Reconstruction& value, bool producerColor)
{
    if (!ImGui::CollapsingHeader("Advanced reconstruction"))
    {
        return;
    }
    const char* presets[]{"Default", "Shipping"};
    ImGui::Combo("Network preset", &value.preset, presets, IM_ARRAYSIZE(presets));
    DrawSettingsHelp("NR network selection, separate from DLSS model presets.");
    const char* methods[]{"Auto | Reconstruct reduced input", "Residual | Add NR changes",
                          "Ratio | Transfer lighting and colour"};
    int method = static_cast<int>(value.method);
    ImGui::BeginDisabled(producerColor);
    if (ImGui::Combo("Reconstruction", &method, methods, IM_ARRAYSIZE(methods)))
    {
        value.method = static_cast<TheosRenderPipeline::NeuralRendering::ResolveMethod>(method);
    }
    ImGui::EndDisabled();
    if (producerColor)
    {
        DrawSettingsHelp("CS restores scene colour automatically before DLSS.");
    }
    else
    {
        DrawSettingsHelp("Residual clamps colour to 0..1. Use Ratio for linear HDR input.");
    }
    ImGui::BeginDisabled(!producerColor && value.method != TheosRenderPipeline::NeuralRendering::ResolveMethod::Ratio);
    ImGui::SliderFloat(producerColor ? "Effect strength" : "Ratio effect strength", &value.transferStrength, 0, 2);
    ImGui::SliderFloat(producerColor ? "Colour strength" : "Ratio colour strength", &value.colourStrength, 0, 2);
    ImGui::SliderFloat(producerColor ? "Maximum scene gain" : "Maximum luma ratio", &value.maxRatio,
                       producerColor ? 1.0f : 0.01f, 16);
    ImGui::InputFloat(producerColor ? "Scene normalization" : "HDR encode white point", &value.whitePoint, 0, 0,
                      "%.4f");
    ImGui::BeginDisabled(producerColor);
    ImGui::Checkbox("Input colour is linear HDR", &value.colorIsHDR);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    DrawSettingsHelp(producerColor
                         ? "Scene normalization sets the brightness range NR sees. Changes restart its history and may "
                           "stall briefly."
                         : "HDR input is used by Ratio reconstruction; it does not change the display HDR mode.");
}

void DrawSourceNeuralControls(TheosRenderPipeline::SourceDLSSG::Preferences& draft, int upscaleType,
                              bool nrRuntimePresent)
{
    auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
    const auto state = backend.NeuralState();
    const auto* frameGen = SourceFrameGeneration::GetSingleton();
    DrawSettingsHelp("Scene processing, cost and appearance");
    const char* unavailableReason = nullptr;
    if (frameGen->settings.neuralRenderingRuntimePath.empty())
    {
        unavailableReason = "NR runtime path is empty. Configure NeuralRenderingRuntimePath in TheosRenderPipeline.ini "
                            "and restart Skyrim.";
    }
    else if (!nrRuntimePresent)
    {
        unavailableReason =
            "NR runtime DLL not found. Install the optional nvngx_dlssnr.dll at the path below and restart Skyrim.";
    }
    else if (!backend.Ready())
    {
        unavailableReason = "The NVIDIA host is not ready. Check runtime status and restart Skyrim.";
    }
    else if (state.failed)
    {
        unavailableReason = "NR failed. Open NR runtime details below for the reported error.";
    }
    else if (!TheosRenderPipeline::SupportsNeuralRenderingMode(upscaleType,
                                                               TheosRenderPipeline::CommunityShaders::Active()))
    {
        unavailableReason = "NR requires DLSS or DLAA mode. Select either in Image and restart Skyrim.";
    }
    else if (!TheosRenderPipeline::CommunityShaders::Active() && !NvidiaHost::GetSingleton()->DedicatedUITextureMode())
    {
        unavailableReason = "NR requires dedicated native UI composition. Enable Native UI in Compatibility (Lab mode) "
                            "and restart Skyrim.";
    }
    const bool unavailable = unavailableReason != nullptr;
    const auto applied = backend.NeuralConfiguration();
    ImGui::Text("Session request: %s | %s upscaling | %d %s", applied.enabled ? "On" : "Off",
                applied.beforeUpscaling ? "before" : "after", applied.passes, applied.passes == 1 ? "pass" : "passes");
    ImGui::TextColored(state.failed   ? kRust
                       : state.active ? kSage
                                      : kMuted,
                       "%s",
                       state.failed      ? "NR failed"
                       : state.active    ? "NR active"
                       : unavailable     ? "NR unavailable"
                       : applied.enabled ? "NR waiting for a world frame"
                                         : "NR off");
    if (unavailable)
    {
        ImGui::TextWrapped("%s", unavailableReason);
        ImGui::TextWrapped("NR runtime path: %s", frameGen->settings.neuralRenderingRuntimePath.c_str());
    }
    ImGui::BeginDisabled(unavailable);
    DrawSettingsHeading("Placement");
    ImGui::Checkbox("Enable Neural Rendering##sourceNR", &draft.neuralEnabled);
    int placement = draft.neuralBeforeUpscaling ? 0 : 1;
    const char* placements[]{"Before upscaling", "After upscaling"};
    if (ImGui::Combo("Placement##sourceNR", &placement, placements, IM_ARRAYSIZE(placements)))
    {
        draft.neuralBeforeUpscaling = placement == 0;
    }
    if (TheosRenderPipeline::CommunityShaders::Active())
    {
        ImGui::TextWrapped(
            draft.neuralBeforeUpscaling
                ? "NR processes the CS scene before DLSS and preserves its brightness range. CS draws UI afterward."
                : "NR processes the final CS image after post-processing. CS draws UI afterward.");
    }
    else if (draft.neuralBeforeUpscaling)
    {
        ImGui::TextWrapped("NR processes the world before DLSS. UI correction is unused; native UI is added later.");
    }
    DrawSettingsHeading("Performance");
    auto& reconstruction = draft.neuralReconstruction;
    float percent = reconstruction.inputScale * 100;
    if (ImGui::SliderFloat("NR input resolution", &percent, 25, 100, "%.1f%%"))
    {
        reconstruction.inputScale = percent / 100;
    }
    DrawSettingsHelp("Percentage of width and height at the selected stage: render resolution before upscaling, output "
                     "resolution after. Game and UI sizes stay unchanged.");
    int passChoice = draft.neuralPasses - 1;
    const char* passes[]{"One", "Two"};
    if (ImGui::Combo("Passes##sourceNR", &passChoice, passes, IM_ARRAYSIZE(passes)))
    {
        draft.neuralPasses = passChoice + 1;
    }
    if (draft.neuralPasses == 2)
    {
        ImGui::TextWrapped("The second pass processes the first result with separate history. It adds GPU time and "
                           "memory; stronger processing may also amplify artifacts.");
    }
    ImGui::Checkbox("Peripheral compression (experimental)", &reconstruction.peripheralCompression);
    if (reconstruction.peripheralCompression)
    {
        DrawSettingsHelp(
            "Preserves sampling density across the central 80% of each axis and compresses the edges. Uses about 19% "
            "fewer model pixels at the same input resolution. Inspect edge quality when moving.");
    }
    ImGui::Checkbox("Combined preparation (experimental)", &reconstruction.fusedPreparation);
    if (reconstruction.fusedPreparation)
    {
        DrawSettingsHelp("Combines colour encoding with downsampling where needed, and depth/motion packing with "
                         "peripheral compression. Some configurations have no passes to combine.");
    }
    const auto* host = NvidiaHost::GetSingleton();
    const auto width = draft.neuralBeforeUpscaling ? host->RenderWidth() : host->OutputWidth();
    const auto height = draft.neuralBeforeUpscaling ? host->RenderHeight() : host->OutputHeight();
    if (width && height)
    {
        ImGui::Text("Input preview at current stage size: %u x %u",
                    TheosRenderPipeline::NeuralRendering::ModelExtent(width, reconstruction),
                    TheosRenderPipeline::NeuralRendering::ModelExtent(height, reconstruction));
    }
    DrawSettingsHelp(
        "Placement, network or resolution changes recreate NR and may stall briefly. Ctrl-click sliders to type.");
    if (ImGui::CollapsingHeader("Appearance##sourceNR"))
    {
        const char* styles[]{"Style 0", "Style 1", "Style 2", "Style 3", "Style 4", "Style 5", "Style 6", "Style 7"};
        ImGui::Combo("Style##sourceNR", &draft.neuralTuning.style, styles, IM_ARRAYSIZE(styles));
        ImGui::SliderFloat("Intensity##sourceNR", &draft.neuralTuning.intensity, 0, 2);
        ImGui::SliderFloat("Local tone##sourceNR", &draft.neuralTuning.localToneStrength, 0, 2);
        ImGui::SliderFloat("Local structure##sourceNR", &draft.neuralTuning.localStructureStrength, 0, 2);
        ImGui::SliderFloat("Skin structure##sourceNR", &draft.neuralTuning.skinStructureStrength, -1, 2);
        ImGui::Checkbox("Automatic skin mask##sourceNR", &draft.neuralTuning.useAutoSkinMask);
        ImGui::BeginDisabled(draft.neuralBeforeUpscaling);
        if (TheosRenderPipeline::CommunityShaders::Active())
        {
            ImGui::TextWrapped("CS draws UI after NR in both placements.");
        }
        else
        {
            ImGui::Checkbox("UI correction##sourceNR", &draft.neuralTuning.uiCorrection);
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Skin -1 follows local structure. Ctrl-click a slider to type.");
    }
    DrawNRReconstructionControls(draft.neuralReconstruction,
                                 TheosRenderPipeline::CommunityShaders::Active() && draft.neuralBeforeUpscaling);
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("NR runtime details##sourceNR"))
    {
        ImGui::TextWrapped("%s", state.status.c_str());
        ImGui::Text("Submitted frames %llu | history resets %llu", static_cast<unsigned long long>(state.evaluations),
                    static_cast<unsigned long long>(state.resets));
        ImGui::TextWrapped("NR feeds both real and generated scene frames. Native UI is added afterwards. Frame "
                           "generation can be off while NR remains on.");
    }
}
} // namespace

void OverlayUI::DrawNeuralRenderingPanel(float tabCardHeight)
{
    if (ImGui::BeginTabItem("Neural Rendering", nullptr,
                            requestedPage == SettingsPage::NeuralRendering ? ImGuiTabItemFlags_SetSelected
                                                                           : ImGuiTabItemFlags_None))
    {
        ImGui::BeginChild("##neuralRenderingPage", ImVec2(0.0f, tabCardHeight), false);
        ImGui::PushTextWrapPos(0.0f);
        DrawSourceNeuralControls(settingsDraft.sourceDLSSG, settingsDraft.upscaleType, nrRuntimePresent);
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
}
