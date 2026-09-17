# Steam Skyrim 1.6.640 port

Status: work in progress. The plugin still accepts only Skyrim 1.5.97 and
1.6.1170. This branch is not a 1.6.640-compatible release.

The intended result is one Full DLL and one Standard DLL, each supporting
Steam 1.6.640 alongside the existing two runtimes. Their existing feature split,
settings, plugin identity and public companion interfaces remain intact.

The first implementation step is to select loading-artwork callers by exact
runtime and keep plugin admission and exported compatibility metadata in sync.
The existing SE/AE family selection cannot distinguish 1.6.640 artwork callers
from 1.6.1170 callers.

## Work remaining

- [ ] Centralize exact runtime admission and loading-artwork profiles.
- [ ] Verify the existing two runtime profiles and rejection of unverified versions.
- [ ] Inspect an identified Steam 1.6.640 executable with its matching Address Library.
- [ ] Verify hook instructions, original-call contracts, graphics/input layouts and artwork callers.
- [ ] Add the verified 1.6.640 profile and update Query, Load and version metadata together.
- [ ] Build Full and Standard and verify their final DLL compatibility metadata.
- [ ] Assemble experimental packages and complete separately requested game acceptance.

The 1.6.640 Address Library contains the 18 hook/data locations in the current
port inventory. Address presence does not validate instructions or calling
conventions. Exact executable verification is required before admitting the new
runtime. Older third-party ImGui producer DLLs also need their own compatibility
checks.

Game acceptance must retain native UI/input, DLSS, full-edition NR and x4-or-higher
MFG checks. Loading artwork, previews and resource reconstruction also need
coverage. Builds and offline tests do not establish gameplay or physical cadence.
