#include <PCH.h>
#include "WeatherAppearanceRuntime.h"

namespace TheosRenderPipeline::Appearance
{
namespace
{
Weather ReadWeather(const RE::TESWeather* weather)
{
    Weather result;
    if (!weather) { return result; }
    if (const auto* file = weather->GetFile(0)) {
        result.record = RuntimeRecord(std::string(file->GetFilename()), weather->GetFormID(), file->IsLight());
    }
    using Flag = RE::TESWeather::WeatherDataFlag;
    const auto flags = weather->data.flags;
    if (flags.any(Flag::kSnow)) { result.group = Group::Snow; }
    else if (flags.any(Flag::kRainy)) { result.group = Group::Rain; }
    else if (flags.any(Flag::kCloudy)) { result.group = Group::Cloudy; }
    else if (flags.any(Flag::kPleasant)) { result.group = Group::Clear; }
    return result;
}
Context ReadContext()
{
    Context result;
    auto* ui = RE::UI::GetSingleton();
    if (!ui || ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) { return result; }
    const auto* player = RE::PlayerCharacter::GetSingleton();
    const auto* sky = RE::Sky::GetSingleton();
    const auto* cell = player ? player->GetParentCell() : nullptr;
    if (!sky || !cell || !std::isfinite(sky->currentGameHour)) { return result; }
    result.interior = cell->IsInteriorCell() && !cell->UsesSkyLighting();
    // Sky-lit interior cells intentionally follow exterior weather/time settings.
    result.valid = result.interior || sky->currentWeather;
    result.hour = Hour(sky->currentGameHour);
    result.transition = FiniteClamp(sky->currentWeatherPct, 0, 1, 1);
    result.cell = cell->GetFormID();
    result.incoming = ReadWeather(sky->currentWeather);
    result.outgoing = ReadWeather(sky->lastWeather);
    return result;
}
}
float Runtime::Apply(SourceDLSSG::NeuralOptions& options, float sharpness)
{
    const auto context = ReadContext();
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(mutex_);
    const float elapsed = std::chrono::duration<float>(now - lastFrame_).count();
    lastFrame_ = now;
    return controller_.Apply(context, options, sharpness, elapsed);
}
}
