#pragma once

#include <algorithm>
#include <cmath>

namespace TheosRenderPipeline::NeuralRendering
{
    struct CombatSettings
    {
        bool inCombat{false};
        bool weaponsDrawn{false};
        float recoverySeconds{5.0f};
        bool Enabled() const { return inCombat || weaponsDrawn; }
        bool operator==(const CombatSettings&) const = default;
    };

    inline CombatSettings SanitizeCombatSettings(CombatSettings value)
    {
        value.recoverySeconds = std::isfinite(value.recoverySeconds) ?
            std::clamp(value.recoverySeconds, 0.0f, 30.0f) : 5.0f;
        return value;
    }

    enum class PassOverride { None, Combat, WeaponsDrawn, Recovery };

    inline const char* PassOverrideName(PassOverride value)
    {
        switch (value) {
        case PassOverride::Combat: return "combat";
        case PassOverride::WeaponsDrawn: return "weapons/spells drawn";
        case PassOverride::Recovery: return "recovery delay";
        default: return "none";
        }
    }

    struct GameplayState
    {
        bool available{}, paused{}, inCombat{}, weaponsDrawn{};
    };

    // A temporary execution policy. It never edits the requested pass count or tuning.
    class CombatPolicy
    {
    public:
        PassOverride Update(CombatSettings settings, bool enabled, int passes, GameplayState state, double now)
        {
            settings = SanitizeCombatSettings(settings);
            if (!enabled || passes != 2 || !settings.Enabled() || !state.available) {
                Reset();
                return PassOverride::None;
            }
            if (settings != settings_) { Reset(); settings_ = settings; }
            const double elapsed = hasTime_ && !paused_ && !state.paused ? (std::max)(0.0, now - lastTime_) : 0.0;
            lastTime_ = now;
            hasTime_ = true;
            paused_ = state.paused;
            if (settings.inCombat && state.inCombat) {
                reason_ = PassOverride::Combat;
                quietSeconds_ = 0;
            } else if (settings.weaponsDrawn && state.weaponsDrawn) {
                reason_ = PassOverride::WeaponsDrawn;
                quietSeconds_ = 0;
            } else if (reason_ != PassOverride::None) {
                // Start counting on the first quiet observation, never from the last combat frame.
                if (reason_ == PassOverride::Recovery) { quietSeconds_ += elapsed; }
                reason_ = !state.paused && quietSeconds_ >= settings.recoverySeconds ?
                    PassOverride::None : PassOverride::Recovery;
            }
            return reason_;
        }
        void Reset() { *this = {}; }
    private:
        CombatSettings settings_{};
        PassOverride reason_{PassOverride::None};
        double lastTime_{}, quietSeconds_{};
        bool hasTime_{}, paused_{};
    };
}
