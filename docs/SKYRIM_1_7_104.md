# Steam Skyrim 1.7.104 port

This experimental port admits Steam **1.7.104.0** alongside 1.5.97, 1.6.640
and 1.6.1170 in Full and Standard. It is based on the
[1.6.640 PR](https://github.com/theosw/theosrenderpipeline/pull/3), not the GOG draft.
**The new DLLs have not been tested in-game on any runtime.** Static engine
inspection and offline checks establish the port's implementation, not gameplay,
input-close or physical-cadence acceptance.

## Identified inputs

The target uses matching [SKSE64 2.3.1](https://skse.silverlock.org/) and
[Address Library All in One (1.7.104.0) v13](https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files).
The user supplied both target files; they remain outside Git and were not installed.

| Input | Identity | SHA-256 |
| --- | --- | --- |
| Steam SkyrimSE.exe | 1.7.104.0, 37,910,440 bytes | `846EFCCF0C1374D71F892907F46549560F2FCB0A75CB87A3EED438BAA0F1402F` |
| Address Library archive | v13 ZIP | `BE0C7C07FB63FD1C1403690D4797C7A8FDF8799118ACCEB605F285D7B0EE055F` |
| versionlib-1-7-104-0.bin | format 5, pointer size 8, 565,759 dense entries | `8AAB3DD251D135B849BD983F86A4A205C920FA3E81F8E30C0E63CCFEF9423842` |

Steam executable manifest: `4886117324142477814` in depot `489833` of app `489830`.
This executable-only depot does not prepare a complete game installation.

## Engine changes

All 18 inventoried function/data locations resolve in the real v13 table.
Disassembly verifies the hook instructions, relevant callees and layouts against
the identified executable. Five offsets differ from the 1.6 AE profile:

| Site | AE Address Library ID | 1.7.104 offset | Instruction RVA |
| --- | --- | --- | --- |
| Loading mist background | 52727 | `+0x7B9` | `0x98A179` |
| Renderer GetClientRect | 77245 | `+0x1DC` | `0x1009F0C` |
| Update jitter call | 77245 | `+0x133` | `0x1009E63` |
| Camera jitter branch | 77520 | `+0x1D6` | `0x101E6B6` |
| Render world call | 36559 | `+0x85E` | `0x6576BE` |

The renderer GetClientRect hook selects the second call, used for render extent.
The new first call at `+0x40` checks window resizing and keeps its native path.
The loading-artwork sender retains its entry contract; eligible exterior/interior
return RVAs are `0x743590` and `0x7435C9`. Other callers remain excluded.

Graphics runtime data moves to `State+0x70`, camera cache to `+0xB8`, DRS ratios
to `+0x114/+0x118` and frame count to `+0x54`. The actual counter increment is at
RVA `0x100A02B`. Camera entries retain stride `0x290` and UseJitter at `+0x284`.
World jitter remains at `+0x44/+0x48`; the new engine UI projection fields at
`+0x4C/+0x50` are preserved. Their appearance needs game verification.

Exact profiles now own the changed offsets and graphics accessors. Query, Load
and exported metadata admit only the four listed versions; adjacent versions,
GOG and VR remain rejected. No blanket Address Library independence is claimed.

## Dependency migration

The build now requires CommonLibSSE-NG **8.1.0**, pinned to
[`3c0f5a87c3b166c9a6712d5c3bd180e9ac5ad0fd`](https://github.com/alandtse/CommonLibSSE-NG/tree/3c0f5a87c3b166c9a6712d5c3bd180e9ac5ad0fd).
It classifies 1.7 as AE, loads format 5 and supplies the versioned renderer/control
accessors. SE and AE remain compiled together, with VR disabled. The project
retains upstream patch-boundary diagnostics and the corresponding HDE64 notices.
See [build instructions](BUILD.md) and [third-party terms](../THIRD-PARTY.md).

Renderer singleton/depth access and SKSE declarations follow the updated APIs.
Control toggles explicitly pass `storeState=true`, preserving the previous
wrapper's stored-mask policy through the verified engine function on all four
runtimes. Enabled/stored/text fields are `+0x118/+0x11C/+0x120` on 1.5.97/1.6.640
and `+0x120/+0x124/+0x128` on 1.6.1170/1.7.104. This is not an input-close fix
or a substitute for an overlay-close game test.

## Offline verification

Full and Standard both build successfully in independent directories under `out/`. Both retain the public
plugin identity and companion exports; Full retains NR and the Ada MFG unlock.
No vendor runtime was removed or installed as part of this port.

All five public CTests pass in each configuration: exact runtime/caller policy
and four isolated layout/loader cases. It exercises the linked CommonLib loader
with formats 1/2/5, verifies version rejection, DRS/camera/frame access and the
updated renderer/control accessors. A private fixture additionally checks all
four real Address Library tables against recorded engine locations; all four cases pass. These
fixtures do not execute engine hooks.

The final DLL checks pass Query for 30 runtime/editor combinations per
edition and inspect exact metadata, unchanged exports, feature markers and
absence of private source paths. Query does not invoke plugin Load.

All ten Python audit tests pass and cover formats 1/2/5, sparse encodings, dense holes,
malformed/truncated inputs and CLI identity/coverage. The preparation also
reproduced 396 recorded location comparisons across 22 legacy tables. To run:

```powershell
python -m unittest discover -s tests -p test_address_library.py -v
python tools/audit_address_library.py out/research/versionlib-1-7-104-0.bin `
  --expect-version 1.7.104.0 --family ae > out/research/address-coverage.json
```

The audit reports containing function/data RVAs, not verified patch instructions.
Exit status is 0 for requested coverage, 1 for missing IDs and 2 for input errors.
Private executable/table copies and disassembly reports stay outside Git.

No package, deployment or game launch is included. Game acceptance remains
pending for startup, world rendering, native UI/previews, overlay close, loading
artwork, NR and MFG. Earlier 1.6.1170 release evidence does not transfer to these
new DLLs or establish acceptance on 1.7.104.
