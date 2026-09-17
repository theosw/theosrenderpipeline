# Steam Skyrim 1.6.640 port

Status: experimental implementation; Release builds and offline checks pass.
Game acceptance remains pending. This branch adds Steam 1.6.640 to each Full and
Standard DLL alongside 1.5.97 and 1.6.1170.

`SkyrimRuntime.h` provides the exact admitted versions and their loading-artwork
callers. Query, the early Load check and exported SKSE version metadata all use
that table. The artwork detour selects its profile before installation, retaining
the existing sender-prologue check and setting, readiness and message gates.
Unverified nearby versions, GOG and VR remain rejected.

## Executable and engine evidence

The inspected Steam executable is version `1.6.640.0`, from app `489830`, depot
`489833`, manifest `5291801952219815735`. Its SHA-256 is
`EB77D225CDD1832076001281C3C570DD0C95864AC5748701E0CF5652B2A24517`.
The matching `versionlib-1-6-640-0.bin` Address Library SHA-256 is
`4E4BEC0AF54CC99580908A714EA1418C877D634437F97BB208EA07F889850EC7`.
The executable, Address Library and disassembly captures are not distributed in
this repository.

Static inspection covers the 18 locations in the current port inventory:
DRS, cursor bounds, screen and engine dimensions, mist background, world
completion, D3D initialization, GetClientRect, jitter/camera patches, world
rendering, interface and inventory detours, text-input capture, TAA owner,
graphics state and the loading-artwork sender. The existing AE offsets land on
the expected instruction boundaries. Direct calls resolve to the same original
callee IDs as 1.6.1170; the six-byte import call resolves to GetClientRect.
The jitter/camera patch bytes and artwork sender prologue match.

The inspected graphics fields match the existing wrapper: runtime data at
`+0x60`, camera cache at `+0xA8`, camera stride `0x290`, jitter flag `+0x284`,
DRS ratios at `+0x104/+0x108`, jitter at `+0x44/+0x48` and frame count at `+0x4C`.
The native text-input function uses ControlMap `+0x120` on 1.6.640 versus
`+0x128` on 1.6.1170; the renderer already calls the engine function through
Address Library instead of accessing that field directly.

The two queued loading-artwork return addresses differ by runtime:

| Runtime | Exterior return RVA | Interior return RVA |
| --- | --- | --- |
| 1.5.97 | `0x69C9A5` | `0x69C9DD` |
| 1.6.640 | `0x6D6AE0` | `0x6D6B19` |
| 1.6.1170 | `0x7309F0` | `0x730A29` |

The 1.6.640 call sites pass the same four arguments to the verified sender.
Other calls in that function remain outside the artwork override. Treating all
AE versions as 1.6.1170 would miss the new runtime's two eligible transitions.

## Verification

- [x] Identify the executable and matching Address Library.
- [x] Inspect hook instructions, original-call contracts, critical graphics/input fields and artwork callers.
- [x] Centralize exact admission, metadata and artwork profiles.
- [x] Build Full and Standard in Release and run the public profile checks.
- [x] Exercise all three runtime selections with real Address Library files and the production graphics wrapper.
- [x] Verify final DLL Query behavior, compatibility metadata and public exports.
- [ ] Assemble experimental packages and complete separately requested game acceptance.

Set `TRP_BUILD_COMPATIBILITY_TESTS=ON` as described in [BUILD.md](BUILD.md).
The public test checks exact admission/rejection, all artwork gating combinations
and isolation from adjacent and other-runtime callers. Checks remain active in
Release builds.

Both Release `ARPBaseline` builds pass. The public test passes in each edition;
three private runtime-selection fixtures pass with the real Address Library
files and production wrapper. Each final DLL passes 24 Query cases, declares
exactly the three admitted versions and preserves all six public exports.
Binary checks also retain the Full/Standard feature split and exclude private
source paths. Query checks never call `SKSEPlugin_Load` or launch Skyrim.

Game acceptance must cover native UI/input and End-menu closure, DLSS, loading
artwork, inventory/spell previews, cell transitions and resource reconstruction.
Full also needs NR and x4-or-higher MFG checks. Older third-party ImGui producer
DLLs need separate compatibility evidence. Static inspection, builds and offline
checks do not establish gameplay, input-close acceptance or physical cadence.
