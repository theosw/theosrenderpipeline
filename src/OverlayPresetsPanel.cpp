#include <PCH.h>
#include "Localization.h"
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
constexpr std::array<const char*, 6> GroupLabels{"Anywhere outside", "Clear", "Cloudy", "Rain", "Snow", "Interiors"};

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
    // Matches the runtime: a linked pass takes Pass 1's values.
    setup.extra = NeuralRendering::EffectiveExtraPasses(nr.neuralExtraPasses, nr.neuralReconstruction, nr.neuralTuning);
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
    nr.neuralExtraPasses = setup.extra;
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
    return Appearance::Valid(record) ? std::format("{} / {:06X}", record.plugin, record.localID) : TR("Unavailable");
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
        if (preset.groups[i]) { text += std::format("{}{}", text.empty() ? "" : ", ", TR(GroupLabels[i])); }
    }
    if (weathers) { text += TRF("{}{} weather{}", text.empty() ? "" : " + ", weathers, weathers == 1 ? "" : "s"); }
    if (text.empty()) { text = TR("Not used yet"); }
    const auto changes = preset.profile.changes.size();
    text += changes ? std::format(" | {} change{}", changes, changes == 1 ? "" : "s") : " | no changes";
    return preset.profile.enabled ? text : "Off | " + text;
}
void Note(const char* text) { ImGui::TextDisabled("%s", text); }
const char* ShownName(const Appearance::NamedProfile& preset) { return preset.name.empty() ? TR("Unnamed override") : preset.name.c_str(); }
void Tooltip(const char* text)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { ImGui::SetTooltip("%s", text); }
}
// Base's value of one setting, as the controls show it.
std::string FieldValue(const Appearance::Field& field, const Appearance::Setup& setup)
{
    const float value = field.get(setup);
    const std::string_view key = field.key;
    if (key == "BeforeUpscaling") { return value != 0 ? TR("before upscaling") : TR("after upscaling"); }
    if (key == "Passes")
    {
        static constexpr const char* counts[]{"one pass", "two passes", "three passes", "four passes", "five passes"};
        static_assert(std::size(counts) == NeuralRendering::kMaxNeuralPasses);
        return TR(counts[std::clamp(static_cast<int>(value), 1, NeuralRendering::kMaxNeuralPasses) - 1]);
    }
    if (key.ends_with("SameAsPass1")) { return value != 0 ? TR("follows Pass 1") : TR("own settings"); }
    if (key == "ReconstructionMethod") { return value == 2 ? TR("Ratio") : value == 1 ? TR("Residual") : TR("Auto"); }
    if (key == "ModelRate") { return value != 0 ? TR("every other frame") : TR("every frame"); }
    // Keys stay English in the INI, so they are compared without translating.
    if (key.ends_with("Network")) { return NeuralNetworkName(value != 0 ? 1 : 0); }
    if (key.ends_with("Style")) { return NeuralStyleName(static_cast<int>(value)); }
    if (key.ends_with("InputScale")) { return std::format("{:.1f}%", value * 100); }
    if (key == "ReturnDelay") { return TRF("{:.1f} s", value); }
    if (field.integral && field.low == 0 && field.high == 1) { return value != 0 ? TRC("on", "OverlayPresetsPanel") : TRC("off", "OverlayPresetsPanel"); }
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
        right -= ImGui::CalcTextSize(TR("NOW")).x;
        draw->AddText(ImVec2(right, pos.y + style.FramePadding.y), ImGui::GetColorU32(kSage), TR("NOW"));
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
    int ReferencePasses() const override { return base.passes; }
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

    DrawSettingsHeading(TR("Weather overrides"));
    Note("Change settings for the weathers and places you choose, with different values by time of day if you want. "
         "Everywhere else uses Base.");
    if (scene.valid && !settings.presets.empty()) {
        const auto applied = state.paused ? std::string(TR("Base (paused)")) : state.result.incoming;
        const auto place = scene.interior ? std::string(TR("Interior")) : ShortWeatherLabel(scene.incoming.record, *catalogue);
        ImGui::Text(TR("Now: %s | %s | %s"), applied.c_str(), Clock(scene.hour).c_str(), place.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (!scene.interior) { ImGui::TextUnformatted(WeatherLabel(scene.incoming.record, *catalogue).c_str()); }
            const auto time = TimeIndex(settings.hours, scene.hour);
            ImGui::Text(TR("%s, blending toward %s"), Appearance::Times[time], Appearance::Times[(time + 1) % Appearance::Times.size()]);
            const auto& setup = state.result.setup;
            ImGui::Text(TR("Pass 1: intensity %.2f | tone %.2f | structure %.2f"), setup.tuning.intensity,
                setup.tuning.localToneStrength, setup.tuning.localStructureStrength);
            for (int pass = 2; pass <= setup.passes; ++pass) {
                const auto& extra = setup.extra[pass - 2];
                if (extra.linked) { continue; }
                ImGui::Text(TR("Pass %d: intensity %.2f | tone %.2f | structure %.2f"), pass, extra.tuning.intensity,
                    extra.tuning.localToneStrength, extra.tuning.localStructureStrength);
            }
            if (!cs) { ImGui::Text(TR("Sharpening strength %.3f"), setup.sharpness); }
            ImGui::EndTooltip();
        }
        if (!scene.interior && !state.paused && Appearance::Valid(scene.outgoing.record) && scene.transition < 1) {
            ImGui::TextDisabled(TR("Changing from %s"), state.result.outgoing.c_str());
            ImGui::SameLine();
            ImGui::ProgressBar(scene.transition, ImVec2(Px(120.0f), 0), "");
        }
    }
    if (!settings.presets.empty()) {
        bool paused = state.paused;
        if (ImGui::Checkbox(TR("Pause overrides"), &paused)) { PausePresets(paused); }
        Tooltip(TR("Uses Base right away, until you untick this or restart the game. Not saved."));
    }

    // Mark the overrides the edited configuration would select for the current weather.
    Appearance::Selection now;
    if (scene.valid && Appearance::UsesPresets(settings) && !state.paused) {
        now = Appearance::Select(Appearance::ResolveClaims(settings), scene.incoming, scene.interior);
    }
    const bool baseNow = scene.valid && std::ranges::none_of(now.layers, [](const auto* layer) { return layer != nullptr; });
    if (!PresetEditorSelected()) { presetSelected = BaseRow; }

    ImGui::Spacing();
    if (PresetRow(-1, TR("Base"), TR("Your settings, used everywhere else"), presetSelected == BaseRow, baseNow, false)) { presetSelected = BaseRow; }
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
    if (ImGui::Button(TR("New override"))) {
        if (const auto id = Appearance::AddPreset(settings, settings.presets.empty() ? TR("My first override") : TR("New override"))) { presetSelected = id; }
    }
    Tooltip(TR("Starts with no changes. Choose when it applies, then change the settings you want different from Base."));
    if (const auto* selected = Appearance::FindPreset(settings, presetSelected)) {
        ImGui::SameLine();
        if (ImGui::Button(TR("Duplicate"))) {
            const auto copy = *selected;
            if (const auto id = Appearance::AddPreset(settings, copy.name + " copy", copy.profile)) { presetSelected = id; }
        }
        Tooltip(TR("Copies the selected override's changes. Choose when the copy applies."));
    }
    ImGui::EndDisabled();
    if (Appearance::FindPreset(settings, presetSelected)) {
        const auto index = std::ranges::find_if(settings.presets, [&](const auto& item) { return item.id == presetSelected; }) - settings.presets.begin();
        ImGui::SameLine();
        ImGui::BeginDisabled(index == 0);
        if (ImGui::Button(TR("Move up"))) { Appearance::MovePreset(settings, presetSelected, -1); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(index + 1 == static_cast<std::ptrdiff_t>(settings.presets.size()));
        if (ImGui::Button(TR("Move down"))) { Appearance::MovePreset(settings, presetSelected, 1); }
        ImGui::EndDisabled();
        Tooltip(TR("When two overrides use the same weather, the higher one wins."));
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Timing..."))) { ImGui::OpenPopup("presetTiming"); }
    Tooltip(TR("When each time of day starts, and how smoothly overrides change."));
    if (ImGui::BeginPopup("presetTiming")) {
        ImGui::PushItemWidth(Px(220.0f));
        ImGui::SliderFloat(TR("Transition smoothing (seconds)"), &settings.smoothingSeconds, 0, 10, "%.1f");
        ImGui::TextDisabled("%s", TR("Softens changes between overrides. Zero follows weather and time directly."));
        ImGui::Spacing();
        ImGui::TextUnformatted(TR("Time of day starts (game hours)"));
        for (std::size_t i = 0; i < Appearance::Times.size(); ++i) { ImGui::InputFloat(Appearance::Times[i], &settings.hours[i], .25f, 1, "%.2f"); }
        ImGui::PopItemWidth();
        if (ImGui::Button(TR("Reset times"))) { settings.hours = Appearance::DefaultHours; }
        ImGui::SameLine();
        if (ImGui::Button(TR("Copy ENB timing"))) {
            wchar_t executable[MAX_PATH]{};
            const auto length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
            const auto path = std::filesystem::path(executable).parent_path() / L"enbseries.ini";
            CSimpleIniA ini;
            const auto hours = length && length < MAX_PATH && ini.LoadFile(path.c_str()) >= 0 ? Appearance::ReadENBSchedule(ini) : std::nullopt;
            if (hours) { settings.hours = *hours; actionMessage = TR("Copied ENB clock markers and dawn/dusk boundaries. Review the times, then Apply."); actionMessageIsError = false; }
            else { actionMessage = TR("Could not derive an increasing 24-hour schedule from the game's enbseries.ini. Enter the times manually."); actionMessageIsError = true; }
        }
        Tooltip("Uses ENB's Night, Sunrise, Day and Sunset markers, plus Sunrise minus DawnDuration and Sunset plus DuskDuration. "
            "It does not reproduce ENB's internal phase weights or change ENB settings.");
        ImGui::TextDisabled("%s", TR("Times must increase from Night to Dusk. Overrides blend between neighbouring times."));
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
        flow.Text(changes ? TRF("{} change{}", changes, changes == 1 ? "" : "s").c_str() : TR("No changes yet"));
        Tooltip(TR("Changed settings are amber. Everything else follows Base, including later Base edits."));
        flow.Checkbox(TR("Use"), &profile.enabled);
        Tooltip(TR("Unticked overrides keep their settings but are ignored."));
        ImGui::BeginDisabled(profile.changes.empty());
        if (flow.Button(TR("Reset all to Base"))) { profile.changes.clear(); }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(settings.presets.size() >= Appearance::MaxPresets);
        if (flow.Button(TR("Save full copy"))) { fullCopy = true; }
        ImGui::EndDisabled();
        Tooltip(TR("Adds a copy with every setting stored, so it looks the same with anyone's Base. Share its file."));
    }
    const auto claims = Appearance::ResolveClaims(settings);
    // A claim a higher override already uses is shown muted with who uses it.
    const auto shadowedBy = [&](const Appearance::NamedProfile* owner) {
        return owner && owner != &preset ? owner : nullptr;
    };
    // One checkbox per weather type or place, flowing within its cell; any number can be ticked.
    const auto groupToggle = [&](Flow& flow, Appearance::Group value) {
        const auto i = static_cast<std::size_t>(value);
        const auto* owner = shadowedBy(claims.groups[i]);
        // Decided once: the checkbox may change the value between push and pop.
        const bool muted = owner && preset.groups[i];
        if (muted) { ImGui::PushStyleColor(ImGuiCol_Text, kMuted); }
        flow.Checkbox(TR(GroupLabels[i]), &preset.groups[i]);
        if (muted) { ImGui::PopStyleColor(); }
        if (owner) {
            Tooltip(TRF("\"{}\" uses this{}. The higher override in the list wins; move this one up to use it here.",
                ShownName(*owner), preset.groups[i] ? " instead" : "").c_str());
        } else if (value == Appearance::Group::Exterior) {
            Tooltip(TR("Any outdoor weather not covered by a more specific override."));
        }
    };
    std::optional<Appearance::Record> unassign;
    bool openPicker = false;
    const auto grid = NeuralRowsGrid();
    const GridScope gridScope(grid);
    if (BeginSettingRows("overrideWhen", LabelWidth({TR("Scene normalization")}))) {
        SettingRow(TR("Name"), nullptr, false, false, [&] {
            char name[81]{}; std::snprintf(name, sizeof(name), "%s", preset.name.c_str());
            // The name is the override's file name, so characters files cannot use are not typed.
            const bool changed = ImGui::InputText("##v", name, sizeof(name), ImGuiInputTextFlags_CallbackCharFilter,
                [](ImGuiInputTextCallbackData* data) { return Appearance::FileNameCharacter(data->EventChar) ? 0 : 1; });
            if (changed) { preset.name = name; }
            return changed;
        });

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        HintLabel("Weathers", "An exact weather wins over a weather type, which wins over Anywhere outside. If two "
                              "overrides share one, the higher in the list wins.");
        ImGui::TableNextColumn();
        {
            Flow flow;
            for (std::size_t i = 0; i < preset.weathers.size(); ++i) {
                const auto& record = preset.weathers[i];
                const bool loaded = FindWeather(record, *catalogue);
                const auto* owner = shadowedBy(Appearance::WeatherOwner(claims, record));
                const auto label = std::format("{}  x##chip{}", ShortWeatherLabel(record, *catalogue), i);
                flow.Next(ImGui::CalcTextSize(label.c_str(), nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2);
                if (Chip(label.c_str(), !loaded || owner)) { unassign = record; }
                Tooltip(TRF("{}{}{}\nClick to remove.", WeatherLabel(record, *catalogue),
                    loaded ? "" : "\nNot loaded this session; kept for when its plugin returns.",
                    owner ? std::format("\n\"{}\" uses this instead; it is higher in the list.", ShownName(*owner)) : std::string()).c_str());
            }
            const bool currentHere = Appearance::HasWeather(preset, scene.incoming.record);
            ImGui::BeginDisabled(!scene.valid || scene.interior || !Appearance::Valid(scene.incoming.record) || currentHere);
            if (flow.Button(TR("Add current"))) {
                const std::array records{scene.incoming.record};
                if (!Appearance::AddWeathers(settings, preset.id, records)) { actionMessage = TR("Weather assignment limit reached."); actionMessageIsError = true; }
            }
            ImGui::EndDisabled();
            if (scene.valid && !scene.interior) {
                Tooltip(currentHere ? TRF("{} is already in this override.", WeatherLabel(scene.incoming.record, *catalogue)).c_str()
                                    : WeatherLabel(scene.incoming.record, *catalogue).c_str());
            } else {
                Tooltip(TR("Outside, adds the weather you are in."));
            }
            if (flow.Button(TR("Add..."))) { openPicker = true; }
            Tooltip(TR("Pick exact weathers, such as fog or ash, from every weather the game has loaded."));
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("Weather types"));
        ImGui::TableNextColumn();
        {
            Flow flow(Px(18.0f));
            for (const auto value : {Appearance::Group::Clear, Appearance::Group::Cloudy, Appearance::Group::Rain, Appearance::Group::Snow}) {
                groupToggle(flow, value);
            }
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("Places"));
        ImGui::TableNextColumn();
        {
            Flow flow(Px(18.0f));
            groupToggle(flow, Appearance::Group::Interior);
            groupToggle(flow, Appearance::Group::Exterior);
        }

        // All day, or one look per time of day that blends into the next.
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        HintLabel(TR("Time of day"), "Pick a time to give it its own look; it blends into the next. Intensity, local tone, "
                                 "local structure and sharpening strength can differ by time; other settings stay the same "
                                 "all day. Green is now.");
        ImGui::TableNextColumn();
        {
            const auto& times = Appearance::Times;
            std::array<std::string, 6> help;
            for (std::size_t i = 0; i < times.size(); ++i) {
                const auto next = (i + 1) % times.size();
                help[i] = TRF("{}, from {}, blending toward {} at {}.{}", times[i], Clock(settings.hours[i]),
                    times[next], Clock(settings.hours[next]), i == nowIndex ? " Now." : "");
            }
            const auto time = [&](std::size_t i) { return Segment{times[i], true, help[i].c_str(), i == nowIndex}; };
            int selected = presetTimed ? presetTime + 1 : 0;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (Segmented("timeOfDay", selected, {{TR("All day"), true, TR("One look all day. Choosing this keeps the selected time's values.")},
                    time(0), time(1), time(2), time(3), time(4), time(5)})) {
                if (selected == 0) {
                    // Back to one look: the selected time's values apply all day.
                    presetTimed = false;
                    for (auto& change : profile.changes) { const auto point = change.points[presetTime]; change.points.fill(point); }
                } else {
                    presetTimed = true;
                    presetTime = selected - 1;
                }
            }
        }
        ImGui::EndTable();
    }
    if (unassign) { Appearance::RemoveWeather(settings, preset.id, *unassign); }
    if (openPicker) { ImGui::OpenPopup("weatherPicker"); }
    ImGui::Spacing();
    if (ImGui::BeginPopup("weatherPicker")) {
        ImGui::Text(TR("Add weathers to %s"), ShownName(preset));
        ImGui::PushItemWidth(Px(420.0f));
        ImGui::InputTextWithHint(TR("Search"), TR("Name, plugin, FormID or weather type"), presetSearch, sizeof(presetSearch));
        const char* filters[]{TR("All"), TR("Unclassified"), TR("Clear"), TR("Cloudy"), TR("Rain"), TR("Snow")};
        ImGui::Combo(TR("Weather type"), &presetWeatherGroup, filters, 6);
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
                if (Appearance::HasWeather(preset, entry.weather.record)) { ImGui::SameLine(); ImGui::TextDisabled("%s", TR("(in this override)")); }
                else if (const auto* owner = Appearance::WeatherOwner(claims, entry.weather.record)) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(TR("-> %s"), ShownName(*owner));
                }
                ImGui::PopID();
            } }
        }
        ImGui::EndChild();
        ImGui::Text(TR("%zu loaded | %zu shown | %zu selected"), catalogue->size(), filtered.size(), presetPickerSelection.size());
        if (ImGui::Button(TR("Select shown"))) {
            for (const auto* entry : filtered) {
                if (std::ranges::find(presetPickerSelection, entry->weather.record) == presetPickerSelection.end()) { presetPickerSelection.push_back(entry->weather.record); }
            }
        }
        ImGui::SameLine(); if (ImGui::Button(TR("Clear selection"))) { presetPickerSelection.clear(); }
        ImGui::SameLine(); if (ImGui::Button(TR("Refresh list"))) { RefreshWeatherList(); }
        ImGui::BeginDisabled(presetPickerSelection.empty());
        if (ImGui::Button(TR("Add selected"))) {
            if (Appearance::AddWeathers(settings, preset.id, presetPickerSelection)) { presetPickerSelection.clear(); ImGui::CloseCurrentPopup(); }
            else { actionMessage = TR("Weather assignment limit reached; no assignments were changed."); actionMessageIsError = true; }
        }
        ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::Button(TR("Close"))) { ImGui::CloseCurrentPopup(); }
        ImGui::TextDisabled("%s", TR("A weather in several overrides uses the highest one in the list. Names come from the game or an editor-ID plugin."));
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
    ActiveEditScope() = preset.id;
    drawSettings(nr, sharpening, sharpness);
    ActiveEditScope() = 0;
    ActivePresetDecor() = nullptr;
    auto edited = base;
    edited.neural = nr.neuralEnabled;
    edited.beforeUpscaling = nr.neuralBeforeUpscaling;
    edited.passes = nr.neuralPasses;
    edited.combat = nr.neuralCombat;
    edited.reconstruction = nr.neuralReconstruction;
    edited.tuning = nr.neuralTuning;
    edited.extra = nr.neuralExtraPasses;
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
    for (std::size_t i = 0; i < edited.extra.size(); ++i) {
        if (!edited.extra[i].linked || shown.extra[i].linked) { continue; }
        // Reset to Pass 1 drops only this pass's own settings from this preset.
        const auto prefix = std::format("Pass{}", i + 2);
        std::erase_if(profile.changes, [&](const auto& change) {
            return change.key.starts_with(prefix) && change.key != prefix + "SameAsPass1";
        });
    }
    for (const auto& key : restore) { Appearance::ClearChange(profile, key); }
    const auto* nrChange = Appearance::FindChange(profile, "NeuralRendering");
    const auto* sharpeningChange = Appearance::FindChange(profile, "SharpeningEnabled");
    if ((nrChange && nrChange->points[0] != 0 && !base.neural) || (sharpeningChange && sharpeningChange->points[0] != 0 && !base.sharpening)) {
        Note(TR("Overrides can switch NR or sharpening off, but not on while Base has them off."));
    }

    ImGui::Spacing();
    bool remove = false;
    if (ImGui::Button(TR("Delete override..."))) { ImGui::OpenPopup("deletePreset"); }
    if (ImGui::BeginPopup("deletePreset")) {
        ImGui::Text(TR("Delete \"%s\"?"), ShownName(preset));
        ImGui::TextDisabled("%s", TR("Where it applied, Base or another override is used.\nSave as default deletes its file; Discard undoes this."));
        if (ImGui::Button(TR("Delete"))) { remove = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button(TR("Cancel"))) { ImGui::CloseCurrentPopup(); }
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

namespace
{
// Names the most specific override in use now that changes a setting, and its value now.
// Resolves the overrides selected for the incoming weather as the runtime does, so a
// note shows the value they give and names the most specific one that sets it. During
// a weather transition the runtime still blends from the outgoing weather.
struct BaseOverrideNotes final : OverrideNotes
{
    Appearance::Selection now;
    const Appearance::Setup& base;
    Appearance::Setup resolved;
    bool interior;
    BaseOverrideNotes(Appearance::Selection now, const Appearance::Settings& settings, const Appearance::Setup& base, float hour, bool interior)
        : now(now), base(base), resolved(Appearance::Resolve(Appearance::Flatten(now), settings, hour, base)), interior(interior) {}
    std::string Note(const char* key) const override
    {
        const auto* field = Appearance::FindField(key);
        if (!field || field->get(resolved) == field->get(base)) { return {}; }
        const char* owner = nullptr;
        for (auto it = now.layers.rbegin(); it != now.layers.rend() && !owner; ++it) {
            if (*it && Appearance::FindChange((*it)->profile, key)) { owner = ShownName(**it); }
        }
        const auto where = interior ? "place" : "weather";
        const auto value = FieldValue(*field, resolved);
        return owner ? TRF("\"{}\" replaces this in this {} ({}). Base still applies everywhere else.", owner, where, value)
                     : TRF("This {}'s weather overrides change this ({}). Base still applies everywhere else.", where, value);
    }
};
}

void OverlayUI::DrawBaseSettings(const FrameView& view, const std::function<void()>& drawSettings)
{
    const auto& state = view.appearance;
    const auto& settings = settingsDraft.appearance;
    const auto& scene = state.context;
    Appearance::Selection now;
    if (scene.valid && Appearance::UsesPresets(settings) && !state.paused) {
        now = Appearance::Select(Appearance::ResolveClaims(settings), scene.incoming, scene.interior);
    }
    const auto base = DraftSetup(settingsDraft);
    BaseOverrideNotes notes(now, settings, base, scene.hour, scene.interior);
    const bool any = std::ranges::any_of(now.layers, [](const auto* layer) { return layer != nullptr; });
    if (any) { ActiveOverrideNotes() = &notes; }
    drawSettings();
    ActiveOverrideNotes() = nullptr;
}

void OverlayUI::DrawPresetSharpeningStatus(const FrameView& view)
{
    const auto& state = view.appearance;
    if (!state.result.active || state.result.sharpnessSource == "Base") { return; }
    ImGui::TextDisabled(TR("Now %.2f from the \"%s\" weather override."), state.result.setup.sharpness, state.result.sharpnessSource.c_str());
}
