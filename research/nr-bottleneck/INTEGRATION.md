# Experimental renderer integration

`NRBottleneckReuse=false` adds a separate **Bottleneck reuse (test)** checkbox to
Neural Rendering. Applying a toggle drains/recreates the feature through the
existing host path. Existing NR placement, passes, tuning, peripheral compression
and combined preparation remain available. No runtime or release version changes.

The integration is limited to the exact NR 310.8 SHA-256 documented in README.
One process-lifetime bridge observes kernel creation through that DLL's resolver
import. It preserves foreign import owners and scopes filtering to a thread-local
evaluation and exact command list. Other calls pass through. Cached callbacks and
their modules remain pinned through process detach; feature plans use ordinary
RAII and never share history. An unavailable bridge leaves ordinary NR available.

Each feature first records a complete validated kernel sequence. Successful
submission on the host's ordered D3D12 queue admits alternate 42-kernel reuse.
This adds no CPU fence wait: command ordering establishes producer/consumer
execution, while the existing interop drain still proves retirement before
teardown. Unsubmitted evaluations cannot become history. Loading/camera/settings
resets require a fresh full evaluation. Two passes own two separate plans.

Before any omission, an unknown sequence disables the optimization and preserves
ordinary NR. A mismatch after omissions rejects the entire unsubmitted list via
the existing terminal NR failure path; it never submits partial work or invents
resource states for a fallback. It is not a guarantee against every internal
GPU dependency: argument scans cannot prove interior-buffer accesses or counters.

## Validation before the local game test

RTX 4080 SUPER, driver 591.86; pinned NR 310.8. CPU fixtures cover alternation,
reset, submission admission, fresh generations, changed names/shapes/bindings,
decoder aliases and recreation on option changes. Release checks remain active.

The integrated feature runs 32 moving 1280x720 frames. Observation is bit-identical
to ordinary NR; reset frames are exact. Reuse averages 2.683 ms of Evaluate GPU
time, versus 3.101 ms observation. RGB mean absolute error is 0.001551 and maximum
0.212646. These are synthetic input errors, not a perceptual quality rating.
Independent-feature and off/on recreation sequences also complete with exact
reset images; the toggle run admits eight reuse frames, the dual run sixteen.

The production NeuralPass harness completes 50 frames each at 5120x1440 with
UNORM8 input. Native NR averages 13.790 ms total and reuse 12.836 ms (about 7%).
A separate lifecycle run changes placement, one/two passes, input scale,
peripheral compression, combined preparation and reuse, with retired recreation.
These results precede the final diagnostics/assertion-only rebuild; its focused
recheck is recorded in the private run evidence.

This is a local test candidate, not release acceptance. Visual detail, motion
stability and physical cadence require gameplay observation. Alternating full
and cheaper evaluations can still feel uneven. Other GPUs and runtime builds
are unverified. Raw inputs, timings, runtime hashes and executable identities
remain under ignored out/runs in the isolated checkout.

Final candidate validation: Universal builds and passes all 53 registered CTests;
the research suite passes both plan fixtures. The final production-pass binary
passes explicit reuse-admission assertions in both 5120x1440 checks and completes
normal NGX shutdown. Standard was not rebuilt for this local Universal-only test.
