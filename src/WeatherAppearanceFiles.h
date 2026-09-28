#pragma once

#include "WeatherAppearanceINI.h"
#include <SimpleIni.h>
#include <filesystem>
#include <memory>

namespace TheosRenderPipeline::Appearance
{
// Preset names are UTF-8; paths must not pass through the ANSI code page.
inline std::filesystem::path Utf8Path(const std::string& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }
inline std::string Utf8Name(const std::filesystem::path& path)
{
    const auto text = path.filename().u8string();
    return std::string(text.begin(), text.end());
}

// The readable preset files in a folder, and the ones that could not be read.
struct PresetFiles
{
    std::vector<std::pair<std::string, std::unique_ptr<CSimpleIniA>>> files;
    std::vector<std::string> unreadable;
    std::vector<std::pair<std::string, const CSimpleIniA*>> Views() const
    {
        std::vector<std::pair<std::string, const CSimpleIniA*>> views;
        for (const auto& [name, file] : files) { views.emplace_back(name, file.get()); }
        return views;
    }
};
inline PresetFiles ReadPresetFolder(const std::filesystem::path& folder)
{
    PresetFiles result;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (!entry.is_regular_file(error) || Lower(Utf8Name(entry.path().extension())) != ".ini") { continue; }
        auto file = std::make_unique<CSimpleIniA>();
        file->SetUnicode();
        if (file->LoadFile(entry.path().c_str()) < 0) { result.unreadable.push_back(Utf8Name(entry.path())); continue; }
        result.files.emplace_back(Utf8Name(entry.path()), std::move(file));
    }
    return result;
}

struct PresetSave
{
    // The settings with each preset's saved file name and any deletions still pending.
    Settings settings;
    bool written{true}, removed{true};
    std::size_t files{}, unchanged{};
    std::vector<std::string> errors;
    bool Ok() const { return written && removed; }
};
// Writes each preset's file, renames files of renamed presets and deletes files of
// deleted ones. A file already holding a preset is left alone, whatever its layout,
// so hand-written files and presets from mods stay untouched. Deletions that fail
// stay pending for the next save.
inline PresetSave SavePresetFiles(Settings settings, const std::filesystem::path& folder)
{
    PresetSave result;
    settings = Sanitize(std::move(settings));
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::vector<std::string> written;
    for (auto& preset : settings.presets) {
        const auto name = PresetFileName(preset.name);
        std::filesystem::path path;
        try {
            path = folder / Utf8Path(name);
        } catch (const std::exception&) {
            result.written = false;
            result.errors.push_back(std::format("preset name is not valid UTF-8; not saved: {}", name));
            continue;
        }
        // A rename that changes only letter case must rename the file itself.
        if (!preset.file.empty() && preset.file != name && FoldCase(preset.file) == FoldCase(name)) {
            try { std::filesystem::rename(folder / Utf8Path(preset.file), path, error); } catch (const std::exception&) {}
        }
        CSimpleIniA existing;
        existing.SetUnicode();
        NamedProfile stored;
        if (preset.file == name && existing.LoadFile(path.c_str()) >= 0 && LoadPresetFile(existing, stored)) {
            stored.profile = Sanitize(std::move(stored.profile));
            if (stored.profile == preset.profile && stored.groups == preset.groups && stored.weathers == preset.weathers) {
                ++result.unchanged;
                ++result.files;
                written.push_back(FoldCase(name));
                continue;
            }
        }
        CSimpleIniA file;
        file.SetUnicode();
        StorePresetFile(file, preset);
        if (file.SaveFile(path.c_str()) < 0) {
            result.written = false;
            result.errors.push_back(std::format("could not write preset file {}", name));
            continue;
        }
        // A renamed preset's old file goes once the new one is written.
        if (!preset.file.empty() && FoldCase(preset.file) != FoldCase(name)) { settings.removedFiles.push_back(preset.file); }
        preset.file = name;
        written.push_back(FoldCase(name));
        ++result.files;
    }
    std::vector<std::string> pending;
    for (const auto& name : settings.removedFiles) {
        if (std::ranges::find(written, FoldCase(name)) != written.end()) { continue; }
        bool removed = false;
        try {
            std::filesystem::remove(folder / Utf8Path(name), error);
            removed = !error;
        } catch (const std::exception&) {}
        if (!removed) {
            pending.push_back(name);
            result.removed = false;
            result.errors.push_back(std::format("could not remove preset file {}; it will be removed at the next save", name));
        }
    }
    settings.removedFiles = std::move(pending);
    result.settings = std::move(settings);
    return result;
}
// Saves the preset files, then the main INI's appearance settings only if every
// file was written, so older-format presets are never removed before their files exist.
inline PresetSave SaveAppearance(CSimpleIniA& ini, const Settings& settings, const std::filesystem::path& folder)
{
    auto result = SavePresetFiles(settings, folder);
    if (result.written) { StoreSettings(ini, result.settings); }
    return result;
}
}
