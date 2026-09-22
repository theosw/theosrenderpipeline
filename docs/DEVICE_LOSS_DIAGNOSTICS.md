# Device-loss diagnostics

A frozen image while the game continues can follow a failed graphics device.
The first terminal source-host error now writes a bounded `[GPUFailure]` report
to `TheosRenderPipeline.log`, then flushes the log. Both Standard and Universal
include this reporting. It does not recover the device or change frame generation,
NR settings, failure results, GPU waits, or resource retirement.

The report includes the original operation/HRESULT, last session frame index,
both D3D11/D3D12 device-removal reasons, and any retained interop failure. The
interop record distinguishes queue waits, fence reads/event waits, allocator/list
resets and submission, with the work type, allocator slot and fence values.
`submitted` is the slot's prior submission; `waitTarget` is the CPU retirement
target when a CPU wait was considered. Availability/`waitPerformed` fields
distinguish unobserved values from zero or a successful wait. Later failures
cannot replace this first record. `sessionFrame` is the latest session snapshot,
not a scanout count or necessarily the currently starting early-NR frame.

`begin NR before DLSS` detects failures before recording the next NR inference.
Its label does not identify the GPU operation that caused device removal. A
healthy device reason is S_OK; an unavailable device is reported explicitly.
Runtime output counts cached after failure do not establish rendering recovery.

## Optional GPU fault capture

For a diagnostic run, create `Data/SKSE/Plugins/TheosRenderPipeline.Diagnostics.ini`
(in an MO2 overlay, use `SKSE/Plugins/TheosRenderPipeline.Diagnostics.ini`):

```ini
[DeviceLoss]
EnableDRED=true
```

Restart Skyrim after changing it. This separate file leaves the normal rendering
INI and saved settings intact. Missing file/key or explicit false means TRP does
not request DRED or change existing process DRED settings. There is no live menu
toggle. The example under `docs/examples/` is suitable for a diagnostic overlay;
normal release packaging should not install that enabled example by default.

DRED must be configured before creating the host D3D12 device. When requested,
TRP enables automatic breadcrumbs and GPU page-fault tracking. Settings affect
subsequently created D3D12 devices in the process and add tracing overhead, so
this is opt-in. No debug-layer installation or debugger attachment is required
by this implementation. Unsupported interfaces/settings are logged and do not
block startup. `configured=true` means the settings calls completed; it is not
proof that any particular fault will provide useful data.

On observed device loss, TRP queries available DRED data even if TRP did not
request tracing (Windows or another component may have enabled it). Logs include
command-list/queue names, completed operation counts, a bounded history window,
available context markers, page-fault address and matching existing/recently
freed allocations. TRP's transport lists name the work index (0 upscaling/NR,
1 frame generation, 2 swapchain) and slot. Traversal, strings and operation windows
are bounded; truncated/missing data and failed queries remain explicit.

Return the entire log after reproducing, before another launch overwrites it.
Keep the same rendering settings for the first diagnostic comparison. Disable
the diagnostic overlay, or set EnableDRED=false and restart, after collecting it.
No NVIDIA runtime replacement is needed. A diagnostic capture is not a fix or
a performance measurement. NVIDIA internals, missing page-fault data and driver
command scheduling can limit exact causal attribution.

References: [device-removal reasons](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getdeviceremovedreason),
[DRED setup and limitations](https://learn.microsoft.com/en-us/windows/win32/direct3d12/use-dred).

## Offline checks

The registered `DeviceLossDiagnostics-contracts`, `DeviceLossDiagnostics-warp`
and `DeviceLossDiagnostics-dred` tests exercise the production reporting and
transport code. The latter two remove only a verified software WARP device,
with tracing respectively unrequested and requested before device creation.
They check real device reasons/DRED query results, retained stage/slot evidence,
original HRESULT, one report, healthy operation and later failure retention.
The fixture records copy work and leaves a copy queued at removal. On the tested
WARP runtime, explicit RemoveDevice returns successful DRED queries with empty
breadcrumb/allocation lists; this does not validate physical-GPU breadcrumb
collection. The log preserves that absence instead of treating it as success
evidence for the faulting operation. Nonempty serialization is covered below.
Synthetic DRED lists cover missing pointers, invalid completion counts, 64K
history wrapping, cyclic lists, allocation lists and string sanitization.
These tests do not reproduce a physical GPU/driver failure or validate Skyrim
gameplay, input, visual output or display cadence.
