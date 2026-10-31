#include "RendererSettings.h"
#include "FrameGen/NeuralAdmission.h"
#include "RendererSettingsAction.h"
#include <SimpleIni.h>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace TheosRenderPipeline;
namespace {
void Require(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
void PascalAdmission()
{
    using namespace SourceDLSSG;
    RendererSettingsDraft current; current.valid = true; current.upscaleType = XeSS;
    current.sourceDLSSG.neuralEnabled = true;
    current.sourceDLSSG.neuralPasses = 3;
    RendererSettingsCapabilities capabilities{true, true, true, false};
    capabilities.nvidiaFeatures = false;
    for (const auto* reason : {NeuralRendering::kPascalEditionReason, NeuralRendering::kPascalDriverReason,
            NeuralRendering::kPascalAssetReason, NeuralRendering::kPascalBudgetReason}) {
        capabilities.neuralAdmissionReason = reason;
        Require(NeuralSettingsUnavailable(XeSS, capabilities) == reason, "menu and Apply use the same admission reason");
        Require(!CanEditNeuralEnabled(false, NeuralSettingsUnavailable(XeSS, capabilities) == nullptr), "unavailable NR Enable row is greyed");
        Require(!ValidateRendererSettings(current, capabilities, &current), "unchanged unavailable preference allows unrelated save");
        CSimpleIniA ini;
        StorePreferences(ini, current.sourceDLSSG);
        const auto saved = LoadPreferences(ini);
        Require(saved.neuralEnabled && saved.neuralPasses == 3, "unavailable NR preference survives save/reload");
        auto draft = current; draft.sourceDLSSG.neuralPasses = 4;
        Require(ValidateRendererSettings(draft, capabilities, &current) == reason, "unavailable pass edits refuse with capability reason");
        draft = current; draft.sourceDLSSG.neuralEnabled = false;
        Require(!ValidateRendererSettings(draft, capabilities, &current), "turning unavailable NR off still applies");
    }
    capabilities.neuralAdmissionReason = nullptr;
    current.sourceDLSSG.neuralEnabled = false;
    auto draft = current; draft.sourceDLSSG.neuralEnabled = true;
    Require(!ValidateRendererSettings(draft, capabilities, &current), "Enable applies independently of NVIDIA FG availability");
    Require(draft.sourceDLSSG.neuralPasses == current.sourceDLSSG.neuralPasses &&
        draft.sourceDLSSG.provider == current.sourceDLSSG.provider &&
        draft.sourceDLSSG.neuralBeforeUpscaling == current.sourceDLSSG.neuralBeforeUpscaling &&
        draft.sourceDLSSG.neuralReconstruction.modelRate == current.sourceDLSSG.neuralReconstruction.modelRate,
        "NR enable preserves passes, placement, provider and model rate");
}
void Feedback()
{
    for (const bool save : {false, true}) {
        std::vector<std::string> log;
        const auto result = RejectSettingsAction(save, "Dynamic target must exceed 60 FPS.",
            [&](const std::string& message) { log.push_back(message); });
        Require(result.error && !result.applied, "rejected action keeps the draft unapplied");
        Require(log.size() == 1 && log[0].find(save ? "Save as default rejected" : "Apply rejected") != std::string::npos &&
            log[0].find(result.message) != std::string::npos, "log records action and rejection reason");
        const auto status = SettingsStatus(4, result.message, result.error);
        Require(status.kind == SettingsStatusKind::Error && status.text == result.message,
            "rejection remains visible with pending edits");
    }
    Require(SettingsStatus(2, "Settings saved.", false).kind == SettingsStatusKind::Pending, "new edits supersede old success");
    Require(SettingsStatus(0, "Settings saved.", false).kind == SettingsStatusKind::Success, "successful save remains visible");
    Require(SettingsStatus(0, "", false).kind == SettingsStatusKind::Neutral, "idle footer");
}
void Generation()
{
    using namespace SourceDLSSG;
    {
        CSimpleIniA ini;
        Require(LoadPreferences(ini).vsync==VSyncMode::Auto,"existing settings default to Auto VSync");
        for (auto mode : {VSyncMode::Auto,VSyncMode::Off,VSyncMode::On}) {
            Preferences preferences;preferences.vsync=mode;StorePreferences(ini,preferences);
            Require(LoadPreferences(ini).vsync==mode,"VSync Save/restart round trip");
        }
        ini.SetLongValue("FrameGeneration","VSync",99);
        Require(LoadPreferences(ini).vsync==VSyncMode::Auto,"invalid saved VSync recovers to Auto");
        RendererSettingsDraft current;current.valid=true;
        auto draft=current;draft.sourceDLSSG.vsync=VSyncMode::Off;
        Require(CountRendererSettingsChanges(draft,current)==1,"VSync participates in Apply/Discard");
    }
    for (const unsigned count : {1u, 3u, 5u}) for (const bool dynamic : {false, true}) {
        for (const unsigned target : {0u, 1u, 60u, 61u, 165u, 1000u, 1001u}) {
            GenerationRequest request{count, target, dynamic};
            Require(ValidGenerationRequest(request) == (!dynamic || target == 0 || (target >= 61 && target <= 1000)),
                "only active dynamic targets block validation");
            Preferences preferences; preferences.generation = request;
            CSimpleIniA ini; StorePreferences(ini, preferences);
            ini.SetLongValue("SourceDLSSG", "DynamicTargetFPS", target); // Also cover manually edited invalid INIs.
            const auto loaded = LoadPreferences(ini).generation;
            Require(loaded.generatedFrames == count && loaded.dynamic == dynamic, "invalid target cannot discard multiplier or dynamic selection");
            Require(loaded.dynamicTargetFPS == (ValidDynamicTarget(target) ? target : 0), "invalid target recovers to display refresh");
            Require(SelectGeneration(loaded, 5, true).effective.dynamicTargetFPS == (dynamic ? loaded.dynamicTargetFPS : 0),
                "fixed mode never submits a hidden target to runtime");
        }
    }
    Require(!ValidGenerationRequest({0,0,false}) && !ValidGenerationRequest({6,0,false}), "bad multipliers remain invalid");
    {
        using TheosRenderPipeline::SourceDLSSG::DlssgPreset;
        CSimpleIniA ini;
        Require(LoadPreferences(ini).preset == DlssgPreset::B, "new INIs default to Preset B, the old UI recomposition default");
        ini.SetBoolValue("SourceDLSSG", "UIRecomposition", false);
        Require(LoadPreferences(ini).preset == DlssgPreset::A, "a saved UI recomposition opt-out becomes Preset A");
        Preferences preferences; preferences.preset = DlssgPreset::C;
        StorePreferences(ini, preferences);
        Require(LoadPreferences(ini).preset == DlssgPreset::C, "Preset C round trips");
        Require(!ini.GetBoolValue("SourceDLSSG", "UIRecomposition", true), "older readers see Preset C as no UI recomposition");
        ini.SetLongValue("SourceDLSSG", "FrameGenerationPreset", 9);
        Require(LoadPreferences(ini).preset == DlssgPreset::B, "an invalid preset recovers to B");
        RendererSettingsDraft current; current.valid = true;
        auto draft = current; draft.sourceDLSSG.preset = DlssgPreset::C;
        Require(CountRendererSettingsChanges(draft, current) == 1, "the preset participates in Apply/Discard");
    }
    {
        CSimpleIniA ini;
        Require(LoadPreferences(ini).xefgFrameTime, "existing INIs without the key send XeFG the frame time");
        Preferences preferences; preferences.xefgFrameTime = false;
        StorePreferences(ini, preferences);
        Require(!LoadPreferences(ini).xefgFrameTime, "a saved XeFG frame-time opt-out round trips");
        Require(!LoadPreferences(ini).xefgTensorCores, "existing INIs leave the optional tensor backend off");
        preferences.xefgTensorCores = true;
        StorePreferences(ini, preferences);
        Require(LoadPreferences(ini).xefgTensorCores, "the explicit tensor opt-in survives save and restart");
        RendererSettingsDraft current; current.valid = true;
        auto draft = current; draft.sourceDLSSG.xefgFrameTime = false;
        Require(CountRendererSettingsChanges(draft, current) == 1, "XeFG frame time participates in Apply/Discard");
    }
    {
        using TheosRenderPipeline::XeFGDefaultSceneChangeThreshold;
        CSimpleIniA ini;
        Require(LoadPreferences(ini).xefgSceneChangeThreshold == XeFGDefaultSceneChangeThreshold,
            "existing INIs without the key keep Intel's 0.7");
        Preferences preferences; preferences.xefgSceneChangeThreshold = 0.35f;
        StorePreferences(ini, preferences);
        Require(LoadPreferences(ini).xefgSceneChangeThreshold == 0.35f, "a saved scene-change threshold round trips");
        const std::pair<const char*, float> cases[]{{"3", 1.0f}, {"-1", 0.0f}, {"nan", XeFGDefaultSceneChangeThreshold},
            {"0.333", 0.33f}, {"garbage", XeFGDefaultSceneChangeThreshold}};
        for (const auto& [text, expected] : cases) {
            ini.SetValue("FrameGeneration", "XeFGSceneChangeThreshold", text);
            Require(LoadPreferences(ini).xefgSceneChangeThreshold == expected,
                "scene-change thresholds clamp to 0-1 in hundredths; invalid text is Intel's default");
        }
        RendererSettingsDraft current; current.valid = true;
        auto draft = current; draft.sourceDLSSG.xefgSceneChangeThreshold = 0.5f;
        Require(CountRendererSettingsChanges(draft, current) == 1, "the scene-change threshold participates in Apply/Discard");
    }
}
void MenuKey()
{
    RendererSettingsDraft current; current.valid = true;
    Require(current.menuHotkey == 0x23, "draft defaults to End");
    auto draft = current; draft.menuHotkey = 0x79;
    Require(CountRendererSettingsChanges(draft, current) == 1, "menu key participates in Apply/Discard");
    RendererSettingsCapabilities host{true, true, true, false};
    Require(ValidateRendererSettings(draft, host, &current) == nullptr, "picked key validates");
    for (const int invalid : {0, -1, 0xFF}) {
        draft.menuHotkey = invalid;
        Require(ValidateRendererSettings(draft, host, &current) != nullptr, "invalid menu key rejected");
    }
}
void Neural()
{
    {
        RendererSettingsDraft draft; draft.valid = true; draft.upscaleType = XeSS;
        RendererSettingsCapabilities amd{true, true, true, false}; amd.nvidiaAdapter = false;
        Require(!ValidateRendererSettings(draft, amd), "XeSS settings can apply without NVIDIA hardware");
        draft.sourceDLSSG.neuralEnabled = true;
        Require(ValidateRendererSettings(draft, amd), "NVIDIA NR cannot be enabled on a non-NVIDIA GPU");
        draft.sourceDLSSG.neuralEnabled = false; draft.upscaleType = DLSS;
        Require(ValidateRendererSettings(draft, amd), "unsupported DLSS startup cannot be saved on a non-NVIDIA GPU");
    }
    {
        RendererSettingsDraft draft; draft.valid = true; draft.upscaleType = XeSS;
        draft.sourceDLSSG.neuralEnabled = true;
        RendererSettingsCapabilities pascalHost{true, true, true, false};
        pascalHost.nvidiaFeatures = false;
        Require(!ValidateRendererSettings(draft, pascalHost), "research NR with XeSS applies without DLSS services");
        Require(!NeuralSettingsUnavailable(XeSS, pascalHost), "NR controls stay usable independently of DLSS availability");
        draft.upscaleType = DLSS;
        Require(ValidateRendererSettings(draft, pascalHost), "unavailable DLSS cannot apply on research GTX host");
    }
    RendererSettingsDraft current; current.valid = true; current.sourceDLSSG.neuralEnabled = true;
    auto draft = current; draft.upscaleType = DLAA; draft.qualityLevel = 4;
    RendererSettingsCapabilities lostUI{true,true,false,false};
    Require(ValidateRendererSettings(draft, lostUI) != nullptr, "new unavailable NR requests rejected");
    Require(ValidateRendererSettings(draft, lostUI, &current) == nullptr, "unchanged NR cannot block unrelated DLAA save");
    Require(NeuralSettingsUnavailable(draft.upscaleType, lostUI) != nullptr, "preserved request cannot enable execution without composition");
    draft.sourceDLSSG.neuralPasses = 2;
    Require(ValidateRendererSettings(draft, lostUI, &current) != nullptr, "cannot reconfigure enabled NR after capability loss");
    auto combatDraft = current;
    combatDraft.sourceDLSSG.neuralCombat.inCombat = true;
    Require(CountRendererSettingsChanges(combatDraft, current) == 1, "combat edit participates in Apply/Discard pending changes");
    Require(!SameNeuralPreferences(combatDraft.sourceDLSSG, current.sourceDLSSG) &&
        ValidateRendererSettings(combatDraft, lostUI, &current), "combat edits obey NR availability validation");
    draft.sourceDLSSG.neuralEnabled = false;
    Require(ValidateRendererSettings(draft, lostUI, &current) == nullptr, "turn NR off despite unavailable composition");
    for (const bool enabled : {false,true}) for (const bool available : {false,true}) {
        Require(CanEditNeuralEnabled(enabled, available) == (enabled || available), "off action available, unavailable activation disabled");
    }
    RendererSettingsCapabilities failed{true,true,true,false,false};
    draft = current;
    Require(!ValidateRendererSettings(draft, failed, &current) && NeuralSettingsUnavailable(draft.upscaleType, failed),
        "unchanged NR request can persist but cannot resume after runtime failure");
    auto off = current; off.sourceDLSSG.neuralEnabled = false;
    Require(ValidateRendererSettings(draft, failed, &off), "off-to-on request after failure still rejected");
    failed.sourceHost = false;
    Require(ValidateRendererSettings(draft, failed, &current), "unavailable source host still rejected");
}
}
int main()
{
    try { PascalAdmission(); Feedback(); Generation(); MenuKey(); Neural(); std::cout << "PASS: visible/logged rejection, generation round trips, menu key and NR capability loss\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
