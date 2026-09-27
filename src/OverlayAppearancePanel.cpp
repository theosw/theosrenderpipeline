#include <PCH.h>
#include "OverlayUI.h"
#include "OverlayFrameView.h"
#include "OverlayUIStyle.h"
#include "WeatherAppearanceRuntime.h"
#include "WeatherAppearanceINI.h"
#include "CommunityShaderIntegration.h"
#include <SimpleIni.h>
#include <cstdio>

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
std::string WeatherLabel(const Appearance::Record& record, const std::vector<Appearance::WeatherEntry>& catalogue)
{
    for (const auto& entry : catalogue) { if (entry.weather.record == record) { return entry.label; } }
    return Appearance::Valid(record) ? std::format("{} / {:06X}", record.plugin, record.localID) : "Unavailable";
}
void PresetChoice(const char* label, std::uint32_t& id, const Appearance::Settings& settings, bool inherit = true)
{
    const auto* selected = Appearance::FindPreset(settings, id);
    if (ImGui::BeginCombo(label, selected ? selected->name.c_str() : "Inherit")) {
        if (inherit && ImGui::Selectable("Inherit", id == 0)) { id = 0; }
        for (const auto& preset : settings.presets) {
            ImGui::PushID(static_cast<int>(preset.id));
            if (ImGui::Selectable(preset.name.c_str(), id == preset.id)) { id = preset.id; }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}
}

void OverlayUI::DrawAppearancePanel(float height, const FrameView& view)
{
    if (!ImGui::BeginTabItem("Appearance")) { return; }
    if (!BeginSettingsColumns("appearance", height, view)) { ImGui::EndTabItem(); return; }
    auto& runtime = Appearance::Runtime::Get();
    const auto state = runtime.State();
    const auto catalogue = runtime.Catalogue();
    const bool cs = CommunityShaders::Active();
    DrawSettingsHeading("Active appearance", "");
    DrawSettingsValue("Mode", state.paused ? "Manual for this session" : state.result.active ? "Automatic" : "Manual defaults");
    if (state.context.valid) {
        DrawSettingsValue("Location", state.context.interior ? "Interior" : "Exterior / sky lighting");
        DrawSettingsValue("Game time", std::format("{:.2f}", state.context.hour).c_str());
        ImGui::TextWrapped("Weather: %s", WeatherLabel(state.context.incoming.record, *catalogue).c_str());
        if (!state.context.interior && Appearance::Valid(state.context.outgoing.record) && state.context.transition < 1) {
            ImGui::TextWrapped("From: %s", WeatherLabel(state.context.outgoing.record, *catalogue).c_str());
            ImGui::ProgressBar(state.context.transition, ImVec2(-1, 0), "Weather transition");
        }
        ImGui::TextWrapped("NR preset: %s", state.result.incoming.c_str());
        if (state.result.outgoing != state.result.incoming) { ImGui::TextWrapped("NR from: %s", state.result.outgoing.c_str()); }
        if (!cs) {
            ImGui::TextWrapped("Sharpening preset: %s", state.result.incomingSharpening.c_str());
            if (state.result.outgoingSharpening != state.result.incomingSharpening) {
                ImGui::TextWrapped("Sharpening from: %s", state.result.outgoingSharpening.c_str());
            }
        }
        for (std::size_t pass = 0; pass < 2; ++pass) {
            if (pass && settingsDraft.sourceDLSSG.neuralPasses != 2) { break; }
            const auto& nr = state.result.values.passes[pass];
            ImGui::Text("Pass %u: intensity %.2f | tone %.2f | structure %.2f", static_cast<unsigned>(pass + 1), nr.intensity, nr.tone, nr.structure);
        }
        if (!cs) { DrawSettingsValue("Sharpening strength", std::format("{:.3f}", state.result.values.sharpness).c_str()); }
        DrawSettingsHelp("Resolved values follow the NR and sharpening enable controls. Edits on the right take effect with Apply.");
    } else { DrawSettingsHelp("Current weather is available after entering the world."); }
    bool paused = state.paused;
    if (ImGui::Checkbox("Use manual settings for this session", &paused)) { runtime.Pause(paused); }
    DrawSettingsHelp("Takes effect immediately. Tune manual NR and Image settings, then capture them in a preset.");

    NextSettingsColumn(height);
    auto& settings = settingsDraft.appearance;
    DrawSettingsHeading("Weather and time profiles", "Apply / Save as default");
    ImGui::Checkbox("Automatic appearance", &settings.enabled);
    ImGui::SliderFloat("Transition smoothing (seconds)", &settings.smoothingSeconds, 0, 10, "%.1f");
    if (cs) { DrawSettingsHelp("Community Shaders owns sharpening. On this route these presets adjust TRP NR only."); }
    if (ImGui::Button("Create missing starter templates")) {
        if (!Appearance::AddStarterProfiles(settings, EditedValues(settingsDraft))) {
            actionMessage = "Preset limit reached. Remove an unused preset first."; actionMessageIsError = true;
        }
    }
    DrawSettingsHelp("Creates missing group presets from your edited settings at every time point. These are starting templates for visual tuning; existing assignments stay intact.");
    if (ImGui::CollapsingHeader("Broad weather groups", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (std::size_t i = 0; i < Appearance::Groups.size(); ++i) { PresetChoice(Appearance::Groups[i], settings.groups[i], settings); }
        DrawSettingsHelp("Exterior is the fallback. Clear, Cloudy, Rain and Snow follow the weather's classification. Interior is separate. Disabled presets inherit. Fog, ash and special weather can be assigned individually below.");
    }

    ImGui::Separator();
    DrawSettingsHeading("Shared presets", "");
    if (!Appearance::FindPreset(settings, appearancePreset)) { appearancePreset = settings.presets.empty() ? 0 : settings.presets.front().id; }
    PresetChoice("Edit preset", appearancePreset, settings, false);
    ImGui::BeginDisabled(settings.presets.size() >= Appearance::MaxPresets);
    if (ImGui::Button("New from edited NR / Image")) {
        appearancePreset = Appearance::AddPreset(settings, "New preset", Appearance::FromValues(EditedValues(settingsDraft)));
    }
    if (const auto* preset = Appearance::FindPreset(settings, appearancePreset)) {
        ImGui::SameLine();
        if (ImGui::Button("Duplicate")) {
            const auto copy = *preset;
            appearancePreset = Appearance::AddPreset(settings, copy.name + " copy", copy.profile);
        }
    }
    ImGui::EndDisabled();
    auto found = std::ranges::find_if(settings.presets, [&](const auto& item) { return item.id == appearancePreset; });
    if (found != settings.presets.end()) {
        char name[81]{}; std::snprintf(name, sizeof(name), "%s", found->name.c_str());
        if (ImGui::InputText("Preset name", name, sizeof(name))) { found->name = name; }
        auto& profile = found->profile;
        ImGui::Checkbox("Use this preset", &profile.enabled);
        ImGui::Checkbox("Override NR tuning", &profile.neural);
        ImGui::BeginDisabled(cs); ImGui::Checkbox("Override sharpening strength", &profile.sharpening); ImGui::EndDisabled();
        if (ImGui::Button("Copy edited NR / Image to all times")) { profile.points.fill(EditedValues(settingsDraft)); }
        ImGui::Combo("Time point", &appearanceTime, Appearance::Times.data(), static_cast<int>(Appearance::Times.size()));
        auto& point = profile.points[appearanceTime];
        ImGui::Text("At %.2f game hours; blends toward the next point", settings.hours[appearanceTime]);
        if (ImGui::Button("Capture edited values at this time")) { point = EditedValues(settingsDraft); }
        ImGui::SameLine();
        if (ImGui::Button("Copy this time to all")) { profile.points.fill(point); }
        for (int pass = 0; pass < 2; ++pass) {
            ImGui::PushID(pass); ImGui::Text("NR pass %d", pass + 1);
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
        DrawSettingsHelp("Editing a shared preset changes every group and weather assigned to it. Network, style, skin, resolution, placement and pass count remain manual.");
        if (ImGui::CollapsingHeader("Delete preset")) {
            DrawSettingsHelp("Its groups and weather assignments will return to inherited settings. Discard can undo unapplied edits.");
            if (ImGui::Button("Delete this preset and its assignments")) { Appearance::RemovePreset(settings, appearancePreset); appearancePreset = 0; }
        }
    }

    if (ImGui::CollapsingHeader("Assign individual weathers", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("%zu loaded weathers | %zu / %zu assignments | %zu selected", catalogue->size(), settings.weathers.size(), Appearance::MaxWeathers, appearanceSelection.size());
        ImGui::InputTextWithHint("Search weather", "Name, plugin, FormID or classification", appearanceSearch, sizeof(appearanceSearch));
        const char* filters[]{"All", "Unclassified", "Clear", "Cloudy", "Rain", "Snow"};
        ImGui::Combo("Classification", &appearanceWeatherGroup, filters, 6);
        ImGui::Checkbox("Assigned only", &appearanceAssignedOnly);
        const auto query = Appearance::SearchKey(appearanceSearch);
        auto assignment = [&](const Appearance::Record& record) -> const Appearance::NamedProfile* {
            const auto match = std::ranges::find_if(settings.weathers, [&](const auto& entry) { return entry.record == record; });
            return match == settings.weathers.end() ? nullptr : Appearance::FindPreset(settings, match->preset);
        };
        std::vector<const Appearance::WeatherEntry*> filtered;
        for (const auto& entry : *catalogue) {
            if (entry.search.find(query) == std::string::npos ||
                (appearanceWeatherGroup && static_cast<int>(entry.weather.group) != appearanceWeatherGroup - 1) ||
                (appearanceAssignedOnly && !assignment(entry.weather.record))) { continue; }
            filtered.push_back(&entry);
        }
        if (ImGui::Button("Select results")) {
            for (const auto* entry : filtered) {
                if (std::ranges::find(appearanceSelection, entry->weather.record) == appearanceSelection.end()) { appearanceSelection.push_back(entry->weather.record); }
            }
        }
        ImGui::SameLine(); if (ImGui::Button("Clear selection")) { appearanceSelection.clear(); }
        if (ImGui::BeginChild("weatherResults", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 9), true, ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGuiListClipper clipper; clipper.Begin(static_cast<int>(filtered.size()));
            while (clipper.Step()) { for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& entry = *filtered[i];
                const auto selected = std::ranges::find(appearanceSelection, entry.weather.record);
                bool checked = selected != appearanceSelection.end();
                ImGui::PushID(static_cast<int>(entry.runtimeID));
                if (ImGui::Checkbox(entry.label.c_str(), &checked)) {
                    if (checked) { appearanceSelection.push_back(entry.weather.record); } else { appearanceSelection.erase(selected); }
                }
                if (const auto* preset = assignment(entry.weather.record)) { ImGui::SameLine(); ImGui::TextDisabled("-> %s", preset->name.c_str()); }
                ImGui::PopID();
            } }
        }
        ImGui::EndChild();
        ImGui::BeginDisabled(appearanceSelection.empty() || !Appearance::FindPreset(settings, appearancePreset));
        if (ImGui::Button("Assign selected to edited preset")) {
            if (!Appearance::AssignMany(settings, appearanceSelection, appearancePreset)) {
                actionMessage = "Weather assignment limit reached; no assignments were changed."; actionMessageIsError = true;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::BeginDisabled(appearanceSelection.empty());
        if (ImGui::Button("Remove selected assignments")) { Appearance::AssignMany(settings, appearanceSelection, 0); }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!state.context.valid || state.context.interior || !Appearance::Valid(state.context.incoming.record) || !Appearance::FindPreset(settings, appearancePreset));
        if (ImGui::Button("Assign current weather to edited preset")) {
            if (!Appearance::Assign(settings, state.context.incoming.record, appearancePreset)) { actionMessage = "Weather assignment limit reached."; actionMessageIsError = true; }
        }
        ImGui::EndDisabled();
        if (ImGui::Button("Refresh loaded weather list")) { SKSE::GetTaskInterface()->AddTask([] { Appearance::Runtime::Get().CaptureCatalogue(); }); }
        DrawSettingsHelp("Names use editor IDs when the game exposes them; plugin and local FormID are always shown. Selection survives filtering. Assignments use plugin identity, not load-order numbers.");
        if (ImGui::CollapsingHeader("Assignments for unavailable weathers")) {
            for (auto it = settings.weathers.begin(); it != settings.weathers.end();) {
                const bool available = std::ranges::any_of(*catalogue, [&](const auto& entry) { return entry.weather.record == it->record; });
                if (available) { ++it; continue; }
                const auto label = WeatherLabel(it->record, *catalogue);
                ImGui::PushID(label.c_str());
                const bool remove = ImGui::SmallButton("Remove"); ImGui::SameLine(); ImGui::TextUnformatted(label.c_str());
                ImGui::PopID();
                if (remove) { it = settings.weathers.erase(it); } else { ++it; }
            }
        }
    }
    if (ImGui::CollapsingHeader("Time schedule")) {
        for (std::size_t i = 0; i < Appearance::Times.size(); ++i) { ImGui::InputFloat(Appearance::Times[i], &settings.hours[i], .25f, 1, "%.2f"); }
        if (ImGui::Button("Reset time schedule")) { settings.hours = Appearance::DefaultHours; }
        ImGui::SameLine();
        if (ImGui::Button("Copy ENB timing as anchors")) {
            wchar_t executable[MAX_PATH]{};
            const auto length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
            const auto path = std::filesystem::path(executable).parent_path() / L"enbseries.ini";
            CSimpleIniA ini;
            const auto hours = length && length < MAX_PATH && ini.LoadFile(path.c_str()) >= 0 ? Appearance::ReadENBSchedule(ini) : std::nullopt;
            if (hours) { settings.hours = *hours; actionMessage = "Copied ENB clock markers and dawn/dusk boundaries. Review the anchors, then Apply."; actionMessageIsError = false; }
            else { actionMessage = "Could not derive an increasing 24-hour schedule from the game's enbseries.ini. Enter the anchors manually."; actionMessageIsError = true; }
        }
        DrawSettingsHelp("TRP interpolates between these editable anchors. ENB copying uses Night, Sunrise, Day and Sunset markers, plus Sunrise minus DawnDuration and Sunset plus DuskDuration. It does not reproduce ENB's internal phase weights or change ENB settings.");
    }
    EndSettingsColumns();
    ImGui::EndTabItem();
}
