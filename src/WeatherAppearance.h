#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
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

struct Neural
{
    float intensity{1}, tone{1}, structure{1};
    bool operator==(const Neural&) const = default;
};
struct Values
{
    std::array<Neural, 2> passes{};
    float sharpness{0.3f};
    bool operator==(const Values&) const = default;
};
inline float FiniteClamp(float value, float low, float high, float fallback)
{
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
inline Values Sanitize(Values value)
{
    for (auto& pass : value.passes) {
        pass.intensity = FiniteClamp(pass.intensity, 0, 2, 1);
        pass.tone = FiniteClamp(pass.tone, 0, 2, 1);
        pass.structure = FiniteClamp(pass.structure, 0, 2, 1);
    }
    value.sharpness = FiniteClamp(value.sharpness, 0, 1, 0.3f);
    return value;
}
inline Values Blend(const Values& a, const Values& b, float weight)
{
    Values result;
    const float t = FiniteClamp(weight, 0, 1, 1);
    for (std::size_t i = 0; i < result.passes.size(); ++i) {
        result.passes[i] = {std::lerp(a.passes[i].intensity, b.passes[i].intensity, t),
            std::lerp(a.passes[i].tone, b.passes[i].tone, t),
            std::lerp(a.passes[i].structure, b.passes[i].structure, t)};
    }
    result.sharpness = std::lerp(a.sharpness, b.sharpness, t);
    return result;
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
struct Profile
{
    bool enabled{}, neural{true}, sharpening{true};
    std::array<Values, 6> points{};
    bool operator==(const Profile&) const = default;
};
inline Profile FromValues(Values values)
{
    Profile result;
    result.enabled = true;
    result.points.fill(Sanitize(values));
    return result;
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
inline std::uint32_t AddPreset(Settings& settings, std::string name, Profile profile)
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
inline bool AddStarterProfiles(Settings& settings, Values values)
{
    const auto missing = std::ranges::count(settings.groups, 0u);
    if (settings.presets.size() + missing > MaxPresets) { return false; }
    for (std::size_t i = 0; i < Groups.size(); ++i) {
        if (!settings.groups[i]) {
            settings.groups[i] = AddPreset(settings, Groups[i], FromValues(values));
        }
    }
    return true;
}
inline bool UsesPresets(const Settings& settings)
{
    return std::ranges::any_of(settings.presets, [](const auto& preset) { return preset.profile.enabled; });
}
inline bool ValidHours(const std::array<float, 6>& hours)
{
    for (std::size_t i = 0; i < hours.size(); ++i) {
        if (!std::isfinite(hours[i]) || hours[i] < 0 || hours[i] >= 24 ||
            (i && hours[i] - hours[i - 1] < 0.01f)) { return false; }
    }
    return true;
}
inline Settings Sanitize(Settings settings)
{
    settings.smoothingSeconds = FiniteClamp(settings.smoothingSeconds, 0, 30, 2);
    if (!ValidHours(settings.hours)) { settings.hours = DefaultHours; }
    auto sanitizeProfile = [](Profile& profile) {
        for (auto& point : profile.points) { point = Sanitize(point); }
    };
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
        sanitizeProfile(preset.profile);
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
inline Values AtTime(const Profile& profile, const std::array<float, 6>& hours, float hour)
{
    const float time = Hour(hour);
    for (std::size_t i = 0; i < hours.size(); ++i) {
        const auto next = (i + 1) % hours.size();
        const float end = next ? hours[next] : hours[0] + 24;
        const float sample = time < hours[0] ? time + 24 : time;
        if (sample >= hours[i] && sample < end) {
            return Blend(profile.points[i], profile.points[next], (sample - hours[i]) / (end - hours[i]));
        }
    }
    return profile.points[0];
}
struct Selection
{
    const Profile* neural{};
    const Profile* sharpening{};
    std::string neuralName{"Manual defaults"}, sharpeningName{"Manual defaults"};
};
inline void Overlay(Selection& selected, const NamedProfile* preset)
{
    if (!preset) { return; }
    const auto& profile = preset->profile;
    if (!profile.enabled || (!profile.neural && !profile.sharpening)) { return; }
    if (profile.neural) { selected.neural = &profile; selected.neuralName = preset->name; }
    if (profile.sharpening) { selected.sharpening = &profile; selected.sharpeningName = preset->name; }
}
// Resolve only when weather, location or configuration changes. These pointers
// refer to the controller's owned configuration and never escape in UI snapshots.
inline Selection Select(const Settings& settings, const Weather& weather, bool interior)
{
    Selection result;
    if (interior) {
        Overlay(result, FindPreset(settings, settings.groups[static_cast<std::size_t>(Group::Interior)]));
        return result;
    }
    Overlay(result, FindPreset(settings, settings.groups[0]));
    const auto group = static_cast<std::size_t>(weather.group);
    if (group > 0 && group < static_cast<std::size_t>(Group::Interior)) {
        Overlay(result, FindPreset(settings, settings.groups[group]));
    }
    for (const auto& entry : settings.weathers) {
        if (entry.record == weather.record) {
            Overlay(result, FindPreset(settings, entry.preset));
            break;
        }
    }
    return result;
}
inline Values Sample(const Selection& selection, const Settings& settings, float hour, Values base)
{
    if (selection.neural) { base.passes = AtTime(*selection.neural, settings.hours, hour).passes; }
    if (selection.sharpening) { base.sharpness = AtTime(*selection.sharpening, settings.hours, hour).sharpness; }
    return base;
}
struct Evaluation
{
    Values values;
    std::string outgoing{"Manual defaults"}, incoming{"Manual defaults"};
    std::string outgoingSharpening{"Manual defaults"}, incomingSharpening{"Manual defaults"};
    bool active{};
};
inline Evaluation Evaluate(const Settings& settings, const Context& context, Values base)
{
    base = Sanitize(base);
    if (!settings.enabled || !context.valid || !std::isfinite(context.hour)) { return {base}; }
    const auto incoming = Select(settings, context.incoming, context.interior);
    // Skyrim can have no outgoing weather outside a transition. Do not fade from stale state.
    const auto outgoing = Valid(context.outgoing.record) && !context.interior ?
        Select(settings, context.outgoing, false) : incoming;
    return {Blend(Sample(outgoing, settings, context.hour, base), Sample(incoming, settings, context.hour, base),
        context.interior ? 1 : context.transition), outgoing.neuralName, incoming.neuralName,
        outgoing.sharpeningName, incoming.sharpeningName, true};
}

class Smoother
{
public:
    Values Update(Values target, float seconds, float elapsed, bool active)
    {
        if (!active) { initialized_ = false; return target; }
        if (!initialized_ || seconds <= 0) { value_ = target; initialized_ = true; }
        else {
            // Limit recovery after a long pause; loading invalidates the smoother separately.
            const float dt = FiniteClamp(elapsed, 0, 0.25f, 0);
            value_ = Blend(value_, target, -std::expm1(-dt / seconds));
        }
        return value_;
    }
    void Reset() { initialized_ = false; }
private:
    bool initialized_{};
    Values value_;
};
}
