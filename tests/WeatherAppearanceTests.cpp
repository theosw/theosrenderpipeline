#include "WeatherAppearanceController.h"
#include "WeatherAppearanceINI.h"
#include "RendererSettings.h"
#include <SimpleIni.h>
#include <cstdio>
#include <cstdlib>
#include <limits>

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
static void Resolution()
{
    Settings settings;
    const auto base = Value(.6f, .3f);
    auto scene = Scene();
    Require(Evaluate(settings, scene, base).values == base, "disabled preserves manual defaults");
    settings.enabled = true;
    Require(Evaluate(settings, scene, base).values == base, "empty profiles preserve manual defaults");
    settings.groups[0] = FromValues(Value(.8f, .4f));
    settings.groups[1] = FromValues(Value(1.2f, .6f));
    settings.groups[3] = FromValues(Value(.4f, .2f));
    Require(Near(Evaluate(settings, scene, base).values.passes[0].intensity, 1.2f), "classification over general exterior");
    settings.weathers.push_back({scene.incoming.record, FromValues(Value(1.8f, .9f))});
    Require(Near(Evaluate(settings, scene, base).values.sharpness, .9f), "exact weather overrides classification");
    settings.weathers[0].profile.neural = false;
    Require(Near(Evaluate(settings, scene, base).values.passes[0].intensity, 1.2f), "per-channel exact fallback");
    scene.incoming.record.plugin = "another.esp";
    Require(Near(Evaluate(settings, scene, base).values.sharpness, .6f), "same local ID in another plugin is distinct");
    scene.incoming.group = Group::Exterior;
    Require(Near(Evaluate(settings, scene, base).values.sharpness, .4f), "unknown weather inherits exterior");
    scene.interior = true;
    Require(Evaluate(settings, scene, base).values == base, "interior never inherits stale exterior weather");
    settings.groups[5] = FromValues(Value(.25f, .1f));
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
    settings.groups[1] = profile;
    settings.groups[3] = FromValues(Value(1, .4f));
    auto scene = Scene(); scene.hour = 22; scene.outgoing = scene.incoming;
    scene.incoming = {Normalize({"Rain.esp", 0xA10}), Group::Rain}; scene.transition = .5f;
    Require(Near(Evaluate(settings, scene, {}).values.sharpness, .45f), "time interpolation precedes weather interpolation");
    settings.weathers.push_back({Normalize({"WEATHER.ESP", 0x123456}), profile});
    settings.weathers.push_back({Normalize({"Rain.esl", 0x801}), FromValues(Value(.5f, .75f))});
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
    loaded.SetDoubleValue("Appearance.Exterior.Day", "Sharpness", -100);
    auto corrected = LoadSettings(loaded);
    Require(corrected.weathers.empty() && corrected.hours == DefaultHours && corrected.groups[0].points[3].sharpness == 0,
        "malformed IDs, schedule and strength are sanitized");
    CSimpleIniA old;
    old.LoadData("[SourceDLSSG]\nNRIntensity=0.75\n");
    Require(LoadSettings(old) == Settings{}, "old installations remain manual");
    settings.weathers.push_back(settings.weathers.front());
    settings.weathers.push_back({{"", 0}, profile});
    Require(Sanitize(settings).weathers.size() == 1, "duplicates and invalid keys cannot shadow profiles");
    settings.groups[0].points[0].sharpness = std::numeric_limits<float>::infinity();
    Require(std::isfinite(Sanitize(settings).groups[0].points[0].sharpness), "non-finite profiles sanitize");
    RendererSettingsDraft draft, baseline; draft.valid = baseline.valid = true;
    draft.appearance = settings;
    Require(CountRendererSettingsChanges(draft, baseline) == 1, "profile edits participate in Apply/Discard changes");
    draft.appearance.hours[3] = draft.appearance.hours[2];
    Require(ValidateRendererSettings(draft, {true, true, true, false}) != nullptr, "bad UI schedule is rejected visibly");
}
static void FrameHistory()
{
    Settings settings; settings.enabled = true; settings.smoothingSeconds = 1;
    settings.groups[1] = FromValues(Value(.3f, .2f));
    settings.groups[3] = FromValues(Value(1.7f, .9f));
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
int main()
{
    Resolution(); TimeAndPersistence(); FrameHistory();
    std::puts("PASS: weather/time resolution, persistence, frame smoothing, manual overrides and NR history");
}
