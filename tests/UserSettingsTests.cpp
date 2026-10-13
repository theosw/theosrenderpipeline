#include "UserSettings.h"
#include "PackagedDefaults.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <process.h>
#include <fstream>
#include <sstream>

using namespace TheosRenderPipeline;

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
static void Write(const std::filesystem::path& path, const char* text) { std::ofstream(path, std::ios::binary) << text; }
static std::string Read(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream text; text << in.rdbuf(); return text.str();
}

// Shaped like the packaged INI: comments, spacing variants, floats, hex.
static constexpr const char* Defaults =
    "; packaged\n[Settings]\nUpscaleType=0\nQualityLevel=2\nSharpness=0.3\nSharpening=true\n"
    "[Hotkeys]\nToggleOverlay=0x23\n[FrameGeneration]\nProvider=0\nEnabled=true\n[Appearance]\nWeatherCount=0\n";

int main(int argc, char** argv)
{
    Require(argc == 2, "packaged INI path");
    const auto root = std::filesystem::temp_directory_path() / std::format(L"TRPUserSettingsTests-{}", _getpid());
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    const UserSettings::Paths paths{root / L"TheosRenderPipeline.ini", root / L"TheosRenderPipeline.User.ini"};
    Write(paths.defaults, Defaults);
    auto load = [&](CSimpleIniA& ini) { ini.SetUnicode(); return UserSettings::Load(ini, paths); };

    // Value comparison as TRP reads values.
    Require(UserSettings::ValuesEqual("0.3", "0.300000") && UserSettings::ValuesEqual(" 2 ", "2"), "numbers compare by value");
    Require(UserSettings::ValuesEqual("0x23", "35") && UserSettings::ValuesEqual("true", "1") &&
        UserSettings::ValuesEqual("False", "no"), "hex and booleans compare by meaning");
    Require(!UserSettings::ValuesEqual("2", "true") && !UserSettings::ValuesEqual("a|b", "b|a") &&
        !UserSettings::ValuesEqual("0.3", "0.31"), "different values differ");

    {   // No user file: exactly the packaged defaults.
        CSimpleIniA ini; SI_Error user = SI_FAIL;
        ini.SetUnicode();
        Require(UserSettings::Load(ini, paths, &user) >= 0 && user == SI_OK, "absent user file loads");
        Require(ini.GetLongValue("Settings", "QualityLevel", -1) == 2 && ini.GetLongValue("Hotkeys", "ToggleOverlay", 0, nullptr) == 35,
            "absent user file behaves like the defaults");
        Require(!UserSettings::Exists(paths.user), "loading never creates the user file");
    }
    {   // Save writes only differences, and they survive replacing the packaged INI.
        CSimpleIniA ini; load(ini);
        ini.SetLongValue("Settings", "QualityLevel", 4);
        ini.SetLongValue("FrameGeneration", "Provider", 1);
        ini.SetDoubleValue("Settings", "Sharpness", 0.3);  // Rewritten with another format, same value.
        ini.SetBoolValue("FrameGeneration", "Enabled", true);
        ini.SetValue("Research", "Retired", "kept");         // Unknown keys are preserved.
        ini.Delete("Appearance", "WeatherCount");             // Deletion falls back to the default.
        std::vector<std::string> changed;
        Require(UserSettings::Save(ini, paths, nullptr, &changed), "save succeeds");
        Require(changed == std::vector<std::string>{"Settings/QualityLevel", "FrameGeneration/Provider", "Research/Retired"},
            "only differing keys are written");
        const auto text = Read(paths.user);
        Require(text.starts_with("; Theo's Render Pipeline user settings") && text.find("Sharpness") == std::string::npos,
            "user file has a header and no unchanged values");
        Write(paths.defaults, "[Settings]\nUpscaleType=0\nQualityLevel=2\nSharpness=0.5\n[FrameGeneration]\nProvider=0\n");
        CSimpleIniA reloaded; load(reloaded);
        Require(reloaded.GetLongValue("Settings", "QualityLevel", 0) == 4 && reloaded.GetLongValue("FrameGeneration", "Provider", 0) == 1,
            "saved values survive a replaced packaged INI");
        Require(reloaded.GetDoubleValue("Settings", "Sharpness", 0) == 0.5, "unsaved values follow the new packaged default");
        Require(std::string(reloaded.GetValue("Research", "Retired", "")) == "kept", "unknown keys survive");
    }
    {   // Removing a key from the user file falls back; resetting to the default removes it.
        Write(paths.user, "[Settings]\nUpscaleType=4\n");
        CSimpleIniA ini; load(ini);
        Require(ini.GetLongValue("Settings", "UpscaleType", 0) == 4 && ini.GetLongValue("Settings", "QualityLevel", 0) == 2,
            "user values override, others fall back");
        ini.SetLongValue("Settings", "UpscaleType", 0);
        Require(UserSettings::Save(ini, paths) && Read(paths.user).find("UpscaleType") == std::string::npos,
            "setting a value back to the default removes it from the user file");
    }
    {   // Migration: once, only differences from this build's defaults, without touching the packaged-path INI.
        Write(paths.defaults, Defaults);
        std::filesystem::remove(paths.user);
        std::vector<std::string> imported;
        Require(UserSettings::Migrate(Defaults, paths, &imported) == UserSettings::Migration::NotNeeded && !UserSettings::Exists(paths.user),
            "an unmodified packaged INI needs no migration and creates no file");
        const char* customized = "; packaged\n[Settings]\nUpscaleType=4\nQualityLevel=2\nSharpness=0.300000\nSharpening=true\n"
            "[Hotkeys]\nToggleOverlay=0x77\n[FrameGeneration]\nProvider=0\nEnabled=true\n[Appearance]\nWeatherCount=0\nFormat=4\n";
        Write(paths.defaults, customized);
        Require(UserSettings::Migrate(Defaults, paths, &imported) == UserSettings::Migration::Imported, "a customized INI migrates");
        Require(imported == std::vector<std::string>{"Settings/UpscaleType", "Hotkeys/ToggleOverlay", "Appearance/Format"},
            "migration imports exactly the differences");
        Require(Read(paths.defaults) == customized, "migration leaves the packaged-path INI unchanged");
        CSimpleIniA beforeUpdate; load(beforeUpdate);
        beforeUpdate.SetLongValue("Settings", "QualityLevel", 1);
        Require(UserSettings::Save(beforeUpdate, paths, Defaults), "save while the legacy customized INI remains");
        Write(paths.defaults, Defaults);  // An update replaces the packaged INI.
        CSimpleIniA ini; load(ini);
        Require(ini.GetLongValue("Settings", "UpscaleType", 0) == 4 && ini.GetLongValue("Hotkeys", "ToggleOverlay", 0) == 0x77,
            "migrated values survive saving before the update");
        Require(ini.GetLongValue("Settings", "QualityLevel", -1) == 1, "newly saved values also survive the update");
        Write(paths.defaults, customized);
        imported.clear();
        Require(UserSettings::Migrate(Defaults, paths, &imported) == UserSettings::Migration::NotNeeded && imported.empty(),
            "migration runs once: an existing user file is never replaced");
    }
    {   // An unreadable user file is reported and leaves the defaults.
        std::filesystem::remove(paths.user);
        std::filesystem::create_directory(paths.user);  // A directory cannot be opened as a file.
        CSimpleIniA ini; SI_Error user = SI_OK;
        ini.SetUnicode();
        Require(UserSettings::Load(ini, paths, &user) >= 0 && user < 0, "unreadable user file is reported");
        Require(ini.GetLongValue("Settings", "UpscaleType", -1) == 4, "unreadable user file leaves the packaged values");
        std::filesystem::remove(paths.user);
    }
    {   // Save falls back to built-in defaults if the packaged INI is unreadable.
        std::filesystem::remove(paths.defaults);
        CSimpleIniA ini; ini.SetUnicode(); ini.LoadData(Defaults);
        ini.SetLongValue("Settings", "QualityLevel", 1);
        std::vector<std::string> changed;
        Require(UserSettings::Save(ini, paths, Defaults, &changed) && changed == std::vector<std::string>{"Settings/QualityLevel"},
            "save diffs against built-in defaults when the packaged INI is missing");
    }
    {   // The embedded defaults are this package's INI: a fresh install never migrates.
        const std::filesystem::path packaged = argv[1];
        Require(Read(packaged) == UserSettings::PackagedDefaults, "embedded defaults match the packaged INI");
        const UserSettings::Paths fresh{packaged, root / L"Fresh.User.ini"};
        Require(UserSettings::Migrate(UserSettings::PackagedDefaults, fresh) == UserSettings::Migration::NotNeeded &&
            !UserSettings::Exists(fresh.user), "the shipped INI needs no migration");
    }
    {   // A failed write keeps the previous file intact and leaves nothing behind.
        const auto target = root / L"Durable.User.ini";
        const auto temporary = root / L"Durable.User.ini.saving";
        const std::string previous = "[Settings]\nQualityLevel=4\nUpscaleType=4\n";
        Write(target, previous.c_str());
        auto shortWrite = [](const void* data, std::size_t size, std::size_t count, FILE* file) {
            return std::fwrite(data, size, count / 2, file);
        };
        Require(!UserSettings::WriteTextWith(target, "[Settings]\nQualityLevel=1\nUpscaleType=0\n", shortWrite),
            "a short write reports failure");
        Require(Read(target) == previous && !std::filesystem::exists(temporary),
            "a short write keeps the previous settings byte for byte and removes its temporary file");
        const auto fresh = root / L"Unwritten.User.ini";
        Require(!UserSettings::WriteTextWith(fresh, "[Settings]\nQualityLevel=1\n", shortWrite) && !UserSettings::Exists(fresh),
            "a failed first write creates no file, so a failed migration can retry");
        const std::string next = "[Settings]\nQualityLevel=1\n";
        Require(UserSettings::WriteText(target, next) && Read(target) == next && !std::filesystem::exists(temporary),
            "a complete write replaces the file");
    }
    std::filesystem::remove_all(root, error);
    std::puts("PASS user settings merge order, fallback, save differences, one-time migration and failed-write durability");
}
