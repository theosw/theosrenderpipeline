#include <PCH.h>
#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "WeatherAppearanceRuntime.h"
#include "WeatherAppearanceINI.h"
#include "CommunityShaderIntegration.h"
#include <SimpleIni.h>
#include <cstdio>
#include <optional>
#include <unordered_map>

using namespace TheosRenderPipeline;
using namespace TheosRenderPipeline::Overlay;

namespace
{
// presetSelected uses 0 for Base; stored preset IDs are 1..0x7FFFFFFF.
constexpr std::uint32_t BaseRow = 0;
constexpr std::array<const char*, 6> GroupLabels{"Outdoors", "Clear", "Cloudy", "Rain", "Snow", "Interior"};

Appearance::Values EditedValues(const RendererSettingsDraft& draft)
{
    SourceDLSSG::NeuralOptions options;
    options.tuning = draft.sourceDLSSG.neuralTuning;
    options.reconstruction = draft.sourceDLSSG.neuralReconstruction;
    options.secondPass = draft.sourceDLSSG.neuralSecondPass;
    return Appearance::FromNeural(options, draft.sharpness);
}
const Appearance::WeatherEntry* FindWeather(const Appearance::Record& record, const std::vector<Appearance::WeatherEntry>& catalogue)
{
    for (const auto& entry : catalogue) { if (entry.weather.record == record) { return &entry; } }
    return nullptr;
}
std::string WeatherLabel(const Appearance::Record& record, const std::vector<Appearance::WeatherEntry>& catalogue)
{
    if (const auto* entry = FindWeather(record, catalogue)) { return entry->label; }
    return Appearance::Valid(record) ? std::format("{} / {:06X}", record.plugin, record.localID) : "Unavailable";
}
std::string ShortWeatherLabel(const Appearance::Record& record, const std::vector<Appearance::WeatherEntry>& catalogue)
{
    const auto* entry = FindWeather(record, catalogue);
    return entry && !entry->name.empty() ? entry->name : std::format("{} / {:06X}", record.plugin, record.localID);
}
// The controller names the no-preset fallback "Manual defaults"; the UI calls it Base.
std::string DisplayName(const std::string& name) { return name == "Manual defaults" ? "Base" : name; }
std::string Clock(float hour)
{
    const int minutes = static_cast<int>(std::round(Appearance::Hour(hour) * 60)) % (24 * 60);
    return std::format("{:02}:{:02}", minutes / 60, minutes % 60);
}
// The anchor the current hour blends away from, matching Appearance::AtTime.
std::size_t TimeIndex(const std::array<float, 6>& hours, float hour)
{
    const float time = Appearance::Hour(hour);
    const float sample = time < hours[0] ? time + 24 : time;
    for (std::size_t i = 0; i < hours.size(); ++i) {
        const auto next = (i + 1) % hours.size();
        if (sample >= hours[i] && sample < (next ? hours[next] : hours[0] + 24)) { return i; }
    }
    return 0;
}
std::string UsedWhen(const Appearance::Settings& settings, const Appearance::NamedProfile& preset, std::size_t weathers)
{
    std::string text;
    for (std::size_t i = 0; i < GroupLabels.size(); ++i) {
        if (settings.groups[i] == preset.id) { text += std::format("{}{}", text.empty() ? "" : ", ", GroupLabels[i]); }
    }
    if (weathers) { text += std::format("{}{} weather{}", text.empty() ? "" : " + ", weathers, weathers == 1 ? "" : "s"); }
    if (text.empty()) { text = "Not used yet"; }
    return preset.profile.enabled ? text : "Off | " + text;
}
void Note(const char* text) { ImGui::TextDisabled("%s", text); }
void Tooltip(const char* text)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { ImGui::SetTooltip("%s", text); }
}
// Two-line list row: name, then when it applies. Returns true when clicked.
bool PresetRow(int id, const char* name, const std::string& detail, bool selected, bool now, bool off)
{
    const auto& style = ImGui::GetStyle();
    const float line = ImGui::GetTextLineHeight();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = line * 2 + style.FramePadding.y * 3;
    const auto pos = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg));
    ImGui::PushID(id);
    const bool clicked = ImGui::Selectable("##row", selected, 0, ImVec2(width, height));
    ImGui::PopID();
    draw->AddRectFilled(pos, ImVec2(pos.x + 3, pos.y + height), ImGui::GetColorU32(selected ? kAmber : kAmberDim));
    const float text = pos.x + 3 + style.FramePadding.x * 2;
    float right = pos.x + width - style.FramePadding.x;
    if (now) {
        right -= ImGui::CalcTextSize("NOW").x;
        draw->AddText(ImVec2(right, pos.y + style.FramePadding.y), ImGui::GetColorU32(kSage), "NOW");
        right -= style.ItemSpacing.x;
    }
    draw->PushClipRect(pos, ImVec2(right, pos.y + height), true);
    draw->AddText(ImVec2(text, pos.y + style.FramePadding.y), ImGui::GetColorU32(off ? kMuted : kIvory), name);
    draw->PopClipRect();
    draw->PushClipRect(pos, ImVec2(pos.x + width - style.FramePadding.x, pos.y + height), true);
    draw->AddText(ImVec2(text, pos.y + style.FramePadding.y * 2 + line), ImGui::GetColorU32(off ? kMuted : kAmber), detail.c_str());
    draw->PopClipRect();
    return clicked;
}
void NeuralSliders(Appearance::Neural& values)
{
    ImGui::SliderFloat("Intensity", &values.intensity, 0, 2);
    ImGui::SliderFloat("Local tone", &values.tone, 0, 2);
    ImGui::SliderFloat("Local structure", &values.structure, 0, 2);
}
}

bool OverlayUI::PresetEditorSelected() const
{
    return presetSelected != BaseRow && Appearance::FindPreset(settingsDraft.appearance, presetSelected);
}

void OverlayUI::DrawPresetList()
{
    auto& runtime = Appearance::Runtime::Get();
    const auto state = runtime.State();
    const auto catalogue = runtime.Catalogue();
    const bool cs = CommunityShaders::Active();
    auto& settings = settingsDraft.appearance;
    const auto& scene = state.context;

    DrawSettingsHeading("Presets");
    Note("Presets change NR and sharpening for the weather or places you choose. Everywhere else uses Base.");
    if (scene.valid && !settings.presets.empty()) {
        std::string applied = state.paused ? "Base (paused)" : DisplayName(state.result.incoming);
        if (!state.paused && !cs && state.result.incomingSharpening != state.result.incoming) {
            applied = std::format("{} | sharpening {}", applied, DisplayName(state.result.incomingSharpening));
        }
        const auto place = scene.interior ? std::string("Interior") : ShortWeatherLabel(scene.incoming.record, *catalogue);
        ImGui::Text("Now: %s | %s | %s", applied.c_str(), Clock(scene.hour).c_str(), place.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (!scene.interior) { ImGui::TextUnformatted(WeatherLabel(scene.incoming.record, *catalogue).c_str()); }
            ImGui::Text("%s, blending toward %s", Appearance::Times[TimeIndex(settings.hours, scene.hour)],
                Appearance::Times[(TimeIndex(settings.hours, scene.hour) + 1) % Appearance::Times.size()]);
            for (std::size_t pass = 0; pass < 2; ++pass) {
                if (pass && settingsDraft.sourceDLSSG.neuralPasses != 2) { break; }
                const auto& nr = state.result.values.passes[pass];
                ImGui::Text("Pass %u: intensity %.2f | tone %.2f | structure %.2f", static_cast<unsigned>(pass + 1), nr.intensity, nr.tone, nr.structure);
            }
            if (!cs) { ImGui::Text("Sharpening strength %.3f", state.result.values.sharpness); }
            ImGui::EndTooltip();
        }
        if (!scene.interior && !state.paused && Appearance::Valid(scene.outgoing.record) && scene.transition < 1) {
            ImGui::TextDisabled("Changing from %s", DisplayName(state.result.outgoing).c_str());
            ImGui::SameLine();
            ImGui::ProgressBar(scene.transition, ImVec2(120, 0), "");
        }
    }
    if (!settings.presets.empty()) {
        bool paused = state.paused;
        if (ImGui::Checkbox("Pause presets", &paused)) { runtime.Pause(paused); }
        Tooltip("Uses Base right away, until you untick this or restart the game. Not saved.");
    }

    // Mark what the edited configuration would select for the current weather.
    std::uint32_t nowNeural = BaseRow, nowSharpening = BaseRow;
    bool baseNow = false;
    if (scene.valid) {
        if (!Appearance::UsesPresets(settings) || state.paused) { baseNow = true; }
        else {
            const auto selection = Appearance::Select(settings, scene.incoming, scene.interior);
            for (const auto& preset : settings.presets) {
                if (&preset.profile == selection.neural) { nowNeural = preset.id; }
                if (&preset.profile == selection.sharpening) { nowSharpening = preset.id; }
            }
            baseNow = !selection.neural || (!cs && !selection.sharpening);
        }
    }
    if (!PresetEditorSelected()) { presetSelected = BaseRow; }

    ImGui::Spacing();
    if (PresetRow(-1, "Base", "Your Neural Rendering settings", presetSelected == BaseRow, baseNow, false)) { presetSelected = BaseRow; }
    std::unordered_map<std::uint32_t, std::size_t> weatherCounts;
    for (const auto& entry : settings.weathers) { ++weatherCounts[entry.preset]; }
    const float rowHeight = ImGui::GetTextLineHeight() * 2 + ImGui::GetStyle().FramePadding.y * 3;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(settings.presets.size()), rowHeight + ImGui::GetStyle().ItemSpacing.y);
    while (clipper.Step()) { for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const auto& preset = settings.presets[i];
        const bool now = preset.id == nowNeural || (!cs && preset.id == nowSharpening);
        const auto count = weatherCounts.find(preset.id);
        if (PresetRow(static_cast<int>(preset.id), preset.name.c_str(), UsedWhen(settings, preset, count == weatherCounts.end() ? 0 : count->second),
                presetSelected == preset.id, now, !preset.profile.enabled)) { presetSelected = preset.id; }
    } }

    const bool full = settings.presets.size() >= Appearance::MaxPresets;
    ImGui::BeginDisabled(full);
    if (ImGui::Button("Create new preset")) {
        const auto name = settings.presets.empty() ? "My first preset" : "New preset";
        if (const auto id = Appearance::AddPreset(settings, name, Appearance::FromValues(EditedValues(settingsDraft)))) { presetSelected = id; }
    }
    Tooltip("Starts from your Base values. Choose when it applies, then Apply.");
    if (const auto* selected = Appearance::FindPreset(settings, presetSelected)) {
        ImGui::SameLine();
        if (ImGui::Button("Duplicate")) {
            const auto copy = *selected;
            if (const auto id = Appearance::AddPreset(settings, copy.name + " copy", copy.profile)) { presetSelected = id; }
        }
        Tooltip("Copies the selected preset's values. Choose when the copy applies.");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Timing...")) { ImGui::OpenPopup("presetTiming"); }
    Tooltip("When each time of day starts, and how smoothly presets change.");
    if (ImGui::BeginPopup("presetTiming")) {
        ImGui::PushItemWidth(220);
        ImGui::SliderFloat("Transition smoothing (seconds)", &settings.smoothingSeconds, 0, 10, "%.1f");
        ImGui::TextDisabled("Softens changes between presets. Zero follows weather and time directly.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Time of day starts (game hours)");
        for (std::size_t i = 0; i < Appearance::Times.size(); ++i) { ImGui::InputFloat(Appearance::Times[i], &settings.hours[i], .25f, 1, "%.2f"); }
        ImGui::PopItemWidth();
        if (ImGui::Button("Reset times")) { settings.hours = Appearance::DefaultHours; }
        ImGui::SameLine();
        if (ImGui::Button("Copy ENB timing")) {
            wchar_t executable[MAX_PATH]{};
            const auto length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
            const auto path = std::filesystem::path(executable).parent_path() / L"enbseries.ini";
            CSimpleIniA ini;
            const auto hours = length && length < MAX_PATH && ini.LoadFile(path.c_str()) >= 0 ? Appearance::ReadENBSchedule(ini) : std::nullopt;
            if (hours) { settings.hours = *hours; actionMessage = "Copied ENB clock markers and dawn/dusk boundaries. Review the times, then Apply."; actionMessageIsError = false; }
            else { actionMessage = "Could not derive an increasing 24-hour schedule from the game's enbseries.ini. Enter the times manually."; actionMessageIsError = true; }
        }
        Tooltip("Uses ENB's Night, Sunrise, Day and Sunset markers, plus Sunrise minus DawnDuration and Sunset plus DuskDuration. "
            "It does not reproduce ENB's internal phase weights or change ENB settings.");
        ImGui::TextDisabled("Times must increase from Night to Dusk. Presets blend between neighbouring times.");
        ImGui::EndPopup();
    }
    settings.enabled = Appearance::UsesPresets(settings);
}

void OverlayUI::DrawPresetEditor()
{
    auto& runtime = Appearance::Runtime::Get();
    const auto state = runtime.State();
    const auto catalogue = runtime.Catalogue();
    const bool cs = CommunityShaders::Active();
    auto& settings = settingsDraft.appearance;
    const auto& scene = state.context;
    const auto& nr = settingsDraft.sourceDLSSG;
    auto found = std::ranges::find_if(settings.presets, [&](const auto& item) { return item.id == presetSelected; });
    if (found == settings.presets.end()) { return; }
    auto& preset = *found;
    auto& profile = preset.profile;
    const auto nowIndex = scene.valid ? TimeIndex(settings.hours, scene.hour) : Appearance::Times.size();
    if (presetShown != preset.id) {
        presetShown = preset.id;
        presetTimed = std::ranges::any_of(profile.points, [&](const auto& point) { return point != profile.points[0]; });
        presetTime = static_cast<int>(nowIndex < Appearance::Times.size() ? nowIndex : 3);
        presetPickerSelection.clear();
    }

    DrawSettingsHeading(preset.name.c_str());
    Note("Replaces Base's values below whenever this preset applies. Everything else keeps using Base.");
    char name[81]{}; std::snprintf(name, sizeof(name), "%s", preset.name.c_str());
    if (ImGui::InputText("Name", name, sizeof(name))) { preset.name = name; }

    DrawSettingsHeading("Use when");
    auto group = [&](Appearance::Group value) {
        const auto i = static_cast<std::size_t>(value);
        bool on = settings.groups[i] == preset.id;
        if (ImGui::Checkbox(GroupLabels[i], &on)) { settings.groups[i] = on ? preset.id : 0; }
        if (const auto* owner = Appearance::FindPreset(settings, settings.groups[i]); owner && owner->id != preset.id) {
            Tooltip(std::format("\"{}\" uses this now. Ticking moves it here.", owner->name).c_str());
        } else if (value == Appearance::Group::Exterior) {
            Tooltip("Any outdoor weather not covered by a more specific preset.");
        }
    };
    group(Appearance::Group::Clear); ImGui::SameLine();
    group(Appearance::Group::Cloudy); ImGui::SameLine();
    group(Appearance::Group::Rain); ImGui::SameLine();
    group(Appearance::Group::Snow);
    group(Appearance::Group::Interior); ImGui::SameLine();
    group(Appearance::Group::Exterior);
    Note("Specific weathers win over weather types, which win over Outdoors. Each type uses one preset.");

    std::optional<Appearance::Record> unassign;
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    float used = 0;
    bool any = false;
    for (std::size_t i = 0; i < settings.weathers.size(); ++i) {
        const auto& entry = settings.weathers[i];
        if (entry.preset != preset.id) { continue; }
        const bool loaded = FindWeather(entry.record, *catalogue);
        const auto label = std::format("{}  x##chip{}", ShortWeatherLabel(entry.record, *catalogue), i);
        const float width = ImGui::CalcTextSize(label.c_str(), nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
        if (any && used + ImGui::GetStyle().ItemSpacing.x + width <= rowWidth) { ImGui::SameLine(); used += ImGui::GetStyle().ItemSpacing.x; }
        else { used = 0; }
        used += width;
        any = true;
        if (!loaded) { ImGui::PushStyleColor(ImGuiCol_Text, kMuted); }
        if (ImGui::SmallButton(label.c_str())) { unassign = entry.record; }
        if (!loaded) { ImGui::PopStyleColor(); }
        Tooltip(std::format("{}{}\nClick to remove.", WeatherLabel(entry.record, *catalogue),
            loaded ? "" : "\nNot loaded this session; kept for when its plugin returns.").c_str());
    }
    if (unassign) { Appearance::Assign(settings, *unassign, 0); }
    if (ImGui::Button("Add weathers...")) { ImGui::OpenPopup("weatherPicker"); }
    Tooltip("Pick exact weathers, such as fog or ash, that the weather types above don't describe well.");
    ImGui::SameLine();
    const bool currentHere = std::ranges::any_of(settings.weathers,
        [&](const auto& entry) { return entry.preset == preset.id && entry.record == scene.incoming.record; });
    ImGui::BeginDisabled(!scene.valid || scene.interior || !Appearance::Valid(scene.incoming.record) || currentHere);
    if (ImGui::Button("Add current weather")) {
        if (!Appearance::Assign(settings, scene.incoming.record, preset.id)) { actionMessage = "Weather assignment limit reached."; actionMessageIsError = true; }
    }
    ImGui::EndDisabled();
    if (scene.valid && !scene.interior) { Tooltip(WeatherLabel(scene.incoming.record, *catalogue).c_str()); }

    if (ImGui::BeginPopup("weatherPicker")) {
        ImGui::Text("Add weathers to %s", preset.name.c_str());
        ImGui::PushItemWidth(420);
        ImGui::InputTextWithHint("Search", "Name, plugin, FormID or weather type", presetSearch, sizeof(presetSearch));
        const char* filters[]{"All", "Unclassified", "Clear", "Cloudy", "Rain", "Snow"};
        ImGui::Combo("Weather type", &presetWeatherGroup, filters, 6);
        ImGui::PopItemWidth();
        const auto query = Appearance::SearchKey(presetSearch);
        std::vector<const Appearance::WeatherEntry*> filtered;
        for (const auto& entry : *catalogue) {
            if (entry.search.find(query) == std::string::npos ||
                (presetWeatherGroup && static_cast<int>(entry.weather.group) != presetWeatherGroup - 1)) { continue; }
            filtered.push_back(&entry);
        }
        if (ImGui::BeginChild("weatherResults", ImVec2(640, ImGui::GetTextLineHeightWithSpacing() * 12), ImGuiChildFlags_Border, ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGuiListClipper results; results.Begin(static_cast<int>(filtered.size()));
            while (results.Step()) { for (int i = results.DisplayStart; i < results.DisplayEnd; ++i) {
                const auto& entry = *filtered[i];
                const auto selected = std::ranges::find(presetPickerSelection, entry.weather.record);
                bool checked = selected != presetPickerSelection.end();
                ImGui::PushID(static_cast<int>(entry.runtimeID));
                if (ImGui::Checkbox(entry.label.c_str(), &checked)) {
                    if (checked) { presetPickerSelection.push_back(entry.weather.record); } else { presetPickerSelection.erase(selected); }
                }
                const auto assigned = std::ranges::find_if(settings.weathers, [&](const auto& item) { return item.record == entry.weather.record; });
                if (assigned != settings.weathers.end()) {
                    const auto* owner = Appearance::FindPreset(settings, assigned->preset);
                    ImGui::SameLine();
                    if (owner == &preset) { ImGui::TextDisabled("(in this preset)"); }
                    else if (owner) { ImGui::TextDisabled("-> %s", owner->name.c_str()); }
                }
                ImGui::PopID();
            } }
        }
        ImGui::EndChild();
        ImGui::Text("%zu loaded | %zu shown | %zu selected", catalogue->size(), filtered.size(), presetPickerSelection.size());
        if (ImGui::Button("Select shown")) {
            for (const auto* entry : filtered) {
                if (std::ranges::find(presetPickerSelection, entry->weather.record) == presetPickerSelection.end()) { presetPickerSelection.push_back(entry->weather.record); }
            }
        }
        ImGui::SameLine(); if (ImGui::Button("Clear selection")) { presetPickerSelection.clear(); }
        ImGui::SameLine(); if (ImGui::Button("Refresh list")) { SKSE::GetTaskInterface()->AddTask([] { Appearance::Runtime::Get().CaptureCatalogue(); }); }
        ImGui::BeginDisabled(presetPickerSelection.empty());
        if (ImGui::Button("Add selected")) {
            if (Appearance::AssignMany(settings, presetPickerSelection, preset.id)) { presetPickerSelection.clear(); ImGui::CloseCurrentPopup(); }
            else { actionMessage = "Weather assignment limit reached; no assignments were changed."; actionMessageIsError = true; }
        }
        ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::Button("Close")) { ImGui::CloseCurrentPopup(); }
        ImGui::TextDisabled("Weathers in another preset move here. Names come from the game or an editor-ID plugin.");
        ImGui::EndPopup();
    }

    DrawSettingsHeading("Values");
    if (ImGui::Checkbox("Vary by time of day", &presetTimed) && !presetTimed) {
        const auto point = profile.points[presetTime];
        profile.points.fill(point);
    }
    Tooltip("Off uses one set of values all day. Turning it off keeps the selected time's values.");
    if (presetTimed) {
        for (std::size_t i = 0; i < Appearance::Times.size(); ++i) {
            const bool selected = presetTime == static_cast<int>(i);
            if (selected) { ImGui::PushStyleColor(ImGuiCol_Button, kAmberDim); }
            if (i == nowIndex) { ImGui::PushStyleColor(ImGuiCol_Text, kSage); }
            if (ImGui::Button(std::format("{}##time{}", Appearance::Times[i], i).c_str())) { presetTime = static_cast<int>(i); }
            ImGui::PopStyleColor(static_cast<int>(selected) + static_cast<int>(i == nowIndex));
            const auto next = (i + 1) % Appearance::Times.size();
            Tooltip(std::format("From {}, blending toward {} at {}.{}", Clock(settings.hours[i]), Appearance::Times[next],
                Clock(settings.hours[next]), i == nowIndex ? " Current time." : "").c_str());
            if (i + 1 < Appearance::Times.size()) { ImGui::SameLine(); }
        }
        Note("Values blend smoothly between times. Green is the current time.");
    }
    auto& point = profile.points[presetTimed ? presetTime : 0];
    // Pass 2 values only matter when two unlinked passes are configured in Base.
    const bool second = nr.neuralPasses == 2 && !nr.neuralSecondPass.linked;
    ImGui::BeginDisabled(!profile.neural);
    ImGui::TextDisabled("%s", second ? "NR pass 1" : "NR");
    ImGui::PushID("pass1"); NeuralSliders(point.passes[0]); ImGui::PopID();
    if (second) { ImGui::TextDisabled("NR pass 2"); ImGui::PushID("pass2"); NeuralSliders(point.passes[1]); ImGui::PopID(); }
    ImGui::EndDisabled();
    if (cs) { Note("Community Shaders owns sharpening on this setup, so presets change NR only."); }
    else {
        ImGui::BeginDisabled(!profile.sharpening);
        ImGui::SliderFloat("Sharpening", &point.sharpness, 0, 1, "%.3f");
        ImGui::EndDisabled();
        Note(settingsDraft.sharpening ? "Base sharpening is in the Image tab." : "Sharpening is off in the Image tab, so this has no effect.");
    }
    if (ImGui::Button(presetTimed ? "Set this time from Base" : "Reset to Base")) { point = EditedValues(settingsDraft); }
    if (!presetTimed) { const auto value = point; profile.points.fill(value); }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("More options")) {
        ImGui::Checkbox("Use this preset", &profile.enabled);
        Tooltip("Unticked presets keep their settings but are ignored.");
        ImGui::Checkbox("Change NR", &profile.neural);
        ImGui::SameLine();
        ImGui::BeginDisabled(cs); ImGui::Checkbox("Change sharpening", &profile.sharpening); ImGui::EndDisabled();
        Note("Unticked values follow Base or a less specific preset.");
    }
    bool remove = false;
    if (ImGui::Button("Delete preset...")) { ImGui::OpenPopup("deletePreset"); }
    if (ImGui::BeginPopup("deletePreset")) {
        ImGui::Text("Delete \"%s\"?", preset.name.c_str());
        ImGui::TextDisabled("Where it applied, Base or a less specific preset is used.\nDiscard undoes unapplied edits.");
        if (ImGui::Button("Delete")) { remove = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    // Removing a preset invalidates the references held above.
    if (remove) { Appearance::RemovePreset(settings, presetSelected); presetSelected = BaseRow; }
    settings.enabled = Appearance::UsesPresets(settings);
}

void OverlayUI::DrawPresetSharpeningStatus()
{
    const auto state = Appearance::Runtime::Get().State();
    if (!state.result.active || state.result.incomingSharpening == "Manual defaults") { return; }
    ImGui::TextDisabled("Now %.2f from the \"%s\" preset. This slider is Base.", state.result.values.sharpness,
        state.result.incomingSharpening.c_str());
}
