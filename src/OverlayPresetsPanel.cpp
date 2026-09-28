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
// List rows besides presets. Stored preset IDs are 1..0x7FFFFFFF.
constexpr std::uint32_t BaseRow = 0;
constexpr std::uint32_t TimingRow = 0xFFFFFFFF;
constexpr std::array<const char*, 6> GroupLabels{"All exteriors", "Clear", "Cloudy", "Rain", "Snow", "Interior"};

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
// The controller names the no-preset fallback "Manual defaults"; this tab calls it Base.
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
std::string AppliesTo(const Appearance::Settings& settings, const Appearance::NamedProfile& preset, std::size_t weathers)
{
    std::string text;
    for (std::size_t i = 0; i < GroupLabels.size(); ++i) {
        if (settings.groups[i] == preset.id) { text += std::format("{}{}", text.empty() ? "" : ", ", GroupLabels[i]); }
    }
    if (weathers) { text += std::format("{}{} weather{}", text.empty() ? "" : " + ", weathers, weathers == 1 ? "" : "s"); }
    if (text.empty()) { text = "Not assigned"; }
    return preset.profile.enabled ? text : "Off | " + text;
}
void Tooltip(const char* text)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { ImGui::SetTooltip("%s", text); }
}
// Two-line list row: name, then what it applies to. Returns true when clicked.
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
        const auto size = ImGui::CalcTextSize("NOW");
        right -= size.x;
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
void NeuralSliders(float& intensity, float& tone, float& structure)
{
    ImGui::SliderFloat("Intensity", &intensity, 0, 2);
    ImGui::SliderFloat("Local tone", &tone, 0, 2);
    ImGui::SliderFloat("Local structure", &structure, 0, 2);
}
// Pass 2 values only matter when two unlinked passes are configured in the NR tab.
bool SecondPassEditable(const SourceDLSSG::Preferences& nr)
{
    ImGui::TextDisabled("NR pass 2");
    if (nr.neuralPasses != 2) { DrawSettingsHelp("Pass 2 is off in the Neural Rendering tab."); return false; }
    if (nr.neuralSecondPass.linked) { DrawSettingsHelp("Pass 2 follows Pass 1 while linked in the Neural Rendering tab."); return false; }
    return true;
}
}

void OverlayUI::DrawPresetsPanel(float height)
{
    if (!ImGui::BeginTabItem("Presets")) { return; }
    auto& runtime = Appearance::Runtime::Get();
    const auto state = runtime.State();
    const auto catalogue = runtime.Catalogue();
    const bool cs = CommunityShaders::Active();
    auto& settings = settingsDraft.appearance;
    const auto& scene = state.context;
    const float top = ImGui::GetCursorPosY();

    ImGui::Checkbox("Automatic presets", &settings.enabled);
    Tooltip("Chooses presets from weather and game time. Apply to use; Save as default to keep.");
    ImGui::SameLine();
    bool paused = state.paused;
    if (ImGui::Checkbox("Pause (use Base)", &paused)) { runtime.Pause(paused); }
    Tooltip("Takes effect immediately for this session and is not saved.");
    ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 3);
    if (!scene.valid) {
        ImGui::TextDisabled("Current weather is available after entering the world.");
    } else {
        std::string applied = DisplayName(state.result.incoming);
        if (state.paused) { applied = "Base (paused)"; }
        else if (!cs && state.result.incomingSharpening != state.result.incoming) {
            applied = std::format("NR {} | sharpening {}", applied, DisplayName(state.result.incomingSharpening));
        }
        const auto place = scene.interior ? std::string("Interior") : ShortWeatherLabel(scene.incoming.record, *catalogue);
        ImGui::Text("Now: %s | %s %s | %s", applied.c_str(), Appearance::Times[TimeIndex(settings.hours, scene.hour)],
            Clock(scene.hour).c_str(), place.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (!scene.interior) { ImGui::TextUnformatted(WeatherLabel(scene.incoming.record, *catalogue).c_str()); }
            for (std::size_t pass = 0; pass < 2; ++pass) {
                if (pass && settingsDraft.sourceDLSSG.neuralPasses != 2) { break; }
                const auto& nr = state.result.values.passes[pass];
                ImGui::Text("Pass %u: intensity %.2f | tone %.2f | structure %.2f", static_cast<unsigned>(pass + 1), nr.intensity, nr.tone, nr.structure);
            }
            if (!cs) { ImGui::Text("Sharpening strength %.3f", state.result.values.sharpness); }
            ImGui::TextDisabled("Resolved values follow the NR and sharpening enable controls.");
            ImGui::EndTooltip();
        }
        if (!scene.interior && !state.paused && Appearance::Valid(scene.outgoing.record) && scene.transition < 1) {
            ImGui::SameLine();
            ImGui::TextDisabled("from %s", DisplayName(state.result.outgoing).c_str());
            ImGui::SameLine();
            ImGui::ProgressBar(scene.transition, ImVec2(120, 0), "");
        }
    }
    ImGui::Separator();

    // Mark what the edited configuration would select for the current weather.
    std::uint32_t nowNeural = TimingRow, nowSharpening = TimingRow;
    bool baseNow = false;
    if (scene.valid) {
        if (!settings.enabled || state.paused) { baseNow = true; }
        else {
            const auto selection = Appearance::Select(settings, scene.incoming, scene.interior);
            for (const auto& preset : settings.presets) {
                if (&preset.profile == selection.neural) { nowNeural = preset.id; }
                if (&preset.profile == selection.sharpening) { nowSharpening = preset.id; }
            }
            baseNow = !selection.neural || (!cs && !selection.sharpening);
        }
    }
    if (presetSelected != BaseRow && presetSelected != TimingRow && !Appearance::FindPreset(settings, presetSelected)) {
        presetSelected = BaseRow;
    }

    const float bodyHeight = (std::max)(160.0f, height - (ImGui::GetCursorPosY() - top));
    const float listWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.32f, 240.0f, 440.0f);
    const float rowHeight = ImGui::GetTextLineHeight() * 2 + ImGui::GetStyle().FramePadding.y * 3;
    ImGui::BeginChild("##presetColumn", ImVec2(listWidth, bodyHeight));
    ImGui::BeginChild("##presetRows", ImVec2(0, -(rowHeight + ImGui::GetStyle().ItemSpacing.y)), ImGuiChildFlags_Border);
    if (PresetRow(-1, "Base", "NR and Image settings | fallback", presetSelected == BaseRow, baseNow, false)) {
        presetSelected = BaseRow;
    }
    std::unordered_map<std::uint32_t, std::size_t> weatherCounts;
    for (const auto& entry : settings.weathers) { ++weatherCounts[entry.preset]; }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(settings.presets.size()), rowHeight + ImGui::GetStyle().ItemSpacing.y);
    while (clipper.Step()) { for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const auto& preset = settings.presets[i];
        const bool now = preset.id == nowNeural || (!cs && preset.id == nowSharpening);
        const auto count = weatherCounts.find(preset.id);
        if (PresetRow(static_cast<int>(preset.id), preset.name.c_str(), AppliesTo(settings, preset, count == weatherCounts.end() ? 0 : count->second),
                presetSelected == preset.id, now, !preset.profile.enabled)) { presetSelected = preset.id; }
    } }
    if (settings.presets.empty()) {
        ImGui::Spacing();
        DrawSettingsHelp("No presets yet. Select Base and copy it, or create one preset per weather group from Base.");
        if (ImGui::Button("Create starter presets")) {
            if (!Appearance::AddStarterProfiles(settings, EditedValues(settingsDraft))) {
                actionMessage = "Preset limit reached. Remove an unused preset first."; actionMessageIsError = true;
            }
        }
    }
    ImGui::EndChild();
    if (PresetRow(-2, "Timing", std::format("Time anchors | {:.1f} s smoothing", settings.smoothingSeconds),
            presetSelected == TimingRow, false, false)) { presetSelected = TimingRow; }
    ImGui::EndChild();

    ImGui::SameLine(0, ColumnGap);
    ImGui::BeginChild("##presetDetails", ImVec2(0, bodyHeight));
    ImGui::PushTextWrapPos(0);
    ImGui::PushItemWidth(-180.0f);
    auto& nr = settingsDraft.sourceDLSSG;
    std::optional<Appearance::NamedProfile> copy;
    bool remove = false;

    if (presetSelected == BaseRow) {
        DrawSettingsHeading("Base");
        DrawSettingsHelp("The same values as the Neural Rendering and Image tabs; editing them here edits them there. "
            "Base applies wherever no preset does, and while automatic presets are off or paused.");
        ImGui::BeginDisabled(settings.presets.size() >= Appearance::MaxPresets);
        if (ImGui::Button("Copy into new preset")) { copy = Appearance::NamedProfile{0, "New preset", Appearance::FromValues(EditedValues(settingsDraft))}; }
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::TextDisabled("NR pass 1");
        ImGui::PushID("pass1");
        NeuralSliders(nr.neuralTuning.intensity, nr.neuralTuning.localToneStrength, nr.neuralTuning.localStructureStrength);
        ImGui::PopID();
        if (SecondPassEditable(nr)) {
            auto& second = nr.neuralSecondPass.tuning;
            ImGui::PushID("pass2");
            NeuralSliders(second.intensity, second.localToneStrength, second.localStructureStrength);
            ImGui::PopID();
        }
        ImGui::TextDisabled("Image");
        if (cs) { DrawSettingsHelp("Community Shaders owns sharpening on this route."); }
        else {
            ImGui::BeginDisabled(!settingsDraft.sharpening);
            ImGui::SliderFloat("Sharpening strength", &settingsDraft.sharpness, 0, 1, "%.3f");
            ImGui::EndDisabled();
            if (!settingsDraft.sharpening) { DrawSettingsHelp("Sharpening is off in the Image tab."); }
        }
        DrawSettingsHelp("Network, style, skin, resolution, placement and pass count are in the Neural Rendering tab.");
    } else if (presetSelected == TimingRow) {
        DrawSettingsHeading("Timing");
        ImGui::SliderFloat("Transition smoothing (seconds)", &settings.smoothingSeconds, 0, 10, "%.1f");
        DrawSettingsHelp("Softens changes between presets. An exponential time constant, not a fixed duration; zero follows weather and time directly.");
        ImGui::Spacing();
        ImGui::TextDisabled("Time anchors");
        for (std::size_t i = 0; i < Appearance::Times.size(); ++i) { ImGui::InputFloat(Appearance::Times[i], &settings.hours[i], .25f, 1, "%.2f"); }
        if (ImGui::Button("Reset time anchors")) { settings.hours = Appearance::DefaultHours; }
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
        DrawSettingsHelp("Game hours, increasing from Night to Dusk. Presets blend between neighbouring anchors. ENB copying uses Night, Sunrise, Day and Sunset markers, plus Sunrise minus DawnDuration and Sunset plus DuskDuration. It does not reproduce ENB's internal phase weights or change ENB settings.");
    } else if (auto found = std::ranges::find_if(settings.presets, [&](const auto& item) { return item.id == presetSelected; });
               found != settings.presets.end()) {
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
        char name[81]{}; std::snprintf(name, sizeof(name), "%s", preset.name.c_str());
        if (ImGui::InputText("Name", name, sizeof(name))) { preset.name = name; }
        ImGui::BeginDisabled(settings.presets.size() >= Appearance::MaxPresets);
        if (ImGui::Button("Copy into new preset")) { copy = Appearance::NamedProfile{0, preset.name + " copy", profile}; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Delete...")) { ImGui::OpenPopup("deletePreset"); }
        if (ImGui::BeginPopup("deletePreset")) {
            ImGui::Text("Delete \"%s\"?", preset.name.c_str());
            ImGui::TextDisabled("Its groups and weathers fall back to less specific presets.\nDiscard undoes unapplied edits.");
            if (ImGui::Button("Delete")) { remove = true; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
        ImGui::Checkbox("Use this preset", &profile.enabled);
        Tooltip("An unused preset keeps its assignments; they fall back to less specific presets.");
        ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 3);
        ImGui::TextDisabled("Adjusts");
        ImGui::SameLine();
        ImGui::Checkbox("NR", &profile.neural);
        ImGui::SameLine();
        ImGui::BeginDisabled(cs); ImGui::Checkbox("Sharpening", &profile.sharpening); ImGui::EndDisabled();
        if (cs) { Tooltip("Community Shaders owns sharpening on this route; presets adjust NR only."); }

        DrawSettingsHeading("Applies to");
        auto group = [&](std::size_t i) {
            bool on = settings.groups[i] == preset.id;
            if (ImGui::Checkbox(GroupLabels[i], &on)) { settings.groups[i] = on ? preset.id : 0; }
            if (const auto* owner = Appearance::FindPreset(settings, settings.groups[i]); owner && owner->id != preset.id) {
                Tooltip(std::format("Uses \"{}\" now. Ticking moves it here.", owner->name).c_str());
            }
        };
        group(static_cast<std::size_t>(Appearance::Group::Exterior));
        ImGui::SameLine();
        group(static_cast<std::size_t>(Appearance::Group::Interior));
        for (auto i : {Appearance::Group::Clear, Appearance::Group::Cloudy, Appearance::Group::Rain, Appearance::Group::Snow}) {
            group(static_cast<std::size_t>(i));
            if (i != Appearance::Group::Snow) { ImGui::SameLine(); }
        }
        DrawSettingsHelp("Clear, Cloudy, Rain and Snow refine All exteriors; individual weathers refine both. Each group uses one preset. "
            "Assign fog, ash and special weather individually.");

        ImGui::TextDisabled("Weathers");
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
                loaded ? "" : "\nNot loaded this session; the assignment is kept for when its plugin returns.").c_str());
        }
        if (!any) { ImGui::TextDisabled("None"); }
        if (unassign) { Appearance::Assign(settings, *unassign, 0); }
        if (ImGui::Button("Add weathers...")) { ImGui::OpenPopup("weatherPicker"); }
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
            ImGui::InputTextWithHint("Search", "Name, plugin, FormID or classification", presetSearch, sizeof(presetSearch));
            const char* filters[]{"All", "Unclassified", "Clear", "Cloudy", "Rain", "Snow"};
            ImGui::Combo("Classification", &presetWeatherGroup, filters, 6);
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
            ImGui::TextDisabled("Weathers in another preset move here. Assignments use plugin identity, not load order.");
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
                Tooltip(std::format("{} at {}{}", Appearance::Times[i], Clock(settings.hours[i]), i == nowIndex ? " | current time" : "").c_str());
                if (i + 1 < Appearance::Times.size()) { ImGui::SameLine(); }
            }
            const auto next = (static_cast<std::size_t>(presetTime) + 1) % Appearance::Times.size();
            ImGui::TextDisabled("From %s, blending toward %s at %s. Green marks the current time.", Clock(settings.hours[presetTime]).c_str(),
                Appearance::Times[next], Clock(settings.hours[next]).c_str());
        }
        auto& point = profile.points[presetTimed ? presetTime : 0];
        ImGui::BeginDisabled(!profile.neural);
        ImGui::TextDisabled("NR pass 1");
        ImGui::PushID("pass1");
        NeuralSliders(point.passes[0].intensity, point.passes[0].tone, point.passes[0].structure);
        ImGui::PopID();
        if (SecondPassEditable(nr)) {
            ImGui::PushID("pass2");
            NeuralSliders(point.passes[1].intensity, point.passes[1].tone, point.passes[1].structure);
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        if (!cs) {
            ImGui::TextDisabled("Image");
            ImGui::BeginDisabled(!profile.sharpening);
            ImGui::SliderFloat("Sharpening strength", &point.sharpness, 0, 1, "%.3f");
            ImGui::EndDisabled();
        }
        if (ImGui::Button(presetTimed ? "Set this time from Base" : "Reset to Base")) { point = EditedValues(settingsDraft); }
        if (!presetTimed) { const auto value = point; profile.points.fill(value); }
        DrawSettingsHelp("Editing a preset changes every group and weather that uses it. Presets never turn NR or sharpening on.");
    }

    ImGui::PopItemWidth();
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    // Adding or removing presets invalidates references held while drawing.
    if (copy) {
        if (const auto id = Appearance::AddPreset(settings, std::move(copy->name), std::move(copy->profile))) { presetSelected = id; }
    }
    if (remove) { Appearance::RemovePreset(settings, presetSelected); presetSelected = BaseRow; }
    ImGui::EndTabItem();
}
