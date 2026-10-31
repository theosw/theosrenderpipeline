#pragma once
#include <string>
#include <utility>

namespace TheosRenderPipeline::NeuralRendering
{
    enum class RecoveryState { Available, Enabled, Refused, SessionDisabled, RestartRequired };

    // Render-thread state. Publish a copy under the backend's snapshot mutex.
    // Neither a preference reload nor a weather/frame update starts an attempt.
    class Recovery
    {
    public:
        RecoveryState state{RecoveryState::Available};
        std::string reason;
        bool Disabled() const { return state == RecoveryState::SessionDisabled || state == RecoveryState::RestartRequired; }
        bool Retryable() const { return state == RecoveryState::SessionDisabled; }
        void Enabled() { if (!Disabled()) { state = RecoveryState::Enabled; } }
        bool Retry()
        {
            if (!Retryable()) { return false; }
            state = RecoveryState::Available;
            reason.clear();
            return true;
        }
        // discard must cancel ALL unsubmitted commands before any release. A
        // false retirement/ownership proof is terminal, retaining the old owner.
        template<class Discard, class Retire, class Safe, class Release>
        bool Refuse(std::string why, Discard discard, Retire retire, Safe safe, Release release)
        {
            if (Disabled()) { return state == RecoveryState::SessionDisabled; }
            state = RecoveryState::Refused;
            reason = std::move(why);
            if (!discard() || !retire() || !safe()) { state = RecoveryState::RestartRequired; return false; }
            // A failed teardown leaves a quarantined owner. Presentation is
            // independent after retirement, but another NR owner is forbidden.
            state = release() ? RecoveryState::SessionDisabled : RecoveryState::RestartRequired;
            return true;
        }
    };
}
