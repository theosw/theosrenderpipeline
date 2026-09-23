#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "OverlayFrameView.h"

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
    if (!ImGui::CollapsingHeader("Reconstruction"))
    {
        return;
    }
    const char* methods[]{"Auto | Reconstruct reduced input", "Residual | Add NR changes",
                          "Ratio | Transfer lighting and colour"};
    int method = static_cast<int>(value.method);
    ImGui::BeginDisabled(producerColor);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##reconstructionMethod", &method, methods, IM_ARRAYSIZE(methods)))
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
    ImGui::InputFloat(producerColor ? "Scene normalization" : "HDR white point", &value.whitePoint, 0, 0, "%.4f");
    DrawSettingsHelp(producerColor ? "Scene normalization sets the brightness range NR sees. Changes restart its "
                                     "history and may stall briefly."
                                   : "White point for encoding linear HDR input before model evaluation.");
    ImGui::BeginDisabled(producerColor);
    ImGui::Checkbox("Input colour is linear HDR", &value.colorIsHDR);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    DrawSettingsHelp("HDR input is used by Ratio reconstruction; it does not change the display HDR mode.");
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
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::BeginTooltip();
        DrawNRInputPreview("Requested input", width, height, inputScale, shared);
        ImGui::TextUnformatted("Relative to the selected stage's scene size. Apply to update.");
        ImGui::EndTooltip();
    }
    const char* presets[]{"Default", "Shipping"};
    ImGui::Combo("Network preset", &preset, presets, IM_ARRAYSIZE(presets));
    DrawSettingsHelp(
        "NR network selection, separate from the DLSS model preset. Changing it recreates NR and may pause briefly.");
    ImGui::SliderFloat("Intensity", &tuning.intensity, 0, 2);
    ImGui::EndDisabled();
    if (ImGui::CollapsingHeader("Appearance"))
    {
        ImGui::BeginDisabled(readOnly);
        const char* styles[]{"Style 0", "Style 1", "Style 2", "Style 3", "Style 4", "Style 5", "Style 6", "Style 7"};
        ImGui::Combo("Style", &tuning.style, styles, IM_ARRAYSIZE(styles));
        ImGui::SliderFloat("Local tone", &tuning.localToneStrength, 0, 2);
        ImGui::SliderFloat("Local structure", &tuning.localStructureStrength, 0, 2);
        ImGui::SliderFloat("Skin structure", &tuning.skinStructureStrength, -1, 2);
        DrawSettingsHelp("-1 follows local structure. Ctrl-click a slider to type.");
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
    }
}

void DrawNRAppliedPasses(const TheosRenderPipeline::SourceDLSSG::NeuralOptions& applied)
{
    const auto* host = NvidiaHost::GetSingleton();
    const auto width = applied.beforeUpscaling ? host->RenderWidth() : host->OutputWidth();
    const auto height = applied.beforeUpscaling ? host->RenderHeight() : host->OutputHeight();
    DrawSettingsValue("Placement", applied.beforeUpscaling ? "Before upscaling" : "After upscaling");
    DrawSettingsValue("Scene", std::format("{} x {}", width, height).c_str());
    auto pass = [&](const char* label, float scale, int preset) {
        auto config = applied.reconstruction;
        config.inputScale = scale;
        ImGui::Separator();
        DrawSettingsValue(label,
                          std::format("{} x {}", TheosRenderPipeline::NeuralRendering::ModelExtent(width, config),
                                      TheosRenderPipeline::NeuralRendering::ModelExtent(height, config))
                              .c_str());
        DrawSettingsValue("Network", preset == 1 ? "Shipping" : "Default");
    };
    pass("Pass 1", applied.reconstruction.inputScale, applied.reconstruction.preset);
    if (applied.passes == 2)
    {
        const auto second = applied.EffectiveSecond();
        pass("Pass 2", second.inputScale, second.preset);
        DrawSettingsValue("Settings", second.linked ? "Linked" : "Independent");
    }
}

void DrawSourceNeuralControls(TheosRenderPipeline::SourceDLSSG::Preferences& draft, int upscaleType,
                              bool nrRuntimePresent)
{
    auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
    const auto state = backend.NeuralState();
    const auto* frameGen = SourceFrameGeneration::GetSingleton();
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
        unavailableReason = "NR failed. See the error in the left column.";
    }
    else if (!TheosRenderPipeline::SupportsNeuralRenderingMode(upscaleType,
                                                               TheosRenderPipeline::CommunityShaders::Active()))
    {
        unavailableReason = "NR requires DLSS or DLAA mode. Select either in Image and restart Skyrim.";
    }
    else if (!TheosRenderPipeline::CommunityShaders::Active() && !NvidiaHost::GetSingleton()->DedicatedUITextureMode())
    {
        unavailableReason = "NR requires dedicated UI composition. Set NativeUICompositionMode=0 in the INI and "
                            "restart Skyrim. If it is already 0, check the log for a composition failure.";
    }
    const bool unavailable = unavailableReason != nullptr;
    const auto applied = backend.NeuralConfiguration();
    if (unavailable)
    {
        ImGui::TextWrapped("%s", unavailableReason);
        ImGui::TextWrapped("NR runtime path: %s", frameGen->settings.neuralRenderingRuntimePath.c_str());
    }
    if (applied.enabled && !state.active && !unavailable)
    {
        ImGui::TextWrapped("%s", state.status.c_str());
    }
    ImGui::BeginDisabled(!TheosRenderPipeline::CanEditNeuralEnabled(draft.neuralEnabled, !unavailable));
    ImGui::Checkbox("Neural Rendering##sourceNR", &draft.neuralEnabled);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(unavailable);
    int placement = draft.neuralBeforeUpscaling ? 0 : 1;
    const char* placements[]{"Before upscaling", "After upscaling"};
    if (ImGui::Combo("Placement##sourceNR", &placement, placements, IM_ARRAYSIZE(placements)))
    {
        draft.neuralBeforeUpscaling = placement == 0;
    }
    DrawSettingsHelp("Both passes use the selected side of upscaling. UI is composed afterwards.");
    auto& reconstruction = draft.neuralReconstruction;
    int passChoice = draft.neuralPasses - 1;
    const char* passes[]{"One", "Two"};
    if (ImGui::Combo("Passes##sourceNR", &passChoice, passes, IM_ARRAYSIZE(passes)))
    {
        draft.neuralPasses = passChoice + 1;
    }
    if (draft.neuralPasses == 2)
    {
        DrawSettingsHelp("Pass 2 processes Pass 1's result with separate history. Both passes run on the selected "
                         "side of upscaling. "
                         "The second evaluation adds GPU time and memory.");
    }
    ImGui::Checkbox("Peripheral compression", &reconstruction.peripheralCompression);
    DrawSettingsHelp(
        "Preserves sampling density across the central 80% of each axis and compresses the edges. Uses about 19% "
        "fewer model pixels at the same input resolution. Inspect edge quality when moving.");
    ImGui::Checkbox("Combined preparation", &reconstruction.fusedPreparation);
    DrawSettingsHelp("Combines colour encoding with downsampling where needed, and depth/motion packing with "
                     "peripheral compression. Some configurations have no passes to combine. Applies to both passes.");
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
        DrawSettingsHelp("Pass 2 follows Pass 1 while linked. Your custom Pass 2 settings are retained for later use.");
        if (!second.linked)
        {
            const float buttonWidth = ImGui::CalcTextSize("Copy Pass 1").x + ImGui::GetStyle().FramePadding.x * 2;
            if (ImGui::GetContentRegionAvail().x >
                ImGui::GetItemRectSize().x + ImGui::GetStyle().ItemSpacing.x + buttonWidth)
                ImGui::SameLine();
            if (ImGui::Button("Copy Pass 1"))
            {
                second.inputScale = reconstruction.inputScale;
                second.preset = reconstruction.preset;
                second.tuning = draft.neuralTuning;
            }
        }
        // Display linked values through a temporary; never overwrite saved custom settings.
        auto inherited =
            TheosRenderPipeline::NeuralRendering::EffectiveSecondPass(second, reconstruction, draft.neuralTuning);
        auto& shown = second.linked ? inherited : second;
        DrawNRPassControls(shown.inputScale, shown.preset, shown.tuning, reconstruction, width, height,
                           draft.neuralBeforeUpscaling, second.linked);
        ImGui::PopID();
    }
    DrawNRReconstructionControls(reconstruction,
                                 TheosRenderPipeline::CommunityShaders::Active() && draft.neuralBeforeUpscaling);
    ImGui::EndDisabled();
}
} // namespace

void OverlayUI::DrawNeuralRenderingPanel(float height, const FrameView& view)
{
    if (!ImGui::BeginTabItem("Neural Rendering", nullptr,
                             requestedPage == SettingsPage::NeuralRendering ? ImGuiTabItemFlags_SetSelected : 0))
        return;
    if (BeginSettingsColumns("neural", height, view))
    {
        const auto applied = TheosRenderPipeline::SourceDLSSG::Backend::Get().NeuralConfiguration();
        const auto& state = view.sourceNeural;
        DrawStatusLabel(state.failed      ? "NR failed"
                        : state.active    ? "NR active"
                        : applied.enabled ? "NR inactive"
                                          : "NR off",
                        state.failed   ? UIHealth::kError
                        : state.active ? UIHealth::kHealthy
                                       : UIHealth::kIdle);
        const auto& timing = state.telemetry;
        DrawSettingsValue("Inference", state.active && timing.gpuSamples
                                           ? std::format("{:.2f} ms", timing.AverageGPUMicroseconds() / 1000.0).c_str()
                                           : "unavailable");
        DrawSettingsHelp("Combined model inference and inter-pass preparation. Excludes input preparation, final Pass "
                         "2 restoration, reconstruction, UI composition and the D3D11/D3D12 handoff.");
        DrawNRAppliedPasses(applied);
        if (state.failed || (applied.enabled && !state.active))
            ImGui::TextWrapped("%s", state.status.c_str());
        if (showDeveloperControls)
        {
            ImGui::Separator();
            if (!state.failed && (!applied.enabled || state.active))
                ImGui::TextWrapped("%s", state.status.c_str());
            ImGui::TextWrapped("Submitted frames %llu | history resets %llu",
                               static_cast<unsigned long long>(state.evaluations),
                               static_cast<unsigned long long>(state.resets));
            if (timing.gpuSamples)
                ImGui::TextWrapped("Inference max %.3f ms | %llu samples | %llu query failures",
                                   timing.MaximumGPUMicroseconds() / 1000.0,
                                   static_cast<unsigned long long>(timing.gpuSamples),
                                   static_cast<unsigned long long>(timing.gpuQueryFailures));
        }
        NextSettingsColumn(height);
        DrawSourceNeuralControls(settingsDraft.sourceDLSSG, settingsDraft.upscaleType, nrRuntimePresent);
        EndSettingsColumns();
    }
    ImGui::EndTabItem();
}
