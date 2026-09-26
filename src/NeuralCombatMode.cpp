#include <PCH.h>
#include "NeuralCombatMode.h"
#include <chrono>
#include <mutex>

namespace TheosRenderPipeline::NeuralRendering
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        struct Sample
        {
            std::mutex mutex;
            GameplayState state{};
            Clock::time_point time{};
            double gameplaySeconds{};
            bool pending{}, pauseObserved{};
        };
        Sample sample;
        CombatPolicy policy; // Owned by the active producer's render thread.
    }

    void ApplyCombatMode(SourceDLSSG::NeuralOptions& options, bool worldEligible)
    {
        options.passOverride = PassOverride::None;
        if (!worldEligible || !options.enabled || options.passes != 2 || !options.combat.Enabled()) {
            policy.Reset();
            std::scoped_lock lock(sample.mutex);
            sample.state = {};
            return;
        }
        GameplayState state;
        double gameplaySeconds{};
        bool queue{};
        auto* tasks = SKSE::GetTaskInterface();
        auto* ui = RE::UI::GetSingleton();
        const bool paused = ui && ui->GameIsPaused();
        {
            std::scoped_lock lock(sample.mutex);
            // Rendering can continue while a pause menu stops main-thread tasks.
            // Remember even a short pause until the next task consumes the interval.
            sample.pauseObserved |= paused;
            state = sample.state;
            state.paused |= paused;
            gameplaySeconds = sample.gameplaySeconds;
            queue = tasks && !sample.pending;
            if (queue) { sample.pending = true; }
        }
        if (queue) {
            tasks->AddTask([] {
                GameplayState next;
                auto* ui = RE::UI::GetSingleton();
                auto* player = RE::PlayerCharacter::GetSingleton();
                next.available = ui && player && player->Is3DLoaded() &&
                    !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
                if (next.available) {
                    next.paused = ui->GameIsPaused();
                    next.inCombat = player->IsInCombat();
                    // Include drawing and sheathing so the override begins before an attack.
                    next.weaponsDrawn = player->AsActorState()->GetWeaponState() != RE::WEAPON_STATE::kSheathed;
                }
                std::scoped_lock lock(sample.mutex);
                const auto now = Clock::now();
                const double elapsed = std::chrono::duration<double>(now - sample.time).count();
                // Some paused menus stop main-thread tasks. Conservatively exclude
                // unsampled gaps as well as explicitly paused time from recovery.
                if (next.available && sample.state.available && !next.paused && !sample.state.paused &&
                    !sample.pauseObserved && elapsed >= 0.0 && elapsed < 1.0) {
                    sample.gameplaySeconds += elapsed;
                }
                sample.pauseObserved = next.paused;
                sample.state = next;
                sample.time = now;
                sample.pending = false;
            });
        }
        options.passOverride = policy.Update(options.combat, options.enabled, options.passes, state, gameplaySeconds);
    }
}
