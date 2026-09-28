#pragma once

#include "WeatherAppearanceSetup.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <span>
#include <vector>

namespace TheosRenderPipeline::Appearance
{
inline constexpr std::array<const char*, 6> Times{"Night", "Dawn", "Sunrise", "Day", "Sunset", "Dusk"};
inline constexpr std::array<float, 6> DefaultHours{0, 5, 7, 12, 18, 20};
enum class Group : std::size_t { Exterior, Clear, Cloudy, Rain, Snow, Interior, Count };
inline constexpr std::array<const char*, 6> Groups{"Exterior", "Clear", "Cloudy", "Rain", "Snow", "Interior"};
inline constexpr std::size_t MaxWeathers = 4096;
inline constexpr std::size_t MaxPresets = 512;

inline float FiniteClamp(float value, float low, float high, float fallback)
{
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

struct Record
{
    std::string plugin;
    std::uint32_t localID{};
    bool operator==(const Record&) const = default;
};
inline Record Normalize(Record record)
{
    // Plugin names are case-insensitive on Skyrim's supported Windows filesystem.
    for (char& c : record.plugin) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c + ('a' - 'A')); }
    }
    return record;
}
inline bool Valid(const Record& record)
{
    return !record.plugin.empty() && record.plugin.size() <= 255 && record.localID > 0 &&
        record.localID <= 0xFFFFFF && record.plugin.find_first_of("/\\\r\n\t") == std::string::npos;
}
inline Record RuntimeRecord(std::string plugin, std::uint32_t formID, bool light)
{
    // Strip the full-plugin index or both parts of the FE light-plugin index.
    return Normalize({std::move(plugin), formID & (light ? 0xFFFu : 0xFFFFFFu)});
}

// One setting a preset changes. Look settings keep a value per time of day;
// other settings hold the same value at every point.
struct Change
{
    std::string key;
    std::array<float, 6> points{};
    bool operator==(const Change&) const = default;
};
// A preset changes only the settings it lists; everything else follows Base.
struct Profile
{
    bool enabled{true};
    std::vector<Change> changes;
    bool operator==(const Profile&) const = default;
};
inline const Change* FindChange(const Profile& profile, std::string_view key)
{
    for (const auto& change : profile.changes) { if (change.key == key) { return &change; } }
    return nullptr;
}
// time < 0 sets every time of day. A new look change at one time starts from value everywhere.
inline void SetChange(Profile& profile, std::string_view key, float value, int time = -1)
{
    const auto* field = FindField(key);
    if (!field) { return; }
    value = ClampField(*field, value);
    auto it = std::ranges::find_if(profile.changes, [&](const auto& change) { return change.key == key; });
    if (it == profile.changes.end()) {
        const auto index = FieldIndex(key);
        it = profile.changes.insert(std::ranges::find_if(profile.changes, [&](const auto& change) { return FieldIndex(change.key) > index; }),
            Change{std::string(key)});
        it->points.fill(value);
    }
    if (time < 0 || !field->look) { it->points.fill(value); }
    else { it->points[static_cast<std::size_t>(time)] = value; }
}
inline void ClearChange(Profile& profile, std::string_view key)
{
    std::erase_if(profile.changes, [&](const auto& change) { return change.key == key; });
}
inline bool Timed(const Profile& profile)
{
    return std::ranges::any_of(profile.changes, [](const auto& change) {
        return std::ranges::any_of(change.points, [&](float point) { return point != change.points[0]; });
    });
}
struct WeatherProfile
{
    Record record;
    std::uint32_t preset{};
    bool operator==(const WeatherProfile&) const = default;
};
struct NamedProfile
{
    std::uint32_t id{};
    std::string name;
    Profile profile;
    bool operator==(const NamedProfile&) const = default;
};
struct Settings
{
    // Derived by Sanitize: presets apply whenever one is in use. Pause is the
    // session-only override. Still persisted so older builds read a matching value.
    bool enabled{};
    float smoothingSeconds{2};
    std::array<float, 6> hours{DefaultHours};
    std::array<std::uint32_t, 6> groups{};
    std::vector<NamedProfile> presets;
    std::vector<WeatherProfile> weathers;
    bool operator==(const Settings&) const = default;
};
inline const NamedProfile* FindPreset(const Settings& settings, std::uint32_t id)
{
    if (id) {
        for (const auto& preset : settings.presets) { if (preset.id == id) { return &preset; } }
    }
    return nullptr;
}
inline std::uint32_t AddPreset(Settings& settings, std::string name, Profile profile = {})
{
    if (settings.presets.size() >= MaxPresets) { return 0; }
    std::uint32_t id = 1;
    while (FindPreset(settings, id)) { ++id; }
    settings.presets.push_back({id, std::move(name), std::move(profile)});
    return id;
}
inline bool Assign(Settings& settings, Record record, std::uint32_t preset)
{
    record = Normalize(std::move(record));
    if (!Valid(record) || (preset && !FindPreset(settings, preset))) { return false; }
    for (auto it = settings.weathers.begin(); it != settings.weathers.end(); ++it) {
        if (it->record == record) {
            if (preset) { it->preset = preset; } else { settings.weathers.erase(it); }
            return true;
        }
    }
    if (!preset) { return true; }
    if (settings.weathers.size() >= MaxWeathers) { return false; }
    settings.weathers.push_back({std::move(record), preset});
    return true;
}
inline void RemovePreset(Settings& settings, std::uint32_t id)
{
    std::erase_if(settings.presets, [=](const auto& item) { return item.id == id; });
    std::erase_if(settings.weathers, [=](const auto& item) { return item.preset == id; });
    for (auto& group : settings.groups) { if (group == id) { group = 0; } }
}
inline bool AssignMany(Settings& settings, std::span<const Record> records, std::uint32_t preset)
{
    auto updated = settings;
    for (const auto& record : records) { if (!Assign(updated, record, preset)) { return false; } }
    settings = std::move(updated);
    return true;
}
inline bool UsesPresets(const Settings& settings)
{
    return std::ranges::any_of(settings.presets, [](const auto& preset) { return preset.profile.enabled; });
}
// Presets that request two passes keep two allocated, so dropping to one never recreates NR.
inline bool PresetsRequestTwoPasses(const Settings& settings)
{
    return std::ranges::any_of(settings.presets, [](const auto& preset) {
        const auto* passes = FindChange(preset.profile, "Passes");
        return preset.profile.enabled && passes && passes->points[0] == 2;
    });
}
inline bool ValidHours(const std::array<float, 6>& hours)
{
    for (std::size_t i = 0; i < hours.size(); ++i) {
        if (!std::isfinite(hours[i]) || hours[i] < 0 || hours[i] >= 24 ||
            (i && hours[i] - hours[i - 1] < 0.01f)) { return false; }
    }
    return true;
}
inline Profile Sanitize(Profile profile)
{
    std::vector<Change> changes;
    for (auto& change : profile.changes) {
        const auto* field = FindField(change.key);
        if (!field || std::ranges::any_of(changes, [&](const auto& old) { return old.key == change.key; })) { continue; }
        const float fallback = field->get(Setup{});
        for (auto& point : change.points) { point = ClampField(*field, std::isfinite(point) ? point : fallback); }
        if (!field->look) { change.points.fill(change.points[0]); }
        changes.push_back(std::move(change));
    }
    std::ranges::stable_sort(changes, {}, [](const auto& change) { return FieldIndex(change.key); });
    profile.changes = std::move(changes);
    return profile;
}
inline Settings Sanitize(Settings settings)
{
    settings.smoothingSeconds = FiniteClamp(settings.smoothingSeconds, 0, 30, 2);
    if (!ValidHours(settings.hours)) { settings.hours = DefaultHours; }
    std::vector<NamedProfile> presets;
    for (auto& preset : settings.presets) {
        if (!preset.id || preset.id > 0x7FFFFFFF || presets.size() == MaxPresets ||
            std::ranges::any_of(presets, [&](const auto& old) { return old.id == preset.id; })) { continue; }
        for (char& c : preset.name) { if (static_cast<unsigned char>(c) < 32) { c = ' '; } }
        // INI values cannot safely retain leading/trailing whitespace or line breaks.
        const auto start = preset.name.find_first_not_of(' ');
        preset.name = start == std::string::npos ? std::format("Preset {}", preset.id) :
            preset.name.substr(start, preset.name.find_last_not_of(' ') - start + 1);
        if (preset.name.size() > 80) { preset.name.resize(80); }
        preset.profile = Sanitize(std::move(preset.profile));
        presets.push_back(std::move(preset));
    }
    settings.presets = std::move(presets);
    for (auto& id : settings.groups) { if (!FindPreset(settings, id)) { id = 0; } }
    std::vector<WeatherProfile> unique;
    for (auto& entry : settings.weathers) {
        entry.record = Normalize(std::move(entry.record));
        if (!Valid(entry.record) || !FindPreset(settings, entry.preset) || unique.size() == MaxWeathers ||
            std::ranges::any_of(unique, [&](const auto& old) { return old.record == entry.record; })) { continue; }
        unique.push_back(std::move(entry));
    }
    settings.weathers = std::move(unique);
    settings.enabled = UsesPresets(settings);
    return settings;
}

struct Weather
{
    Record record;
    Group group{Group::Exterior};
};
struct WeatherEntry
{
    std::uint32_t runtimeID{};
    Weather weather;
    std::string name, label, search;
};
inline std::string SearchKey(std::string text)
{
    for (char& c : text) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
    return text;
}
inline WeatherEntry CatalogueEntry(std::uint32_t runtimeID, Weather weather, std::string name)
{
    WeatherEntry result{runtimeID, std::move(weather), std::move(name)};
    const auto& record = result.weather.record;
    result.label = std::format("{}{}{} / {:06X}", result.name, result.name.empty() ? "" : " — ", record.plugin, record.localID);
    result.search = SearchKey(std::format("{} {} {:08X}", result.label, Groups[static_cast<std::size_t>(result.weather.group)], runtimeID));
    return result;
}
struct Context
{
    bool valid{}, interior{};
    float hour{}, transition{1};
    std::uint32_t cell{};
    Weather outgoing, incoming;
};
inline float Hour(float hour) { return std::fmod(std::fmod(hour, 24.0f) + 24.0f, 24.0f); }
inline float AtTime(const std::array<float, 6>& points, const std::array<float, 6>& hours, float hour)
{
    const float time = Hour(hour);
    for (std::size_t i = 0; i < hours.size(); ++i) {
        const auto next = (i + 1) % hours.size();
        const float end = next ? hours[next] : hours[0] + 24;
        const float sample = time < hours[0] ? time + 24 : time;
        if (sample >= hours[i] && sample < end) {
            return std::lerp(points[i], points[next], (sample - hours[i]) / (end - hours[i]));
        }
    }
    return points[0];
}
// Presets for one weather, least specific first. Disabled presets are skipped.
struct Selection
{
    std::array<const NamedProfile*, 3> layers{};
};
inline void Add(Selection& selection, const NamedProfile* preset)
{
    if (!preset || !preset->profile.enabled) { return; }
    for (auto& layer : selection.layers) { if (!layer) { layer = preset; return; } }
}
// Resolve only when weather, location or configuration changes. These pointers
// refer to the controller's owned configuration and never escape in UI snapshots.
inline Selection Select(const Settings& settings, const Weather& weather, bool interior)
{
    Selection result;
    if (interior) {
        Add(result, FindPreset(settings, settings.groups[static_cast<std::size_t>(Group::Interior)]));
        return result;
    }
    Add(result, FindPreset(settings, settings.groups[0]));
    const auto group = static_cast<std::size_t>(weather.group);
    if (group > 0 && group < static_cast<std::size_t>(Group::Interior)) {
        Add(result, FindPreset(settings, settings.groups[group]));
    }
    for (const auto& entry : settings.weathers) {
        if (entry.record == weather.record) {
            Add(result, FindPreset(settings, entry.preset));
            break;
        }
    }
    return result;
}
// Most specific first, for display.
inline std::string Names(const Selection& selection)
{
    std::string text;
    for (auto it = selection.layers.rbegin(); it != selection.layers.rend(); ++it) {
        if (*it) { text += std::format("{}{}", text.empty() ? "" : " + ", (*it)->name); }
    }
    return text.empty() ? "Base" : text;
}
inline std::string SourceOf(const Selection& selection, std::string_view key)
{
    for (auto it = selection.layers.rbegin(); it != selection.layers.rend(); ++it) {
        if (*it && FindChange((*it)->profile, key)) { return (*it)->name; }
    }
    return "Base";
}
// A selection's changes with their fields looked up once, least specific first.
using Changes = std::vector<std::pair<const Field*, const Change*>>;
inline Changes Flatten(const Selection& selection)
{
    Changes result;
    for (const auto* layer : selection.layers) {
        if (!layer) { continue; }
        for (const auto& change : layer->profile.changes) {
            if (const auto* field = FindField(change.key)) { result.emplace_back(field, &change); }
        }
    }
    return result;
}
inline Setup Resolve(const Changes& changes, const Settings& settings, float hour, const Setup& base)
{
    Setup result = base;
    for (const auto& [field, change] : changes) {
        field->set(result, field->look ? AtTime(change->points, settings.hours, hour) : change->points[0]);
    }
    // Presets can switch NR or sharpening off, never on: Base carries availability.
    result.neural = base.neural && result.neural;
    result.sharpening = base.sharpening && result.sharpening;
    return result;
}
struct Evaluation
{
    Setup setup;
    std::string outgoing{"Base"}, incoming{"Base"};
    std::string sharpnessSource{"Base"};
    bool active{};
};
inline Evaluation Evaluate(const Settings& settings, const Context& context, Setup base)
{
    if (!settings.enabled || !context.valid || !std::isfinite(context.hour)) { return {base}; }
    const auto incoming = Select(settings, context.incoming, context.interior);
    // Skyrim can have no outgoing weather outside a transition. Do not fade from stale state.
    const auto outgoing = Valid(context.outgoing.record) && !context.interior ?
        Select(settings, context.outgoing, false) : incoming;
    return {Blend(Resolve(Flatten(outgoing), settings, context.hour, base), Resolve(Flatten(incoming), settings, context.hour, base),
        context.interior ? 1 : context.transition), Names(outgoing), Names(incoming), SourceOf(incoming, "Sharpness"), true};
}

class Smoother
{
public:
    // Look settings ease toward the target; other settings take it immediately.
    Setup Update(const Setup& target, float seconds, float elapsed, bool active)
    {
        if (!active) { initialized_ = false; return target; }
        if (!initialized_ || seconds <= 0) { value_ = target; initialized_ = true; }
        else {
            // Limit recovery after a long pause; loading invalidates the smoother separately.
            const float dt = FiniteClamp(elapsed, 0, 0.25f, 0);
            const float weight = -std::expm1(-dt / seconds);
            auto next = target;
            for (const auto& field : Fields()) {
                if (field.look) { field.set(next, std::lerp(field.get(value_), field.get(target), weight)); }
            }
            value_ = next;
        }
        return value_;
    }
    void Reset() { initialized_ = false; }
private:
    bool initialized_{};
    Setup value_;
};
}
