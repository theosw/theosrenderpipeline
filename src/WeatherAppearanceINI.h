#pragma once

#include "WeatherAppearance.h"
#include <charconv>
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
template<class Ini> Profile LoadProfile(const Ini& ini, const std::string& section)
{
    Profile profile;
    profile.enabled = ini.GetBoolValue(section.c_str(), "Enabled", false);
    profile.neural = ini.GetBoolValue(section.c_str(), "NR", true);
    profile.sharpening = ini.GetBoolValue(section.c_str(), "Sharpening", true);
    for (std::size_t i = 0; i < Times.size(); ++i) {
        auto& values = profile.points[i];
        const auto timeSection = section + "." + Times[i];
        values.sharpness = static_cast<float>(ini.GetDoubleValue(timeSection.c_str(), "Sharpness", 0.3));
        for (std::size_t pass = 0; pass < 2; ++pass) {
            const auto prefix = std::format("Pass{}", pass + 1);
            values.passes[pass] = {
                static_cast<float>(ini.GetDoubleValue(timeSection.c_str(), (prefix + "Intensity").c_str(), 1)),
                static_cast<float>(ini.GetDoubleValue(timeSection.c_str(), (prefix + "LocalTone").c_str(), 1)),
                static_cast<float>(ini.GetDoubleValue(timeSection.c_str(), (prefix + "LocalStructure").c_str(), 1))};
        }
    }
    return profile;
}
template<class Ini> Settings LoadSettings(const Ini& ini)
{
    Settings settings;
    settings.enabled = ini.GetBoolValue("Appearance", "Enabled", false);
    settings.smoothingSeconds = static_cast<float>(ini.GetDoubleValue("Appearance", "SmoothingSeconds", 2));
    for (std::size_t i = 0; i < Times.size(); ++i) {
        settings.hours[i] = static_cast<float>(ini.GetDoubleValue("Appearance", (std::string(Times[i]) + "Hour").c_str(), DefaultHours[i]));
    }
    const bool shared = ini.GetLongValue("Appearance", "Format", 1) >= 2;
    if (shared) {
        const auto count = std::clamp(ini.GetLongValue("Appearance", "PresetCount", 0), 0L, static_cast<long>(MaxPresets));
        for (long i = 0; i < count; ++i) {
            const auto section = std::format("Appearance.Preset{}", i);
            const auto id = ini.GetLongValue(section.c_str(), "ID", 0);
            if (id > 0) {
                settings.presets.push_back({static_cast<std::uint32_t>(id), ini.GetValue(section.c_str(), "Name", ""), LoadProfile(ini, section)});
            }
        }
    }
    for (std::size_t i = 0; i < Groups.size(); ++i) {
        const auto section = std::string("Appearance.") + Groups[i];
        if (shared) {
            settings.groups[i] = static_cast<std::uint32_t>((std::max)(0L, ini.GetLongValue(section.c_str(), "Preset", 0)));
        } else {
            const auto profile = LoadProfile(ini, section);
            if (profile != Profile{}) { settings.groups[i] = AddPreset(settings, Groups[i], profile); }
        }
    }
    const auto count = std::clamp(ini.GetLongValue("Appearance", "WeatherCount", 0), 0L, static_cast<long>(MaxWeathers));
    for (long i = 0; i < count; ++i) {
        const auto section = std::format("Appearance.Weather{}", i);
        Record record{ini.GetValue(section.c_str(), "Plugin", "")};
        std::string id = ini.GetValue(section.c_str(), "FormID", "");
        if (id.starts_with("0x") || id.starts_with("0X")) { id.erase(0, 2); }
        const auto [end, ec] = std::from_chars(id.data(), id.data() + id.size(), record.localID, 16);
        if (ec == std::errc{} && end == id.data() + id.size()) {
            const auto preset = shared ? static_cast<std::uint32_t>((std::max)(0L, ini.GetLongValue(section.c_str(), "Preset", 0))) :
                AddPreset(settings, std::format("{} / {:06X}", record.plugin, record.localID), LoadProfile(ini, section));
            settings.weathers.push_back({std::move(record), preset});
        }
    }
    return Sanitize(std::move(settings));
}
template<class Ini> void StoreProfile(Ini& ini, const std::string& section, const Profile& profile)
{
    ini.SetBoolValue(section.c_str(), "Enabled", profile.enabled);
    ini.SetBoolValue(section.c_str(), "NR", profile.neural);
    ini.SetBoolValue(section.c_str(), "Sharpening", profile.sharpening);
    for (std::size_t i = 0; i < Times.size(); ++i) {
        const auto& values = profile.points[i];
        const auto timeSection = section + "." + Times[i];
        StoreFloat(ini, timeSection.c_str(), "Sharpness", values.sharpness);
        for (std::size_t pass = 0; pass < 2; ++pass) {
            const auto prefix = std::format("Pass{}", pass + 1);
            StoreFloat(ini, timeSection.c_str(), (prefix + "Intensity").c_str(), values.passes[pass].intensity);
            StoreFloat(ini, timeSection.c_str(), (prefix + "LocalTone").c_str(), values.passes[pass].tone);
            StoreFloat(ini, timeSection.c_str(), (prefix + "LocalStructure").c_str(), values.passes[pass].structure);
        }
    }
}
template<class Ini> void StoreSettings(Ini& ini, Settings settings)
{
    settings = Sanitize(std::move(settings));
    const auto oldCount = std::clamp(ini.GetLongValue("Appearance", "WeatherCount", 0), 0L, static_cast<long>(MaxWeathers));
    const auto oldPresets = std::clamp(ini.GetLongValue("Appearance", "PresetCount", 0), 0L, static_cast<long>(MaxPresets));
    auto removeProfile = [&](const std::string& section) {
        ini.Delete(section.c_str(), nullptr);
        for (const auto* time : Times) { ini.Delete((section + "." + time).c_str(), nullptr); }
    };
    // Replace only this feature's indexed records, including retired legacy time sections.
    for (long i = 0; i < oldCount; ++i) { removeProfile(std::format("Appearance.Weather{}", i)); }
    for (long i = 0; i < oldPresets; ++i) { removeProfile(std::format("Appearance.Preset{}", i)); }
    ini.SetLongValue("Appearance", "Format", 2);
    ini.SetBoolValue("Appearance", "Enabled", settings.enabled);
    StoreFloat(ini, "Appearance", "SmoothingSeconds", settings.smoothingSeconds);
    for (std::size_t i = 0; i < Times.size(); ++i) {
        StoreFloat(ini, "Appearance", (std::string(Times[i]) + "Hour").c_str(), settings.hours[i]);
    }
    for (std::size_t i = 0; i < Groups.size(); ++i) {
        const auto section = std::string("Appearance.") + Groups[i];
        removeProfile(section);
        ini.SetLongValue(section.c_str(), "Preset", settings.groups[i]);
    }
    ini.SetLongValue("Appearance", "PresetCount", static_cast<long>(settings.presets.size()));
    for (std::size_t i = 0; i < settings.presets.size(); ++i) {
        const auto section = std::format("Appearance.Preset{}", i);
        const auto& preset = settings.presets[i];
        ini.SetLongValue(section.c_str(), "ID", preset.id);
        ini.SetValue(section.c_str(), "Name", preset.name.c_str());
        StoreProfile(ini, section, preset.profile);
    }
    ini.SetLongValue("Appearance", "WeatherCount", static_cast<long>(settings.weathers.size()));
    for (std::size_t i = 0; i < settings.weathers.size(); ++i) {
        const auto section = std::format("Appearance.Weather{}", i);
        const auto& entry = settings.weathers[i];
        ini.SetValue(section.c_str(), "Plugin", entry.record.plugin.c_str());
        ini.SetValue(section.c_str(), "FormID", std::format("{:06X}", entry.record.localID).c_str());
        ini.SetLongValue(section.c_str(), "Preset", entry.preset);
    }
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
