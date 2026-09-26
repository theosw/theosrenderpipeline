#include <PCH.h>
#include "OverlayUI.h"
#include "OverlayFrameView.h"
#include "OverlayUIStyle.h"
#include "WeatherAppearanceRuntime.h"
#include "CommunityShaderIntegration.h"

using namespace TheosRenderPipeline;
using namespace TheosRenderPipeline::Overlay;

namespace
{
Appearance::Values EditedValues(const RendererSettingsDraft& draft)
{
    SourceDLSSG::NeuralOptions options;
    options.tuning = draft.sourceDLSSG.neuralTuning;
    options.reconstruction = draft.sourceDLSSG.neuralReconstruction;
    options.secondPass = draft.sourceDLSSG.neuralSecondPass;
    return Appearance::FromNeural(options, draft.sharpness);
}
std::string WeatherLabel(const Appearance::Record& record)
{
    return Appearance::Valid(record) ? std::format("{} / {:06X}", record.plugin, record.localID) : "Unavailable";
}
}

void OverlayUI::DrawAppearancePanel(float height, const FrameView& view)
{
    if (!ImGui::BeginTabItem("Appearance")) { return; }
    if (!BeginSettingsColumns("appearance", height, view)) { ImGui::EndTabItem(); return; }
    auto& runtime = Appearance::Runtime::Get();
    const auto state = runtime.State();
    const bool cs = CommunityShaders::Active();
    DrawSettingsHeading("Active appearance", "");
    DrawSettingsValue("Mode", state.paused ? "Manual for this session" : state.result.active ? "Automatic" : "Manual defaults");
    if (state.context.valid) {
        DrawSettingsValue("Location", state.context.interior ? "Interior" : "Exterior / sky lighting");
        DrawSettingsValue("Game time", std::format("{:.2f}", state.context.hour).c_str());
        ImGui::TextWrapped("Weather: %s", WeatherLabel(state.context.incoming.record).c_str());
        if (!state.context.interior && Appearance::Valid(state.context.outgoing.record) && state.context.transition < 1) {
            ImGui::TextWrapped("From: %s", WeatherLabel(state.context.outgoing.record).c_str());
            ImGui::ProgressBar(state.context.transition, ImVec2(-1, 0), "Weather transition");
        }
        ImGui::TextWrapped("Profile: %s", state.result.incoming.c_str());
        if (state.result.outgoing != state.result.incoming) {
            ImGui::TextWrapped("Blending from: %s", state.result.outgoing.c_str());
        }
        const auto& values = state.result.values;
        for (std::size_t pass = 0; pass < 2; ++pass) {
            if (pass && settingsDraft.sourceDLSSG.neuralPasses != 2) { break; }
            const auto& nr = values.passes[pass];
            ImGui::Text("Pass %u: intensity %.2f | tone %.2f | structure %.2f",
                static_cast<unsigned>(pass + 1), nr.intensity, nr.tone, nr.structure);
        }
        if (!cs) { DrawSettingsValue("Sharpening strength", std::format("{:.3f}", values.sharpness).c_str()); }
        DrawSettingsHelp("These are the resolved appearance values. NR and sharpening still follow their enable controls.");
    } else { DrawSettingsHelp("Weather information is available after entering the world."); }
    bool paused = state.paused;
    if (ImGui::Checkbox("Use manual settings for this session", &paused)) { runtime.Pause(paused); }
    DrawSettingsHelp("Takes effect immediately. Use the NR and Image tabs to tune manual settings, then capture them in a profile.");

    NextSettingsColumn(height);
    auto& settings = settingsDraft.appearance;
    DrawSettingsHeading("Weather and time profiles", "Apply / Save as default");
    ImGui::Checkbox("Automatic appearance", &settings.enabled);
    DrawSettingsHelp("Profiles blend with Skyrim's weather and game time. Disabled profiles inherit less specific settings.");
    ImGui::SliderFloat("Transition smoothing (seconds)", &settings.smoothingSeconds, 0, 10, "%.1f");
    if (cs) { DrawSettingsHelp("Community Shaders owns sharpening; these profiles adjust TRP's NR tuning only on that route."); }

    const int groupCount = static_cast<int>(Appearance::Groups.size());
    const int count = groupCount + static_cast<int>(settings.weathers.size());
    appearanceProfile = std::clamp(appearanceProfile, 0, count - 1);
    const auto label = [&](int index) -> std::string {
        return index < groupCount ? Appearance::Groups[index] : WeatherLabel(settings.weathers[index - groupCount].record);
    };
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##appearanceProfile", label(appearanceProfile).c_str())) {
        for (int i = 0; i < count; ++i) {
            if (ImGui::Selectable(label(i).c_str(), appearanceProfile == i)) { appearanceProfile = i; }
        }
        ImGui::EndCombo();
    }
    const bool canCaptureWeather = state.context.valid && !state.context.interior && Appearance::Valid(state.context.incoming.record);
    ImGui::BeginDisabled(!canCaptureWeather);
    if (ImGui::Button("Profile for current weather")) {
        const auto found = std::ranges::find_if(settings.weathers, [&](const auto& entry) {
            return entry.record == state.context.incoming.record;
        });
        if (found != settings.weathers.end()) {
            appearanceProfile = groupCount + static_cast<int>(found - settings.weathers.begin());
        } else if (settings.weathers.size() < Appearance::MaxWeathers) {
            settings.weathers.push_back({state.context.incoming.record, Appearance::FromValues(EditedValues(settingsDraft))});
            appearanceProfile = groupCount + static_cast<int>(settings.weathers.size()) - 1;
        } else {
            actionMessage = "The 64 weather profile limit is reached; remove an unused profile first.";
            actionMessageIsError = true;
        }
    }
    ImGui::EndDisabled();
    DrawSettingsHelp("Creates a profile from your edited NR and Image values, or selects its existing profile. Save as default keeps it for the next session.");
    if (appearanceProfile >= groupCount) {
        if (ImGui::Button("Remove this weather profile")) {
            settings.weathers.erase(settings.weathers.begin() + (appearanceProfile - groupCount));
            appearanceProfile = 0;
        }
    }
    auto& profile = appearanceProfile < groupCount ? settings.groups[appearanceProfile] :
        settings.weathers[appearanceProfile - groupCount].profile;
    ImGui::Separator();
    ImGui::TextWrapped("Editing: %s", label(appearanceProfile).c_str());
    ImGui::Checkbox("Use this profile", &profile.enabled);
    ImGui::Checkbox("Override NR tuning", &profile.neural);
    ImGui::BeginDisabled(cs);
    ImGui::Checkbox("Override sharpening strength", &profile.sharpening);
    ImGui::EndDisabled();
    if (ImGui::Button("Copy edited NR / Image settings to all times")) {
        const bool nr = profile.neural, sharpen = profile.sharpening;
        profile = Appearance::FromValues(EditedValues(settingsDraft));
        profile.neural = nr; profile.sharpening = sharpen;
    }
    ImGui::Combo("Time point", &appearanceTime, Appearance::Times.data(), static_cast<int>(Appearance::Times.size()));
    auto& point = profile.points[appearanceTime];
    ImGui::Text("At %.2f game hours; blends toward the next point", settings.hours[appearanceTime]);
    if (ImGui::Button("Capture edited values at this time")) { point = EditedValues(settingsDraft); }
    ImGui::SameLine();
    if (ImGui::Button("Copy this time to all")) { profile.points.fill(point); }
    for (int pass = 0; pass < 2; ++pass) {
        ImGui::PushID(pass);
        ImGui::Text("NR pass %d", pass + 1);
        const bool linked = pass == 1 && settingsDraft.sourceDLSSG.neuralSecondPass.linked;
        ImGui::BeginDisabled(!profile.neural || linked);
        auto& values = point.passes[pass];
        ImGui::SliderFloat("Intensity", &values.intensity, 0, 2);
        ImGui::SliderFloat("Local tone", &values.tone, 0, 2);
        ImGui::SliderFloat("Local structure", &values.structure, 0, 2);
        ImGui::EndDisabled();
        if (linked) { DrawSettingsHelp("Pass 2 follows Pass 1 while linked in the NR tab."); }
        ImGui::PopID();
    }
    ImGui::BeginDisabled(!profile.sharpening || cs);
    ImGui::SliderFloat("Sharpening strength", &point.sharpness, 0, 1, "%.3f");
    ImGui::EndDisabled();
    DrawSettingsHelp("Network, style, skin controls, resolution, placement and pass count remain controlled in the NR tab.");
    if (ImGui::CollapsingHeader("Time schedule")) {
        for (std::size_t i = 0; i < Appearance::Times.size(); ++i) {
            ImGui::InputFloat(Appearance::Times[i], &settings.hours[i], 0.25f, 1.0f, "%.2f");
        }
        if (ImGui::Button("Reset time schedule")) { settings.hours = Appearance::DefaultHours; }
        DrawSettingsHelp("Game hours, in increasing order from Night to Dusk. The cycle wraps through midnight. These times are independent of ENB's schedule.");
    }
    EndSettingsColumns();
    ImGui::EndTabItem();
}
