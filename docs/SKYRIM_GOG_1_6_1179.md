# GOG Skyrim 1.6.1179 port

Status: preparation and address-table audit. **GOG is not yet supported or
admitted by this branch.** Its exact executable is required before adding a
runtime profile. No GOG hook instructions, engine layouts or game behavior
have been verified.

The target is GOG `1.6.1179.0` with the GOG-specific SKSE64 `2.2.6` build listed
on the [official SKSE page](https://skse.silverlock.org/) on September 16, 2026.
Older GOG 1.6.659 requires a separate executable audit and is outside this PR.

This PR builds on [the Steam 1.6.640 profile work in PR #3](https://github.com/theosw/theosrenderpipeline/pull/3).
Its base is `compat/skyrim-1.6.640`; retarget it to `main` after that dependency
merges. The intended result is a fourth exact runtime profile in each existing
Full and Standard DLL. Their current feature split, settings, plugin identity,
public companion APIs and original call chaining remain part of the scope.

## Available evidence

The local `versionlib-1-6-1179-0.bin` has format 2, header version
`1.6.1179.0`, image name `SkyrimSE.exe` and 428,510 entries. SHA-256:
`3EE46B2F3A8A24B9CDA1F2AA63B0F0F47DEA347E52701A6739327E5EA1B5838E`.
All 18 locations in the current port inventory have nonzero address entries
(16 distinct IDs). The target executable was not found in the inspected local
game folders or GOG installation registry keys.

These are **function/data base RVAs from Address Library**, not verified hook
instruction addresses:

| Location | AE ID | GOG base RVA |
| --- | --- | --- |
| DRS call owner | 36555 | `0x645E60` |
| Cursor bounds owner | 51498 | `0x915F30` |
| Screen size owner | 77397 | `0xE4E710` |
| Engine dimensions owner | 106583 | `0x14B3E30` |
| Mist background owner | 52727 | `0x974060` |
| World completion / interface detour | 82084 | `0xFA6A00` |
| D3D initialization owner | 77226 | `0xE43E00` |
| GetClientRect / jitter call owner | 77245 | `0xE45FC0` |
| Jitter patch owner | 77518 | `0xE5A510` |
| Camera patch owner | 77520 | `0xE5A680` |
| Render world owner | 36559 | `0x646710` |
| Inventory 3D detour | 51755 | `0x9295A0` |
| Native text capture | 68552 | `0xCD73D0` |
| TAA owner pointer | 414660 | `0x332BA70` |
| Graphics state | 411479 | `0x328E020` |
| Artwork sender | 13363 | `0x1A0F80` |

The artwork caller function, ID 40438, begins at `0x732720`. Its queued
exterior/interior return RVAs must be recovered from this executable; the Steam
caller pairs are not a GOG profile.

The current Steam AE GetClientRect hook adds `0x18B` to ID 77245. Historical
source has a newer-GOG `0x1DC` hint. Neither is verified for this target. Inspect
the imported call and argument setup, along with the nearby jitter call, before
selecting a GOG offset. Review the complete hook inventory rather than assuming
that this is the only difference.

## Implementation and acceptance

- [x] Select the exact GOG/SKSE target and isolate its branch.
- [x] Check matching Address Library identity, format and 18-location coverage.
- [ ] Identify the user-supplied GOG executable by version, hash and distribution.
- [ ] Inspect all hook boundaries, patch bytes, original callees and call arguments.
- [ ] Verify graphics/camera/DRS fields and native input accessors.
- [ ] Recover and verify the two queued loading-artwork callers.
- [ ] Add the verified exact profile and any required per-runtime hook offsets.
- [ ] Build Full and Standard, test all four runtime selections and inspect final DLL Query/metadata/exports.
- [ ] Assemble experimental packages and perform separately requested game acceptance.

Only port documentation changes in this preparation commit. Existing runtime
admission remains 1.5.97, 1.6.640 and 1.6.1170. The parent PR's build/test results
are evidence for that implementation, not GOG acceptance; no renderer rebuild
was required for this address-only audit.

Game acceptance must cover native UI/input and End closure, DLSS, artwork,
inventory/spell previews, transitions and resource reconstruction. Full must
retain NR and x4-or-higher MFG coverage. Different GOG builds of third-party
ImGui producers need separate checks. No deployment or game launch is included
in this preparation, and game binaries/Address Library files stay outside Git.
