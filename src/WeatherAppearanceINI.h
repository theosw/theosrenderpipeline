#pragma once

#include "WeatherAppearance.h"
#include <charconv>
#include <limits>
#include <format>
#include <optional>

namespace TheosRenderPipeline::Appearance
{
template<class Ini> void StoreFloat(Ini& ini, const char* section, const char* key, float value)
{
    // SimpleIni's SetDoubleValue writes six decimal places. Nine significant
    // digits preserve an authored float exactly across save/restart cycles.
    ini.SetValue(section, key, std::format("{:.9g}", value).c_str());
}
template<class Ini> std::optional<float> ReadFloat(const Ini& ini, const char* section, const char* key)
{
    const char* text = ini.GetValue(section, key, nullptr);
    if (!text) { return std::nullopt; }
    // On/off settings may be written as words, like Enabled = true.
    const auto word = Lower(text);
    if (word == "true" || word == "on" || word == "yes") { return 1.0f; }
    if (word == "false" || word == "off" || word == "no") { return 0.0f; }
    // Malformed values load as NaN; Sanitize replaces them with the setting's default.
    return static_cast<float>(ini.GetDoubleValue(section, key, std::numeric_limits<double>::quiet_NaN()));
}
template<class Ini> bool HasProfile(const Ini& ini, const std::string& section)
{
    if (ini.GetSection(section.c_str())) { return true; }
    return std::ranges::any_of(Times, [&](const char* time) { return ini.GetSection((section + "." + time).c_str()) != nullptr; });
}
// Formats 1-2 stored NR intensity, tone and structure plus sharpness at every
// time of day, gated by NR/Sharpening flags. They migrate to look changes.
inline constexpr std::array<const char*, 7> LegacyLookKeys{"Pass1Intensity", "Pass1LocalTone", "Pass1LocalStructure",
    "Pass2Intensity", "Pass2LocalTone", "Pass2LocalStructure", "Sharpness"};
template<class Ini> Profile LoadProfile(const Ini& ini, const std::string& section, long format)
{
    Profile profile;
    profile.enabled = ini.GetBoolValue(section.c_str(), "Enabled", format >= 3);
    if (format < 3) {
        const bool neural = ini.GetBoolValue(section.c_str(), "NR", true);
        const bool sharpening = ini.GetBoolValue(section.c_str(), "Sharpening", true);
        for (const auto* key : LegacyLookKeys) {
            const bool sharpness = std::string_view(key) == "Sharpness";
            if (sharpness ? !sharpening : !neural) { continue; }
            Change change{key};
            for (std::size_t i = 0; i < Times.size(); ++i) {
                const auto timeSection = section + "." + Times[i];
                change.points[i] = static_cast<float>(ini.GetDoubleValue(timeSection.c_str(), key, sharpness ? 0.3 : 1));
            }
            profile.changes.push_back(std::move(change));
        }
        return profile;
    }
    for (const auto& field : Fields()) {
        Change change{field.key};
        bool present = false;
        if (field.look) {
            std::optional<float> first;
            for (std::size_t i = 0; i < Times.size(); ++i) {
                const auto value = ReadFloat(ini, (section + "." + Times[i]).c_str(), field.key);
                if (value) { present = true; change.points[i] = *value; if (!first) { first = value; } }
                else { change.points[i] = std::numeric_limits<float>::quiet_NaN(); }
            }
            // A partly missing time keeps the first saved value rather than a default.
            if (first) { for (auto& point : change.points) { if (std::isnan(point)) { point = *first; } } }
        } else if (const auto value = ReadFloat(ini, section.c_str(), field.key)) {
            present = true;
            change.points.fill(*value);
        }
        if (present) { profile.changes.push_back(std::move(change)); }
    }
    return profile;
}
inline constexpr std::array<const char*, 6> GroupLabels{"Outdoors", "Clear", "Cloudy", "Rain", "Snow", "Interior"};
inline std::optional<std::size_t> ParseGroup(std::string_view text)
{
    const auto key = Lower(std::string(text));
    if (key == "exterior") { return 0; }
    for (std::size_t i = 0; i < GroupLabels.size(); ++i) { if (key == Lower(GroupLabels[i])) { return i; } }
    return std::nullopt;
}
inline std::vector<std::string> Split(std::string_view text, char separator)
{
    std::vector<std::string> parts;
    while (!text.empty()) {
        const auto end = text.find(separator);
        auto part = text.substr(0, end);
        while (!part.empty() && part.front() == ' ') { part.remove_prefix(1); }
        while (!part.empty() && part.back() == ' ') { part.remove_suffix(1); }
        if (!part.empty()) { parts.emplace_back(part); }
        if (end == std::string_view::npos) { break; }
        text.remove_prefix(end + 1);
    }
    return parts;
}
// "Plugin.esp|0ABCDE": plugin identity and local FormID, stable across load orders.
inline std::optional<Record> ParseRecord(std::string_view text)
{
    const auto bar = text.rfind('|');
    if (bar == std::string_view::npos) { return std::nullopt; }
    Record record{std::string(text.substr(0, bar))};
    auto id = text.substr(bar + 1);
    if (id.starts_with("0x") || id.starts_with("0X")) { id.remove_prefix(2); }
    const auto [end, ec] = std::from_chars(id.data(), id.data() + id.size(), record.localID, 16);
    if (ec != std::errc{} || end != id.data() + id.size()) { return std::nullopt; }
    record = Normalize(std::move(record));
    return Valid(record) ? std::optional(record) : std::nullopt;
}
// Formats 1-3 kept presets and their assignments in the main INI.
template<class Ini> void LoadLegacyPresets(const Ini& ini, long format, Settings& settings)
{
    const bool shared = format >= 2;
    if (shared) {
        const auto count = std::clamp(ini.GetLongValue("Appearance", "PresetCount", 0), 0L, static_cast<long>(MaxPresets));
        for (long i = 0; i < count; ++i) {
            const auto section = std::format("Appearance.Preset{}", i);
            const auto id = ini.GetLongValue(section.c_str(), "ID", 0);
            if (id > 0 && !FindPreset(settings, static_cast<std::uint32_t>(id))) {
                settings.presets.push_back({static_cast<std::uint32_t>(id), ini.GetValue(section.c_str(), "Name", ""), LoadProfile(ini, section, format)});
            }
        }
    }
    for (std::size_t i = 0; i < Groups.size(); ++i) {
        const auto section = std::string("Appearance.") + Groups[i];
        auto* preset = shared ? FindPreset(settings, static_cast<std::uint32_t>((std::max)(0L, ini.GetLongValue(section.c_str(), "Preset", 0)))) :
            HasProfile(ini, section) ? FindPreset(settings, AddPreset(settings, Groups[i], LoadProfile(ini, section, format))) : nullptr;
        if (preset) { preset->groups[i] = true; }
    }
    const auto count = std::clamp(ini.GetLongValue("Appearance", "WeatherCount", 0), 0L, static_cast<long>(MaxWeathers));
    for (long i = 0; i < count; ++i) {
        const auto section = std::format("Appearance.Weather{}", i);
        const auto record = ParseRecord(std::format("{}|{}", ini.GetValue(section.c_str(), "Plugin", ""), ini.GetValue(section.c_str(), "FormID", "")));
        if (!record) { continue; }
        auto* preset = shared ? FindPreset(settings, static_cast<std::uint32_t>((std::max)(0L, ini.GetLongValue(section.c_str(), "Preset", 0)))) :
            FindPreset(settings, AddPreset(settings, std::format("{} / {:06X}", record->plugin, record->localID), LoadProfile(ini, section, format)));
        if (preset) { preset->weathers.push_back(*record); }
    }
}
// A preset file: its file name is the preset's name. [Preset] holds when it
// applies; [Changes] and [Changes.Night] etc. hold only the settings it changes.
template<class Ini> void StoreChanges(Ini& ini, const std::string& section, const Profile& profile)
{
    for (const auto& change : profile.changes) {
        const auto* field = FindField(change.key);
        if (!field) { continue; }
        if (!field->look) { StoreFloat(ini, section.c_str(), field->key, change.points[0]); continue; }
        for (std::size_t i = 0; i < Times.size(); ++i) {
            StoreFloat(ini, (section + "." + Times[i]).c_str(), field->key, change.points[i]);
        }
    }
}
template<class Ini> bool LoadPresetFile(const Ini& file, NamedProfile& preset)
{
    if (!file.GetSection("Preset")) { return false; }
    preset.profile = LoadProfile(file, "Changes", 3);
    preset.profile.enabled = file.GetBoolValue("Preset", "Enabled", true);
    for (const auto& group : Split(file.GetValue("Preset", "UseWhen", ""), '|')) {
        if (const auto index = ParseGroup(group)) { preset.groups[*index] = true; }
    }
    const auto count = std::clamp(file.GetLongValue("Preset", "WeatherCount", 0), 0L, static_cast<long>(MaxWeathers));
    for (long i = 0; i < count; ++i) {
        if (const auto record = ParseRecord(file.GetValue("Preset", std::format("Weather{}", i).c_str(), ""))) { preset.weathers.push_back(*record); }
    }
    return true;
}
template<class Ini> void StorePresetFile(Ini& file, const NamedProfile& preset)
{
    file.Reset();
    file.SetLongValue("Preset", "Format", 1);
    file.SetBoolValue("Preset", "Enabled", preset.profile.enabled);
    std::string groups;
    for (std::size_t i = 0; i < GroupLabels.size(); ++i) {
        if (preset.groups[i]) { groups += std::format("{}{}", groups.empty() ? "" : "|", GroupLabels[i]); }
    }
    file.SetValue("Preset", "UseWhen", groups.c_str());
    file.SetLongValue("Preset", "WeatherCount", static_cast<long>(preset.weathers.size()));
    for (std::size_t i = 0; i < preset.weathers.size(); ++i) {
        const auto& record = preset.weathers[i];
        file.SetValue("Preset", std::format("Weather{}", i).c_str(), std::format("{}|{:06X}", record.plugin, record.localID).c_str());
    }
    StoreChanges(file, "Changes", preset.profile);
}
// files: each preset file's name and contents, in any order. PresetOrder in the
// main INI gives their priority; files it does not list follow by name.
// legacyBase is Base as saved in the same INI; formats 1-3 stored every look value,
// so their values equal to it are not kept as changes.
template<class Ini> Settings LoadSettings(const Ini& ini, const std::vector<std::pair<std::string, const Ini*>>& files = {},
    const Setup* legacyBase = nullptr)
{
    Settings settings;
    settings.smoothingSeconds = static_cast<float>(ini.GetDoubleValue("Appearance", "SmoothingSeconds", 2));
    for (std::size_t i = 0; i < Times.size(); ++i) {
        settings.hours[i] = static_cast<float>(ini.GetDoubleValue("Appearance", (std::string(Times[i]) + "Hour").c_str(), DefaultHours[i]));
    }
    const long format = ini.GetLongValue("Appearance", "Format", 1);
    if (format <= 3) {
        LoadLegacyPresets(ini, format, settings);
        if (legacyBase) { DropBaseEqual(settings.presets, *legacyBase); }
        // Those formats had a master switch; presets it kept off stay off, per preset.
        if (!ini.GetBoolValue("Appearance", "Enabled", false)) {
            for (auto& preset : settings.presets) { preset.profile.enabled = false; }
        }
        // A save interrupted after writing files leaves both; the file is newer.
        std::erase_if(settings.presets, [&](const auto& preset) {
            return std::ranges::any_of(files, [&](const auto& file) { return FoldCase(file.first) == FoldCase(PresetFileName(preset.name)); });
        });
    }
    auto order = Split(ini.GetValue("Appearance", "PresetOrder", ""), '|');
    for (auto& name : order) { name = FoldCase(name); }
    auto sorted = files;
    const auto rank = [&](const std::string& name) {
        return std::pair(static_cast<std::size_t>(std::ranges::find(order, FoldCase(name)) - order.begin()), FoldCase(name));
    };
    std::ranges::stable_sort(sorted, {}, [&](const auto& file) { return rank(file.first); });
    for (const auto& [name, file] : sorted) {
        NamedProfile preset;
        preset.name = name.substr(0, name.size() - (Lower(name).ends_with(".ini") ? 4 : 0));
        if (!file || !LoadPresetFile(*file, preset)) { continue; }
        const auto id = AddPreset(settings, preset.name, preset.profile);
        if (auto* added = FindPreset(settings, id)) {
            added->groups = preset.groups;
            added->weathers = std::move(preset.weathers);
            added->file = name;
        }
    }
    return Sanitize(std::move(settings));
}
// The main INI keeps the schedule, smoothing and preset order; presets live in files.
template<class Ini> void StoreSettings(Ini& ini, Settings settings)
{
    settings = Sanitize(std::move(settings));
    const auto oldCount = std::clamp(ini.GetLongValue("Appearance", "WeatherCount", 0), 0L, static_cast<long>(MaxWeathers));
    const auto oldPresets = std::clamp(ini.GetLongValue("Appearance", "PresetCount", 0), 0L, static_cast<long>(MaxPresets));
    auto removeProfile = [&](const std::string& section) {
        ini.Delete(section.c_str(), nullptr);
        for (const auto* time : Times) { ini.Delete((section + "." + time).c_str(), nullptr); }
    };
    // Remove formats 1-3's presets and assignments; the preset files replace them.
    for (long i = 0; i < oldCount; ++i) { removeProfile(std::format("Appearance.Weather{}", i)); }
    for (long i = 0; i < oldPresets; ++i) { removeProfile(std::format("Appearance.Preset{}", i)); }
    for (const auto* group : Groups) { removeProfile(std::string("Appearance.") + group); }
    ini.Delete("Appearance", "WeatherCount");
    ini.Delete("Appearance", "PresetCount");
    ini.SetLongValue("Appearance", "Format", 4);
    ini.SetBoolValue("Appearance", "Enabled", settings.enabled);
    StoreFloat(ini, "Appearance", "SmoothingSeconds", settings.smoothingSeconds);
    for (std::size_t i = 0; i < Times.size(); ++i) {
        StoreFloat(ini, "Appearance", (std::string(Times[i]) + "Hour").c_str(), settings.hours[i]);
    }
    std::string order;
    for (const auto& preset : settings.presets) { order += std::format("{}{}", order.empty() ? "" : "|", PresetFileName(preset.name)); }
    ini.SetValue("Appearance", "PresetOrder", order.c_str());
}
// Copy ENB's authored clock markers and dawn/dusk boundaries into TRP anchors.
// This is an explicit editable approximation, not ENB's internal phase weights.
template<class Ini> std::optional<std::array<float, 6>> ReadENBSchedule(const Ini& ini)
{
    auto read = [&](const char* key) { return static_cast<float>(ini.GetDoubleValue("TIMEOFDAY", key, -100)); };
    const float sunrise = read("SunriseTime"), sunset = read("SunsetTime");
    const float dawn = read("DawnDuration"), dusk = read("DuskDuration");
    const std::array<float, 6> hours{read("NightTime"), sunrise - dawn, sunrise, read("DayTime"), sunset, sunset + dusk};
    if (dawn <= 0 || dusk <= 0 || !ValidHours(hours)) { return std::nullopt; }
    return hours;
}
}
