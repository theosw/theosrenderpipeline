# ReShade startup compatibility

This candidate addresses an `input reuse fence failed HRESULT=0x80070057`
startup failure when a graphics wrapper exposes a different device identity
from the underlying device that owns its fences.

TRP obtains the underlying identity from a fence it created itself. Incoming
completion fences must belong to either that device or the original host
interface. Foreign devices are still rejected. Queue waits, the D3D11 bridge,
resource retirement and partial-startup failure handling remain intact. Failure
logs now distinguish identity validation from individual wait/signal operations.
No private ReShade interface or modified ReShade DLL is required by this fix.

## Validation

`TRPSourceDLSSGInteropFenceTests` exercises the production interop implementation.
It requires a hardware adapter supporting D3D11/D3D12, shared fences, and the
Windows WARP adapter. It checks same-device fences at zero and nonzero values,
rejects fences from the separate WARP device before enqueueing work, observes a
pending input blocking later queue progress, and retires the completed work.
It also checks invalid input and recording-state rejection. Checks remain active
in Release builds.

For a ReShade comparison, copy the built test executable and an identified
64-bit ReShade DLL named `dxgi.dll` into a separate empty test directory. Run:

```powershell
.\TRPSourceDLSSGInteropFenceTests.exe --require-wrapped
```

The flag requires different host/fence-owner identities, so a missing or
non-intercepting proxy cannot silently pass as the wrapped regression case.
Run the original executable without that flag and without a local proxy for
the ordinary comparison. Keep ReShade.log and the DLL version/hash with results.
Do not place test files in the game directory. No ReShade DLL is included in Git.

The initial standalone comparison passed with ReShade 6.3.3.1921 on RTX 4080
SUPER; the previous identity check failed on the same ReShade binary. The test
creates no swapchain, loads no NVIDIA inference runtimes, and does not run Skyrim.

## Remaining scope

Game startup and visual/input acceptance remain pending. This correction does
not provide explicit ReShade effect placement before/after upscaling, depth
binding, or coordinated HUD-less/FG color processing. Successful startup alone
does not establish those behaviors, compatibility with every ReShade build,
NR appearance, MFG presentation cadence, or ReShade overlay input handling.

A requested game check should identify the package, ReShade and NVIDIA runtime
versions; retain the current ENB/CS and NR/MFG feature scope; and separately
observe startup, gameplay, menus, effects, overlay close and a loading transition.
An effects-off toggle leaves ReShade's device hooks loaded and is not a proxy
absence comparison.
