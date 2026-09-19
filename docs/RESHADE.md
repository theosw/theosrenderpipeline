# ReShade compatibility

These fixes let TRP accept its own fences and cached depth shaders when
ReShade exposes a proxy for their device. Unrelated devices remain rejected.

## Setup

Use your existing ReShade installation, preset and settings. TRP does not
include a ReShade DLL. Leave **SSE ReShade Helper disabled**: the tested
configuration lost the graphics device when that helper was enabled.

The fixes have been checked with ReShade 6.3.3.1921 and Skyrim 1.6.1170 using
Cabbage ENB on an RTX 4080 SUPER. Gameplay renders, but this startup-only
candidate still has an unresolved performance issue. Source-frame effects,
depth binding and overlay integration are provided by the separate ReShade
integration change. Other ReShade versions and hardware remain unverified.

## Developer checks

Build `TRPSourceDLSSGInteropFenceTests` and `TRPD3D11FrameCopyTests`. Normal
CTest covers unwrapped devices. For each wrapped check, place its executable
in an ignored test directory beside your ReShade DLL named `dxgi.dll` and run:

```powershell
./TRPSourceDLSSGInteropFenceTests.exe --require-wrapped
./TRPD3D11FrameCopyTests.exe --require-wrapped
```

These checks require a proxy/native identity split and verify foreign-device
rejection, fence waits, repeated depth copies, pixel output and context state.
They do not launch Skyrim. Record the ReShade version with the results.
