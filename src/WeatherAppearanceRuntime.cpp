#include <PCH.h>
#include "WeatherAppearanceRuntime.h"
#include "WeatherAppearanceINI.h"
#include "FrameGen/SourceDLSSGSettings.h"
#include <fstream>
#include <iterator>
#include "WeatherEditorID.h"

namespace TheosRenderPipeline::Appearance
{
namespace
{
// Preset names are UTF-8; paths must not pass through the ANSI code page.
std::filesystem::path Utf8Path(const std::string& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }
std::string Utf8Name(const std::filesystem::path& path)
{
    const auto text = path.filename().u8string();
    return std::string(text.begin(), text.end());
}
Group Classification(const RE::TESWeather* weather)
{
    if (!weather) { return Group::Exterior; }
    using Flag = RE::TESWeather::WeatherDataFlag;
    const auto flags = weather->data.flags;
    if (flags.any(Flag::kSnow)) { return Group::Snow; }
    if (flags.any(Flag::kRainy)) { return Group::Rain; }
    if (flags.any(Flag::kCloudy)) { return Group::Cloudy; }
    if (flags.any(Flag::kPleasant)) { return Group::Clear; }
    return Group::Exterior;
}
Weather ReadWeather(const RE::TESWeather* weather)
{
    Weather result;
    if (!weather) { return result; }
    if (const auto* file = weather->GetFile(0)) {
        result.record = RuntimeRecord(std::string(file->GetFilename()), weather->GetFormID(), file->IsLight());
    }
    result.group = Classification(weather);
    return result;
}
}
void Runtime::CaptureCatalogue()
{
    // Main-thread DataLoaded or an explicit SKSE task; no form pointers escape.
    auto* data = RE::TESDataHandler::GetSingleton();
    if (!data) { return; }
    auto entries = std::make_shared<std::vector<WeatherEntry>>();
    const auto nativeModule = GetModuleHandleW(L"NativeEditorIDFix.dll");
    const auto tweaksModule = GetModuleHandleW(L"po3_Tweaks.dll");
    const auto nativeLookup = nativeModule ? reinterpret_cast<NativeEditorLookup>(GetProcAddress(nativeModule, "NEIF_GetEditorID")) : nullptr;
    const auto tweaksLookup = tweaksModule ? reinterpret_cast<TweaksEditorLookup>(GetProcAddress(tweaksModule, "GetFormEditorID")) : nullptr;
    for (const auto* weather : data->GetFormArray<RE::TESWeather>()) {
        if (!weather) { continue; }
        auto value = ReadWeather(weather);
        if (!Valid(value.record)) { continue; }
        const auto name = EditorName(weather->GetFormEditorID(),
            {weather->GetFormID(), static_cast<std::uint8_t>(weather->GetFormType())}, nativeLookup, tweaksLookup);
        entries->push_back(CatalogueEntry(weather->GetFormID(), std::move(value), name));
    }
    std::ranges::sort(*entries, {}, &WeatherEntry::search);
    std::scoped_lock lock(mutex_);
    catalogueIndex_.clear();
    for (std::size_t i = 0; i < entries->size(); ++i) { catalogueIndex_.emplace((*entries)[i].runtimeID, i); }
    catalogue_ = std::move(entries);
    incomingID_ = outgoingID_ = ~0u;
    logger::info("[Appearance] weather catalogue contains {} loaded records", catalogue_->size());
}
void Runtime::ReadContext()
{
    context_.valid = false;
    auto* ui = RE::UI::GetSingleton();
    if (!ui || ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) { return; }
    const auto* player = RE::PlayerCharacter::GetSingleton();
    const auto* sky = RE::Sky::GetSingleton();
    const auto* cell = player ? player->GetParentCell() : nullptr;
    if (!sky || !cell || !std::isfinite(sky->currentGameHour)) { return; }
    context_.interior = cell->IsInteriorCell() && !cell->UsesSkyLighting();
    // Sky-lit interior cells intentionally follow exterior weather/time settings.
    context_.valid = context_.interior || sky->currentWeather;
    context_.hour = Hour(sky->currentGameHour);
    context_.transition = FiniteClamp(sky->currentWeatherPct, 0, 1, 1);
    context_.cell = cell->GetFormID();
    auto update = [&](const RE::TESWeather* weather, std::uint32_t& cachedID, Weather& cached) {
        const auto id = weather ? weather->GetFormID() : 0u;
        if (id != cachedID) {
            const auto entry = catalogueIndex_.find(id);
            cached = entry != catalogueIndex_.end() ? (*catalogue_)[entry->second].weather : ReadWeather(weather);
            cachedID = id;
        }
        cached.group = Classification(weather);
    };
    update(sky->currentWeather, incomingID_, context_.incoming);
    update(sky->lastWeather, outgoingID_, context_.outgoing);
}
void Runtime::Apply(SourceDLSSG::NeuralOptions& options, bool& sharpening, float& sharpness)
{
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(mutex_);
    // Manual mode keeps the context display current at 4 Hz without allocating
    // weather identities or resolving profiles on every rendered frame.
    const bool automatic = controller_.Configuration().enabled && !controller_.State().paused;
    if (!automatic && now < nextIdleSample_) { return; }
    nextIdleSample_ = now + std::chrono::milliseconds(250);
    ReadContext();
    const float elapsed = std::chrono::duration<float>(now - lastFrame_).count();
    lastFrame_ = now;
    const auto resolutions = controller_.SelectionResolutions();
    controller_.Apply(context_, options, sharpening, sharpness, elapsed);
    if (resolutions != controller_.SelectionResolutions()) {
        const auto& state = controller_.State();
        logger::info("[Appearance] resolved hour={:.2f} interior={} weather={}/{:06X} presets='{}' from='{}' sharpness='{}'",
            context_.hour, context_.interior, context_.incoming.record.plugin, context_.incoming.record.localID,
            state.result.incoming, state.result.outgoing, state.result.sharpnessSource);
    }
}
Settings Runtime::Load(const CSimpleIniA& ini)
{
    std::vector<std::pair<std::string, std::unique_ptr<CSimpleIniA>>> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(PresetFolder(), error)) {
        if (!entry.is_regular_file(error) || Lower(Utf8Name(entry.path().extension())) != ".ini") { continue; }
        auto file = std::make_unique<CSimpleIniA>();
        file->SetUnicode();
        if (file->LoadFile(entry.path().c_str()) < 0) {
            logger::warn("[Appearance] could not read preset file {}", Utf8Name(entry.path()));
            continue;
        }
        files.emplace_back(Utf8Name(entry.path()), std::move(file));
    }
    std::vector<std::pair<std::string, const CSimpleIniA*>> views;
    for (const auto& [name, file] : files) { views.emplace_back(name, file.get()); }
    // Base as saved beside older-format presets, to drop values they only copied from it.
    const auto nr = SourceDLSSG::SanitizePreferences(SourceDLSSG::LoadPreferences(ini));
    Setup base;
    base.neural = nr.neuralEnabled;
    base.beforeUpscaling = nr.neuralBeforeUpscaling;
    base.passes = nr.neuralPasses;
    base.combat = nr.neuralCombat;
    base.reconstruction = nr.neuralReconstruction;
    base.tuning = nr.neuralTuning;
    base.second = NeuralRendering::EffectiveSecondPass(nr.neuralSecondPass, nr.neuralReconstruction, nr.neuralTuning);
    base.sharpening = ini.GetBoolValue("Settings", "Sharpening", false);
    base.sharpness = static_cast<float>(ini.GetDoubleValue("Settings", "Sharpness", 0.3));
    auto settings = LoadSettings(ini, views, &base);
    logger::info("[Appearance] loaded {} presets ({} preset files)", settings.presets.size(), files.size());
    return settings;
}
Settings Runtime::SavePresets(Settings settings, bool& ok)
{
    ok = true;
    settings = Sanitize(std::move(settings));
    const auto folder = PresetFolder();
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::vector<std::string> written;
    std::size_t unchanged = 0;
    for (auto& preset : settings.presets) {
        const auto name = PresetFileName(preset.name);
        CSimpleIniA file;
        file.SetUnicode();
        StorePresetFile(file, preset);
        // Leave unchanged files alone, so presets from mods keep their files untouched.
        const auto path = folder / Utf8Path(name);
        std::string text, current;
        file.Save(text, true);
        if (std::ifstream in{path, std::ios::binary}) { current.assign(std::istreambuf_iterator<char>(in), {}); }
        if (text == current) { ++unchanged; }
        else if (file.SaveFile(path.c_str()) < 0) {
            logger::error("[Appearance] could not write preset file {}", name);
            ok = false;
            continue;
        }
        // A renamed preset's old file goes once the new one is written.
        if (!preset.file.empty() && Lower(preset.file) != Lower(name)) { settings.removedFiles.push_back(preset.file); }
        preset.file = name;
        written.push_back(Lower(name));
    }
    for (const auto& name : settings.removedFiles) {
        if (std::ranges::find(written, Lower(name)) != written.end()) { continue; }
        if (!std::filesystem::remove(folder / Utf8Path(name), error) && error) {
            logger::warn("[Appearance] could not remove preset file {}", name);
        }
    }
    settings.removedFiles.clear();
    logger::info("[Appearance] saved {} preset files ({} unchanged)", written.size(), unchanged);
    return settings;
}
}
