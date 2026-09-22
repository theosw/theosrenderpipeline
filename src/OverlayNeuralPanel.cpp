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
    if (!ImGui::CollapsingHeader("Shared reconstruction"))
    {
        return;
    }
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

void DrawNRInputPreview(const char* label, std::uint32_t width, std::uint32_t height, float inputScale,
                        const TheosRenderPipeline::NeuralRendering::Reconstruction& shared)
{
    if (!width || !height)
    {
        return;
    }
    auto reconstruction = shared;
    reconstruction.inputScale = inputScale;
    ImGui::Text("%s: %u x %u", label, TheosRenderPipeline::NeuralRendering::ModelExtent(width, reconstruction),
                TheosRenderPipeline::NeuralRendering::ModelExtent(height, reconstruction));
}

void DrawNRPassControls(float& inputScale, int& preset, TheosRenderPipeline::NeuralRendering::Tuning& tuning,
                        const TheosRenderPipeline::NeuralRendering::Reconstruction& shared, std::uint32_t width,
                        std::uint32_t height, bool beforeUpscaling, bool readOnly = false)
{
    ImGui::BeginDisabled(readOnly);
    float percent = inputScale * 100;
    if (ImGui::SliderFloat("NR input resolution", &percent, 25, 100, "%.1f%%"))
    {
        inputScale = percent / 100;
    }
    const char* presets[]{"Default", "Shipping"};
    ImGui::Combo("Network preset", &preset, presets, IM_ARRAYSIZE(presets));
    ImGui::EndDisabled();
    DrawNRInputPreview("Requested input at current stage size", width, height, inputScale, shared);
    if (ImGui::CollapsingHeader("Appearance"))
    {
        ImGui::BeginDisabled(readOnly);
        const char* styles[]{"Style 0", "Style 1", "Style 2", "Style 3", "Style 4", "Style 5", "Style 6", "Style 7"};
        ImGui::Combo("Style", &tuning.style, styles, IM_ARRAYSIZE(styles));
        ImGui::SliderFloat("Intensity", &tuning.intensity, 0, 2);
        ImGui::SliderFloat("Local tone", &tuning.localToneStrength, 0, 2);
        ImGui::SliderFloat("Local structure", &tuning.localStructureStrength, 0, 2);
        ImGui::SliderFloat("Skin structure", &tuning.skinStructureStrength, -1, 2);
        ImGui::Checkbox("Automatic skin mask", &tuning.useAutoSkinMask);
        if (TheosRenderPipeline::CommunityShaders::Active())
        {
            DrawSettingsHelp("CS draws UI after NR in both placements.");
        }
        else
        {
            ImGui::BeginDisabled(beforeUpscaling);
            ImGui::Checkbox("UI correction", &tuning.uiCorrection);
            ImGui::EndDisabled();
            if (beforeUpscaling)
            {
                DrawSettingsHelp("UI correction is unused before upscaling; native UI is added later.");
            }
        }
        ImGui::EndDisabled();
        DrawSettingsHelp("Skin -1 follows local structure. Ctrl-click a slider to type.");
    }
}

void DrawNRAppliedPasses(const TheosRenderPipeline::SourceDLSSG::NeuralOptions& applied)
{
    const auto* host = NvidiaHost::GetSingleton();
    const auto width = applied.beforeUpscaling ? host->RenderWidth() : host->OutputWidth();
    const auto height = applied.beforeUpscaling ? host->RenderHeight() : host->OutputHeight();
    ImGui::Text("Pass 1 session request: %.1f%% | network %s", applied.reconstruction.inputScale * 100,
                applied.reconstruction.preset == 1 ? "Shipping" : "Default");
    DrawNRInputPreview("Pass 1 input at current stage size", width, height, applied.reconstruction.inputScale,
                       applied.reconstruction);
    if (applied.passes == 2)
    {
        const auto second = applied.EffectiveSecond();
        ImGui::Text("Pass 2 session request: %s | %.1f%% | network %s", second.linked ? "linked" : "custom",
                    second.inputScale * 100, second.preset == 1 ? "Shipping" : "Default");
        DrawNRInputPreview("Pass 2 input at current stage size", width, height, second.inputScale,
                           applied.reconstruction);
    }
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
                       : applied.enabled ? "NR inactive"
                                         : "NR off");
    if (unavailable)
    {
        ImGui::TextWrapped("%s", unavailableReason);
        ImGui::TextWrapped("NR runtime path: %s", frameGen->settings.neuralRenderingRuntimePath.c_str());
    }
    if (applied.enabled && !state.active && !unavailable)
    {
        ImGui::TextWrapped("%s", state.status.c_str());
    }
    ImGui::BeginDisabled(unavailable);
    DrawSettingsHeading("Shared settings");
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
    auto& reconstruction = draft.neuralReconstruction;
    int passChoice = draft.neuralPasses - 1;
    const char* passes[]{"One", "Two"};
    if (ImGui::Combo("Passes##sourceNR", &passChoice, passes, IM_ARRAYSIZE(passes)))
    {
        draft.neuralPasses = passChoice + 1;
    }
    if (draft.neuralPasses == 2)
    {
        ImGui::TextWrapped("Pass 2 processes Pass 1's result with separate history. Both passes run on the selected "
                           "side of upscaling. "
                           "The second evaluation adds GPU time and memory.");
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
    DrawNRReconstructionControls(reconstruction,
                                 TheosRenderPipeline::CommunityShaders::Active() && draft.neuralBeforeUpscaling);
    DrawSettingsHelp("Reconstruction and optimization options are shared. Each input percentage uses the selected "
                     "stage's width and height: render size before upscaling, output size after. NR network presets "
                     "are separate from DLSS model presets.");
    const auto* host = NvidiaHost::GetSingleton();
    const auto width = draft.neuralBeforeUpscaling ? host->RenderWidth() : host->OutputWidth();
    const auto height = draft.neuralBeforeUpscaling ? host->RenderHeight() : host->OutputHeight();

    DrawSettingsHeading("Pass 1");
    ImGui::PushID("nrPass1");
    DrawNRPassControls(reconstruction.inputScale, reconstruction.preset, draft.neuralTuning, reconstruction, width,
                       height, draft.neuralBeforeUpscaling);
    ImGui::PopID();
    if (draft.neuralPasses == 2)
    {
        DrawSettingsHeading("Pass 2");
        ImGui::PushID("nrPass2");
        auto& second = draft.neuralSecondPass;
        ImGui::Checkbox("Use Pass 1 settings", &second.linked);
        if (!second.linked && ImGui::Button("Copy Pass 1 settings"))
        {
            second.inputScale = reconstruction.inputScale;
            second.preset = reconstruction.preset;
            second.tuning = draft.neuralTuning;
        }
        // Display linked values through a temporary; never overwrite saved custom settings.
        auto inherited =
            TheosRenderPipeline::NeuralRendering::EffectiveSecondPass(second, reconstruction, draft.neuralTuning);
        auto& shown = second.linked ? inherited : second;
        DrawNRPassControls(shown.inputScale, shown.preset, shown.tuning, reconstruction, width, height,
                           draft.neuralBeforeUpscaling, second.linked);
        DrawSettingsHelp(second.linked
                             ? "Pass 2 follows Pass 1. Your custom Pass 2 settings are retained for later use."
                             : "Pass 2's percentage uses the scene size, not Pass 1's reduced grid. "
                               "Its colour changes are transferred back to preserve Pass 1's detail.");
        ImGui::PopID();
    }
    DrawSettingsHelp("Placement, network or resolution changes recreate NR and may pause briefly. "
                     "Appearance changes reset its history.");
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("NR runtime details##sourceNR"))
    {
        ImGui::TextWrapped("%s", state.status.c_str());
        DrawNRAppliedPasses(applied);
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
