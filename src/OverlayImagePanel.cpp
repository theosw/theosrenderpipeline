#include "DLSSPreset.h"
#include "Localization.h"
#include "OverlayFrameView.h"
#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "OverlaySettingRows.h"
#include <PCH.h>

using namespace TheosRenderPipeline::Overlay;

namespace
{
bool DrawTextureCapCombo(const char* a_label, std::uint32_t& a_cap)
{
    static constexpr const char* kNames[]{"Full size", "512", "1024", "2048", "4096"};  // translated where shown
    static constexpr std::uint32_t kValues[]{0, 512, 1024, 2048, 4096};
    int selected = 0;
    for (int i = 0; i < static_cast<int>(std::size(kValues)); ++i)
    {
        if (a_cap == kValues[i])
        {
            selected = i;
            break;
        }
    }
    // The table stays English so it can be constexpr; the combo shows the
    // dictionary's names.
    const char* shown[] { TR(kNames[0]), kNames[1], kNames[2], kNames[3], kNames[4] };
    ImGui::SetNextItemWidth(-1.0f);
    if (!ImGui::Combo(a_label, &selected, shown, static_cast<int>(std::size(shown))))
    {
        return false;
    }
    a_cap = kValues[selected];
    return true;
}

int TexturePresetIndex(const TextureProviderBridge::Settings& a_settings)
{
    const auto allEqual = [&](std::uint32_t a_value) {
        return std::ranges::all_of(a_settings.maxSize, [=](std::uint32_t a_cap) { return a_cap == a_value; });
    };
    if (allEqual(2048))
    {
        return 0;
    }
    if (allEqual(1024))
    {
        return 1;
    }
    if (a_settings.maxSize[0] == 1024 && std::ranges::all_of(a_settings.maxSize.begin() + 1, a_settings.maxSize.end(),
                                                             [](std::uint32_t a_cap) { return a_cap == 512; }))
    {
        return 2;
    }
    return 3;
}

void ApplyTexturePreset(TextureProviderBridge::Settings& a_settings, int a_preset)
{
    if (a_preset == 0 || a_preset == 1)
    {
        a_settings.maxSize.fill(a_preset == 0 ? 2048u : 1024u);
    }
    else if (a_preset == 2)
    {
        a_settings.maxSize.fill(512);
        a_settings.maxSize[0] = 1024;
    }
}
} // namespace

namespace
{
float ImageLabelWidth() { return LabelWidth({TR("Highlight strength"), TR("Match Windows SDR"), TR("Peak brightness"), TR("Render scale")}); }
}

void OverlayUI::DrawHDROutputSettings(const FrameView& view)
{
    namespace HDR = TheosRenderPipeline::HDROutput;
    auto& hdr = settingsDraft.sourceDLSSG.hdrOutput;
    const auto& state = view.hdr;
    const float label = ImageLabelWidth();
    if (BeginSettingRows("hdrSwitch", label))
    {
        SettingRow(TR("HDR output"), nullptr, false, false, [&] {
            const bool changed = ImGui::Checkbox("##hdr", &hdr.enabled);
            DrawSettingsHelp(TR("Expands the finished SDR image (including ENB) to HDR10 and shows the UI at its own "
                             "brightness. Highlights the preset already clipped cannot be recovered. Requires Windows "
                             "HDR. The first switch on in a session briefly pauses frame generation."));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", TR("experimental | Apply"));
            return changed;
        });
        ImGui::EndTable();
    }
    // What HDR output is doing now.
    if (hdr.enabled != state.requested)
    {
        ImGui::TextColored(kAmber, TR("Apply to turn HDR output %s"), hdr.enabled ? TRC("on", "OverlayImagePanel") : TRC("off", "OverlayImagePanel"));
    }
    else if (!state.requested && state.native)
    {
        ImGui::TextDisabled("%s", TR("HDR output off; 10-bit output stays allocated until restart"));
    }
    else if (state.requested)
    {
        const auto status = state.displayMaxNits > 0.0f ?
            TRF("{} (display reports {:.0f} nits)", state.reason, state.displayMaxNits) : std::string(state.reason);
        if (state.display) { ImGui::TextDisabled("%s", status.c_str()); }
        else { ImGui::TextColored(kAmber, "%s", status.c_str()); }
        if (state.gpu.samples)
        {
            ImGui::TextDisabled(TR("Output pass GPU: %.2f ms average, %.2f ms max"), state.gpu.AverageUs() / 1000.0,
                                state.gpu.maxUs / 1000.0);
        }
    }
    const bool windowsKnown = state.windowsSDRWhiteNits >= HDR::kMinimumNits;
    const bool followsWindows = hdr.matchWindowsSDR && windowsKnown;
    // While following Windows, show the values in use; the manual values stay saved for unticking.
    auto effective = HDR::Effective(hdr, state.windowsSDRWhiteNits);
    const char* off = TR("Turn HDR output on to adjust it.");
    if (BeginSettingRows("hdrRows", label))
    {
        SettingRow(TR("Match Windows SDR"), nullptr, false, !hdr.enabled, [&] {
            const bool changed = ImGui::Checkbox("##matchWindows", &hdr.matchWindowsSDR);
            DrawSettingsHelp(!hdr.enabled ? off
                : TR("Uses Windows' SDR content brightness (Settings > Display > HDR) for paper white and UI, so whites "
                     "match the desktop. Untick to set them here."));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            if (hdr.matchWindowsSDR && windowsKnown) { ImGui::TextDisabled(TR("%.0f nits from Windows"), state.windowsSDRWhiteNits); }
            else if (hdr.matchWindowsSDR && state.requested) { ImGui::TextColored(kAmber, "%s", TR("Windows brightness unavailable; using the values below")); }
            return changed;
        });
        SettingRow(TR("Paper white"), nullptr, false, !hdr.enabled || followsWindows, [&] {
            const bool changed = ImGui::SliderFloat("##paperWhite", followsWindows ? &effective.paperWhiteNits : &hdr.paperWhiteNits,
                                                    HDR::kMinimumNits, 500.0f, TR("%.0f nits"));
            DrawSettingsHelp(!hdr.enabled ? off : followsWindows ? TR("Following Windows' SDR brightness.")
                             : TR("Brightness of SDR white in the scene. Start near the Windows SDR content brightness."));
            return changed;
        });
        SettingRow(TR("Peak brightness"), nullptr, false, !hdr.enabled, [&] {
            const char* useDisplay = TR("Display peak");
            const bool offer = state.displayMaxNits > 0.0f;
            if (offer)
            {
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(useDisplay).x -
                                        ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x);
            }
            bool changed = ImGui::SliderFloat("##peak", &hdr.peakNits, effective.paperWhiteNits, 4000.0f, TR("%.0f nits"));
            DrawSettingsHelp(!hdr.enabled ? off : TR("Brightest expanded highlight. Set to your display's peak."));
            if (offer)
            {
                ImGui::SameLine();
                if (ImGui::Button(useDisplay)) { hdr.peakNits = state.displayMaxNits; changed = true; }
                DrawSettingsHelp(TRF("Use the {:.0f} nits your display reports.", state.displayMaxNits).c_str());
            }
            return changed;
        });
        SettingRow(TR("UI brightness"), nullptr, false, !hdr.enabled || followsWindows, [&] {
            const bool changed = ImGui::SliderFloat("##ui", followsWindows ? &effective.uiNits : &hdr.uiNits,
                                                    HDR::kMinimumNits, 500.0f, TR("%.0f nits"));
            DrawSettingsHelp(!hdr.enabled ? off : followsWindows ? TR("Following Windows' SDR brightness.")
                             : TR("Brightness of the menus and HUD."));
            return changed;
        });
        SettingRow(TR("Highlight strength"), nullptr, false, !hdr.enabled, [&] {
            const bool changed = ImGui::SliderFloat("##highlight", &hdr.highlightStrength, 0.0f, 1.0f, "%.2f");
            DrawSettingsHelp(!hdr.enabled ? off : TR("0 keeps the SDR range at paper white; 1 expands the brightest pixels "
                                                     "to peak brightness."));
            return changed;
        });
        SettingRow(TR("Expansion start"), nullptr, false, !hdr.enabled, [&] {
            const bool changed = ImGui::SliderFloat("##expansion", &hdr.expansionStart, 0.1f, 0.95f, "%.2f");
            DrawSettingsHelp(!hdr.enabled ? off
                                          : TR("SDR brightness where expansion begins. Higher values boost only the "
                                               "brightest areas."));
            return changed;
        });
        SettingRow(TR("Transfer"), nullptr, false, !hdr.enabled, [&] {
            int transfer = static_cast<int>(hdr.transfer) != 0 ? 1 : 0;
            const bool changed = Segmented("transfer", transfer, {
                {TR("Gamma 2.2"), true, TR("Matches most SDR monitors.")},
                {"sRGB", true, TR("Lifts shadows compared with Gamma 2.2.")}});
            if (changed) { hdr.transfer = static_cast<HDR::Transfer>(transfer); }
            DrawSettingsHelp(!hdr.enabled ? off : TR("How the SDR image is decoded before expansion."));
            return changed;
        });
        ImGui::EndTable();
    }
    hdr = HDR::Sanitize(hdr);
}

void OverlayUI::DrawImagePanel(float tabCardHeight, const FrameView& view)
{
    if (!ImGui::BeginTabItem(TRID("Image").c_str(), nullptr,
                             requestedPage == SettingsPage::Image ? ImGuiTabItemFlags_SetSelected : 0))
    {
        return;
    }
    if (!BeginSettingsColumns("image", tabCardHeight, view))
    {
        ImGui::EndTabItem();
        return;
    }
    DrawImageMeasurements(view);
    NextSettingsColumn(tabCardHeight);
    if (view.communityShaders)
    {
        DrawSettingsHeading(TR("Controlled by Community Shaders"), "");
        ImGui::TextWrapped("%s", TR("Community Shaders controls upscaling, render scale, model preset, sharpening and camera "
                           "jitter. Change those settings in its menu."));
        DrawSettingsHelp(
            TR("TRP's Neural Rendering, frame generation and Reflex controls remain available in their own tabs."));
        ImGui::Separator();
        DrawSettingsHeading("HDR", "");
        ImGui::TextWrapped("%s", TR("Use Community Shaders' HDR Display for HDR with Community Shaders."));
    }
    else
    {
        const bool warmSwitch = view.sourceDLSSGActive && (view.upscalerStartupMode == DLSS ||
            (view.upscalerStartupMode == XeSS && view.nvidiaServicesReady));
        const bool xess = settingsDraft.upscaleType == XeSS;
        const bool native = settingsDraft.upscaleType == DLAA;
        const float label = ImageLabelWidth();
        if (BeginSettingRows("upscaling", label))
        {
            {
                const RestartHelpScope restart(warmSwitch
                    ? TR("DLSS and XeSS can switch on Apply.\nDLAA needs Save as default and a restart.")
                    : TR("Changing the mode needs Save as default and a restart."));
                SettingRow(TR("Mode"), nullptr, true, false, [&] {
                    int mode = xess ? 2 : native ? 1 : 0;
                    const bool changed = Segmented("mode", mode, {
                        {"DLSS", view.nvidiaFeatures, TR("NVIDIA RTX hardware is required for DLSS.")},
                        {"DLAA", view.nvidiaFeatures, TR("NVIDIA RTX hardware is required for DLAA.")},
                        {TR("XeSS"), true, TR("Intel XeSS (experimental). After restart it uses Intel's quality resolutions.")}});
                    if (changed) { settingsDraft.upscaleType = mode == 2 ? XeSS : mode == 1 ? DLAA : DLSS; }
                    return changed;
                });
            }
            {
                const RestartHelpScope restart(TR("Changing the render scale needs Save as default and a restart."));
                SettingRow(TR("Render scale"), nullptr, true, native, [&] {
                    const char* dlssScales[]{TR("50% | Performance"), TR("58% | Balanced"), TR("67% | Quality"), TR("33% | Ultra Performance"),
                                             TR("78% | Ultra Quality")};
                    const char* xessScales[]{TR("Performance"), TR("Balanced"), TR("Quality"), TR("Ultra Performance"), TR("Ultra Quality")};
                    const auto* scales = xess ? xessScales : dlssScales;
                    bool changed = false;
                    if (ImGui::BeginCombo("##renderScale", native ? TR("100% | Native") : scales[std::clamp(settingsDraft.qualityLevel, 0, 4)]))
                    {
                        for (const int i : {3, 0, 1, 2, 4})
                        {
                            if (ImGui::Selectable(scales[i], settingsDraft.qualityLevel == i) && settingsDraft.qualityLevel != i)
                            {
                                settingsDraft.qualityLevel = i;
                                changed = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    const auto help = view.nrFriendlyRenderSizeActive
                        ? std::format(TR("Size of the rendered world before upscaling.\nNR render size this session: {} x {}.\nPreset changes take effect after restart."), view.renderWidth, view.renderHeight)
                        : std::string(native ? TR("DLAA renders at native resolution.") : TR("Size of the rendered world before upscaling."));
                    DrawSettingsHelp(help.c_str());
                    return changed;
                });
            }
            SettingRow(TR("Model preset"), nullptr, false, xess, [&] {
                bool changed = false;
                if (ImGui::BeginCombo("##dlssPreset", TheosRenderPipeline::DLSSPreset::Label(settingsDraft.dlssPreset)))
                {
                    for (const auto& entry : TheosRenderPipeline::DLSSPreset::entries)
                    {
                        if (ImGui::Selectable(entry.label, entry.value == settingsDraft.dlssPreset) && entry.value != settingsDraft.dlssPreset)
                        {
                            settingsDraft.dlssPreset = entry.value;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                DrawSettingsHelp(xess ? TR("DLSS and DLAA only.")
                    : TRF("{}\nRequested preset; NVIDIA chooses the actual model. L/M require a supporting runtime.",
                                  TR(TheosRenderPipeline::DLSSPreset::Description(settingsDraft.dlssPreset))).c_str());
                return changed;
            });
            SettingRow(TR("Auto exposure"), nullptr, false, xess, [&] {
                const bool changed = ImGui::Checkbox("##autoExposure", &settingsDraft.autoExposure);
                DrawSettingsHelp(xess ? TR("XeSS preserves the SDR exposure supplied by the game and ENB.")
                                      : TR("Lets the upscaler estimate exposure."));
                return changed;
            });
            SettingRow(TR("Camera jitter"), nullptr, false, false, [&] {
                const bool changed = ImGui::Checkbox("##jitter", &settingsDraft.enableJitter);
                DrawSettingsHelp(TR("Sub-pixel camera offsets the upscaler needs to reconstruct detail."));
                return changed;
            });
#if defined(TRP_NO_NEURAL_RENDERING)
            // Without NR there is no Neural Rendering tab to hold sharpening.
            SettingRow(TR("Sharpening"), nullptr, false, false, [&] {
                bool changed = ImGui::Checkbox("##sharpening", &settingsDraft.sharpening);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::BeginDisabled(!settingsDraft.sharpening);
                changed |= ImGui::SliderFloat("##sharpness", &settingsDraft.sharpness, 0, 1, "%.2f");
                ImGui::EndDisabled();
                return changed;
            });
#else
            SettingRow(TR("Sharpening"), nullptr, false, false, [&] {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled(TR("%s | set in Neural Rendering"),
                    settingsDraft.sharpening ? std::format("{:.2f}", settingsDraft.sharpness).c_str() : TR("Off"));
                DrawSettingsHelp(TR("Sharpening is set in the Neural Rendering tab and applies with or without NR."));
                return false;
            });
#endif
            ImGui::EndTable();
        }
        // Upscaler state that follows Apply.
        if (view.sourceDLSSGActive)
        {
            if (view.upscalerLiveModeRejected) {
                ImGui::TextColored(kAmber, "%s", TR("Live switch unavailable; selected mode requires restart"));
            }
            if (view.upscalerNeedsRestart)
            {
                ImGui::TextColored(kAmber, "%s", TR("Mode/render scale awaiting restart"));
            }
            if (view.upscalerFailed)
            {
                ImGui::TextColored(kRust, "%s", TR("Feature update failed; restart required"));
            }
            else if (view.upscalerChangeQueued)
            {
                ImGui::TextDisabled("%s", TR("Feature update queued"));
            }
        }
        else
        {
            ImGui::TextColored(kRust, "%s", TR("NVIDIA host is unavailable."));
        }
        DrawPresetSharpeningStatus(view);
        ImGui::Spacing();
        DrawHDROutputSettings(view);

        if (showDeveloperControls)
        {
            DrawOutputOptimizations();
        }
    }
    if (view.textureProviderAvailable)
    {
        DrawTextureMemoryPanel(view);
    }
    EndSettingsColumns();
    ImGui::EndTabItem();
}

void OverlayUI::DrawTextureMemoryPanel(const FrameView& view)
{
    ImGui::Spacing();
    ImGui::SeparatorText(TR("Texture memory"));

    ImGui::TextDisabled("%s", TR("Applies to future texture loads"));
    ImGui::BeginDisabled(!view.textureProviderAvailable);
    ImGui::Checkbox(TR("Enable runtime mip caps"), &settingsDraft.textureProviderSettings.enabled);
    ImGui::TextDisabled("%s", TR("Preset"));
    const char* texturePresetNames[]{TR("Quality | 2048"), TR("Balanced | 1024"), TR("Performance | 1024 / 512"), TR("Custom")};
    int texturePreset = TexturePresetIndex(settingsDraft.textureProviderSettings);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##texturePreset", &texturePreset, texturePresetNames,
                     static_cast<int>(std::size(texturePresetNames))))
    {
        ApplyTexturePreset(settingsDraft.textureProviderSettings, texturePreset);
    }
    ImGui::Spacing();
    static constexpr const char* kTextureCategories[]{"Diffuse", "Normal", "Parallax", "Material", "Glow", "Mask"};
    if (ImGui::BeginTable("##textureCaps", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn(TR("Type"), ImGuiTableColumnFlags_WidthFixed, Px(105.0f));
        ImGui::TableSetupColumn(TR("Maximum dimension"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        for (std::size_t i = 0; i < settingsDraft.textureProviderSettings.maxSize.size(); ++i)
        {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(TRC(kTextureCategories[i], "OverlayImagePanel"));
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            DrawTextureCapCombo("##textureCap", settingsDraft.textureProviderSettings.maxSize[i]);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (view.textureProviderAvailable && settingsDraft.textureProviderSettings.enabled &&
        !view.textureTelemetry.hooksInstalled)
    {
        ImGui::TextColored(kOchre, "%s", TR("The provider started without hooks. Save the startup default and relaunch "
                                   "to enable texture interception."));
    }
    else
    {
        ImGui::TextDisabled("%s", TR("Folder rules: TextureDownscaler menu"));
        DrawSettingsHelp(TR("Existing textures stay unchanged until reloaded. Folder rules and exclusions are in "
                         "TextureDownscaler's Menu Framework page."));
    }
}
