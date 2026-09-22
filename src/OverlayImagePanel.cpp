#include "CommunityShaderIntegration.h"
#include "DLSSPreset.h"
#include "FrameGen/NvidiaHost.h"
#include "OverlayFrameView.h"
#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "RenderPipeline.h"
#include "VideoMemoryTelemetry.h"
#include <PCH.h>

using namespace TheosRenderPipeline::Overlay;

namespace
{
bool DrawTextureCapCombo(const char* a_label, std::uint32_t& a_cap)
{
    static constexpr const char* kNames[]{"Full size", "512", "1024", "2048", "4096"};
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
    ImGui::SetNextItemWidth(-1.0f);
    if (!ImGui::Combo(a_label, &selected, kNames, static_cast<int>(std::size(kNames))))
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

void OverlayUI::DrawImagePanel(float tabCardHeight, const FrameView& view)
{
    auto* host = NvidiaHost::GetSingleton();
    if (!ImGui::BeginTabItem("Image", nullptr,
                             requestedPage == SettingsPage::Image ? ImGuiTabItemFlags_SetSelected : 0))
    {
        return;
    }
    ImGui::BeginChild("##imagePage", ImVec2(0, tabCardHeight), false);
    DrawSettingsHelp("Resolution, reconstruction and detail");
    if (TheosRenderPipeline::CommunityShaders::Active())
    {
        DrawSettingsHeading("Controlled by Community Shaders", "");
        ImGui::TextWrapped("Community Shaders controls upscaling, render scale, model preset, sharpening and camera "
                           "jitter. Change those settings in its menu.");
        DrawSettingsHelp(
            "TRP's Neural Rendering, frame generation and Reflex controls remain available in their own tabs.");
    }
    else
    {
        DrawSettingsHeading("Resolution", "Mode and scale: save and restart");
        if (ImGui::BeginTable("##resolution", 2, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Mode");
            const char* modes[]{"DLSS", "DLAA"};
            int mode = settingsDraft.upscaleType == DLAA ? 1 : 0;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##mode", &mode, modes, 2))
            {
                settingsDraft.upscaleType = mode ? DLAA : DLSS;
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Render scale");
            const char* scales[]{"50% | Performance", "58% | Balanced", "67% | Quality", "33% | Ultra Performance",
                                 "78% | Ultra Quality"};
            const bool native = settingsDraft.upscaleType == DLAA;
            ImGui::BeginDisabled(native);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##renderScale",
                                  native ? "100% | Native" : scales[std::clamp(settingsDraft.qualityLevel, 0, 4)]))
            {
                for (const int i : {3, 0, 1, 2, 4})
                {
                    if (ImGui::Selectable(scales[i], settingsDraft.qualityLevel == i))
                    {
                        settingsDraft.qualityLevel = i;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::EndTable();
        }
        DrawSettingsHelp(
            "DLAA uses native resolution. Lower DLSS render scales reduce the size of the rendered world.");
        if (view.sourceDLSSGActive)
        {
            const auto& configuration = host->SourceUpscalerSettings();
            ImGui::Text("Active: %s | %ux%u -> %dx%d", ModeName(configuration.Effective().mode), host->RenderWidth(),
                        host->RenderHeight(), view.nativeWidth, view.nativeHeight);
            if (configuration.NeedsRestart())
            {
                ImGui::TextColored(kAmber, "Mode/render scale awaiting restart");
            }
            if (configuration.Failed())
            {
                ImGui::TextColored(kRust, "Feature update failed; restart required");
            }
            else if (configuration.NeedsLiveChange())
            {
                DrawSettingsHelp("Feature update queued after this frame.");
            }
        }
        else
        {
            ImGui::TextColored(kRust, "NVIDIA host is unavailable.");
        }

        DrawSettingsHeading("Reconstruction and detail");
        ImGui::TextUnformatted("DLSS model preset");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##dlssPreset", TheosRenderPipeline::DLSSPreset::Label(settingsDraft.dlssPreset)))
        {
            for (const auto& entry : TheosRenderPipeline::DLSSPreset::entries)
            {
                if (ImGui::Selectable(entry.label, entry.value == settingsDraft.dlssPreset))
                {
                    settingsDraft.dlssPreset = entry.value;
                }
            }
            ImGui::EndCombo();
        }
        DrawSettingsHelp(TheosRenderPipeline::DLSSPreset::Description(settingsDraft.dlssPreset));
        DrawSettingsHelp(
            "Requested preset, not confirmation of the model used by NVIDIA. L/M require a supporting runtime.");
        if (view.sourceDLSSGActive)
        {
            const auto& configuration = host->SourceUpscalerSettings();
            ImGui::Text("Session request: %s | Saved: %s",
                        TheosRenderPipeline::DLSSPreset::ShortName(configuration.Effective().preset),
                        TheosRenderPipeline::DLSSPreset::ShortName(configuration.Persisted().preset));
            if (settingsDraft.dlssPreset != configuration.Requested().preset)
            {
                ImGui::TextColored(kAmber, "Preset selection has not been applied.");
            }
            if (configuration.Unsaved())
            {
                DrawSettingsHelp("Session upscaler settings differ from saved defaults.");
            }
        }
        ImGui::Spacing();
        ImGui::Checkbox("Sharpening", &settingsDraft.sharpening);
        ImGui::BeginDisabled(!settingsDraft.sharpening);
        ImGui::SliderFloat("Strength##sharpness", &settingsDraft.sharpness, 0, 1, "%.2f");
        ImGui::EndDisabled();
        if (ImGui::CollapsingHeader("Advanced image settings"))
        {
            DrawSettingsHelp("Apply required");
            ImGui::Checkbox("Auto exposure", &settingsDraft.autoExposure);
            ImGui::Checkbox("Camera jitter", &settingsDraft.enableJitter);
        }
        if (showDeveloperControls)
        {
            DrawOutputOptimizations();
        }
        else
        {
            DrawSettingsHelp("Output experiments are available through Lab mode in Diagnostics.");
        }
    }
    if (view.textureProviderAvailable)
    {
        DrawTextureMemoryPanel(view);
    }
    ImGui::EndChild();
    ImGui::EndTabItem();
}

void OverlayUI::DrawTextureMemoryPanel(const FrameView& view)
{
    auto* textureProvider = TextureProviderBridge::GetSingleton();
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Texture memory"))
    {
        if (ImGui::CollapsingHeader("Texture provider status"))
        {
            ImGui::TextUnformatted("TEXTURE RESIDENCY");
            ImGui::Separator();
            DrawStatusLabel(view.textureProviderAvailable
                                ? (view.textureTelemetry.hooksInstalled ? "PROVIDER ACTIVE" : "RELAUNCH REQUIRED")
                                : "PROVIDER UNAVAILABLE",
                            view.textureProviderAvailable
                                ? (view.textureTelemetry.hooksInstalled ? UIHealth::kHealthy : UIHealth::kWarning)
                                : UIHealth::kIdle);
            ImGui::Spacing();
            ImGui::Text("Textures reduced: %llu",
                        static_cast<unsigned long long>(view.textureTelemetry.reducedTextures));
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
                               "shown in Diagnostics.");
            ImGui::Spacing();
            ImGui::TextDisabled("%s", textureProvider->Status());
        }
        DrawSettingsHelp("Apply or Save as default; affects future texture loads.");
        ImGui::TextUnformatted("LOAD POLICY");
        ImGui::Separator();
        ImGui::BeginDisabled(!view.textureProviderAvailable);
        ImGui::Checkbox("Enable runtime mip caps", &settingsDraft.textureProviderSettings.enabled);
        ImGui::TextDisabled("Preset");
        const char* texturePresetNames[]{"Quality | 2048", "Balanced | 1024", "Performance | 1024 / 512", "Custom"};
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
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 105.0f);
            ImGui::TableSetupColumn("Maximum dimension", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            for (std::size_t i = 0; i < settingsDraft.textureProviderSettings.maxSize.size(); ++i)
            {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kTextureCategories[i]);
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
            ImGui::TextColored(kOchre, "The provider started without hooks. Save the startup default and relaunch "
                                       "to enable texture interception.");
        }
        else
        {
            ImGui::TextWrapped("Applied limits affect the next load of each texture; resources already resident in "
                               "memory are unchanged. Folder rules and exclusions remain available in "
                               "TextureDownscaler's advanced Menu Framework page.");
        }
    }
}
