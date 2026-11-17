#pragma once
#include <cstdint>

namespace TheosRenderPipeline::SourceDLSSG
{
    // Render-thread observation only. Rapid state churn is collapsed to at most
    // four records/second. Stable off/loading/blocked states still get a heartbeat.
    template <class State> class StateLogLimiter
    {
    public:
        bool ShouldLog(std::uint64_t now, const State& state)
        {
            if (!logged_ || (state != previous_ && now - last_ >= 250) || now - last_ >= 10000) {
                logged_ = true; last_ = now; previous_ = state; return true;
            }
            return false;
        }
    private:
        State previous_{};
        std::uint64_t last_{};
        bool logged_{};
    };
    // A worker accepting captures is not evidence of a completed NR image.
    constexpr bool AMDOutputActive(bool acceptsCaptures, std::uint64_t composed, bool failed)
    {
        return acceptsCaptures && composed != 0 && !failed;
    }
    enum class NRDiagnosticReason { Off, Unavailable, NotPrepared, WorldInputs, StageNotEvaluated, Failed, ModelLoading, Running };
    constexpr NRDiagnosticReason NRReason(bool enabled, bool unavailable, bool prepared,
        bool worldEligible, bool evaluated, bool failed, bool passActive)
    {
        return !enabled ? NRDiagnosticReason::Off : failed ? NRDiagnosticReason::Failed :
            unavailable ? NRDiagnosticReason::Unavailable : !prepared ? NRDiagnosticReason::NotPrepared :
            !worldEligible ? NRDiagnosticReason::WorldInputs : !evaluated ? NRDiagnosticReason::StageNotEvaluated :
            !passActive ? NRDiagnosticReason::ModelLoading : NRDiagnosticReason::Running;
    }
    inline const char* NRReasonName(NRDiagnosticReason reason)
    {
        switch (reason) {
        case NRDiagnosticReason::Off: return "request-off";
        case NRDiagnosticReason::Unavailable: return "runtime-or-route-unavailable";
        case NRDiagnosticReason::NotPrepared: return "frame-not-prepared";
        case NRDiagnosticReason::WorldInputs: return "world-inputs-camera-or-transition";
        case NRDiagnosticReason::StageNotEvaluated: return "world-stage-not-evaluated";
        case NRDiagnosticReason::Failed: return "model-failed";
        case NRDiagnosticReason::ModelLoading: return "model-loading";
        case NRDiagnosticReason::Running: return "running";
        }
        return "unknown";
    }
    struct NRDiagnosticState
    {
        bool enabled{}, eligible{}, hostStageEvaluated{}, modelRunning{}, failed{}, wait{};
        int modelState{-1};
        NRDiagnosticReason reason{NRDiagnosticReason::Off};
        bool operator==(const NRDiagnosticState&) const = default;
    };
    struct FGDiagnosticState
    {
        bool requested{}, allowed{}, prepared{}, transitionBlocked{};
        unsigned warmup{};
        bool compatible{true};
        bool operator==(const FGDiagnosticState&) const = default;
    };
    struct HostFGDiagnosticState
    {
        bool requested{}, allowed{}, upscaled{}, cameraValid{}, prepared{}, transitionBlocked{};
        int warmup{};
        bool operator==(const HostFGDiagnosticState&) const = default;
    };
}
