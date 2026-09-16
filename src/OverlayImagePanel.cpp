#include <PCH.h>
#include "OverlayUI.h"
#include "OverlayFrameView.h"
#include "OverlayUIStyle.h"
#include "RenderPipeline.h"
#include "FrameGen/NvidiaHost.h"
#include "DLSSPreset.h"
#include "VideoMemoryTelemetry.h"

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
    const auto allEqual = [&](std::uint32_t a_value)
    { return std::ranges::all_of(a_settings.maxSize, [=](std::uint32_t a_cap) { return a_cap == a_value; }); };
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

void OverlayUI::DrawImagePanel(float tabCardHeight, float nestedCardHeight, const FrameView& view)
{
    auto* upscaler = RenderPipeline::GetSingleton();
    auto* nvidiaHost = NvidiaHost::GetSingleton();
    if (ImGui::BeginTabItem("Image", nullptr, requestedPage == SettingsPage::Image ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
    {
        ImGui::BeginChild("##imagePage", ImVec2(0.0f, tabCardHeight), false);
        ImGui::SeparatorText("UPSCALING");
        if (ImGui::BeginTable("##graphicsLayout", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableNextColumn();
            ImGui::BeginChild("##runtimeCard", ImVec2(0.0f, nestedCardHeight), true);
            ImGui::TextUnformatted("RUNTIME");
            ImGui::Separator();
            ImGui::Text("%s%s", ModeName(upscaler->mUpscaleType), upscaler->IsEnabled() ? "" : " (inactive)");
            ImGui::TextDisabled("%d x %d  ->  %d x %d", upscaler->mRenderSizeX, upscaler->mRenderSizeY,
                                view.nativeWidth, view.nativeHeight);
            ImGui::Spacing();
            if (ImGui::BeginTable("##runtimeNumbers", 2, ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Raster-rendered");
                ImGui::TableNextColumn();
                ImGui::Text("%.1f FPS", renderedFps);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", view.outputLabel);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(view.outputText.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Raster frame time");
                ImGui::TableNextColumn();
                ImGui::Text("%.2f ms", view.avgMs);
                ImGui::EndTable();
            }
            ImGui::Spacing();
            ImGui::TextWrapped("Output counts runtime presentations, not physical screen refreshes. Scanout cadence is "
                               "not measured here.");
            ImGui::TextDisabled("RASTER FRAME TIME (MS)");
            ImGui::PlotLines("##frametimes", frameTimesMs, frameTimeCount, frameTimeIndex, nullptr, 0.0f, 50.0f,
                             ImVec2(-1.0f, 100.0f));
            ImGui::EndChild();

            ImGui::TableNextColumn();
            ImGui::BeginChild("##graphicsCard", ImVec2(0.0f, nestedCardHeight), true);
            ImGui::TextUnformatted("GRAPHICS");
            ImGui::Separator();
            const char* typeNames[] = {"DLSS", "DLAA"};
            int typeIndex = settingsDraft.upscaleType == DLAA ? 1 : 0;
            ImGui::TextDisabled("Mode");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##mode", &typeIndex, typeNames, static_cast<int>(std::size(typeNames))))
            {
                settingsDraft.upscaleType = typeIndex == 1 ? DLAA : DLSS;
            }
            struct RenderScaleOption
            {
                const char* percentage;
                const char* mode;
            };
            static constexpr RenderScaleOption scaleOptions[] = {
                {"50%", "Performance"}, {"58%", "Balanced"}, {"67%", "Quality"},
                {"33%", "Ultra Performance"}, {"78%", "Ultra Quality"}};
            const int scaleCount = static_cast<int>(std::size(scaleOptions));
            const int selectedScale = std::clamp(settingsDraft.qualityLevel, 0, scaleCount - 1);
            const bool nativeScale = settingsDraft.upscaleType == DLAA;
            const auto showScaleTooltip = [&](int index)
            {
                ImGui::SetTooltip("NVIDIA %s mode. Renders at approximately %s of output width and height.",
                                  scaleOptions[index].mode, scaleOptions[index].percentage);
            };
            ImGui::TextDisabled("Render scale");
            ImGui::BeginDisabled(nativeScale);
            ImGui::SetNextItemWidth(-1.0f);
            const bool scaleOpen =
                ImGui::BeginCombo("##renderScale", nativeScale ? "100%" : scaleOptions[selectedScale].percentage);
            const bool scaleHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            if (scaleOpen)
            {
                for (int i = 0; i < scaleCount; ++i)
                {
                    const bool selected = settingsDraft.qualityLevel == i;
                    if (ImGui::Selectable(scaleOptions[i].percentage, selected))
                    {
                        settingsDraft.qualityLevel = i;
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                    if (ImGui::IsItemHovered())
                    {
                        showScaleTooltip(i);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            if (scaleHovered && !scaleOpen)
            {
                if (nativeScale)
                {
                    ImGui::SetTooltip("DLAA renders at native resolution (100%).");
                }
                else
                {
                    showScaleTooltip(selectedScale);
                }
            }
            ImGui::TextWrapped("Mode and render scale take effect after saving and restarting. DLAA uses native resolution.");
            if (view.sourceDLSSGActive)
            {
                const auto& configuration = nvidiaHost->SourceUpscalerSettings();
                ImGui::TextWrapped("Active: %s, %ux%u -> %dx%d (%.1f%%).",
                                   configuration.Effective().mode == DLAA ? "DLAA" : "DLSS", nvidiaHost->RenderWidth(),
                                   nvidiaHost->RenderHeight(), view.nativeWidth, view.nativeHeight,
                                   view.proxyScale * 100.0f);
                if (configuration.NeedsRestart())
                {
                    ImGui::TextColored(kAmber, "Mode/render scale change awaiting restart");
                }
                if (configuration.Failed())
                {
                    ImGui::TextColored(kRust, "Feature update failed; restart required");
                }
                else if (configuration.NeedsLiveChange())
                {
                    ImGui::TextDisabled("Feature update queued after this frame");
                }
            }
            else
            {
                ImGui::TextColored(kRust, "NVIDIA host is unavailable.");
            }

            ImGui::TextDisabled("DLSS model preset");
            ImGui::SetNextItemWidth(-1.0f);
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
            if (view.sourceDLSSGActive)
            {
                const auto& configuration = nvidiaHost->SourceUpscalerSettings();
                ImGui::Text("This session: %s", TheosRenderPipeline::DLSSPreset::Label(configuration.Effective().preset));
                ImGui::TextDisabled("Next launch (saved): %s",
                                    TheosRenderPipeline::DLSSPreset::Label(configuration.Persisted().preset));
                if (settingsDraft.dlssPreset != configuration.Requested().preset)
                {
                    ImGui::TextColored(kAmber, "Selected preset has not been applied.");
                }
                if (configuration.Unsaved())
                {
                    ImGui::TextDisabled("Session upscaler settings differ from saved defaults.");
                }
            }
            ImGui::TextDisabled(
                "L and M require a supporting DLSS runtime. This is a requested preset, not a runtime confirmation.");

            ImGui::Checkbox("Sharpening", &settingsDraft.sharpening);
            if (settingsDraft.sharpening)
            {
                ImGui::TextDisabled("Sharpness");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SliderFloat("##sharpness", &settingsDraft.sharpness, 0.0f, 1.0f, "%.2f");
            }
            ImGui::Checkbox("Auto exposure", &settingsDraft.autoExposure);
            ImGui::Checkbox("Camera jitter", &settingsDraft.enableJitter);
            ImGui::EndChild();
            ImGui::EndTable();
        }

        DrawTextureMemoryPanel(nestedCardHeight, view);
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
}

void OverlayUI::DrawTextureMemoryPanel(float nestedCardHeight, const FrameView& view)
{
    auto* textureProvider = TextureProviderBridge::GetSingleton();
    auto* videoMemory = VideoMemoryTelemetry::GetSingleton();
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Texture memory"))
    {
        if (ImGui::BeginTable("##textureLayout", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableNextColumn();
            ImGui::BeginChild("##textureStatus", ImVec2(0.0f, nestedCardHeight), true);
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
            if (view.memorySnapshot.available && view.memorySnapshot.budget > 0)
            {
                constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
                const auto headroomBytes = view.memorySnapshot.currentUsage < view.memorySnapshot.budget
                                               ? view.memorySnapshot.budget - view.memorySnapshot.currentUsage
                                               : 0;
                const float pressure = static_cast<float>(static_cast<double>(view.memorySnapshot.currentUsage) /
                                                          static_cast<double>(view.memorySnapshot.budget));
                const UIHealth memoryHealth = pressure >= 0.92f   ? UIHealth::kError
                                              : pressure >= 0.80f ? UIHealth::kWarning
                                                                  : UIHealth::kHealthy;
                ImGui::Spacing();
                ImGui::SeparatorText("LOCAL GPU MEMORY");
                DrawStatusLabel(pressure >= 0.92f   ? "BUDGET CRITICAL"
                                : pressure >= 0.80f ? "BUDGET PRESSURE"
                                                    : "BUDGET HEALTHY",
                                memoryHealth);
                ImGui::Text("Current usage: %.2f GiB", static_cast<double>(view.memorySnapshot.currentUsage) / kGiB);
                ImGui::Text("Driver budget: %.2f GiB", static_cast<double>(view.memorySnapshot.budget) / kGiB);
                ImGui::Text("Budget headroom: %.2f GiB", static_cast<double>(headroomBytes) / kGiB);
                if (view.memorySnapshot.dedicatedCapacity > 0)
                {
                    ImGui::TextDisabled("Physical dedicated memory: %.2f GiB",
                                        static_cast<double>(view.memorySnapshot.dedicatedCapacity) / kGiB);
                }
                char pressureLabel[32]{};
                std::snprintf(pressureLabel, sizeof(pressureLabel), "%.1f%% of budget", pressure * 100.0f);
                ImGui::ProgressBar(std::clamp(pressure, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), pressureLabel);
            }
            else
            {
                ImGui::Spacing();
                ImGui::SeparatorText("LOCAL GPU MEMORY");
                ImGui::TextDisabled("%s", videoMemory->Status());
            }
            const double nameCoverage = view.textureTelemetry.nameLookups > 0
                                            ? static_cast<double>(view.textureTelemetry.namesResolved) * 100.0 /
                                                  static_cast<double>(view.textureTelemetry.nameLookups)
                                            : 0.0;
            ImGui::Text("Texture names resolved: %llu / %llu (%.1f%%)",
                        static_cast<unsigned long long>(view.textureTelemetry.namesResolved),
                        static_cast<unsigned long long>(view.textureTelemetry.nameLookups), nameCoverage);
            ImGui::Spacing();
            ImGui::TextWrapped("The provider lowers the authored top mip before Skyrim allocates eligible file-backed "
                               "textures. The DXGI budget above is the process's observed local-memory state; avoided "
                               "allocation is the provider's independent estimate and is not added to it.");
            ImGui::Spacing();
            ImGui::TextDisabled("%s", textureProvider->Status());
            ImGui::EndChild();

            ImGui::TableNextColumn();
            ImGui::BeginChild("##texturePolicy", ImVec2(0.0f, nestedCardHeight), true);
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
            static constexpr const char* kTextureCategories[]{"Diffuse",  "Normal", "Parallax",
                                                              "Material", "Glow",   "Mask"};
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
            ImGui::EndChild();
            ImGui::EndTable();
        }
    }
}
