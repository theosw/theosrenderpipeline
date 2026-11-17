#include "FrameGen/RuntimeStateDiagnostics.h"
#include "../third_party/amd-nr/trp_diagnostics.h"
#include <cstdio>
#include <cstdlib>
#include <string>
static void Require(bool value, const char* why)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
int main()
{
    using namespace TheosRenderPipeline::SourceDLSSG;
    Require(NRReason(false, true, false, false, false, true, false) == NRDiagnosticReason::Off, "off request remains distinct from retained failure");
    Require(NRReason(true, false, true, true, true, false, false) == NRDiagnosticReason::ModelLoading, "loading host stage is not running inference");
    Require(NRReason(true, false, false, false, false, true, false) == NRDiagnosticReason::Failed, "failure is visible even when preparation stops");
    Require(NRReason(true, true, true, true, false, false, false) == NRDiagnosticReason::Unavailable, "unavailable runtime is distinguished from missing world input");
    Require(NRReason(true, false, true, false, false, false, false) == NRDiagnosticReason::WorldInputs, "enabled with missing inputs has a refusal reason");
    Require(std::string(TRPAmdDiagnostics::Session()).starts_with(std::to_string(GetCurrentProcessId()) + "-"), "session exists before runtime initialization");
    Require(std::string(TRPAmdDiagnostics::Session()) == TRPAmdDiagnostics::Session(), "session is stable");
    Require(!AMDOutputActive(true, 0, false), "loaded worker with zero compositions is still initializing");
    Require(AMDOutputActive(true, 1, false), "first composed output establishes active NR");
    Require(!AMDOutputActive(true, 100, true), "retained compositions do not hide a failure");
    Require(!AMDOutputActive(false, 100, false), "retained compositions do not imply a running worker");
    StateLogLimiter<NRDiagnosticState> nr;
    NRDiagnosticState state;
    Require(nr.ShouldLog(0, state), "initial off state is visible");
    Require(!nr.ShouldLog(1, state) && !nr.ShouldLog(9999, state), "off frames do not flood log");
    Require(nr.ShouldLog(10000, state), "off state has heartbeat");
    state.enabled = true; state.reason = NRDiagnosticReason::WorldInputs;
    Require(nr.ShouldLog(10250, state), "enabling with missing inputs is observable while inactive");
    state.eligible = true; state.hostStageEvaluated = true; state.modelState = 1; state.reason = NRDiagnosticReason::ModelLoading;
    Require(nr.ShouldLog(10500, state) && !state.modelRunning, "host evaluation does not imply model running");
    state.modelRunning = true; state.modelState = 2; state.reason = NRDiagnosticReason::Running;
    Require(!nr.ShouldLog(10501, state), "fast transitions are bounded");
    Require(nr.ShouldLog(10750, state), "pending transition appears at next allowed sample");
    state = {};
    Require(nr.ShouldLog(11000, state), "disable is visible after model ran");
    state.enabled = true; state.failed = true; state.reason = NRDiagnosticReason::Failed;
    Require(nr.ShouldLog(11250, state) && nr.ShouldLog(21250, state), "failed state transition and heartbeat");
    StateLogLimiter<FGDiagnosticState> fg;
    FGDiagnosticState gate{true, false, false, true, 3, true};
    Require(fg.ShouldLog(0, gate) && !fg.ShouldLog(1, gate), "FG gate is rate limited");
    gate.transitionBlocked = false;
    Require(fg.ShouldLog(250, gate), "opening transition gate still shows warmup");
    gate.warmup = 0; gate.prepared = gate.allowed = true;
    Require(fg.ShouldLog(500, gate), "FG prepared and allowed become observable");
    gate.compatible = gate.allowed = false;
    Require(fg.ShouldLog(750, gate) && fg.ShouldLog(10750, gate), "incompatible input refusal persists in heartbeat");
    std::puts("Runtime state diagnostics passed");
}
