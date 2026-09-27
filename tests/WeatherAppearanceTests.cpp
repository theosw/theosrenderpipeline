#include "WeatherAppearanceController.h"
#include "WeatherAppearanceINI.h"
#include "WeatherEditorID.h"
#include "RendererSettings.h"
#include <SimpleIni.h>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <chrono>

using namespace TheosRenderPipeline;
using namespace TheosRenderPipeline::Appearance;
static void Require(bool value, const char* why)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static bool Near(float a, float b) { return std::abs(a - b) < 0.00001f; }
static Values Value(float intensity, float sharpness)
{
    Values values;
    values.passes[0] = {intensity, intensity / 2, intensity / 4};
    values.passes[1] = {intensity / 2, intensity / 3, intensity / 5};
    values.sharpness = sharpness;
    return values;
}
static Context Scene()
{
    Context scene;
    scene.valid = true; scene.hour = 12; scene.transition = 1;
    scene.incoming = {Normalize({"Weather.esp", 0xABC}), Group::Clear};
    return scene;
}
static void GroupPreset(Settings& settings, std::size_t group, Profile profile)
{
    settings.groups[group] = AddPreset(settings, Groups[group], std::move(profile));
}
static void ExactPreset(Settings& settings, Record record, Profile profile)
{
    const auto id = AddPreset(settings, record.plugin, std::move(profile));
    Require(Assign(settings, std::move(record), id), "create exact weather assignment");
}
static void Resolution()
{
    Require(RuntimeRecord("Weather.esp", 0x02ABCDEF, false) == RuntimeRecord("WEATHER.ESP", 0xB1ABCDEF, false),
        "full-plugin identity survives load-order changes");
    Require(RuntimeRecord("Weather.esl", 0xFE012ABC, true) == RuntimeRecord("WEATHER.ESL", 0xFEFEDABC, true) &&
        RuntimeRecord("Weather.esl", 0xFE012ABC, true).localID == 0xABC, "light-plugin identity excludes both runtime indices");
    Settings settings;
    const auto base = Value(.6f, .3f);
    auto scene = Scene();
    Require(Evaluate(settings, scene, base).values == base, "disabled preserves manual defaults");
    settings.enabled = true;
    Require(Evaluate(settings, scene, base).values == base, "empty profiles preserve manual defaults");
    GroupPreset(settings, 0, FromValues(Value(.8f, .4f)));
    GroupPreset(settings, 1, FromValues(Value(1.2f, .6f)));
    GroupPreset(settings, 3, FromValues(Value(.4f, .2f)));
    Require(Near(Evaluate(settings, scene, base).values.passes[0].intensity, 1.2f), "classification over general exterior");
    ExactPreset(settings, scene.incoming.record, FromValues(Value(1.8f, .9f)));
    Require(Near(Evaluate(settings, scene, base).values.sharpness, .9f), "exact weather overrides classification");
    settings.presets.back().profile.neural = false;
    Require(Near(Evaluate(settings, scene, base).values.passes[0].intensity, 1.2f), "per-channel exact fallback");
    scene.incoming.record.plugin = "another.esp";
    Require(Near(Evaluate(settings, scene, base).values.sharpness, .6f), "same local ID in another plugin is distinct");
    scene.incoming.group = Group::Exterior;
    Require(Near(Evaluate(settings, scene, base).values.sharpness, .4f), "unknown weather inherits exterior");
    scene.interior = true;
    Require(Evaluate(settings, scene, base).values == base, "interior never inherits stale exterior weather");
    GroupPreset(settings, 5, FromValues(Value(.25f, .1f)));
    Require(Near(Evaluate(settings, scene, base).values.sharpness, .1f), "explicit interior profile");

    scene = Scene();
    settings.weathers.clear();
    scene.outgoing = scene.incoming;
    scene.incoming = {Normalize({"Rain.esl", 0x801}), Group::Rain};
    for (float t : {0.0f, .25f, .5f, 1.0f}) {
        scene.transition = t;
        auto result = Evaluate(settings, scene, base);
        Require(Near(result.values.passes[0].intensity, 1.2f - .8f * t), "weather blend endpoints and midpoint");
        Require(Near(result.values.sharpness, .6f - .4f * t), "sharpening follows the same weather progress");
    }
    scene.outgoing = {}; scene.transition = 0;
    Require(Near(Evaluate(settings, scene, base).values.sharpness, .2f), "missing outgoing uses incoming");
    scene.hour = std::numeric_limits<float>::quiet_NaN();
    Require(!Evaluate(settings, scene, base).active, "invalid time falls back without NaNs");
    scene.hour = 12; scene.valid = false;
    Require(Evaluate(settings, scene, base).values == base, "load/main menu fallback");
}
static void TimeAndPersistence()
{
    auto profile = FromValues(Value(0, 0));
    profile.points[5] = Value(2, 1);
    Require(Near(AtTime(profile, DefaultHours, 22).sharpness, .5f), "dusk blends across midnight");
    Require(AtTime(profile, DefaultHours, 24) == profile.points[0], "midnight endpoint wraps");
    Require(AtTime(profile, DefaultHours, -2) == AtTime(profile, DefaultHours, 22), "negative clock wrap");
    Require(std::abs(AtTime(profile, DefaultHours, 23.999f).sharpness -
        AtTime(profile, DefaultHours, 0.001f).sharpness) < .001f, "midnight continuity");
    Settings settings; settings.enabled = true;
    GroupPreset(settings, 1, profile);
    GroupPreset(settings, 3, FromValues(Value(1, .4f)));
    auto scene = Scene(); scene.hour = 22; scene.outgoing = scene.incoming;
    scene.incoming = {Normalize({"Rain.esp", 0xA10}), Group::Rain}; scene.transition = .5f;
    Require(Near(Evaluate(settings, scene, {}).values.sharpness, .45f), "time interpolation precedes weather interpolation");
    ExactPreset(settings, Normalize({"WEATHER.ESP", 0x123456}), profile);
    ExactPreset(settings, Normalize({"Rain.esl", 0x801}), FromValues(Value(.5f, .75f)));
    CSimpleIniA ini;
    ini.SetValue("ForeignFeature", "Keep", "untouched");
    StoreSettings(ini, settings);
    std::string encoded;
    Require(ini.Save(encoded) >= 0, "serialize profiles");
    CSimpleIniA loaded;
    Require(loaded.LoadData(encoded.c_str()) >= 0, "parse profiles");
    Require(LoadSettings(loaded) == settings, "both passes, all time points, stable plugin IDs round trip");
    Require(std::string(loaded.GetValue("ForeignFeature", "Keep", "")) == "untouched", "unrelated INI survives");
    settings.weathers.erase(settings.weathers.begin());
    StoreSettings(loaded, settings);
    Require(LoadSettings(loaded) == settings && !loaded.GetSection("Appearance.Weather1.Day"), "removed weather cannot reappear on reload");
    loaded.SetValue("Appearance.Weather0", "FormID", "801garbage");
    loaded.SetDoubleValue("Appearance", "DayHour", 30);
    loaded.SetDoubleValue("Appearance.Preset0.Day", "Sharpness", -100);
    auto corrected = LoadSettings(loaded);
    Require(corrected.weathers.empty() && corrected.hours == DefaultHours && corrected.presets[0].profile.points[3].sharpness == 0,
        "malformed IDs, schedule and strength are sanitized");
    CSimpleIniA old;
    old.LoadData("[SourceDLSSG]\nNRIntensity=0.75\n");
    Require(LoadSettings(old) == Settings{}, "old installations remain manual");
    settings.weathers.push_back(settings.weathers.front());
    settings.weathers.push_back({{"", 0}, 1});
    Require(Sanitize(settings).weathers.size() == 1, "duplicates and invalid keys cannot shadow profiles");
    settings.presets[0].profile.points[0].sharpness = std::numeric_limits<float>::infinity();
    Require(std::isfinite(Sanitize(settings).presets[0].profile.points[0].sharpness), "non-finite profiles sanitize");
    RendererSettingsDraft draft, baseline; draft.valid = baseline.valid = true;
    draft.appearance = settings;
    Require(CountRendererSettingsChanges(draft, baseline) == 1, "profile edits participate in Apply/Discard changes");
    draft.appearance.hours[3] = draft.appearance.hours[2];
    Require(ValidateRendererSettings(draft, {true, true, true, false}) != nullptr, "bad UI schedule is rejected visibly");
}
static void FrameHistory()
{
    Settings settings; settings.enabled = true; settings.smoothingSeconds = 1;
    GroupPreset(settings, 1, FromValues(Value(.3f, .2f)));
    GroupPreset(settings, 3, FromValues(Value(1.7f, .9f)));
    Controller controller; controller.Configure(settings);
    SourceDLSSG::NeuralOptions base; base.enabled = true; base.passes = 2;
    base.tuning.skinStructureStrength = -1; base.tuning.style = 4;
    base.secondPass.linked = false;
    const auto manual = base;
    SourceDLSSG::NeuralHistory history;
    auto scene = Scene();
    auto first = base;
    controller.Apply(scene, first, .4f, 1.0f / 60);
    Require(history.ResetFor(first, true, false), "first automated frame resets");
    Require(first.tuning.skinStructureStrength == -1 && first.tuning.style == 4, "sentinel and discrete style preserved");
    scene.outgoing = scene.incoming;
    scene.incoming = {Normalize({"rain.esp", 0x812}), Group::Rain};
    float lastIntensity = first.tuning.intensity;
    for (int i = 0; i < 240; ++i) {
        auto frame = base;
        scene.hour += .0001f; scene.transition = static_cast<float>(i) / 239;
        controller.Apply(scene, frame, .4f, 1.0f / 60);
        Require(!history.ResetFor(frame, true, false), "weather/time ramps do not reset history every frame");
        Require(frame.tuning.intensity >= lastIntensity, "weather smoothing moves continuously toward target");
        lastIntensity = frame.tuning.intensity;
    }
    Require(lastIntensity > 1 && base == manual && controller.Configuration() == settings, "ramp changes frames, never defaults or profiles");
    Require(controller.SelectionResolutions() == 2, "ordinary weather progress resolves and labels presets only on identity changes");
    settings.presets[1].profile.points.fill(Value(.45f, .15f));
    settings.presets[1].name = "Edited shared rain";
    controller.Configure(settings);
    auto edited = base; controller.Apply(scene, edited, .4f, .016f);
    Require(history.ResetFor(edited, true, false) && Near(edited.tuning.intensity, .45f) &&
        controller.State().result.incoming == "Edited shared rain", "configuration replacement invalidates cached pointers, labels and history");
    auto frame = base;
    controller.Apply(scene, frame, .4f, .016f);
    Require(history.ResetFor(frame, true, true), "camera reset survives automation");
    Require(!history.ResetFor(frame, true, false), "camera reset is not sticky");
    base.tuning.intensity = .8f; frame = base;
    controller.Apply(scene, frame, .4f, .016f);
    Require(history.ResetFor(frame, true, false), "manual edit resets even under an overriding profile");
    base.secondPass.linked = true; frame = base;
    controller.Apply(scene, frame, .4f, .016f);
    Require(history.ResetFor(frame, true, false), "relink resets history");
    Require(frame.EffectiveSecond().tuning == frame.tuning, "linked second pass uses automated first pass");
    controller.Pause(true); frame = base;
    Require(controller.Apply(scene, frame, .4f, .016f) == .4f && frame == base, "manual session override restores base values");
    Require(history.ResetFor(frame, true, false), "leaving automation resets once");
    controller.Pause(false); frame = base;
    controller.Apply(scene, frame, .4f, .016f); history.ResetFor(frame, true, false);
    scene.hour += 5; frame = base; controller.Apply(scene, frame, .4f, .016f);
    Require(history.ResetFor(frame, true, false), "waiting or fast travel breaks history");
    controller.Invalidate(); frame = base; controller.Apply(scene, frame, .4f, .016f);
    Require(history.ResetFor(frame, true, false), "load invalidation resets history");
    scene.valid = false; frame = base;
    controller.Apply(scene, frame, .4f, .016f);
    Require(frame == base && !controller.State().result.active, "unavailable game state never applies stale weather");
    Require(history.ResetFor(frame, false, false), "ineligible frame breaks history");
    // Even a reused revision cannot excuse a discrete or skin-mode change.
    first.appearanceRevision = 15; history.ResetFor(first, true, false);
    first.tuning.skinStructureStrength = .5f;
    Require(history.ResetFor(first, true, false), "skin sentinel changes always reset");
    first.reconstruction.inputScale = .5f;
    Require(history.ResetFor(first, true, false), "resource choices always reset");
    first.appearanceRevision = 0; history.ResetFor(first, true, false);
    first.tuning.localToneStrength += .1f;
    Require(history.ResetFor(first, true, false), "manual tuning retains its original reset policy");
}
static void SharedPresets()
{
    Settings settings; settings.enabled = true;
    const auto shared = AddPreset(settings, "Rain and mist", FromValues(Value(.8f, .2f)));
    settings.groups[3] = shared;
    const std::vector<Record> records{{"Weather.esp", 0xABC}, {"Weather.esp", 0xDEF}, {"Other.esl", 0xABC}};
    Require(AssignMany(settings, records, shared), "bulk assignment supports shared presets");
    settings.presets[0].profile.points.fill(Value(1.4f, .7f));
    for (const auto& record : records) {
        auto scene = Scene(); scene.incoming.record = Normalize(record);
        Require(Near(Evaluate(settings, scene, {}).values.sharpness, .7f), "one shared edit reaches every assigned weather");
    }
    auto scene = Scene(); scene.incoming.group = Group::Rain; scene.incoming.record = {"missing.esp", 42};
    Require(Near(Evaluate(settings, scene, {}).values.sharpness, .7f), "broad group uses the same shared preset");
    settings.presets[0].profile.enabled = false;
    Require(Evaluate(settings, Scene(), Value(.5f, .3f)).values == Value(.5f, .3f), "disabled shared preset inherits");
    CSimpleIniA ini; StoreSettings(ini, settings);
    Require(LoadSettings(ini) == settings, "shared identity and disabled presets round trip");
    RemovePreset(settings, shared);
    Require(settings.weathers.empty() && settings.groups[3] == 0, "deleting a shared preset clears all its references");
    StoreSettings(ini, settings);
    Require(!ini.GetSection("Appearance.Preset0.Day") && !ini.GetSection("Appearance.Weather0") && LoadSettings(ini) == settings,
        "removed presets and assignments do not return after restart");
    Require(AddStarterProfiles(settings, Value(.6f, .4f)), "starter templates created");
    const auto before = settings;
    Require(AddStarterProfiles(settings, Value(1.9f, .9f)) && settings == before, "starter action does not replace existing tuning");
    for (const auto id : settings.groups) { Require(FindPreset(settings, id)->profile.points[3] == Value(.6f, .4f), "starter copies manual values"); }

    Settings capacity; const auto id = AddPreset(capacity, "Shared", FromValues(Value(1, .2f)));
    for (std::uint32_t i = 1; i <= 170; ++i) { Require(Assign(capacity, {"NAT.esp", i}, id), "LoreRim-sized mapping accepted"); }
    StoreSettings(ini, capacity); Require(LoadSettings(ini) == capacity, "170 assignments share one persisted preset");
    for (std::uint32_t i = 171; i < MaxWeathers; ++i) { Require(Assign(capacity, {"NAT.esp", i}, id), "fill assignment capacity"); }
    const auto count = capacity.weathers.size();
    const std::array<Record, 2> tooMany{{{"other.esp", 1}, {"other.esp", 2}}};
    Require(!AssignMany(capacity, tooMany, id) && capacity.weathers.size() == count, "bulk capacity failure is atomic");
    Require(Assign(capacity, {"NAT.esp", static_cast<std::uint32_t>(MaxWeathers)}, id) &&
        !Assign(capacity, {"extra.esp", 1}, id), "assignment boundary is enforced");
    Require(Assign(capacity, {"NAT.esp", 1}, 0), "remove assignment at capacity");
    const auto entry = CatalogueEntry(0x02000ABC, {Normalize({"Weather.esp", 0xABC}), Group::Rain}, "NAT_Rainstorm");
    Require(entry.search.find("nat_rainstorm") != std::string::npos && entry.search.find("weather.esp") != std::string::npos &&
        entry.search.find("000abc") != std::string::npos && entry.search.find("rain") != std::string::npos, "weather search includes names, owner, ID and classification");
}
static void MigrationAndSchedule()
{
    CSimpleIniA legacy;
    legacy.SetBoolValue("Appearance", "Enabled", true);
    StoreProfile(legacy, "Appearance.Clear", FromValues(Value(.8f, .4f)));
    legacy.SetLongValue("Appearance", "WeatherCount", 2);
    for (int i = 0; i < 2; ++i) {
        const auto section = std::format("Appearance.Weather{}", i);
        legacy.SetValue(section.c_str(), "Plugin", "Old.esp");
        legacy.SetValue(section.c_str(), "FormID", i ? "801" : "800");
        StoreProfile(legacy, section, FromValues(Value(.2f + i, .1f + i * .1f)));
    }
    const auto migrated = LoadSettings(legacy);
    Require(migrated.presets.size() == 3 && migrated.weathers.size() == 2, "legacy groups and exact profiles migrate without dropping tuning");
    auto scene = Scene(); scene.incoming.record = {"old.esp", 0x801};
    Require(Near(Evaluate(migrated, scene, {}).values.passes[0].intensity, 1.2f), "migrated exact values take precedence");
    StoreSettings(legacy, migrated);
    Require(LoadSettings(legacy) == migrated && !legacy.GetSection("Appearance.Weather0.Day"), "migration saves canonical shared format");
    legacy.SetLongValue("Appearance.Weather0", "Preset", 99999);
    legacy.SetLongValue("Appearance.Rain", "Preset", 99999);
    Require(LoadSettings(legacy).weathers.size() == 1 && LoadSettings(legacy).groups[3] == 0, "dangling references cannot retarget new presets");
    CSimpleIniA enb;
    enb.LoadData("[TIMEOFDAY]\nDawnDuration=3.5\nSunriseTime=8.5\nDayTime=10.5\nSunsetTime=17.5\nDuskDuration=4\nNightTime=1\n");
    const auto hours = ReadENBSchedule(enb);
    Require(hours && *hours == std::array<float, 6>{1, 5, 8.5f, 10.5f, 17.5f, 21.5f}, "Cabbage markers and dawn/dusk boundaries convert to editable anchors");
    enb.SetDoubleValue("TIMEOFDAY", "SunsetTime", 23);
    Require(!ReadENBSchedule(enb), "ambiguous cross-midnight ENB schedule is rejected without guessing");
    CSimpleIniA missing; Require(!ReadENBSchedule(missing), "missing ENB never replaces manual schedule");
    const auto nativeLookup = +[](EditorFormInfo form) -> const char* {
        Require(form.id == 0x02000ABC && form.type == 54, "native editor-ID API receives form ID and type by value");
        return "NAT_Clear";
    };
    const auto tweaksLookup = +[](std::uint32_t id) -> const char* {
        Require(id == 0x02000ABC, "Tweaks API receives runtime FormID"); return "NAT_Cloudy";
    };
    const EditorFormInfo form{0x02000ABC, 54};
    Require(EditorName("", form, nativeLookup, tweaksLookup) == "NAT_Clear", "external-only native lookup supplies discarded weather names");
    Require(EditorName(nullptr, form, nullptr, tweaksLookup) == "NAT_Cloudy", "Tweaks name fallback");
    Require(EditorName("", form, nullptr, nullptr).empty(), "absent companions leave stable ID fallback available");
    Require(EditorName("EngineName", form, nullptr, nullptr) == "EngineName", "engine-provided names need no companion");
}
static void Benchmark()
{
    Settings settings; settings.enabled = true;
    const auto preset = AddPreset(settings, "Heavy rain", FromValues(Value(.8f, .4f)));
    for (std::uint32_t i = 1; i <= MaxWeathers; ++i) { Assign(settings, {"weather.esp", i}, preset); }
    auto scene = Scene(); scene.incoming.record.localID = static_cast<std::uint32_t>(MaxWeathers);
    std::array<double, 7> timings{};
    for (int automatic = 0; automatic < 2; ++automatic) {
        settings.enabled = automatic != 0;
        for (auto& timing : timings) {
            Controller controller; controller.Configure(settings);
            SourceDLSSG::NeuralOptions base;
            auto warmup = base; controller.Apply(scene, warmup, .3f, .016f);
            const auto start = std::chrono::steady_clock::now();
            float sum{};
            for (int i = 0; i < 100000; ++i) { auto frame = base; sum += controller.Apply(scene, frame, .3f, .016f); }
            timing = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / 100000;
            Require(sum > 0 && controller.SelectionResolutions() == static_cast<std::uint64_t>(automatic), "benchmark observes stable cached resolution");
        }
        std::ranges::sort(timings);
        std::printf("Controller benchmark: %s, 4096 assignments, median %.1f ns/frame (7 x 100000; standalone CPU only)\n", automatic ? "automatic" : "manual", timings[3]);
    }
}
int main(int argc, char** argv)
{
    Resolution(); TimeAndPersistence(); FrameHistory(); SharedPresets(); MigrationAndSchedule();
    if (argc == 2 && std::string(argv[1]) == "--benchmark") { Benchmark(); }
    std::puts("PASS: weather/time resolution, persistence, frame smoothing, manual overrides and NR history");
}
