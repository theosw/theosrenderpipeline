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
// A preset that changes both passes' look and sharpness, like format-2 presets.
static Profile Look(float intensity, float sharpness)
{
    Profile profile;
    SetChange(profile, "Pass1Intensity", intensity);
    SetChange(profile, "Pass1LocalTone", intensity / 2);
    SetChange(profile, "Pass1LocalStructure", intensity / 4);
    SetChange(profile, "Pass2Intensity", intensity / 2);
    SetChange(profile, "Pass2LocalTone", intensity / 3);
    SetChange(profile, "Pass2LocalStructure", intensity / 5);
    SetChange(profile, "Sharpness", sharpness);
    return profile;
}
static Setup BaseSetup(float intensity, float sharpness)
{
    Setup setup;
    setup.neural = true;
    setup.tuning.intensity = intensity;
    setup.sharpness = sharpness;
    return setup;
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
    FindPreset(settings, AddPreset(settings, Groups[group], std::move(profile)))->groups[group] = true;
}
static void ExactPreset(Settings& settings, Record record, Profile profile)
{
    const auto id = AddPreset(settings, record.plugin, std::move(profile));
    const std::array records{std::move(record)};
    Require(AddWeathers(settings, id, records), "create exact weather assignment");
}
static float ApplyFrame(Controller& controller, const Context& scene, SourceDLSSG::NeuralOptions& frame, float elapsed, float sharpness = .4f)
{
    bool sharpening = true;
    controller.Apply(scene, frame, sharpening, sharpness, elapsed);
    return sharpness;
}
// Saves like the runtime: the main INI plus one in-memory file per preset.
struct Saved
{
    CSimpleIniA main;
    std::vector<std::pair<std::string, std::unique_ptr<CSimpleIniA>>> files;
};
static void Save(Saved& saved, const Settings& settings)
{
    saved.files.clear();
    for (const auto& preset : Sanitize(settings).presets) {
        auto file = std::make_unique<CSimpleIniA>();
        StorePresetFile(*file, preset);
        saved.files.emplace_back(PresetFileName(preset.name), std::move(file));
    }
    StoreSettings(saved.main, settings);
}
static Settings Load(const Saved& saved)
{
    std::vector<std::pair<std::string, const CSimpleIniA*>> views;
    for (const auto& [name, file] : saved.files) { views.emplace_back(name, file.get()); }
    return LoadSettings(saved.main, views);
}
// Loading numbers presets in order and records their files; compare without those.
static Settings Plain(Settings settings)
{
    settings = Sanitize(std::move(settings));
    for (std::size_t i = 0; i < settings.presets.size(); ++i) {
        settings.presets[i].id = static_cast<std::uint32_t>(i + 1);
        settings.presets[i].file.clear();
    }
    return settings;
}
static void Resolution()
{
    Require(RuntimeRecord("Weather.esp", 0x02ABCDEF, false) == RuntimeRecord("WEATHER.ESP", 0xB1ABCDEF, false),
        "full-plugin identity survives load-order changes");
    Require(RuntimeRecord("Weather.esl", 0xFE012ABC, true) == RuntimeRecord("WEATHER.ESL", 0xFEFEDABC, true) &&
        RuntimeRecord("Weather.esl", 0xFE012ABC, true).localID == 0xABC, "light-plugin identity excludes both runtime indices");
    Settings settings;
    const auto base = BaseSetup(.6f, .3f);
    auto scene = Scene();
    Require(Evaluate(settings, scene, base).setup == base, "disabled preserves Base");
    settings.enabled = true;
    Require(Evaluate(settings, scene, base).setup == base, "empty profiles preserve Base");
    GroupPreset(settings, 0, Look(.8f, .4f));
    GroupPreset(settings, 1, Look(1.2f, .6f));
    GroupPreset(settings, 3, Look(.4f, .2f));
    Require(Near(Evaluate(settings, scene, base).setup.tuning.intensity, 1.2f), "classification over general exterior");
    ExactPreset(settings, scene.incoming.record, Look(1.8f, .9f));
    Require(Near(Evaluate(settings, scene, base).setup.sharpness, .9f), "exact weather overrides classification");
    ClearChange(settings.presets.back().profile, "Pass1Intensity");
    Require(Near(Evaluate(settings, scene, base).setup.tuning.intensity, 1.2f), "unchanged settings fall through to less specific presets");
    scene.incoming.record.plugin = "another.esp";
    Require(Near(Evaluate(settings, scene, base).setup.sharpness, .6f), "same local ID in another plugin is distinct");
    scene.incoming.group = Group::Exterior;
    Require(Near(Evaluate(settings, scene, base).setup.sharpness, .4f), "unknown weather inherits Outdoors");
    scene.interior = true;
    Require(Evaluate(settings, scene, base).setup == base, "interior never inherits stale exterior weather");
    GroupPreset(settings, 5, Look(.25f, .1f));
    Require(Near(Evaluate(settings, scene, base).setup.sharpness, .1f), "explicit interior profile");

    scene = Scene();
    SetChange(settings.presets[0].profile, "Passes", 2);
    SetChange(settings.presets[0].profile, "Pass1Style", 3);
    const auto layered = Evaluate(settings, scene, base).setup;
    Require(layered.passes == 2 && layered.tuning.style == 3 && Near(layered.tuning.intensity, 1.2f), "presets layer per setting");
    Require(Evaluate(settings, scene, base).incoming == "weather.esp + Clear + Exterior", "the applied presets are named most specific first");
    for (auto& preset : settings.presets) { preset.weathers.clear(); }
    SetChange(settings.presets[2].profile, "Pass1Network", 1);
    scene.outgoing = scene.incoming;
    scene.incoming = {Normalize({"Rain.esl", 0x801}), Group::Rain};
    for (float t : {0.0f, .25f, .5f, 1.0f}) {
        scene.transition = t;
        auto result = Evaluate(settings, scene, base);
        Require(Near(result.setup.tuning.intensity, 1.2f - .8f * t), "weather blend endpoints and midpoint");
        Require(Near(result.setup.sharpness, .6f - .4f * t), "sharpening follows the same weather progress");
        Require(result.setup.reconstruction.preset == (t < .5f ? 0 : 1), "other settings switch at the transition midpoint");
    }
    scene.outgoing = {}; scene.transition = 0;
    Require(Near(Evaluate(settings, scene, base).setup.sharpness, .2f), "missing outgoing uses incoming");
    auto off = base; off.neural = false; off.sharpening = false;
    SetChange(settings.presets[2].profile, "NeuralRendering", 1);
    SetChange(settings.presets[2].profile, "SharpeningEnabled", 1);
    const auto kept = Evaluate(settings, scene, off).setup;
    Require(!kept.neural && !kept.sharpening, "presets never switch NR or sharpening on");
    SetChange(settings.presets[2].profile, "NeuralRendering", 0);
    Require(!Evaluate(settings, scene, base).setup.neural, "presets can switch NR off");
    scene.hour = std::numeric_limits<float>::quiet_NaN();
    Require(!Evaluate(settings, scene, base).active, "invalid time falls back without NaNs");
    scene.hour = 12; scene.valid = false;
    Require(Evaluate(settings, scene, base).setup == base, "load/main menu fallback");
}
static void TimeAndPersistence()
{
    Profile profile;
    SetChange(profile, "Sharpness", 0);
    SetChange(profile, "Sharpness", 1, 5);
    const auto points = FindChange(profile, "Sharpness")->points;
    Require(Timed(profile) && Near(AtTime(points, DefaultHours, 22), .5f), "dusk blends across midnight");
    Require(AtTime(points, DefaultHours, 24) == points[0], "midnight endpoint wraps");
    Require(AtTime(points, DefaultHours, -2) == AtTime(points, DefaultHours, 22), "negative clock wrap");
    Require(std::abs(AtTime(points, DefaultHours, 23.999f) - AtTime(points, DefaultHours, 0.001f)) < .001f, "midnight continuity");
    Settings settings; settings.enabled = true;
    GroupPreset(settings, 1, profile);
    GroupPreset(settings, 3, Look(1, .4f));
    auto scene = Scene(); scene.hour = 22; scene.outgoing = scene.incoming;
    scene.incoming = {Normalize({"Rain.esp", 0xA10}), Group::Rain}; scene.transition = .5f;
    Require(Near(Evaluate(settings, scene, {}).setup.sharpness, .45f), "time interpolation precedes weather interpolation");
    ExactPreset(settings, Normalize({"WEATHER.ESP", 0x123456}), profile);
    ExactPreset(settings, Normalize({"Rain.esl", 0x801}), Look(.5f, .75f));
    SetChange(settings.presets[1].profile, "Passes", 2);
    SetChange(settings.presets[1].profile, "ReconstructionMethod", 2);
    SetChange(settings.presets[1].profile, "Pass2SameAsPass1", 0);
    SetChange(settings.presets[1].profile, "Pass1InputScale", .5f);
    Saved saved;
    saved.main.SetValue("ForeignFeature", "Keep", "untouched");
    Save(saved, settings);
    std::string encoded;
    Require(saved.main.Save(encoded) >= 0 && saved.files[3].second->Save(encoded) >= 0, "serialize settings and preset files");
    Require(Plain(Load(saved)) == Plain(settings), "look and other settings, all time points and stable plugin IDs round trip");
    Require(saved.files.size() == 4 && saved.files[1].first == "Rain.ini" && !saved.main.GetSection("Appearance.Preset0"),
        "each preset saves to its own file named after it");
    Require(!saved.files[0].second->GetValue("Changes", "Passes", nullptr) && !saved.files[0].second->GetValue("Changes.Day", "Pass1Intensity", nullptr),
        "only changed settings are saved");
    Require(std::string(saved.main.GetValue("ForeignFeature", "Keep", "")) == "untouched", "unrelated INI survives");
    settings.presets[2].weathers.clear();
    Save(saved, settings);
    Require(Plain(Load(saved)) == Plain(settings) && !saved.files[2].second->GetValue("Preset", "Weather0", nullptr),
        "removed weather cannot reappear on reload");
    saved.files[3].second->SetValue("Preset", "Weather0", "rain.esl|801garbage");
    saved.main.SetDoubleValue("Appearance", "DayHour", 30);
    saved.files[0].second->SetDoubleValue("Changes.Day", "Sharpness", -100);
    saved.files[1].second->SetValue("Changes", "Passes", "two");
    auto corrected = Load(saved);
    Require(corrected.presets[3].weathers.empty() && corrected.hours == DefaultHours &&
        FindChange(corrected.presets[0].profile, "Sharpness")->points[3] == 0, "malformed IDs, schedule and strength are sanitized");
    Require(FindChange(corrected.presets[1].profile, "Passes")->points[0] == 1, "malformed settings load their default");
    CSimpleIniA old;
    old.LoadData("[SourceDLSSG]\nNRIntensity=0.75\n");
    Require(LoadSettings(old) == Settings{}, "old installations remain manual");
    settings.presets[3].weathers.push_back(settings.presets[3].weathers.front());
    settings.presets[3].weathers.push_back({"", 0});
    Require(Sanitize(settings).presets[3].weathers.size() == 1, "duplicates and invalid records cannot shadow presets");
    settings.presets[0].profile.changes[0].points[0] = std::numeric_limits<float>::infinity();
    settings.presets[0].profile.changes.push_back({"NotASetting"});
    settings.presets[0].profile.changes.push_back(settings.presets[0].profile.changes[0]);
    const auto clean = Sanitize(settings).presets[0].profile;
    Require(std::isfinite(clean.changes[0].points[0]) && clean.changes.size() == 1, "non-finite, unknown and duplicate changes sanitize");
    RendererSettingsDraft draft, baseline; draft.valid = baseline.valid = true;
    draft.appearance = settings;
    Require(CountRendererSettingsChanges(draft, baseline) == 1, "profile edits participate in Apply/Discard changes");
    draft.appearance.hours[3] = draft.appearance.hours[2];
    Require(ValidateRendererSettings(draft, {true, true, true, false}) != nullptr, "bad UI schedule is rejected visibly");
}
static void FrameHistory()
{
    Settings settings; settings.enabled = true; settings.smoothingSeconds = 1;
    GroupPreset(settings, 1, Look(.3f, .2f));
    GroupPreset(settings, 3, Look(1.7f, .9f));
    Controller controller; controller.Configure(settings);
    SourceDLSSG::NeuralOptions base; base.enabled = true; base.passes = 2;
    base.tuning.skinStructureStrength = -1; base.tuning.style = 4;
    base.secondPass.linked = false;
    const auto manual = base;
    SourceDLSSG::NeuralHistory history;
    auto scene = Scene();
    auto first = base;
    ApplyFrame(controller, scene, first, 1.0f / 60);
    Require(history.ResetFor(first, true, false), "first automated frame resets");
    Require(first.tuning.skinStructureStrength == -1 && first.tuning.style == 4, "sentinel and discrete style preserved");
    scene.outgoing = scene.incoming;
    scene.incoming = {Normalize({"rain.esp", 0x812}), Group::Rain};
    float lastIntensity = first.tuning.intensity;
    for (int i = 0; i < 240; ++i) {
        auto frame = base;
        scene.hour += .0001f; scene.transition = static_cast<float>(i) / 239;
        ApplyFrame(controller, scene, frame, 1.0f / 60);
        Require(!history.ResetFor(frame, true, false), "weather/time ramps do not reset history every frame");
        Require(frame.tuning.intensity >= lastIntensity, "weather smoothing moves continuously toward target");
        lastIntensity = frame.tuning.intensity;
    }
    Require(lastIntensity > 1 && base == manual && controller.Configuration() == settings, "ramp changes frames, never defaults or profiles");
    Require(controller.SelectionResolutions() == 2, "ordinary weather progress resolves and labels presets only on identity changes");
    settings.presets[1].profile = Look(.45f, .15f);
    settings.presets[1].name = "Edited shared rain";
    controller.Configure(settings);
    auto edited = base; ApplyFrame(controller, scene, edited, .016f);
    Require(history.ResetFor(edited, true, false) && Near(edited.tuning.intensity, .45f) &&
        controller.State().result.incoming == "Edited shared rain", "configuration replacement invalidates cached pointers, labels and history");
    auto frame = base;
    ApplyFrame(controller, scene, frame, .016f);
    Require(history.ResetFor(frame, true, true), "camera reset survives automation");
    Require(!history.ResetFor(frame, true, false), "camera reset is not sticky");
    base.tuning.intensity = .8f; frame = base;
    ApplyFrame(controller, scene, frame, .016f);
    Require(history.ResetFor(frame, true, false), "manual edit resets even under an overriding profile");
    base.secondPass.linked = true; frame = base;
    ApplyFrame(controller, scene, frame, .016f);
    Require(history.ResetFor(frame, true, false), "relink resets history");
    Require(frame.EffectiveSecond().tuning == frame.tuning, "linked second pass uses automated first pass");
    controller.Pause(true); frame = base;
    Require(ApplyFrame(controller, scene, frame, .016f) == .4f && frame == base, "manual session override restores base values");
    Require(history.ResetFor(frame, true, false), "leaving automation resets once");
    controller.Pause(false); frame = base;
    ApplyFrame(controller, scene, frame, .016f); history.ResetFor(frame, true, false);
    scene.hour += 5; frame = base; ApplyFrame(controller, scene, frame, .016f);
    Require(history.ResetFor(frame, true, false), "waiting or fast travel breaks history");
    controller.Invalidate(); frame = base; ApplyFrame(controller, scene, frame, .016f);
    Require(history.ResetFor(frame, true, false), "load invalidation resets history");
    scene.valid = false; frame = base;
    ApplyFrame(controller, scene, frame, .016f);
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
static void CombatAppearanceHistory()
{
    // Exercise the production order shared by native and CS producers: appearance
    // modifies the saved frame options, then combat selects the effective passes.
    Settings settings; settings.enabled = true; settings.smoothingSeconds = 1;
    GroupPreset(settings, 1, Look(.3f, .2f));
    GroupPreset(settings, 3, Look(1.7f, .9f));
    for (bool worldOnly : {false, true}) for (bool linked : {false, true}) {
        Controller controller; controller.Configure(settings);
        SourceDLSSG::NeuralOptions base; base.enabled = true; base.passes = 2;
        base.worldOnly = worldOnly; base.secondPass.linked = linked;
        base.combat = {true, true, 5};
        const auto saved = base;
        SourceDLSSG::NeuralHistory history;
        NeuralRendering::CombatPolicy policy;
        auto scene = Scene();
        const NeuralRendering::GameplayState calm{true, false, false, false},
            fighting{true, false, true, false}, armed{true, false, false, true};
        auto update = [&](NeuralRendering::GameplayState state, double seconds, bool reset, bool cameraReset = false) {
            auto frame = base;
            ApplyFrame(controller, scene, frame, 1.0f / 60);
            const auto revision = frame.appearanceRevision;
            frame.passOverride = policy.Update(frame.combat, frame.enabled, frame.passes, state, seconds);
            Require(history.ResetFor(frame, true, cameraReset) == reset,
                "combined history resets only at effective-pass or explicit camera changes");
            Require(base == saved && frame.passes == 2 && frame.secondPass.linked == linked,
                "combined overrides preserve saved allocation and pass-link preferences");
            return std::pair{frame, revision};
        };
        auto [initial, revision] = update(calm, 0, true);
        scene.outgoing = scene.incoming;
        scene.incoming = {Normalize({"rain.esp", 0x812}), Group::Rain};
        scene.transition = 0;
        auto [entry, entryRevision] = update(fighting, 1, true);
        Require(entry.EffectivePasses() == 1 && entryRevision == revision,
            "combat changes pass count without creating an appearance revision");
        float prior = entry.tuning.intensity;
        for (int i = 1; i <= 120; ++i) {
            scene.hour += .0001f; scene.transition = static_cast<float>(i) / 120;
            auto [frame, currentRevision] = update(i < 60 ? fighting : armed, 1 + i / 60.0, false);
            Require(frame.EffectivePasses() == 1 && currentRevision == revision && frame.tuning.intensity >= prior,
                "weather ramp and combat-to-weapons reason changes retain history");
            prior = frame.tuning.intensity;
            if (linked) { Require(frame.EffectiveSecond().tuning == frame.tuning, "automated passes remain linked"); }
        }
        Require(prior > initial.tuning.intensity, "weather tuning continues through combat reduction");
        auto recovery = update(calm, 4, false).first;
        Require(recovery.passOverride == NeuralRendering::PassOverride::Recovery && recovery.EffectivePasses() == 1,
            "recovery reason preserves one-pass history");
        update(calm, 8.9, false);
        Require(update(calm, 9, true).first.EffectivePasses() == 2,
            "second pass returns with reset at the configured recovery deadline");
        update(calm, 10, false);
        update(calm, 11, true, true);
        update(calm, 12, false);
    }
}
static void PresetPasses()
{
    Settings settings;
    FindPreset(settings, AddPreset(settings, "Rain"))->groups[3] = true;
    SetChange(settings.presets[0].profile, "Passes", 1);
    Controller controller; controller.Configure(settings);
    SourceDLSSG::NeuralOptions base; base.enabled = true; base.passes = 2;
    base.reconstruction.producerColor = true;
    auto rain = Scene(); rain.incoming.group = Group::Rain;
    auto clear = Scene();
    SourceDLSSG::NeuralHistory history;
    auto frame = base; ApplyFrame(controller, rain, frame, .016f);
    Require(frame.passes == 2 && frame.presetOnePass && frame.EffectivePasses() == 1,
        "a one-pass preset keeps two passes allocated instead of recreating NR");
    history.ResetFor(frame, true, false);
    frame = base; ApplyFrame(controller, clear, frame, .016f);
    Require(!frame.presetOnePass && frame.EffectivePasses() == 2 && history.ResetFor(frame, true, false),
        "leaving a one-pass preset restores Pass 2 with one history reset");
    SetChange(settings.presets[0].profile, "Passes", 2);
    SetChange(settings.presets[0].profile, "ReconstructionMethod", 2);
    controller.Configure(settings);
    base.passes = 1;
    frame = base; ApplyFrame(controller, clear, frame, .016f);
    Require(frame.passes == 2 && frame.presetOnePass && frame.EffectivePasses() == 1,
        "presets that need two passes keep them allocated everywhere");
    frame = base; ApplyFrame(controller, rain, frame, .016f);
    Require(frame.passes == 2 && !frame.presetOnePass && frame.EffectivePasses() == 2 &&
        frame.reconstruction.method == NeuralRendering::ResolveMethod::Ratio && frame.reconstruction.producerColor,
        "presets change other NR settings but never the adapter's input contract");
    // A preset that gives Pass 2 its own style keeps its other settings following Base's Pass 1.
    Settings styled;
    FindPreset(styled, AddPreset(styled, "Rain"))->groups[3] = true;
    SetChange(styled.presets[0].profile, "Pass2SameAsPass1", 0);
    SetChange(styled.presets[0].profile, "Pass2Style", 7);
    Controller styledController; styledController.Configure(styled);
    SourceDLSSG::NeuralOptions linked; linked.enabled = true; linked.passes = 2;
    linked.tuning.style = 3; linked.tuning.intensity = 1.4f;
    linked.secondPass.tuning.intensity = .2f; // Stale values kept while linked.
    frame = linked; ApplyFrame(styledController, rain, frame, .016f);
    const auto second = frame.EffectiveSecond();
    Require(!frame.secondPass.linked && second.tuning.style == 7 && Near(second.tuning.intensity, 1.4f),
        "unlinking Pass 2 in a preset starts from Base's Pass 1, not stale Pass 2 values");
    frame = linked; ApplyFrame(styledController, clear, frame, .016f);
    Require(frame.secondPass.linked && frame.EffectiveSecond().tuning == frame.tuning, "outside the preset Pass 2 stays linked");
}
static void SharedPresets()
{
    Settings settings; settings.enabled = true;
    const auto shared = AddPreset(settings, "Rain and mist", Look(.8f, .2f));
    FindPreset(settings, shared)->groups[3] = true;
    const std::vector<Record> records{{"Weather.esp", 0xABC}, {"Weather.esp", 0xDEF}, {"Other.esl", 0xABC}};
    Require(AddWeathers(settings, shared, records), "bulk assignment supports shared presets");
    settings.presets[0].profile = Look(1.4f, .7f);
    for (const auto& record : records) {
        auto scene = Scene(); scene.incoming.record = Normalize(record);
        Require(Near(Evaluate(settings, scene, {}).setup.sharpness, .7f), "one shared edit reaches every assigned weather");
    }
    auto scene = Scene(); scene.incoming.group = Group::Rain; scene.incoming.record = {"missing.esp", 42};
    Require(Near(Evaluate(settings, scene, {}).setup.sharpness, .7f), "broad group uses the same shared preset");
    settings.presets[0].profile.enabled = false;
    Require(Evaluate(settings, Scene(), BaseSetup(.5f, .3f)).setup == BaseSetup(.5f, .3f), "disabled shared preset inherits");
    Saved saved; Save(saved, settings);
    Require(Plain(Load(saved)) == Plain(settings), "shared identity and disabled presets round trip");
    Require(!Load(saved).enabled, "presets apply only while one is in use");
    settings.presets[0].profile.enabled = true;
    Require(Sanitize(settings).enabled && !Sanitize(Settings{}).enabled, "an enabled preset turns presets on");
    RemovePreset(settings, shared);
    Require(settings.presets.empty(), "deleting a preset removes its claims with it");
    Save(saved, settings);
    Require(saved.files.empty() && Plain(Load(saved)) == Plain(settings), "removed presets and assignments do not return after restart");
    Settings fresh;
    const auto first = AddPreset(fresh, "My first preset");
    FindPreset(fresh, first)->groups[1] = true;
    Require(FindPreset(fresh, first)->profile.changes.empty() && Sanitize(fresh).enabled &&
        Evaluate(Sanitize(fresh), Scene(), BaseSetup(.5f, .3f)).setup == BaseSetup(.5f, .3f),
        "a new preset starts with no changes, turns presets on and shows Base");

    Settings capacity; const auto id = AddPreset(capacity, "Shared", Look(1, .2f));
    std::vector<Record> nat;
    for (std::uint32_t i = 1; i <= 170; ++i) { nat.push_back({"NAT.esp", i}); }
    Require(AddWeathers(capacity, id, nat), "LoreRim-sized mapping accepted");
    Save(saved, capacity); Require(Plain(Load(saved)) == Plain(capacity), "170 assignments share one persisted preset");
    nat.clear();
    for (std::uint32_t i = 171; i < MaxWeathers; ++i) { nat.push_back({"NAT.esp", i}); }
    Require(AddWeathers(capacity, id, nat), "fill assignment capacity");
    const auto count = WeatherCount(capacity);
    const std::array<Record, 2> tooMany{{{"other.esp", 1}, {"other.esp", 2}}};
    Require(!AddWeathers(capacity, id, tooMany) && WeatherCount(capacity) == count, "bulk capacity failure is atomic");
    const std::array last{Record{"NAT.esp", static_cast<std::uint32_t>(MaxWeathers)}};
    const std::array extra{Record{"extra.esp", 1}};
    Require(AddWeathers(capacity, id, last) && !AddWeathers(capacity, id, extra), "assignment boundary is enforced");
    RemoveWeather(capacity, id, {"NAT.esp", 1});
    Require(WeatherCount(capacity) == MaxWeathers - 1, "remove assignment at capacity");
    const auto entry = CatalogueEntry(0x02000ABC, {Normalize({"Weather.esp", 0xABC}), Group::Rain}, "NAT_Rainstorm");
    Require(entry.search.find("nat_rainstorm") != std::string::npos && entry.search.find("weather.esp") != std::string::npos &&
        entry.search.find("000abc") != std::string::npos && entry.search.find("rain") != std::string::npos, "weather search includes names, owner, ID and classification");
}
// Formats 1 and 2 wrote every look value at each time of day.
static void WriteLegacyProfile(CSimpleIniA& ini, const std::string& section, float intensity, float sharpness)
{
    ini.SetBoolValue(section.c_str(), "Enabled", true);
    for (const auto* time : Times) {
        const auto timeSection = section + "." + time;
        ini.SetDoubleValue(timeSection.c_str(), "Sharpness", sharpness);
        ini.SetDoubleValue(timeSection.c_str(), "Pass1Intensity", intensity);
        ini.SetDoubleValue(timeSection.c_str(), "Pass1LocalTone", intensity / 2);
        ini.SetDoubleValue(timeSection.c_str(), "Pass1LocalStructure", intensity / 4);
        ini.SetDoubleValue(timeSection.c_str(), "Pass2Intensity", intensity / 2);
        ini.SetDoubleValue(timeSection.c_str(), "Pass2LocalTone", intensity / 3);
        ini.SetDoubleValue(timeSection.c_str(), "Pass2LocalStructure", intensity / 5);
    }
}
// Presets in files: list order decides shared weathers, and files map to names.
static void PresetFiles()
{
    Settings settings; settings.enabled = true;
    const auto moody = AddPreset(settings, "Moody rain", Look(.5f, .1f));
    const auto bright = AddPreset(settings, "Bright rain", Look(1.5f, .9f));
    FindPreset(settings, moody)->groups[3] = FindPreset(settings, bright)->groups[3] = true;
    auto rain = Scene(); rain.incoming.group = Group::Rain;
    Require(Near(Evaluate(settings, rain, {}).setup.sharpness, .1f), "the higher preset wins a shared weather type");
    MovePreset(settings, bright, -1);
    Require(settings.presets[0].id == bright && Near(Evaluate(settings, rain, {}).setup.sharpness, .9f), "moving a preset up gives it the weather");
    FindPreset(settings, bright)->profile.enabled = false;
    Require(Near(Evaluate(settings, rain, {}).setup.sharpness, .1f), "an unused higher preset leaves the weather to the next");
    FindPreset(settings, bright)->profile.enabled = true;
    Saved saved; Save(saved, settings);
    std::ranges::reverse(saved.files);
    Require(Load(saved).presets[0].name == "Bright rain", "the saved order survives any file listing order");
    auto pack = std::make_unique<CSimpleIniA>();
    pack->LoadData("[Preset]\nName = Added pack\nUseWhen = Rain|Exterior\nWeatherCount = 1\nWeather0 = Weather.esp|0ABC\n[Changes]\nPass2Style = 7\n");
    saved.files.emplace_back("Added pack.ini", std::move(pack));
    auto renamed = std::make_unique<CSimpleIniA>();
    renamed->LoadData("[Preset]\nName = Something else\nUseWhen = Snow\n");
    saved.files.emplace_back("Renamed copy.ini", std::move(renamed));
    auto stray = std::make_unique<CSimpleIniA>();
    stray->LoadData("[Unrelated]\nKey = 1\n");
    saved.files.emplace_back("notes.ini", std::move(stray));
    const auto withPack = Load(saved);
    const auto& added = withPack.presets[2];
    Require(withPack.presets.size() == 4 && withPack.presets[3].name == "Renamed copy" && withPack.presets[3].groups[4],
        "a preset's name is its file name, not a Name line");
    Require(!saved.files[0].second->GetValue("Preset", "Name", nullptr), "preset files do not store a name");
    Require(withPack.presets.size() == 4 && added.name == "Added pack" && added.groups[0] && added.groups[3] &&
        added.weathers == std::vector<Record>{{"weather.esp", 0xABC}} && FindChange(added.profile, "Pass2Style")->points[0] == 7 &&
        added.file == "Added pack.ini", "preset files from packs load after the saved order; other INI files are ignored");
    auto edited = withPack;
    RemovePreset(edited, added.id);
    Require(edited.removedFiles == std::vector<std::string>{"Added pack.ini"}, "deleting a loaded preset queues its file for removal");
    Settings named;
    AddPreset(named, "Rain"); AddPreset(named, "rain"); AddPreset(named, "Rain?");
    Require(named.presets[1].name == "rain 2" && PresetFileName("Rain?") == "Rain_.ini" && PresetFileName("con") == "con_.ini" &&
        PresetFileName("Dusk. ") == "Dusk.ini", "preset names map to distinct, valid file names");
    named.presets[2].name = "Rain: heavy?";
    Require(Sanitize(named).presets[2].name == "Rain_ heavy_" && !FileNameCharacter('?') && FileNameCharacter('a'),
        "names keep only what a file name allows");
}
static void MigrationAndSchedule()
{
    CSimpleIniA legacy;
    legacy.SetBoolValue("Appearance", "Enabled", true);
    WriteLegacyProfile(legacy, "Appearance.Clear", .8f, .4f);
    legacy.SetLongValue("Appearance", "WeatherCount", 2);
    for (int i = 0; i < 2; ++i) {
        const auto section = std::format("Appearance.Weather{}", i);
        legacy.SetValue(section.c_str(), "Plugin", "Old.esp");
        legacy.SetValue(section.c_str(), "FormID", i ? "801" : "800");
        WriteLegacyProfile(legacy, section, .2f + i, .1f + i * .1f);
    }
    const auto migrated = LoadSettings(legacy);
    Require(migrated.presets.size() == 3 && WeatherCount(migrated) == 2 && migrated.presets[0].groups[1],
        "legacy groups and exact profiles migrate without dropping tuning");
    auto scene = Scene(); scene.incoming.record = {"old.esp", 0x801};
    Require(Near(Evaluate(migrated, scene, {}).setup.tuning.intensity, 1.2f), "migrated exact values take precedence");
    Saved saved;
    std::string legacyText;
    legacy.Save(legacyText);
    saved.main.LoadData(legacyText.c_str());
    Save(saved, migrated);
    Require(Plain(Load(saved)) == Plain(migrated) && saved.files.size() == 3 && !saved.main.GetSection("Appearance.Weather0.Day") &&
        !saved.main.GetSection("Appearance.Clear") && saved.main.GetLongValue("Appearance", "Format", 0) == 4,
        "migration moves presets into files and removes the old sections");
    CSimpleIniA shared;
    shared.SetLongValue("Appearance", "Format", 2);
    shared.SetLongValue("Appearance", "PresetCount", 1);
    shared.SetLongValue("Appearance.Preset0", "ID", 1);
    shared.SetValue("Appearance.Preset0", "Name", "Rain");
    WriteLegacyProfile(shared, "Appearance.Preset0", 1.5f, .5f);
    shared.SetBoolValue("Appearance.Preset0", "NR", false);
    shared.SetLongValue("Appearance.Rain", "Preset", 1);
    shared.SetLongValue("Appearance.Snow", "Preset", 99999);
    shared.SetLongValue("Appearance", "WeatherCount", 1);
    shared.SetValue("Appearance.Weather0", "Plugin", "Old.esp");
    shared.SetValue("Appearance.Weather0", "FormID", "800");
    shared.SetLongValue("Appearance.Weather0", "Preset", 99999);
    const auto format2 = LoadSettings(shared);
    Require(!format2.presets[0].groups[4] && WeatherCount(format2) == 0, "dangling references cannot retarget new presets");
    const auto base = BaseSetup(1.5f, .5f);
    Require(LoadSettings(shared, {}, &base).presets[0].profile.changes.empty(), "older values equal to Base are not kept as changes");
    Settings full;
    const auto kept = AddPreset(full, "Full", Look(1, .3f));
    Saved pinned; Save(pinned, full);
    Require(Load(pinned).presets[0].profile == FindPreset(full, kept)->profile, "preset files keep values equal to Base");
    Require(format2.presets[0].groups[3] && format2.presets.size() == 1 && format2.presets[0].profile.changes.size() == 1 &&
        FindChange(format2.presets[0].profile, "Sharpness")->points[3] == .5f, "format 2 presets keep only the channels they changed");
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
    const auto preset = AddPreset(settings, "Heavy rain", Look(.8f, .4f));
    std::vector<Record> records;
    for (std::uint32_t i = 1; i <= MaxWeathers; ++i) { records.push_back({"weather.esp", i}); }
    AddWeathers(settings, preset, records);
    auto scene = Scene(); scene.incoming.record.localID = static_cast<std::uint32_t>(MaxWeathers);
    std::array<double, 7> timings{};
    for (int automatic = 0; automatic < 2; ++automatic) {
        settings.presets.front().profile.enabled = automatic != 0;
        for (auto& timing : timings) {
            Controller controller; controller.Configure(settings);
            SourceDLSSG::NeuralOptions base;
            auto warmup = base; ApplyFrame(controller, scene, warmup, .016f, .3f);
            const auto start = std::chrono::steady_clock::now();
            float sum{};
            for (int i = 0; i < 100000; ++i) { auto frame = base; sum += ApplyFrame(controller, scene, frame, .016f, .3f); }
            timing = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / 100000;
            Require(sum > 0 && controller.SelectionResolutions() == static_cast<std::uint64_t>(automatic), "benchmark observes stable cached resolution");
        }
        std::ranges::sort(timings);
        std::printf("Controller benchmark: %s, 4096 assignments, median %.1f ns/frame (7 x 100000; standalone CPU only)\n", automatic ? "automatic" : "manual", timings[3]);
    }
}
int main(int argc, char** argv)
{
    Resolution(); TimeAndPersistence(); FrameHistory(); CombatAppearanceHistory(); PresetPasses(); SharedPresets(); PresetFiles(); MigrationAndSchedule();
    if (argc == 2 && std::string(argv[1]) == "--benchmark") { Benchmark(); }
    std::puts("PASS: weather/time resolution, per-setting presets, persistence, frame smoothing, manual overrides and NR history");
}
