#include "NeuralCombatPolicy.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace TheosRenderPipeline::NeuralRendering;
static void Require(bool value, const char* why)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
int main()
{
    const GameplayState calm{true, false, false, false}, combat{true, false, true, false},
        armed{true, false, false, true}, both{true, false, true, true};
    CombatPolicy policy;
    CombatSettings settings{true, true, 5};
    auto update = [&](GameplayState state, double time) { return policy.Update(settings, true, 2, state, time); };
    Require(update(calm, 0) == PassOverride::None, "no delay before the first trigger");
    Require(update(combat, 1) == PassOverride::Combat, "immediate combat entry");
    Require(update(armed, 2) == PassOverride::WeaponsDrawn, "other selected trigger keeps one pass");
    Require(update(both, 3) == PassOverride::Combat, "combat takes display precedence");
    Require(update(calm, 10) == PassOverride::Recovery, "delay starts at first quiet observation");
    Require(update(calm, 14.99) == PassOverride::Recovery, "no early restoration");
    Require(update(calm, 15) == PassOverride::None, "restore at delay boundary");
    Require(update(combat, 16) == PassOverride::Combat, "reentry");
    Require(update(calm, 17) == PassOverride::Recovery, "new recovery");
    Require(update(armed, 20) == PassOverride::WeaponsDrawn, "retrigger cancels recovery");
    Require(update(calm, 21) == PassOverride::Recovery, "retrigger starts fresh delay");
    Require(update(calm, 25) == PassOverride::Recovery, "old deadline not used");
    auto paused = calm; paused.paused = true;
    Require(update(paused, 100) == PassOverride::Recovery, "paused interval does not expire delay");
    Require(update(calm, 101) == PassOverride::Recovery, "resume excludes paused elapsed time");
    Require(update(calm, 102) == PassOverride::None, "remaining unpaused second restores");

    settings.weaponsDrawn = false;
    Require(update(armed, 103) == PassOverride::None, "unselected weapon trigger ignored");
    Require(update(combat, 104) == PassOverride::Combat, "combat-only option");
    settings = {false, true, 5};
    Require(update(combat, 105) == PassOverride::None, "disabling active trigger clears override");
    Require(update(armed, 106) == PassOverride::WeaponsDrawn, "weapon-only option");
    Require(update({}, 107) == PassOverride::None, "loading/title/missing player clears latch");
    Require(update(calm, 108) == PassOverride::None, "new world has no old cooldown");
    settings = {true, true, 0};
    Require(update(combat, 109) == PassOverride::Combat && update(calm, 110) == PassOverride::None,
        "zero delay restores immediately");
    for (int passes : {1, 2}) for (bool enabled : {false, true}) {
        policy.Reset();
        Require((policy.Update(settings, enabled, passes, both, 0) != PassOverride::None) == (enabled && passes == 2),
            "NR off and configured one pass are unaffected");
    }
    Require(policy.Update({}, true, 2, both, 1) == PassOverride::None, "defaults are opt-in");
    Require(SanitizeCombatSettings({true, true, -1}).recoverySeconds == 0, "negative delay clamped");
    Require(SanitizeCombatSettings({true, true, 100}).recoverySeconds == 30, "large delay clamped");
    Require(SanitizeCombatSettings({true, true, std::numeric_limits<float>::quiet_NaN()}).recoverySeconds == 5,
        "invalid delay defaults");
    std::puts("PASS: combat/weapon selection, recovery/reentry, paused time, lifecycle, disabled modes and sanitization");
}
