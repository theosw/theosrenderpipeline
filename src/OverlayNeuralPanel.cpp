#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "OverlaySettingRows.h"
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
    if (backend.Ready() && !backend.NvidiaAdapter()) { return "Neural Rendering requires NVIDIA RTX hardware. XeSS upscaling and XeFG remain available."; }
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

// Base and presets share these controls. Presets add each row's state column.
void DrawNeuralSettings(TheosRenderPipeline::SourceDLSSG::Preferences& draft, bool& sharpening, float& sharpness,
                        bool unavailable)
{
    namespace NR = TheosRenderPipeline::NeuralRendering;
    auto& reconstruction = draft.neuralReconstruction;
    auto& second = draft.neuralSecondPass;
    const bool cs = TheosRenderPipeline::CommunityShaders::Active();
    const bool producerColor = cs && draft.neuralBeforeUpscaling;
    const bool twoPasses = draft.neuralPasses == 2;
    const float label = LabelWidth({"One pass while weapons are drawn", "Input colour is linear HDR", "NR input resolution"});
    const auto* host = NvidiaHost::GetSingleton();
    const auto width = draft.neuralBeforeUpscaling ? host->RenderWidth() : host->OutputWidth();
    const auto height = draft.neuralBeforeUpscaling ? host->RenderHeight() : host->OutputHeight();
    const char* placements[]{"Before upscaling", "After upscaling"};
    const char* styles[]{"Style 0", "Style 1", "Style 2", "Style 3", "Style 4", "Style 5", "Style 6", "Style 7"};
    const char* networks[]{"Default", "Shipping"};
    const char* methods[]{"Auto | Reconstruct reduced input", "Residual | Add NR changes",
                          "Ratio | Transfer lighting and colour"};

    if (BeginSettingRows("nrTop", label))
    {
        SettingRow("Neural Rendering", "NeuralRendering", false,
                   !TheosRenderPipeline::CanEditNeuralEnabled(draft.neuralEnabled, !unavailable),
                   [&] { return ImGui::Checkbox("##v", &draft.neuralEnabled); });
        SettingRow("Placement", "BeforeUpscaling", true, unavailable, [&] {
            int placement = draft.neuralBeforeUpscaling ? 0 : 1;
            const bool changed = ImGui::Combo("##v", &placement, placements, IM_ARRAYSIZE(placements));
            if (changed) { draft.neuralBeforeUpscaling = placement == 0; }
            DrawSettingsHelp("Both passes use the selected side of upscaling. UI is composed afterwards.");
            return changed;
        });
        ImGui::EndTable();
    }
    ImGui::TextDisabled("! restarts NR briefly when changed");

    const auto* decor = ActivePresetDecor();
    const auto heading = decor && decor->EditingTime() ? std::format("Passes  |  editing {}", decor->EditingTime())
                                                       : std::string("Passes");
    DrawSettingsHeading(heading.c_str());
    // Pass 2 follows Pass 1 until one of its own settings changes.
    auto shown = NR::EffectiveSecondPass(second, reconstruction, draft.neuralTuning);
    const auto before = shown;
    if (BeginPassTable("passes", label))
    {
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kAmber, "Pass 1");
        ImGui::TableNextColumn();
        if (AnyChanged({"Passes", "Pass2SameAsPass1"})) { ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ChangedTint()); }
        ImGui::BeginDisabled(unavailable);
        bool enabled = twoPasses;
        ImGui::PushStyleColor(ImGuiCol_Text, enabled ? kAmber : kMuted);
        if (ImGui::Checkbox("Pass 2", &enabled)) { draft.neuralPasses = enabled ? 2 : 1; }
        ImGui::PopStyleColor();
        DrawSettingsHelp("Pass 2 processes Pass 1's result with separate history and adds GPU time and memory. "
                         "Presets switch it on and off without restarting NR.");
        ImGui::SameLine(0, Px(14.0f));
        if (!enabled)
        {
            ImGui::TextDisabled("off");
        }
        else if (second.linked)
        {
            ImGui::TextDisabled("follows Pass 1");
            DrawSettingsHelp("Changing a Pass 2 setting gives it its own settings, starting from Pass 1's.");
        }
        else if (ImGui::SmallButton("Match Pass 1"))
        {
            second.linked = true;
        }
        else
        {
            DrawSettingsHelp("Pass 2 follows Pass 1 again. Its own settings are kept for later.");
        }
        ImGui::EndDisabled();
        StateCell({"Passes", "Pass2SameAsPass1"});

        auto tuning = [&](int pass) -> NR::Tuning& { return pass ? shown.tuning : draft.neuralTuning; };
        PassRow("Style", "Style", false, unavailable, enabled, [&](int pass) {
            return ImGui::Combo("##v", &tuning(pass).style, styles, IM_ARRAYSIZE(styles));
        });
        PassRow("Intensity", "Intensity", false, unavailable, enabled,
                [&](int pass) { return ImGui::SliderFloat("##v", &tuning(pass).intensity, 0, 2); });
        PassRow("Local tone", "LocalTone", false, unavailable, enabled,
                [&](int pass) { return ImGui::SliderFloat("##v", &tuning(pass).localToneStrength, 0, 2); });
        PassRow("Local structure", "LocalStructure", false, unavailable, enabled,
                [&](int pass) { return ImGui::SliderFloat("##v", &tuning(pass).localStructureStrength, 0, 2); });
        PassRow("Skin structure", "SkinStructure", false, unavailable, enabled, [&](int pass) {
            const bool changed = ImGui::SliderFloat("##v", &tuning(pass).skinStructureStrength, -1, 2);
            DrawSettingsHelp("-1 follows local structure. Ctrl-click a slider to type.");
            return changed;
        });
        PassRow("Automatic skin mask", "AutoSkinMask", false, unavailable, enabled,
                [&](int pass) { return ImGui::Checkbox("##v", &tuning(pass).useAutoSkinMask); });
        PassRow("UI correction", "UICorrection", false, unavailable, enabled, [&](int pass) {
            ImGui::BeginDisabled(cs || draft.neuralBeforeUpscaling);
            const bool changed = ImGui::Checkbox("##v", &tuning(pass).uiCorrection);
            ImGui::EndDisabled();
            if (cs || draft.neuralBeforeUpscaling)
            {
                ImGui::SameLine();
                ImGui::TextDisabled(cs ? "CS draws UI after NR" : "after upscaling only");
            }
            return changed;
        });
        PassRow("NR input resolution", "InputScale", true, unavailable, enabled, [&](int pass) {
            float& scale = pass ? shown.inputScale : reconstruction.inputScale;
            float percent = scale * 100;
            const bool changed = ImGui::SliderFloat("##v", &percent, 25, 100, "%.1f%%");
            if (changed) { scale = percent / 100; }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::BeginTooltip();
                DrawNRInputPreview("Requested input", width, height, scale, reconstruction);
                ImGui::TextUnformatted("Relative to the selected stage's scene size. Apply to update.");
                ImGui::EndTooltip();
            }
            return changed;
        });
        PassRow("Network preset", "Network", true, unavailable, enabled, [&](int pass) {
            const bool changed = ImGui::Combo("##v", pass ? &shown.preset : &reconstruction.preset, networks,
                                              IM_ARRAYSIZE(networks));
            DrawSettingsHelp("NR network selection, separate from the DLSS model preset.");
            return changed;
        });
        ImGui::EndTable();
    }
    if (shown != before)
    {
        second = shown;
        second.linked = false;
    }

    DrawSettingsHeading("Sharpening");
    if (BeginSettingRows("sharpening", label))
    {
        SettingRow("Sharpening", "SharpeningEnabled", false, cs, [&] {
            const bool changed = ImGui::Checkbox("##v", &sharpening);
            ImGui::SameLine();
            ImGui::TextDisabled(cs ? "Community Shaders controls sharpening" : "with or without NR");
            return changed;
        });
        SettingRow("Sharpening strength", "Sharpness", false, cs || !sharpening,
                   [&] { return ImGui::SliderFloat("##v", &sharpness, 0, 1, "%.2f"); });
        ImGui::EndTable();
    }

    DrawSettingsHeading("Performance");
    if (BeginSettingRows("performance", label))
    {
        const char* twoPassHelp = "Temporarily skips Pass 2 when either selected condition is active. Keeps your NR "
                                  "resolution, tuning and saved two-pass setting. The image may change when switching.";
        SettingRow("One pass in combat", "OnePassInCombat", false, unavailable || !twoPasses, [&] {
            const bool changed = ImGui::Checkbox("##v", &draft.neuralCombat.inCombat);
            DrawSettingsHelp(twoPassHelp);
            return changed;
        });
        SettingRow("One pass while weapons are drawn", "OnePassWeaponsDrawn", false, unavailable || !twoPasses, [&] {
            const bool changed = ImGui::Checkbox("##v", &draft.neuralCombat.weaponsDrawn);
            DrawSettingsHelp(twoPassHelp);
            return changed;
        });
        SettingRow("Return delay", "ReturnDelay", false, unavailable || !twoPasses || !draft.neuralCombat.Enabled(), [&] {
            const bool changed = ImGui::SliderFloat("##v", &draft.neuralCombat.recoverySeconds, 0.0f, 30.0f, "%.1f s");
            DrawSettingsHelp("Waits this long after all selected conditions clear before restoring two passes. "
                             "Pausing the game pauses the delay.");
            return changed;
        });
        SettingRow("Peripheral compression", "PeripheralCompression", true, unavailable, [&] {
            const bool changed = ImGui::Checkbox("##v", &reconstruction.peripheralCompression);
            DrawSettingsHelp("Preserves sampling density across the central 80% of each axis and compresses the edges. "
                             "Uses about 19% fewer model pixels at the same input resolution. Inspect edge quality "
                             "when moving.");
            return changed;
        });
        SettingRow("Combined preparation", "CombinedPreparation", true, unavailable, [&] {
            const bool changed = ImGui::Checkbox("##v", &reconstruction.fusedPreparation);
            DrawSettingsHelp("Combines colour encoding with downsampling where needed, and depth/motion packing with "
                             "peripheral compression. Some configurations have no passes to combine. Applies to both "
                             "passes.");
            return changed;
        });
        ImGui::EndTable();
    }

    DrawSettingsHeading("Reconstruction");
    if (BeginSettingRows("reconstruction", label))
    {
        SettingRow("Method", "ReconstructionMethod", true, unavailable || producerColor, [&] {
            int method = static_cast<int>(reconstruction.method);
            const bool changed = ImGui::Combo("##v", &method, methods, IM_ARRAYSIZE(methods));
            if (changed) { reconstruction.method = static_cast<NR::ResolveMethod>(method); }
            DrawSettingsHelp(producerColor ? "CS restores scene colour automatically before DLSS."
                                           : "Residual clamps colour to 0..1. Use Ratio for linear HDR input.");
            return changed;
        });
        const bool ratioOff = unavailable || (!producerColor && reconstruction.method != NR::ResolveMethod::Ratio);
        SettingRow(producerColor ? "Effect strength" : "Ratio effect strength", "EffectStrength", false, ratioOff,
                   [&] { return ImGui::SliderFloat("##v", &reconstruction.transferStrength, 0, 2); });
        SettingRow(producerColor ? "Colour strength" : "Ratio colour strength", "ColourStrength", false, ratioOff,
                   [&] { return ImGui::SliderFloat("##v", &reconstruction.colourStrength, 0, 2); });
        SettingRow(producerColor ? "Maximum scene gain" : "Maximum luma ratio", "MaximumRatio", false, ratioOff,
                   [&] { return ImGui::SliderFloat("##v", &reconstruction.maxRatio, producerColor ? 1.0f : 0.01f, 16); });
        SettingRow(producerColor ? "Scene normalization" : "HDR white point", "WhitePoint", true, ratioOff, [&] {
            const bool changed = ImGui::InputFloat("##v", &reconstruction.whitePoint, 0, 0, "%.4f");
            DrawSettingsHelp(producerColor ? "Scene normalization sets the brightness range NR sees."
                                           : "White point for encoding linear HDR input before model evaluation.");
            return changed;
        });
        SettingRow("Input colour is linear HDR", "InputHDR", true, ratioOff || producerColor, [&] {
            const bool changed = ImGui::Checkbox("##v", &reconstruction.colorIsHDR);
            DrawSettingsHelp("HDR input is used by Ratio reconstruction; it does not change the display HDR mode.");
            return changed;
        });
        ImGui::EndTable();
    }
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
