#include <PCH.h>
#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "OverlaySettingRows.h"
#include "OverlayFrameView.h"
#include "WeatherAppearanceINI.h"
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

Appearance::Setup DraftSetup(const RendererSettingsDraft& draft)
{
    const auto& nr = draft.sourceDLSSG;
    Appearance::Setup setup;
    setup.neural = nr.neuralEnabled;
    setup.beforeUpscaling = nr.neuralBeforeUpscaling;
    setup.passes = nr.neuralPasses;
    setup.combat = nr.neuralCombat;
    setup.reconstruction = nr.neuralReconstruction;
    setup.tuning = nr.neuralTuning;
    // Matches the runtime: a linked Pass 2 takes Pass 1's values.
    setup.second = NeuralRendering::EffectiveSecondPass(nr.neuralSecondPass, nr.neuralReconstruction, nr.neuralTuning);
    setup.sharpening = draft.sharpening;
    setup.sharpness = draft.sharpness;
    return setup;
}
void WriteSetup(const Appearance::Setup& setup, SourceDLSSG::Preferences& nr, bool& sharpening, float& sharpness)
{
    nr.neuralEnabled = setup.neural;
    nr.neuralBeforeUpscaling = setup.beforeUpscaling;
    nr.neuralPasses = setup.passes;
    nr.neuralCombat = setup.combat;
    nr.neuralReconstruction = setup.reconstruction;
    nr.neuralTuning = setup.tuning;
    nr.neuralSecondPass = setup.second;
    sharpening = setup.sharpening;
    sharpness = setup.sharpness;
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
std::string Summary(const Appearance::NamedProfile& preset)
{
    std::string text;
    const auto weathers = preset.weathers.size();
    for (std::size_t i = 0; i < GroupLabels.size(); ++i) {
        if (preset.groups[i]) { text += std::format("{}{}", text.empty() ? "" : ", ", GroupLabels[i]); }
    }
    if (weathers) { text += std::format("{}{} weather{}", text.empty() ? "" : " + ", weathers, weathers == 1 ? "" : "s"); }
    if (text.empty()) { text = "Not used yet"; }
    const auto changes = preset.profile.changes.size();
    text += changes ? std::format(" | {} change{}", changes, changes == 1 ? "" : "s") : " | no changes";
    return preset.profile.enabled ? text : "Off | " + text;
}
void Note(const char* text) { ImGui::TextDisabled("%s", text); }
const char* ShownName(const Appearance::NamedProfile& preset) { return preset.name.empty() ? "Unnamed preset" : preset.name.c_str(); }
void Tooltip(const char* text)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { ImGui::SetTooltip("%s", text); }
}
// Base's value of one setting, as the controls show it.
std::string FieldValue(const Appearance::Field& field, const Appearance::Setup& setup)
{
    const float value = field.get(setup);
    const std::string_view key = field.key;
    if (key == "BeforeUpscaling") { return value != 0 ? "before upscaling" : "after upscaling"; }
    if (key == "Passes") { return value == 2 ? "two passes" : "one pass"; }
    if (key == "Pass2SameAsPass1") { return value != 0 ? "follows Pass 1" : "own settings"; }
    if (key == "ReconstructionMethod") { return value == 2 ? "Ratio" : value == 1 ? "Residual" : "Auto"; }
    if (key.ends_with("Network")) { return value != 0 ? "Shipping" : "Default"; }
    if (key.ends_with("Style")) { return std::format("Style {}", static_cast<int>(value)); }
    if (key.ends_with("InputScale")) { return std::format("{:.1f}%", value * 100); }
    if (key == "ReturnDelay") { return std::format("{:.1f} s", value); }
    if (field.integral && field.low == 0 && field.high == 1) { return value != 0 ? "on" : "off"; }
    return field.integral ? std::format("{}", static_cast<int>(value)) : std::format("{:.2f}", value);
}
// Every setting at every time, resolved on Base, so a copy looks the same with any Base.
Appearance::Profile FullProfile(const Appearance::Profile& profile, const Appearance::Setup& base)
{
    Appearance::Profile full;
    full.enabled = profile.enabled;
    for (const auto& field : Appearance::Fields()) {
        Appearance::Change change{field.key};
        for (std::size_t time = 0; time < Appearance::Times.size(); ++time) {
            auto setup = base;
            for (const auto& item : profile.changes) {
                if (const auto* other = Appearance::FindField(item.key)) { other->set(setup, other->look ? item.points[time] : item.points[0]); }
            }
            change.points[time] = field.get(setup);
        }
        full.changes.push_back(change);
    }
    return Appearance::Sanitize(std::move(full));
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
    const float accent = std::round(Px(3.0f));
    draw->AddRectFilled(pos, ImVec2(pos.x + accent, pos.y + height), ImGui::GetColorU32(selected ? kAmber : kAmberDim));
    const float text = pos.x + accent + style.FramePadding.x * 2;
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
// Answers the shared NR controls' questions about the preset being edited.
struct EditorDecor final : PresetDecor
{
    const Appearance::Profile& profile;
    const Appearance::Setup& base;
    const char* time;
    std::vector<std::string>& restore;
    EditorDecor(const Appearance::Profile& profile, const Appearance::Setup& base, const char* time, std::vector<std::string>& restore)
        : profile(profile), base(base), time(time), restore(restore) {}
    bool Changed(const char* key) const override { return Appearance::FindChange(profile, key) != nullptr; }
    std::string BaseValue(const char* key) const override
    {
        const auto* field = Appearance::FindField(key);
        return field ? FieldValue(*field, base) : std::string{};
    }
    void Reset(const char* key) override { restore.emplace_back(key); }
    const char* EditingTime() const override { return time; }
};
}

bool OverlayUI::PresetEditorSelected() const
{
    return presetSelected != BaseRow && Appearance::FindPreset(settingsDraft.appearance, presetSelected);
}

void OverlayUI::DrawPresetList(const FrameView& view)
{
    const auto& state = view.appearance;
    const auto& catalogue = view.weatherCatalogue;
    const bool cs = view.communityShaders;
    auto& settings = settingsDraft.appearance;
    const auto& scene = state.context;

    DrawSettingsHeading("Presets");
    Note("Presets change Neural Rendering settings for the weather or places you choose. Everywhere else uses Base.");
    if (scene.valid && !settings.presets.empty()) {
        const auto applied = state.paused ? std::string("Base (paused)") : state.result.incoming;
        const auto place = scene.interior ? std::string("Interior") : ShortWeatherLabel(scene.incoming.record, *catalogue);
        ImGui::Text("Now: %s | %s | %s", applied.c_str(), Clock(scene.hour).c_str(), place.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (!scene.interior) { ImGui::TextUnformatted(WeatherLabel(scene.incoming.record, *catalogue).c_str()); }
            const auto time = TimeIndex(settings.hours, scene.hour);
            ImGui::Text("%s, blending toward %s", Appearance::Times[time], Appearance::Times[(time + 1) % Appearance::Times.size()]);
            const auto& setup = state.result.setup;
            ImGui::Text("Pass 1: intensity %.2f | tone %.2f | structure %.2f", setup.tuning.intensity,
                setup.tuning.localToneStrength, setup.tuning.localStructureStrength);
            if (setup.passes == 2 && !setup.second.linked) {
                ImGui::Text("Pass 2: intensity %.2f | tone %.2f | structure %.2f", setup.second.tuning.intensity,
                    setup.second.tuning.localToneStrength, setup.second.tuning.localStructureStrength);
            }
            if (!cs) { ImGui::Text("Sharpening strength %.3f", setup.sharpness); }
            ImGui::EndTooltip();
        }
        if (!scene.interior && !state.paused && Appearance::Valid(scene.outgoing.record) && scene.transition < 1) {
            ImGui::TextDisabled("Changing from %s", state.result.outgoing.c_str());
            ImGui::SameLine();
            ImGui::ProgressBar(scene.transition, ImVec2(Px(120.0f), 0), "");
        }
    }
    if (!settings.presets.empty()) {
        bool paused = state.paused;
        if (ImGui::Checkbox("Pause presets", &paused)) { PausePresets(paused); }
        Tooltip("Uses Base right away, until you untick this or restart the game. Not saved.");
    }

    // Mark the presets the edited configuration would select for the current weather.
    Appearance::Selection now;
    if (scene.valid && Appearance::UsesPresets(settings) && !state.paused) {
        now = Appearance::Select(Appearance::ResolveClaims(settings), scene.incoming, scene.interior);
    }
    const bool baseNow = scene.valid && std::ranges::none_of(now.layers, [](const auto* layer) { return layer != nullptr; });
    if (!PresetEditorSelected()) { presetSelected = BaseRow; }

    ImGui::Spacing();
    if (PresetRow(-1, "Base", "Your Neural Rendering settings", presetSelected == BaseRow, baseNow, false)) { presetSelected = BaseRow; }
    const float rowHeight = ImGui::GetTextLineHeight() * 2 + ImGui::GetStyle().FramePadding.y * 3;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(settings.presets.size()), rowHeight + ImGui::GetStyle().ItemSpacing.y);
    while (clipper.Step()) { for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const auto& preset = settings.presets[i];
        const bool active = std::ranges::find(now.layers, &preset) != now.layers.end();
        if (PresetRow(static_cast<int>(preset.id), ShownName(preset), Summary(preset),
                presetSelected == preset.id, active, !preset.profile.enabled)) { presetSelected = preset.id; }
    } }

    const bool full = settings.presets.size() >= Appearance::MaxPresets;
    ImGui::BeginDisabled(full);
    if (ImGui::Button("Create new preset")) {
        if (const auto id = Appearance::AddPreset(settings, settings.presets.empty() ? "My first preset" : "New preset")) { presetSelected = id; }
    }
    Tooltip("Starts with no changes. Choose when it applies, then change the settings you want different from Base.");
    if (const auto* selected = Appearance::FindPreset(settings, presetSelected)) {
        ImGui::SameLine();
        if (ImGui::Button("Duplicate")) {
            const auto copy = *selected;
            if (const auto id = Appearance::AddPreset(settings, copy.name + " copy", copy.profile)) { presetSelected = id; }
        }
        Tooltip("Copies the selected preset's changes. Choose when the copy applies.");
    }
    ImGui::EndDisabled();
    if (Appearance::FindPreset(settings, presetSelected)) {
        const auto index = std::ranges::find_if(settings.presets, [&](const auto& item) { return item.id == presetSelected; }) - settings.presets.begin();
        ImGui::SameLine();
        ImGui::BeginDisabled(index == 0);
        if (ImGui::Button("Move up")) { Appearance::MovePreset(settings, presetSelected, -1); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(index + 1 == static_cast<std::ptrdiff_t>(settings.presets.size()));
        if (ImGui::Button("Move down")) { Appearance::MovePreset(settings, presetSelected, 1); }
        ImGui::EndDisabled();
        Tooltip("When two presets use the same weather, the higher one wins.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Timing...")) { ImGui::OpenPopup("presetTiming"); }
    Tooltip("When each time of day starts, and how smoothly presets change.");
    if (ImGui::BeginPopup("presetTiming")) {
        ImGui::PushItemWidth(Px(220.0f));
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

void OverlayUI::DrawPresetEditor(const FrameView& view,
    const std::function<void(SourceDLSSG::Preferences&, bool&, float&)>& drawSettings)
{
    const auto& state = view.appearance;
    const auto& catalogue = view.weatherCatalogue;
    auto& settings = settingsDraft.appearance;
    const auto& scene = state.context;
    auto found = std::ranges::find_if(settings.presets, [&](const auto& item) { return item.id == presetSelected; });
    if (found == settings.presets.end()) { return; }
    auto& preset = *found;
    auto& profile = preset.profile;
    const auto nowIndex = scene.valid ? TimeIndex(settings.hours, scene.hour) : Appearance::Times.size();
    if (presetShown != preset.id) {
        presetShown = preset.id;
        presetTimed = Appearance::Timed(profile);
        presetTime = static_cast<int>(nowIndex < Appearance::Times.size() ? nowIndex : 3);
        presetPickerSelection.clear();
    }

    bool fullCopy = false;
    DrawSettingsHeading(ShownName(preset));
    {
        Flow flow;
        const auto changes = profile.changes.size();
        flow.Text(changes ? std::format("{} change{}", changes, changes == 1 ? "" : "s").c_str() : "No changes yet");
        flow.Checkbox("Use this preset", &profile.enabled);
        Tooltip("Unticked presets keep their settings but are ignored.");
        ImGui::BeginDisabled(profile.changes.empty());
        if (flow.Button("Reset all to Base")) { profile.changes.clear(); }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(settings.presets.size() >= Appearance::MaxPresets);
        if (flow.Button("Save full copy")) { fullCopy = true; }
        ImGui::EndDisabled();
        Tooltip("Adds a copy with every setting stored, so it looks the same with anyone's Base. Share its file.");
    }
    Note("Changed settings are amber. Everything else follows Base, including later Base edits.");
    if (BeginSettingRows("presetName", LabelWidth({"One pass while weapons are drawn", "Input colour is linear HDR"}))) {
        SettingRow("Name", nullptr, false, false, [&] {
            char name[81]{}; std::snprintf(name, sizeof(name), "%s", preset.name.c_str());
            // The name is the preset's file name, so characters files cannot use are not typed.
            const bool changed = ImGui::InputText("##v", name, sizeof(name), ImGuiInputTextFlags_CallbackCharFilter,
                [](ImGuiInputTextCallbackData* data) { return Appearance::FileNameCharacter(data->EventChar) ? 0 : 1; });
            if (changed) { preset.name = name; }
            return changed;
        });
        ImGui::EndTable();
    }

    DrawSettingsHeading("Use when");
    const auto claims = Appearance::ResolveClaims(settings);
    // A claim a higher preset already uses is shown muted with who uses it.
    const auto shadowedBy = [&](const Appearance::NamedProfile* owner) {
        return owner && owner != &preset ? owner : nullptr;
    };
    {
        Flow flow;
        for (const auto value : {Appearance::Group::Clear, Appearance::Group::Cloudy, Appearance::Group::Rain,
                 Appearance::Group::Snow, Appearance::Group::Interior, Appearance::Group::Exterior}) {
            const auto i = static_cast<std::size_t>(value);
            const auto* owner = shadowedBy(claims.groups[i]);
            if (owner && preset.groups[i]) { ImGui::PushStyleColor(ImGuiCol_Text, kMuted); }
            flow.Checkbox(GroupLabels[i], &preset.groups[i]);
            if (owner && preset.groups[i]) { ImGui::PopStyleColor(); }
            if (owner) {
                Tooltip(std::format("\"{}\" uses this{}. The higher preset in the list wins; move this one up to use it here.",
                    ShownName(*owner), preset.groups[i] ? " instead" : "").c_str());
            } else if (value == Appearance::Group::Exterior) {
                Tooltip("Any outdoor weather not covered by a more specific preset.");
            }
        }
    }
    std::optional<Appearance::Record> unassign;
    {
        Flow flow;
        for (std::size_t i = 0; i < preset.weathers.size(); ++i) {
            const auto& record = preset.weathers[i];
            const bool loaded = FindWeather(record, *catalogue);
            const auto* owner = shadowedBy(Appearance::WeatherOwner(claims, record));
            const auto label = std::format("{}  x##chip{}", ShortWeatherLabel(record, *catalogue), i);
            if (!loaded || owner) { ImGui::PushStyleColor(ImGuiCol_Text, kMuted); }
            if (flow.Button(label.c_str())) { unassign = record; }
            if (!loaded || owner) { ImGui::PopStyleColor(); }
            Tooltip(std::format("{}{}{}\nClick to remove.", WeatherLabel(record, *catalogue),
                loaded ? "" : "\nNot loaded this session; kept for when its plugin returns.",
                owner ? std::format("\n\"{}\" uses this instead; it is higher in the list.", ShownName(*owner)) : std::string()).c_str());
        }
        if (flow.Button("Add weathers...")) { ImGui::OpenPopup("weatherPicker"); }
        Tooltip("Pick exact weathers, such as fog or ash, that the weather types above don't describe well.");
        const bool currentHere = Appearance::HasWeather(preset, scene.incoming.record);
        ImGui::BeginDisabled(!scene.valid || scene.interior || !Appearance::Valid(scene.incoming.record) || currentHere);
        if (flow.Button("Add current weather")) {
            const std::array records{scene.incoming.record};
            if (!Appearance::AddWeathers(settings, preset.id, records)) { actionMessage = "Weather assignment limit reached."; actionMessageIsError = true; }
        }
        ImGui::EndDisabled();
        if (scene.valid && !scene.interior) { Tooltip(WeatherLabel(scene.incoming.record, *catalogue).c_str()); }
    }
    if (unassign) { Appearance::RemoveWeather(settings, preset.id, *unassign); }
    Note("Specific weathers win over weather types, which win over Outdoors. If presets share one, the higher in the list wins.");
    ImGui::Spacing();
    if (ImGui::Checkbox("Different look by time of day", &presetTimed) && !presetTimed) {
        // Turning it off keeps the selected time's values all day.
        for (auto& change : profile.changes) { const auto point = change.points[presetTime]; change.points.fill(point); }
    }
    Tooltip("Intensity, local tone, local structure and sharpening strength can differ at each time of day and blend "
        "between them. Other settings stay the same all day.");
    if (presetTimed) {
        Flow flow;
        for (std::size_t i = 0; i < Appearance::Times.size(); ++i) {
            const bool selected = presetTime == static_cast<int>(i);
            if (selected) { ImGui::PushStyleColor(ImGuiCol_Button, kAmberDim); }
            if (i == nowIndex) { ImGui::PushStyleColor(ImGuiCol_Text, kSage); }
            if (flow.Button(std::format("{}##time{}", Appearance::Times[i], i).c_str())) { presetTime = static_cast<int>(i); }
            ImGui::PopStyleColor(static_cast<int>(selected) + static_cast<int>(i == nowIndex));
            const auto next = (i + 1) % Appearance::Times.size();
            Tooltip(std::format("From {}, blending toward {} at {}.{}", Clock(settings.hours[i]), Appearance::Times[next],
                Clock(settings.hours[next]), i == nowIndex ? " Current time." : "").c_str());
        }
        Note("Pick a time to edit its look; it blends into the next. Green is now. Other settings stay the same all day.");
    }

    if (ImGui::BeginPopup("weatherPicker")) {
        ImGui::Text("Add weathers to %s", ShownName(preset));
        ImGui::PushItemWidth(Px(420.0f));
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
        if (ImGui::BeginChild("weatherResults", ImVec2(Px(640.0f), ImGui::GetTextLineHeightWithSpacing() * 12), ImGuiChildFlags_Border, ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGuiListClipper results; results.Begin(static_cast<int>(filtered.size()));
            while (results.Step()) { for (int i = results.DisplayStart; i < results.DisplayEnd; ++i) {
                const auto& entry = *filtered[i];
                const auto selected = std::ranges::find(presetPickerSelection, entry.weather.record);
                bool checked = selected != presetPickerSelection.end();
                ImGui::PushID(static_cast<int>(entry.runtimeID));
                if (ImGui::Checkbox(entry.label.c_str(), &checked)) {
                    if (checked) { presetPickerSelection.push_back(entry.weather.record); } else { presetPickerSelection.erase(selected); }
                }
                if (Appearance::HasWeather(preset, entry.weather.record)) { ImGui::SameLine(); ImGui::TextDisabled("(in this preset)"); }
                else if (const auto* owner = Appearance::WeatherOwner(claims, entry.weather.record)) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("-> %s", ShownName(*owner));
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
        ImGui::SameLine(); if (ImGui::Button("Refresh list")) { RefreshWeatherList(); }
        ImGui::BeginDisabled(presetPickerSelection.empty());
        if (ImGui::Button("Add selected")) {
            if (Appearance::AddWeathers(settings, preset.id, presetPickerSelection)) { presetPickerSelection.clear(); ImGui::CloseCurrentPopup(); }
            else { actionMessage = "Weather assignment limit reached; no assignments were changed."; actionMessageIsError = true; }
        }
        ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::Button("Close")) { ImGui::CloseCurrentPopup(); }
        ImGui::TextDisabled("A weather in several presets uses the highest one in the list. Names come from the game or an editor-ID plugin.");
        ImGui::EndPopup();
    }

    // Draw the shared NR controls on this preset's view of Base, then record edits as changes.
    const auto base = DraftSetup(settingsDraft);
    const auto time = static_cast<std::size_t>(presetTimed ? presetTime : 0);
    auto shown = base;
    for (const auto& change : profile.changes) {
        if (const auto* field = Appearance::FindField(change.key)) { field->set(shown, field->look ? change.points[time] : change.points[0]); }
    }
    auto nr = settingsDraft.sourceDLSSG;
    bool sharpening{};
    float sharpness{};
    WriteSetup(shown, nr, sharpening, sharpness);
    std::vector<std::string> restore;
    EditorDecor decor(profile, base, presetTimed ? Appearance::Times[presetTime] : nullptr, restore);
    ActivePresetDecor() = &decor;
    drawSettings(nr, sharpening, sharpness);
    ActivePresetDecor() = nullptr;
    auto edited = base;
    edited.neural = nr.neuralEnabled;
    edited.beforeUpscaling = nr.neuralBeforeUpscaling;
    edited.passes = nr.neuralPasses;
    edited.combat = nr.neuralCombat;
    edited.reconstruction = nr.neuralReconstruction;
    edited.tuning = nr.neuralTuning;
    edited.second = nr.neuralSecondPass;
    edited.sharpening = sharpening;
    edited.sharpness = sharpness;
    for (const auto& field : Appearance::Fields()) {
        const float value = field.get(edited);
        if (value == field.get(shown)) { continue; }
        if (field.look && presetTimed) {
            // A new timed change keeps Base's value at the other times.
            if (!Appearance::FindChange(profile, field.key)) { Appearance::SetChange(profile, field.key, field.get(base)); }
            Appearance::SetChange(profile, field.key, value, presetTime);
        } else if (value == field.get(base)) {
            // Setting a value back to Base's removes the change.
            Appearance::ClearChange(profile, field.key);
        } else {
            Appearance::SetChange(profile, field.key, value);
        }
    }
    if (edited.second.linked && !shown.second.linked) {
        // Match Pass 1 drops Pass 2's own settings from this preset.
        std::erase_if(profile.changes, [](const auto& change) {
            return change.key.starts_with("Pass2") && change.key != "Pass2SameAsPass1";
        });
    }
    for (const auto& key : restore) { Appearance::ClearChange(profile, key); }
    const auto* nrChange = Appearance::FindChange(profile, "NeuralRendering");
    const auto* sharpeningChange = Appearance::FindChange(profile, "SharpeningEnabled");
    if ((nrChange && nrChange->points[0] != 0 && !base.neural) || (sharpeningChange && sharpeningChange->points[0] != 0 && !base.sharpening)) {
        Note("Presets can switch NR or sharpening off, but not on while Base has them off.");
    }

    ImGui::Spacing();
    bool remove = false;
    if (ImGui::Button("Delete preset...")) { ImGui::OpenPopup("deletePreset"); }
    if (ImGui::BeginPopup("deletePreset")) {
        ImGui::Text("Delete \"%s\"?", ShownName(preset));
        ImGui::TextDisabled("Where it applied, Base or another preset is used.\nSave as default deletes its preset file; Discard undoes this.");
        if (ImGui::Button("Delete")) { remove = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    // Adding or removing presets invalidates the references held above.
    if (fullCopy) {
        const auto name = preset.name + " (full)";
        auto full = FullProfile(profile, base);
        if (const auto id = Appearance::AddPreset(settings, name, std::move(full))) { presetSelected = id; }
    }
    if (remove) { Appearance::RemovePreset(settings, presetSelected); presetSelected = BaseRow; }
    settings.enabled = Appearance::UsesPresets(settings);
}

void OverlayUI::DrawPresetSharpeningStatus(const FrameView& view)
{
    const auto& state = view.appearance;
    if (!state.result.active || state.result.sharpnessSource == "Base") { return; }
    ImGui::TextDisabled("Now %.2f from the \"%s\" preset.", state.result.setup.sharpness, state.result.sharpnessSource.c_str());
}
