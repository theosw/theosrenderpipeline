#include "RendererSettings.h"
#include "RendererSettingsAction.h"
#include <SimpleIni.h>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace TheosRenderPipeline;
namespace {
void Require(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
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
}
void Neural()
{
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
    try { Feedback(); Generation(); Neural(); std::cout << "PASS: visible/logged rejection, generation round trips and NR capability loss\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
