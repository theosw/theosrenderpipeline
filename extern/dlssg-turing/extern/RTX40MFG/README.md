# RTX40MFG temporal patch

Upstream: https://github.com/dashdogy/RTX40MFG-Unlock
Base commit: `4ab7b5e16941e065f81c665b6d7fe2c2e2ec843f`
Copyright 2026 Michael Robles. MIT license, included alongside these files.

Named temporal profile and capability-patch updates:
e13a9841733b0ae43b7215e8c51fe0eb3897816f (v1.3.3).

The temporal transform and provider identity helpers are adapted from upstream.
The legacy and named slot-9 programs are selected by their descriptor layout
and the specific payload being transformed, independently of DLL version.
Whole-DLL hashes and provider-version allowlists are not runtime admission rules.
Source-program and transformed-program checks remain around the mutation.
The adapter-query portion of midpoint_fix.cpp and its header are adapted to
distinguish an identified non-Ada NVIDIA adapter from a failed query, while
retaining the original boolean observation API. Theo's Render Pipeline uses
these helpers without the Cyberpunk ASI loader or UI.
Integration and capability patching live in SourceDLSSGMFG.cpp and
SourceDLSSGMFGPatch.h. The provider branch uses an aligned two-byte
compare-exchange; module and published-payload lifetimes remain retained.
No NVIDIA DLL or CUDA kernel payload is included here.
The temporal payload is derived from the user's verified loaded NVIDIA module.
