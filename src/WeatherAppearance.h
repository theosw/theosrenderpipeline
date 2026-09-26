#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace TheosRenderPipeline::Appearance
{
inline constexpr std::array<const char*, 6> Times{"Night", "Dawn", "Sunrise", "Day", "Sunset", "Dusk"};
inline constexpr std::array<float, 6> DefaultHours{0, 5, 7, 12, 18, 20};
enum class Group : std::size_t { Exterior, Clear, Cloudy, Rain, Snow, Interior, Count };
inline constexpr std::array<const char*, 6> Groups{"Exterior", "Clear", "Cloudy", "Rain", "Snow", "Interior"};
inline constexpr std::size_t MaxWeathers = 64;

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
    Profile profile;
    bool operator==(const WeatherProfile&) const = default;
};
struct Settings
{
    bool enabled{};
    float smoothingSeconds{2};
    std::array<float, 6> hours{DefaultHours};
    std::array<Profile, 6> groups{};
    std::vector<WeatherProfile> weathers;
    bool operator==(const Settings&) const = default;
};
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
    for (auto& profile : settings.groups) { sanitizeProfile(profile); }
    std::vector<WeatherProfile> unique;
    for (auto& entry : settings.weathers) {
        entry.record = Normalize(std::move(entry.record));
        if (!Valid(entry.record) || unique.size() == MaxWeathers ||
            std::ranges::any_of(unique, [&](const auto& old) { return old.record == entry.record; })) { continue; }
        sanitizeProfile(entry.profile);
        unique.push_back(std::move(entry));
    }
    settings.weathers = std::move(unique);
    return settings;
}

struct Weather
{
    Record record;
    Group group{Group::Exterior};
};
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
    Values values;
    std::string name{"Manual defaults"};
};
inline void Overlay(Selection& selected, const Profile& profile, const Settings& settings, float hour,
    const std::string& name)
{
    if (!profile.enabled || (!profile.neural && !profile.sharpening)) { return; }
    const auto values = AtTime(profile, settings.hours, hour);
    if (profile.neural) { selected.values.passes = values.passes; }
    if (profile.sharpening) { selected.values.sharpness = values.sharpness; }
    selected.name = name;
}
inline Selection Select(const Settings& settings, const Weather& weather, bool interior, float hour, Values base)
{
    Selection result{base};
    if (interior) {
        Overlay(result, settings.groups[static_cast<std::size_t>(Group::Interior)], settings, hour, "Interior");
        return result;
    }
    Overlay(result, settings.groups[0], settings, hour, "Exterior");
    const auto group = static_cast<std::size_t>(weather.group);
    if (group > 0 && group < static_cast<std::size_t>(Group::Interior)) {
        Overlay(result, settings.groups[group], settings, hour, Groups[group]);
    }
    for (const auto& entry : settings.weathers) {
        if (entry.record == weather.record) {
            Overlay(result, entry.profile, settings, hour, std::format("{} / {:06X}", entry.record.plugin, entry.record.localID));
            break;
        }
    }
    return result;
}
struct Evaluation
{
    Values values;
    std::string outgoing{"Manual defaults"}, incoming{"Manual defaults"};
    bool active{};
};
inline Evaluation Evaluate(const Settings& settings, const Context& context, Values base)
{
    base = Sanitize(base);
    if (!settings.enabled || !context.valid || !std::isfinite(context.hour)) { return {base}; }
    const auto incoming = Select(settings, context.incoming, context.interior, context.hour, base);
    // Skyrim can have no outgoing weather outside a transition. Do not fade from stale state.
    const auto outgoing = Valid(context.outgoing.record) && !context.interior ?
        Select(settings, context.outgoing, false, context.hour, base) : incoming;
    return {Blend(outgoing.values, incoming.values, context.interior ? 1 : context.transition),
        outgoing.name, incoming.name, true};
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
