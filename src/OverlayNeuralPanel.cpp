#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "OverlayPresetDecor.h"
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
    if (!ImGui::CollapsingHeader("Reconstruction (!)"))
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
        DrawSettingsHelp(std::format("Residual clamps colour to 0..1. Use Ratio for linear HDR input.{}", RestartsNRHelp).c_str());
    }
    MarkSetting("ReconstructionMethod");
    ImGui::BeginDisabled(!producerColor && value.method != TheosRenderPipeline::NeuralRendering::ResolveMethod::Ratio);
    ImGui::SliderFloat(producerColor ? "Effect strength" : "Ratio effect strength", &value.transferStrength, 0, 2);
    MarkSetting("EffectStrength");
    ImGui::SliderFloat(producerColor ? "Colour strength" : "Ratio colour strength", &value.colourStrength, 0, 2);
    MarkSetting("ColourStrength");
    ImGui::SliderFloat(producerColor ? "Maximum scene gain" : "Maximum luma ratio", &value.maxRatio,
                       producerColor ? 1.0f : 0.01f, 16);
    MarkSetting("MaximumRatio");
    ImGui::InputFloat(producerColor ? "Scene normalization (!)" : "HDR white point", &value.whitePoint, 0, 0, "%.4f");
    DrawSettingsHelp(producerColor ? "Scene normalization sets the brightness range NR sees. Changes restart its "
                                     "history and may stall briefly."
                                   : "White point for encoding linear HDR input before model evaluation.");
    MarkSetting("WhitePoint");
    ImGui::BeginDisabled(producerColor);
    ImGui::Checkbox("Input colour is linear HDR (!)", &value.colorIsHDR);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    DrawSettingsHelp(std::format("HDR input is used by Ratio reconstruction; it does not change the display HDR mode.{}",
                                 RestartsNRHelp).c_str());
    MarkSetting("InputHDR");
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

// Quality controls for one pass: model input size and network. prefix is "Pass1" or "Pass2".
void DrawNRPassQuality(float& inputScale, int& preset, const TheosRenderPipeline::NeuralRendering::Reconstruction& shared,
                       std::uint32_t width, std::uint32_t height, const std::string& prefix)
{
    float percent = inputScale * 100;
    if (ImGui::SliderFloat("NR input resolution (!)", &percent, 25, 100, "%.1f%%"))
    {
        inputScale = percent / 100;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::BeginTooltip();
        DrawNRInputPreview("Requested input", width, height, inputScale, shared);
        ImGui::TextUnformatted("Relative to the selected stage's scene size. Apply to update.");
        ImGui::TextUnformatted(RestartsNRHelp + 1);
        ImGui::EndTooltip();
    }
    MarkSetting((prefix + "InputScale").c_str());
    const char* presets[]{"Default", "Shipping"};
    ImGui::Combo("Network preset (!)", &preset, presets, IM_ARRAYSIZE(presets));
    DrawSettingsHelp(std::format("NR network selection, separate from the DLSS model preset.{}", RestartsNRHelp).c_str());
    MarkSetting((prefix + "Network").c_str());
}

// The tuning presets blend: they change smoothly between weathers and times of day.
void DrawNRLookSliders(TheosRenderPipeline::NeuralRendering::Tuning& tuning, const std::string& prefix)
{
    ImGui::SliderFloat("Intensity", &tuning.intensity, 0, 2);
    MarkSetting((prefix + "Intensity").c_str());
    ImGui::SliderFloat("Local tone", &tuning.localToneStrength, 0, 2);
    MarkSetting((prefix + "LocalTone").c_str());
    ImGui::SliderFloat("Local structure", &tuning.localStructureStrength, 0, 2);
    MarkSetting((prefix + "LocalStructure").c_str());
}

void DrawNRMoreLook(TheosRenderPipeline::NeuralRendering::Tuning& tuning, bool beforeUpscaling, const std::string& prefix)
{
    const char* styles[]{"Style 0", "Style 1", "Style 2", "Style 3", "Style 4", "Style 5", "Style 6", "Style 7"};
    ImGui::Combo("Style", &tuning.style, styles, IM_ARRAYSIZE(styles));
    MarkSetting((prefix + "Style").c_str());
    ImGui::SliderFloat("Skin structure", &tuning.skinStructureStrength, -1, 2);
    DrawSettingsHelp("-1 follows local structure. Ctrl-click a slider to type.");
    MarkSetting((prefix + "SkinStructure").c_str());
    ImGui::Checkbox("Automatic skin mask", &tuning.useAutoSkinMask);
    MarkSetting((prefix + "AutoSkinMask").c_str());
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
        MarkSetting((prefix + "UICorrection").c_str());
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

// Why NR cannot run now, or null. Shown above Base's controls.
const char* NeuralUnavailableReason(int upscaleType, bool nrRuntimePresent)
{
    auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
    const auto* frameGen = SourceFrameGeneration::GetSingleton();
    if (frameGen->settings.neuralRenderingRuntimePath.empty())
    {
        return "NR runtime path is empty. Configure NeuralRenderingRuntimePath in TheosRenderPipeline.ini "
               "and restart Skyrim.";
    }
    if (!nrRuntimePresent)
    {
        return "NR runtime DLL not found. Install the optional nvngx_dlssnr.dll at the path below and restart Skyrim.";
    }
    if (!backend.Ready())
    {
        return "The NVIDIA host is not ready. Check runtime status and restart Skyrim.";
    }
    if (backend.NeuralState().failed)
    {
        return "NR failed. See the error in the left column.";
    }
    if (!TheosRenderPipeline::SupportsNeuralRenderingMode(upscaleType, TheosRenderPipeline::CommunityShaders::Active()))
    {
        return "NR requires DLSS or DLAA mode. Select either in Image and restart Skyrim.";
    }
    if (!TheosRenderPipeline::CommunityShaders::Active() && !NvidiaHost::GetSingleton()->DedicatedUITextureMode())
    {
        return "NR requires dedicated UI composition. Set NativeUICompositionMode=0 in the INI and "
               "restart Skyrim. If it is already 0, check the log for a composition failure.";
    }
    return nullptr;
}

// Base and presets share these controls. Presets mark the settings they change.
void DrawNeuralSettings(TheosRenderPipeline::SourceDLSSG::Preferences& draft, bool& sharpening, float& sharpness,
                        bool unavailable)
{
    ImGui::BeginDisabled(!TheosRenderPipeline::CanEditNeuralEnabled(draft.neuralEnabled, !unavailable));
    ImGui::Checkbox("Neural Rendering##sourceNR", &draft.neuralEnabled);
    ImGui::EndDisabled();
    MarkSetting("NeuralRendering");
    auto& reconstruction = draft.neuralReconstruction;
    auto& second = draft.neuralSecondPass;
    const bool separate = draft.neuralPasses == 2 && !second.linked;
    const bool cs = TheosRenderPipeline::CommunityShaders::Active();

    DrawSettingsHeading("Look");
    DrawSettingsHelp("Presets can change these. They blend smoothly between weathers and times of day.");
    PresetLookControls();
    ImGui::BeginDisabled(unavailable);
    if (separate) { ImGui::TextDisabled("Pass 1"); }
    ImGui::PushID("lookPass1");
    DrawNRLookSliders(draft.neuralTuning, "Pass1");
    ImGui::PopID();
    if (separate)
    {
        ImGui::TextDisabled("Pass 2");
        ImGui::PushID("lookPass2");
        DrawNRLookSliders(second.tuning, "Pass2");
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    // Sharpening follows DLSS with or without NR, so it stays editable here.
    if (cs)
    {
        ImGui::TextDisabled("Community Shaders controls sharpening.");
    }
    else
    {
        ImGui::Checkbox("Sharpening", &sharpening);
        MarkSetting("SharpeningEnabled");
        ImGui::BeginDisabled(!sharpening);
        ImGui::SliderFloat("Sharpening strength", &sharpness, 0, 1, "%.2f");
        ImGui::EndDisabled();
        DrawSettingsHelp("Sharpens DLSS output, with or without NR.");
        MarkSetting("Sharpness");
    }
    ImGui::BeginDisabled(unavailable);
    if (ImGui::CollapsingHeader("More look options"))
    {
        if (separate) { ImGui::TextDisabled("Pass 1"); }
        ImGui::PushID("moreLookPass1");
        DrawNRMoreLook(draft.neuralTuning, draft.neuralBeforeUpscaling, "Pass1");
        ImGui::PopID();
        if (separate)
        {
            ImGui::TextDisabled("Pass 2");
            ImGui::PushID("moreLookPass2");
            DrawNRMoreLook(second.tuning, draft.neuralBeforeUpscaling, "Pass2");
            ImGui::PopID();
        }
    }

    DrawSettingsHeading("Quality");
    int placement = draft.neuralBeforeUpscaling ? 0 : 1;
    const char* placements[]{"Before upscaling", "After upscaling"};
    if (ImGui::Combo("Placement (!)##sourceNR", &placement, placements, IM_ARRAYSIZE(placements)))
    {
        draft.neuralBeforeUpscaling = placement == 0;
    }
    DrawSettingsHelp(std::format("Both passes use the selected side of upscaling. UI is composed afterwards.{}",
                                 RestartsNRHelp).c_str());
    MarkSetting("BeforeUpscaling");
    int passChoice = draft.neuralPasses - 1;
    const char* passes[]{"One", "Two"};
    if (ImGui::Combo("Passes##sourceNR", &passChoice, passes, IM_ARRAYSIZE(passes)))
    {
        draft.neuralPasses = passChoice + 1;
    }
    DrawSettingsHelp("Pass 2 processes Pass 1's result with separate history and adds GPU time and memory. Presets "
                     "switch between one and two passes without restarting NR.");
    MarkSetting("Passes");
    const auto* host = NvidiaHost::GetSingleton();
    const auto width = draft.neuralBeforeUpscaling ? host->RenderWidth() : host->OutputWidth();
    const auto height = draft.neuralBeforeUpscaling ? host->RenderHeight() : host->OutputHeight();
    if (separate) { ImGui::TextDisabled("Pass 1"); }
    ImGui::PushID("nrPass1");
    DrawNRPassQuality(reconstruction.inputScale, reconstruction.preset, reconstruction, width, height, "Pass1");
    ImGui::PopID();
    if (draft.neuralPasses == 2)
    {
        ImGui::PushID("nrPass2");
        ImGui::Checkbox("Pass 2 same as Pass 1", &second.linked);
        DrawSettingsHelp("Pass 2 uses Pass 1's look and quality while ticked. Your separate Pass 2 settings are "
                         "kept for later.");
        MarkSetting("Pass2SameAsPass1");
        if (!second.linked)
        {
            ImGui::SameLine();
            if (ImGui::Button("Copy Pass 1"))
            {
                second.inputScale = reconstruction.inputScale;
                second.preset = reconstruction.preset;
                second.tuning = draft.neuralTuning;
            }
            ImGui::TextDisabled("Pass 2");
            DrawNRPassQuality(second.inputScale, second.preset, reconstruction, width, height, "Pass2");
        }
        ImGui::PopID();
    }

    DrawSettingsHeading("Performance");
    if (draft.neuralPasses == 2)
    {
        ImGui::Checkbox("One pass in combat", &draft.neuralCombat.inCombat);
        MarkSetting("OnePassInCombat");
        ImGui::Checkbox("One pass while weapons/spells are drawn", &draft.neuralCombat.weaponsDrawn);
        DrawSettingsHelp("Temporarily skips Pass 2 when either selected condition is active. Keeps your NR resolution, "
                         "tuning and saved two-pass setting. The image may change when switching.");
        MarkSetting("OnePassWeaponsDrawn");
        if (draft.neuralCombat.Enabled()) {
            ImGui::SliderFloat("Return delay##nrCombat", &draft.neuralCombat.recoverySeconds, 0.0f, 30.0f, "%.1f s");
            DrawSettingsHelp("Waits this long after all selected conditions clear before restoring two passes. "
                             "Pausing the game pauses the delay.");
            MarkSetting("ReturnDelay");
        }
    }
    ImGui::Checkbox("Peripheral compression (!)", &reconstruction.peripheralCompression);
    DrawSettingsHelp(std::format(
        "Preserves sampling density across the central 80% of each axis and compresses the edges. Uses about 19% "
        "fewer model pixels at the same input resolution. Inspect edge quality when moving.{}", RestartsNRHelp).c_str());
    MarkSetting("PeripheralCompression");
    ImGui::Checkbox("Combined preparation (!)", &reconstruction.fusedPreparation);
    DrawSettingsHelp(std::format("Combines colour encoding with downsampling where needed, and depth/motion packing with "
                     "peripheral compression. Some configurations have no passes to combine. Applies to both passes.{}",
                     RestartsNRHelp).c_str());
    MarkSetting("CombinedPreparation");
    ImGui::Spacing();
    DrawNRReconstructionControls(reconstruction, cs && draft.neuralBeforeUpscaling);
    ImGui::EndDisabled();
}

void DrawSourceNeuralControls(TheosRenderPipeline::SourceDLSSG::Preferences& draft, bool& sharpening,
                              float& sharpness, int upscaleType, bool nrRuntimePresent)
{
    auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
    const auto state = backend.NeuralState();
    const char* unavailableReason = NeuralUnavailableReason(upscaleType, nrRuntimePresent);
    const bool unavailable = unavailableReason != nullptr;
    const auto applied = backend.NeuralConfiguration();
    if (unavailable)
    {
        ImGui::TextWrapped("%s", unavailableReason);
        ImGui::TextWrapped("NR runtime path: %s", SourceFrameGeneration::GetSingleton()->settings.neuralRenderingRuntimePath.c_str());
    }
    if (applied.enabled && !state.active && !unavailable)
    {
        ImGui::TextWrapped("%s", state.status.c_str());
    }
    DrawNeuralSettings(draft, sharpening, sharpness, unavailable);
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
        if (state.active) {
            const auto execution = state.passOverride == TheosRenderPipeline::NeuralRendering::PassOverride::None ?
                std::format("{} pass(es)", state.effectivePasses) :
                std::format("1 pass - {}", TheosRenderPipeline::NeuralRendering::PassOverrideName(state.passOverride));
            DrawSettingsValue("Running", execution.c_str());
        }
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
        DrawPresetList();
        NextSettingsColumn(height);
        if (PresetEditorSelected()) {
            const bool unavailable = NeuralUnavailableReason(settingsDraft.upscaleType, nrRuntimePresent) != nullptr;
            DrawPresetEditor([&](TheosRenderPipeline::SourceDLSSG::Preferences& draft, bool& sharpening, float& sharpness) {
                DrawNeuralSettings(draft, sharpening, sharpness, unavailable);
            });
        }
        else { DrawSourceNeuralControls(settingsDraft.sourceDLSSG, settingsDraft.sharpening, settingsDraft.sharpness, settingsDraft.upscaleType, nrRuntimePresent); }
        EndSettingsColumns();
    }
    ImGui::EndTabItem();
}
