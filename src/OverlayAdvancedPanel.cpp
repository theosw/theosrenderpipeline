#include "DLSSPreset.h"
#include "OverlayFrameView.h"
#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include <PCH.h>
using namespace TheosRenderPipeline::Overlay;

void OverlayUI::DrawImageMeasurements(const FrameView& view)
{
    const bool cs = view.communityShaders;
    DrawStatusLabel(cs ? "Community Shaders" : ModeName(view.upscaler.mode), view.upscaleHealth);
    DrawSettingsValue("Render", std::format("{} x {}", view.renderWidth, view.renderHeight).c_str());
    DrawSettingsValue("Output", std::format("{} x {}", view.nativeWidth, view.nativeHeight).c_str());
    if (!cs)
        DrawSettingsValue("Preset request", TheosRenderPipeline::DLSSPreset::ShortName(view.upscaler.preset));
    ImGui::Separator();
    DrawStageMeasurements(view, SettingsPage::Image);
    DrawMemoryMeasurements(view);
    if (view.textureProviderAvailable)
    {
        ImGui::SeparatorText("Texture residency");

        DrawStatusLabel(view.textureProviderAvailable
                            ? (view.textureTelemetry.hooksInstalled ? "PROVIDER ACTIVE" : "RELAUNCH REQUIRED")
                            : "PROVIDER UNAVAILABLE",
                        view.textureProviderAvailable
                            ? (view.textureTelemetry.hooksInstalled ? UIHealth::kHealthy : UIHealth::kWarning)
                            : UIHealth::kIdle);
        ImGui::Spacing();
        ImGui::Text("Textures reduced: %llu", static_cast<unsigned long long>(view.textureTelemetry.reducedTextures));
        ImGui::Text("Estimated allocation avoided: %.2f GiB",
                    static_cast<double>(view.textureTelemetry.estimatedBytesAvoided) / (1024.0 * 1024.0 * 1024.0));
        const double nameCoverage = view.textureTelemetry.nameLookups > 0
                                        ? static_cast<double>(view.textureTelemetry.namesResolved) * 100.0 /
                                              static_cast<double>(view.textureTelemetry.nameLookups)
                                        : 0.0;
        ImGui::Text("Texture names resolved: %llu / %llu (%.1f%%)",
                    static_cast<unsigned long long>(view.textureTelemetry.namesResolved),
                    static_cast<unsigned long long>(view.textureTelemetry.nameLookups), nameCoverage);
        ImGui::Spacing();
        ImGui::TextWrapped("The provider lowers the authored top mip before Skyrim allocates eligible file-backed "
                           "textures. Avoided allocation is an independent estimate; actual GPU memory usage is "
                           "shown in the left column.");
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
        DrawSettingsValue("GPU memory", view.memoryStatus.c_str());
        return;
    }
    constexpr double gib = 1024.0 * 1024.0 * 1024.0;
    const auto& memory = view.memorySnapshot;
    const auto headroom = memory.currentUsage < memory.budget ? memory.budget - memory.currentUsage : 0;
    const float pressure = static_cast<float>(static_cast<double>(memory.currentUsage) / memory.budget);
    const UIHealth health = pressure >= 0.92f   ? UIHealth::kError
                            : pressure >= 0.80f ? UIHealth::kWarning
                                                : UIHealth::kHealthy;
    DrawSettingsValue("GPU memory",
                      std::format("{:.2f} / {:.2f} GiB", memory.currentUsage / gib, memory.budget / gib).c_str());
    DrawSettingsHelp("Current usage / Windows GPU memory budget. The budget can be lower than physical VRAM.");
    char label[32]{};
    std::snprintf(label, sizeof(label), "%.1f%% of budget", pressure * 100);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, HealthColor(health));
    ImGui::ProgressBar(std::clamp(pressure, 0.0f, 1.0f), ImVec2(-1, 0), label);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::Text("Budget headroom: %.2f GiB", headroom / gib);
        if (memory.dedicatedCapacity > 0)
            ImGui::Text("Physical dedicated memory: %.2f GiB", memory.dedicatedCapacity / gib);
        ImGui::EndTooltip();
    }
    if (pressure >= 0.80f)
        DrawStatusLabel(pressure >= 0.92f ? "GPU memory budget critical" : "GPU memory budget pressure", health);
}

void OverlayUI::DrawMenuSizeControl()
{
    bool automatic = layout.uiScale <= 0;
    if (ImGui::Checkbox("Automatic zoom", &automatic))
        layout.uiScale = automatic ? 0.0f : UIScale();
    DrawSettingsHelp("Sizes text and controls from the output height; 1080p is 100%. Changes apply immediately. "
                     "Save as default remembers them.");
    // Rebuild the font once on release rather than for every dragged value.
    if (!menuSizeEditing)
        menuSizeEdit = std::round((automatic ? UIScale() : layout.uiScale) * 100.0f);
    ImGui::BeginDisabled(automatic);
    ImGui::SliderFloat("Zoom", &menuSizeEdit, MinUIScale * 100.0f, MaxUIScale * 100.0f, "%.0f%%",
                       ImGuiSliderFlags_AlwaysClamp);
    menuSizeEditing = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit())
        layout.uiScale = SanitizeUIScale(std::round(menuSizeEdit / 5.0f) * 0.05f);
    ImGui::EndDisabled();
    if (!automatic && UIScale() < layout.uiScale - 0.001f)
        ImGui::TextDisabled("Limited to %.0f%% so the menu fits this output.", UIScale() * 100.0f);
    ImGui::Separator();
}

void OverlayUI::DrawMenuKeyControl(const FrameView& view)
{
    menuKeyControlDrawn = true;
    const bool capturing = hotkeys.IsCapturing();
    const auto key = static_cast<UINT>(settingsDraft.menuHotkey);
    ImGui::AlignTextToFramePadding();
    if (settingsDraft.menuHotkey != view.menuHotkey)
        ImGui::TextColored(kAmber, "Menu key");
    else
        ImGui::TextUnformatted("Menu key");
    ImGui::SameLine();
    const auto label = (capturing ? std::string("Press a key...") : HotkeyName(key)) + "###menuKey";
    if (ImGui::Button(label.c_str(), ImVec2(Px(150.0f), 0.0f)) && !capturing)
    {
        hotkeys.BeginCapture();
        menuKeyError.clear();
    }
    DrawSettingsHelp("Click, then press the key that opens and closes this menu. Escape cancels. Apply uses it "
                     "now; Save as default keeps it. Use separate keys for TRP, Community Shaders, ReShade and KreatE.");
    ImGui::SameLine();
    if (capturing)
    {
        if (ImGui::Button("Cancel"))
            hotkeys.CancelCapture();
    }
    else
    {
        ImGui::BeginDisabled(key == VK_END);
        if (ImGui::Button("Reset to End"))
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
        ImGui::TextDisabled("While a text field is active, click outside it before pressing this key.");
    ImGui::PopTextWrapPos();
}

void OverlayUI::DrawAdvancedPanel(float height, const FrameView& view)
{
    if (!ImGui::BeginTabItem("Advanced", nullptr,
                             requestedPage == SettingsPage::Advanced ? ImGuiTabItemFlags_SetSelected : 0))
        return;
    if (BeginSettingsColumns("advanced", height, view))
    {
        const bool cs = view.communityShaders;
        DrawStatusLabel(cs ? "Community Shaders" : "Skyrim / ENB", view.nativeUIHealth);
        DrawSettingsValue("ReShade request",
                          view.reShadeBeforeUpscaling ? "Before upscaling" : "After upscaling");
        ImGui::TextWrapped("%s", view.reShadeStatus.c_str());
        DrawUIStatusPanel(view);
        DrawStageMeasurements(view, SettingsPage::Advanced);
        DrawReportingDetails(view);
        NextSettingsColumn(height);
        int placement = settingsDraft.reShadeBeforeUpscaling ? 0 : 1;
        const char* placements[]{"Before upscaling", "After upscaling"};
        if (ImGui::Combo("ReShade", &placement, placements, 2))
            settingsDraft.reShadeBeforeUpscaling = placement == 0;
        DrawSettingsHelp("Processes world colour/depth before Skyrim UI. Changing placement reloads effects.");
        ImGui::Checkbox("Request loading artwork", &settingsDraft.requestLoadingArtwork);
        DrawSettingsHelp("Requests artwork at the next eligible cell transition; Skyrim selects the image.");
        ImGui::Separator();
        ImGui::Checkbox("Lab mode", &showDeveloperControls);
        DrawSettingsHelp("Show runtime details and experimental controls. This changes menu visibility only.");
        DrawMeasurementControls(view);
        if (showDeveloperControls && !cs && ImGui::CollapsingHeader("UI integration (Lab)"))
        {
            ImGui::TextDisabled("Save and restart");
            ImGui::Checkbox("Native-resolution Skyrim UI", &settingsDraft.nativeUI);
            ImGui::Checkbox("Startup overlays at native resolution", &settingsDraft.lateOverlayBridge);
        }
        ImGui::Separator();
        DrawMenuSizeControl();
        DrawMenuKeyControl(view);
        ImGui::TextUnformatted("HDR unsupported");
        if (cs)
            ImGui::TextWrapped("Keep CS HDR, frame generation and Reflex off.");
        if (showDeveloperControls && !cs && ImGui::CollapsingHeader("Menu diagnostics (Lab)"))
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
    DrawSettingsValue("UI composition", cs              ? "Community Shaders"
                                        : view.nativeUI ? "Native resolution"
                                                        : "Render resolution");
    if (ImGui::CollapsingHeader("UI details"))
    {
        DrawSettingsValue("External overlays", cs                           ? "CS UI path"
                                               : view.hostStartupConfigured ? "Native host"
                                                                            : "Unavailable");
        ImGui::TextWrapped("%s", cs ? "Community Shaders owns the UI render targets."
                                    : "Supported startup overlays use the native foreground target.");
    }
}

void OverlayUI::DrawReportingDetails(const FrameView& view)
{
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Reporting a problem"))
    {
        const auto& upscaler = view.upscaler;
        const auto& neural = view.neuralApplied;
        if (!view.communityShaders)
        {
            ImGui::Text("Upscaling: %s | requested preset %s", ModeName(upscaler.mode),
                        TheosRenderPipeline::DLSSPreset::ShortName(upscaler.preset));
        }
        ImGui::Text("Frame generation: %s | active x%u", view.frameGenerationRuntimeActive ? "active" : "inactive",
                    view.activeDisplayMultiplier);
        ImGui::Text("NR session request: %s | %s upscaling | %d %s", neural.enabled ? "on" : "off",
                    neural.beforeUpscaling ? "before" : "after", neural.passes, neural.passes == 1 ? "pass" : "passes");
        ImGui::Text("NR Pass 1: %.1f%% | network %s", neural.reconstruction.inputScale * 100,
                    neural.reconstruction.preset == 1 ? "Shipping" : "Default");
        if (neural.passes == 2)
        {
            const auto second = neural.EffectiveSecond();
            ImGui::Text("NR Pass 2: %s | %.1f%% | network %s", second.linked ? "linked" : "custom",
                        second.inputScale * 100, second.preset == 1 ? "Shipping" : "Default");
        }
        ImGui::TextWrapped(
            "Include your GPU, driver, game and TRP versions, the active renderer, settings and reproduction steps.");
        ImGui::TextWrapped("Attach TheosRenderPipeline.log from Documents / My Games / Skyrim Special Edition / SKSE.");
    }
}
