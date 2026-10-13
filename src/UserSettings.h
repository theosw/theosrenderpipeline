#pragma once

#include <SimpleIni.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <io.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Packaged defaults and user settings are separate files.
//
// TheosRenderPipeline.ini ships in both ZIPs and holds defaults only; TRP no
// longer writes it. Save as default writes the keys that differ from it to
// TheosRenderPipeline.User.ini, which no package ships: MO2 places the new file
// in Overwrite and manual installs keep it, so updates replace the defaults
// without touching user choices. Every reader loads the defaults and then
// overlays the user file; a key missing from the user file falls back to the
// packaged default, and an absent user file behaves exactly like the defaults.
namespace TheosRenderPipeline::UserSettings
{
struct Paths
{
    std::filesystem::path defaults, user;
};
inline Paths GamePaths()
{
    // Relative to Skyrim's working directory, inside MO2's virtual Data tree.
    return {L"Data\\SKSE\\Plugins\\TheosRenderPipeline.ini", L"Data\\SKSE\\Plugins\\TheosRenderPipeline.User.ini"};
}

// A missing file loads as empty; an unreadable one is an error.
inline SI_Error LoadOptional(CSimpleIniA& ini, const std::filesystem::path& path)
{
    FILE* file = nullptr;
    const auto error = _wfopen_s(&file, path.c_str(), L"rb");
    if (error != 0) { return error == ENOENT ? SI_OK : SI_FILE; }
    const std::unique_ptr<FILE, decltype(&std::fclose)> input(file, &std::fclose);
    return ini.LoadFile(input.get());
}
inline bool Exists(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

// Later values win for every key present in source.
inline void Overlay(CSimpleIniA& target, const CSimpleIniA& source)
{
    CSimpleIniA::TNamesDepend sections;
    source.GetAllSections(sections);
    for (const auto& section : sections) {
        CSimpleIniA::TNamesDepend keys;
        source.GetAllKeys(section.pItem, keys);
        for (const auto& key : keys) {
            target.SetValue(section.pItem, key.pItem, source.GetValue(section.pItem, key.pItem, ""));
        }
    }
}

namespace detail
{
inline std::string Trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) { return {}; }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}
inline bool Number(const std::string& text, double& value)
{
    if (text.empty()) { return false; }
    char* end = nullptr;
    const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    value = hex ? static_cast<double>(std::strtoll(text.c_str(), &end, 16)) : std::strtod(text.c_str(), &end);
    return end && *end == '\0' && std::isfinite(value);
}
inline int Boolean(const std::string& text)
{
    for (const auto* word : {"true", "yes", "on"}) { if (_stricmp(text.c_str(), word) == 0) { return 1; } }
    for (const auto* word : {"false", "no", "off"}) { if (_stricmp(text.c_str(), word) == 0) { return 0; } }
    return -1;
}
} // namespace detail

// Equal as TRP reads them: 0.3 == 0.300000, 0x23 == 35, true == 1.
inline bool ValuesEqual(const char* left, const char* right)
{
    const auto a = detail::Trim(left ? left : ""), b = detail::Trim(right ? right : "");
    if (_stricmp(a.c_str(), b.c_str()) == 0) { return true; }
    double x{}, y{};
    if (detail::Number(a, x) && detail::Number(b, y)) {
        return std::fabs(x - y) <= 1e-6 * (std::max)(1.0, std::fabs(x));
    }
    const int p = detail::Boolean(a) >= 0 ? detail::Boolean(a) : detail::Number(a, x) && (x == 0 || x == 1) ? int(x) : -1;
    const int q = detail::Boolean(b) >= 0 ? detail::Boolean(b) : detail::Number(b, y) && (y == 0 || y == 1) ? int(y) : -1;
    return p >= 0 && p == q;
}

// Keys of full that are absent from or differ from defaults. A key deleted from
// full but present in defaults cannot be expressed and falls back to the default.
inline void Difference(const CSimpleIniA& defaults, const CSimpleIniA& full, CSimpleIniA& out,
    std::vector<std::string>* changed = nullptr)
{
    CSimpleIniA::TNamesDepend sections;
    full.GetAllSections(sections);
    sections.sort(CSimpleIniA::Entry::LoadOrder());
    for (const auto& section : sections) {
        CSimpleIniA::TNamesDepend keys;
        full.GetAllKeys(section.pItem, keys);
        keys.sort(CSimpleIniA::Entry::LoadOrder());
        for (const auto& key : keys) {
            const auto* value = full.GetValue(section.pItem, key.pItem, "");
            const auto* base = defaults.GetValue(section.pItem, key.pItem, nullptr);
            if (base && ValuesEqual(base, value)) { continue; }
            out.SetValue(section.pItem, key.pItem, value);
            if (changed) { changed->push_back(std::string(section.pItem) + "/" + key.pItem); }
        }
    }
}

// Packaged defaults, then the user file. Returns the defaults' load result;
// an unreadable user file is reported separately and leaves the defaults.
inline SI_Error Load(CSimpleIniA& ini, const Paths& paths = GamePaths(), SI_Error* userResult = nullptr)
{
    const auto result = ini.LoadFile(paths.defaults.c_str());
    CSimpleIniA user;
    user.SetUnicode();
    const auto loaded = LoadOptional(user, paths.user);
    if (userResult) { *userResult = loaded; }
    if (loaded >= 0) { Overlay(ini, user); }
    return result;
}

// Writes a same-directory temporary file, then replaces the target with it, so a
// failed or interrupted save never leaves a truncated settings file and a failed
// first-time migration leaves no file to block the next attempt. `write` is
// fwrite-shaped; tests substitute a failing writer.
template <class Write>
bool WriteTextWith(const std::filesystem::path& path, const std::string& text, Write&& write)
{
    auto temporary = path;
    temporary += L".saving";
    std::error_code error;
    FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || !file) { return false; }
    bool written = write(text.data(), 1, text.size(), file) == text.size() && std::fflush(file) == 0 &&
        _commit(_fileno(file)) == 0;
    written = std::fclose(file) == 0 && written;
    if (written) { std::filesystem::rename(temporary, path, error); }
    if (!written || error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

inline bool WriteText(const std::filesystem::path& path, const std::string& text)
{
    return WriteTextWith(path, text, [](const void* data, std::size_t size, std::size_t count, FILE* file) {
        return std::fwrite(data, size, count, file);
    });
}

inline constexpr std::string_view Header =
    "; Theo's Render Pipeline user settings, written by Save as default.\n"
    "; Holds only values that differ from TheosRenderPipeline.ini, which updates replace.\n"
    "; Delete a line to return that setting to its packaged default; delete the file to\n"
    "; return to all defaults. No TRP package contains this file.\n\n";

// Writes differences from this build's packaged defaults when supplied. The
// installed INI may still contain legacy customizations after migration; using
// it as the comparison would erase those customizations on the next save.
// Without embedded defaults, compare against the installed INI.
inline bool Save(const CSimpleIniA& full, const Paths& paths = GamePaths(), const char* packagedDefaults = nullptr,
    std::vector<std::string>* changed = nullptr)
{
    CSimpleIniA defaults, out;
    defaults.SetUnicode();
    out.SetUnicode();
    if (packagedDefaults) {
        if (defaults.LoadData(packagedDefaults) < 0) { return false; }
    } else if (defaults.LoadFile(paths.defaults.c_str()) < 0) {
        defaults.Reset();
    }
    Difference(defaults, full, out, changed);
    std::string text;
    if (out.Save(text) < 0) { return false; }
    return WriteText(paths.user, std::string(Header) + text);
}

enum class Migration { NotNeeded, Imported, Failed };
// One-time import for installs that predate the user file: if none exists yet and
// the INI at the packaged path differs from this build's packaged defaults, those
// differences are exactly what the user (or an older winning INI) set. Write them
// to the user file. Nothing is guessed, and the packaged-path INI is not modified.
inline Migration Migrate(const char* packagedDefaults, const Paths& paths = GamePaths(),
    std::vector<std::string>* imported = nullptr)
{
    if (!packagedDefaults || Exists(paths.user)) { return Migration::NotNeeded; }
    CSimpleIniA installed, builtIn, out;
    installed.SetUnicode();
    builtIn.SetUnicode();
    out.SetUnicode();
    if (installed.LoadFile(paths.defaults.c_str()) < 0 || builtIn.LoadData(packagedDefaults) < 0) { return Migration::NotNeeded; }
    std::vector<std::string> keys;
    Difference(builtIn, installed, out, &keys);
    if (keys.empty()) { return Migration::NotNeeded; }
    std::string text;
    if (out.Save(text) < 0 ||
        !WriteText(paths.user, std::string(Header) +
            "; Imported once from a customized TheosRenderPipeline.ini.\n\n" + text)) { return Migration::Failed; }
    if (imported) { *imported = std::move(keys); }
    return Migration::Imported;
}
} // namespace TheosRenderPipeline::UserSettings
