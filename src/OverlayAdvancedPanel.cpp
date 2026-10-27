#include "DLSSPreset.h"
#include "Localization.h"
#include "OverlayFrameView.h"
#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "OverlaySettingRows.h"
#include <PCH.h>
using namespace TheosRenderPipeline::Overlay;

void OverlayUI::DrawImageMeasurements(const FrameView& view)
{
    const bool cs = view.communityShaders;
    DrawStatusLabel(cs ? "Community Shaders" : ModeName(view.upscaler.mode), view.upscaleHealth);  // not translated: product names
    DrawSettingsValue(TR("Render"), std::format("{} x {}", view.renderWidth, view.renderHeight).c_str());
    DrawSettingsValue(TR("Output"), std::format("{} x {}", view.nativeWidth, view.nativeHeight).c_str());
    if (!cs && !view.xessActive)
        DrawSettingsValue(TR("Preset request"), TheosRenderPipeline::DLSSPreset::ShortName(view.upscaler.preset));
    ImGui::Separator();
    DrawStageMeasurements(view, SettingsPage::Image);
    DrawMemoryMeasurements(view);
    if (view.textureProviderAvailable)
    {
        ImGui::SeparatorText(TR("Texture residency"));

        DrawStatusLabel(view.textureProviderAvailable
                            ? (view.textureTelemetry.hooksInstalled ? TR("PROVIDER ACTIVE") : TR("RELAUNCH REQUIRED"))
                            : TR("PROVIDER UNAVAILABLE"),
                        view.textureProviderAvailable
                            ? (view.textureTelemetry.hooksInstalled ? UIHealth::kHealthy : UIHealth::kWarning)
                            : UIHealth::kIdle);
        ImGui::Spacing();
        ImGui::Text(TR("Textures reduced: %llu"), static_cast<unsigned long long>(view.textureTelemetry.reducedTextures));
        ImGui::Text(TR("Estimated allocation avoided: %.2f GiB"),
                    static_cast<double>(view.textureTelemetry.estimatedBytesAvoided) / (1024.0 * 1024.0 * 1024.0));
        const double nameCoverage = view.textureTelemetry.nameLookups > 0
                                        ? static_cast<double>(view.textureTelemetry.namesResolved) * 100.0 /
                                              static_cast<double>(view.textureTelemetry.nameLookups)
                                        : 0.0;
        ImGui::Text(TR("Texture names resolved: %llu / %llu (%.1f%%)"),
                    static_cast<unsigned long long>(view.textureTelemetry.namesResolved),
                    static_cast<unsigned long long>(view.textureTelemetry.nameLookups), nameCoverage);
        ImGui::Spacing();
        ImGui::TextWrapped("%s", TR("The provider lowers the authored top mip before Skyrim allocates eligible "
                                "file-backed textures. Avoided allocation is an independent estimate; actual GPU "
                                "memory usage is shown in the left column."));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", view.textureProviderStatus.c_str());
    }
    if (showDeveloperControls)
    {
        DrawLabImageDetails(view);
    }
}

void OverlayUI::DrawMemoryMeasurements(const FrameView& view)
{
    ImGui::Separator();
    if (!view.memorySnapshot.available || view.memorySnapshot.budget == 0)
    {
        DrawSettingsValue(TR("GPU memory"), view.memoryStatus.c_str());
        return;
    }
    constexpr double gib = 1024.0 * 1024.0 * 1024.0;
    const auto& memory = view.memorySnapshot;
    const auto headroom = memory.currentUsage < memory.budget ? memory.budget - memory.currentUsage : 0;
    const float pressure = static_cast<float>(static_cast<double>(memory.currentUsage) / memory.budget);
    const UIHealth health = pressure >= 0.92f   ? UIHealth::kError
                            : pressure >= 0.80f ? UIHealth::kWarning
                                                : UIHealth::kHealthy;
    DrawSettingsValue(TR("GPU memory"),
                      TRF("{:.2f} / {:.2f} GiB", memory.currentUsage / gib, memory.budget / gib).c_str());
    DrawSettingsHelp(TR("Current usage / Windows GPU memory budget. The budget can be lower than physical VRAM."));
    char label[32]{};
    std::snprintf(label, sizeof(label), TR("%.1f%% of budget"), pressure * 100);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, HealthColor(health));
    ImGui::ProgressBar(std::clamp(pressure, 0.0f, 1.0f), ImVec2(-1, 0), label);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::Text(TR("Budget headroom: %.2f GiB"), headroom / gib);
        if (memory.dedicatedCapacity > 0)
            ImGui::Text(TR("Physical dedicated memory: %.2f GiB"), memory.dedicatedCapacity / gib);
        ImGui::EndTooltip();
    }
    if (pressure >= 0.80f)
        DrawStatusLabel(pressure >= 0.92f ? TR("GPU memory budget critical") : TR("GPU memory budget pressure"), health);
}

void OverlayUI::DrawMenuSizeControl()
{
    bool automatic = layout.uiScale <= 0;
    if (ImGui::Checkbox(TR("Automatic zoom"), &automatic))
        layout.uiScale = automatic ? 0.0f : UIScale();
    DrawSettingsHelp(TR("Sizes text and controls from the output height; 1080p is 100%. Changes apply immediately. "
                     "Save as default remembers them."));
    // Rebuild the font once on release rather than for every dragged value.
    if (!menuSizeEditing)
        menuSizeEdit = std::round((automatic ? UIScale() : layout.uiScale) * 100.0f);
    ImGui::BeginDisabled(automatic);
    ImGui::SliderFloat(TR("Zoom"), &menuSizeEdit, MinUIScale * 100.0f, MaxUIScale * 100.0f, "%.0f%%",
                       ImGuiSliderFlags_AlwaysClamp);
    menuSizeEditing = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit())
        layout.uiScale = SanitizeUIScale(std::round(menuSizeEdit / 5.0f) * 0.05f);
    ImGui::EndDisabled();
    if (!automatic && UIScale() < layout.uiScale - 0.001f)
        ImGui::TextDisabled(TR("Limited to %.0f%% so the menu fits this output."), UIScale() * 100.0f);
    ImGui::Separator();
}

void OverlayUI::DrawMenuKeyControl(const FrameView& view)
{
    menuKeyControlDrawn = true;
    const bool capturing = hotkeys.IsCapturing();
    const auto key = static_cast<UINT>(settingsDraft.menuHotkey);
    ImGui::AlignTextToFramePadding();
    if (settingsDraft.menuHotkey != view.menuHotkey)
        ImGui::TextColored(kAmber, "%s", TR("Menu key"));
    else
        ImGui::TextUnformatted(TR("Menu key"));
    ImGui::SameLine();
    const auto label = (capturing ? std::string(TR("Press a key...")) : HotkeyName(key)) + "###menuKey";
    if (ImGui::Button(label.c_str(), ImVec2(Px(150.0f), 0.0f)) && !capturing)
    {
        hotkeys.BeginCapture();
        menuKeyError.clear();
    }
    DrawSettingsHelp(TR("Click, then press the key that opens and closes this menu. Escape cancels. Apply uses "
                        "it now; Save as default keeps it. Use separate keys for TRP, Community Shaders, "
                        "ReShade and KreatE."));
    ImGui::SameLine();
    if (capturing)
    {
        if (ImGui::Button(TR("Cancel")))
            hotkeys.CancelCapture();
    }
    else
    {
        ImGui::BeginDisabled(key == VK_END);
        if (ImGui::Button(TR("Reset to End")))
        {
            settingsDraft.menuHotkey = VK_END;
            menuKeyError.clear();
        }
        ImGui::EndDisabled();
    }
    ImGui::PushTextWrapPos(0);
    if (!menuKeyError.empty())
        ImGui::TextColored(kRust, "%s", menuKeyError.c_str());
    else if (!CanToggleWhileEditing(key))
        ImGui::TextDisabled("%s", TR("While a text field is active, click outside it before pressing this key."));
    ImGui::PopTextWrapPos();
}

void OverlayUI::DrawAdvancedPanel(float height, const FrameView& view)
{
    if (!ImGui::BeginTabItem(TRID("Advanced").c_str(), nullptr,
                             requestedPage == SettingsPage::Advanced ? ImGuiTabItemFlags_SetSelected : 0))
        return;
    if (BeginSettingsColumns("advanced", height, view))
    {
        const bool cs = view.communityShaders;
        DrawStatusLabel(cs ? "Community Shaders" : "Skyrim / ENB", view.nativeUIHealth);  // not translated: product names
        DrawSettingsValue(TR("ReShade request"),
                          view.reShadeBeforeUpscaling ? TR("Before upscaling") : TR("After upscaling"));
        ImGui::TextWrapped("%s", view.reShadeStatus.c_str());
        DrawUIStatusPanel(view);
        DrawStageMeasurements(view, SettingsPage::Advanced);
        DrawReportingDetails(view);
        NextSettingsColumn(height);
        int placement = settingsDraft.reShadeBeforeUpscaling ? 0 : 1;
        const char* placements[]{TR("Before upscaling"), TR("After upscaling")};
        if (ImGui::Combo("ReShade", &placement, placements, 2))
            settingsDraft.reShadeBeforeUpscaling = placement == 0;
        DrawSettingsHelp(TR("Processes world colour/depth before Skyrim UI. Changing placement reloads effects."));
        ImGui::Checkbox(TR("Request loading artwork"), &settingsDraft.requestLoadingArtwork);
        DrawSettingsHelp(TR("Requests artwork at the next eligible cell transition; Skyrim selects the image."));
        ImGui::Separator();
        ImGui::Checkbox(TR("Lab mode"), &showDeveloperControls);
        DrawSettingsHelp(TR("Show runtime details and experimental controls. This changes menu visibility only."));
        DrawMeasurementControls(view);
        if (showDeveloperControls && !cs && ImGui::CollapsingHeader(TRID("UI integration (Lab)").c_str()))
        {
            ImGui::TextDisabled("%s", TR("Save and restart"));
            ImGui::Checkbox(TR("Native-resolution Skyrim UI"), &settingsDraft.nativeUI);
            ImGui::Checkbox(TR("Startup overlays at native resolution"), &settingsDraft.lateOverlayBridge);
        }
        ImGui::Separator();
        DrawMenuSizeControl();
        DrawMenuKeyControl(view);
        ImGui::TextUnformatted(TR("HDR unsupported"));
        if (cs)
            ImGui::TextWrapped("%s", TR("Keep CS HDR, frame generation and Reflex off."));
        if (showDeveloperControls && !cs && ImGui::CollapsingHeader(TRID("Menu diagnostics (Lab)").c_str()))
        {
            DrawLabMenuDiagnostics();
        }
        EndSettingsColumns();
    }
    ImGui::EndTabItem();
}

void OverlayUI::DrawUIStatusPanel(const FrameView& view)
{
    const bool cs = view.communityShaders;
    DrawSettingsValue(TR("UI composition"), cs              ? "Community Shaders"  // not translated: product names
                                        : view.nativeUI ? TR("Native resolution")
                                                        : TR("Render resolution"));
    if (ImGui::CollapsingHeader(TRID("UI details").c_str()))
    {
        DrawSettingsValue(TR("External overlays"), cs                           ? TR("CS UI path")
                                               : view.hostStartupConfigured ? TR("Native host")
                                                                            : TR("Unavailable"));
        ImGui::TextWrapped("%s", cs ? TR("Community Shaders owns the UI render targets.")
                                    : TR("Supported startup overlays use the native foreground target."));
    }
}

void OverlayUI::DrawReportingDetails(const FrameView& view)
{
    ImGui::Spacing();
    if (ImGui::CollapsingHeader(TRID("Reporting a problem").c_str()))
    {
        const auto& upscaler = view.upscaler;
        const auto& neural = view.neuralApplied;
        if (!view.communityShaders)
        {
            ImGui::Text(TR("Upscaling: %s | requested preset %s"), ModeName(upscaler.mode),
                        TheosRenderPipeline::DLSSPreset::ShortName(upscaler.preset));
        }
        ImGui::Text(TR("Frame generation: %s | active x%u"), view.frameGenerationRuntimeActive ? TR("active") : TR("inactive"),
                    view.activeDisplayMultiplier);
        ImGui::Text(TR("NR session request: %s | %s upscaling | %d %s"), neural.enabled ? TRC("on", "OverlayAdvancedPanel") : TRC("off", "OverlayAdvancedPanel"),
                    neural.beforeUpscaling ? TR("before") : TR("after"), neural.passes, neural.passes == 1 ? TR("pass") : TR("passes"));
        ImGui::Text(TR("NR Pass 1: %.1f%% | network %s"), neural.reconstruction.inputScale * 100,
                    NeuralNetworkName(neural.reconstruction.preset));
        DrawSettingsValue(TR("Submitted network"), NeuralSubmittedNetwork(view.neuralState, 0).c_str());
        for (int pass = 2; pass <= neural.passes; ++pass)
        {
            const auto extra = neural.EffectiveExtra(static_cast<std::size_t>(pass - 2));
            ImGui::Text(TR("NR Pass %d: %s | %.1f%% | network %s"), pass, extra.linked ? TR("linked") : TR("custom"),
                        extra.inputScale * 100, NeuralNetworkName(extra.preset));
            DrawSettingsValue(TR("Submitted network"), NeuralSubmittedNetwork(view.neuralState, pass - 1).c_str());
        }
        ImGui::TextWrapped("%s", TR("Include your GPU, driver, game and TRP versions, the active renderer, settings and reproduction steps."));
        ImGui::TextWrapped("%s", TR("Attach TheosRenderPipeline.log from Documents / My Games / Skyrim Special Edition / SKSE."));
    }
}
