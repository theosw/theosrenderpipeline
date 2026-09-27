#include <PCH.h>
#include "WeatherAppearanceRuntime.h"

namespace TheosRenderPipeline::Appearance
{
namespace
{
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
    for (const auto* weather : data->GetFormArray<RE::TESWeather>()) {
        if (!weather) { continue; }
        auto value = ReadWeather(weather);
        if (!Valid(value.record)) { continue; }
        const auto* name = weather->GetFormEditorID();
        entries->push_back(CatalogueEntry(weather->GetFormID(), std::move(value), name ? name : ""));
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
float Runtime::Apply(SourceDLSSG::NeuralOptions& options, float sharpness)
{
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(mutex_);
    // Manual mode keeps the context display current at 4 Hz without allocating
    // weather identities or resolving profiles on every rendered frame.
    const bool automatic = controller_.Configuration().enabled && !controller_.State().paused;
    if (!automatic && now < nextIdleSample_) { return sharpness; }
    nextIdleSample_ = now + std::chrono::milliseconds(250);
    ReadContext();
    const float elapsed = std::chrono::duration<float>(now - lastFrame_).count();
    lastFrame_ = now;
    const auto resolutions = controller_.SelectionResolutions();
    const auto result = controller_.Apply(context_, options, sharpness, elapsed);
    if (resolutions != controller_.SelectionResolutions()) {
        const auto& state = controller_.State();
        logger::info("[Appearance] resolved hour={:.2f} interior={} weather={}/{:06X} NR='{}' from='{}' sharpening='{}' from='{}'",
            context_.hour, context_.interior, context_.incoming.record.plugin, context_.incoming.record.localID,
            state.result.incoming, state.result.outgoing, state.result.incomingSharpening, state.result.outgoingSharpening);
    }
    return result;
}
}
