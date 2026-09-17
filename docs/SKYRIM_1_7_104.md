# Steam Skyrim 1.7.104 port

This draft starts the Steam 1.7.104 port on `compat/skyrim-1.7.104`, based on
the [1.6.640 PR](https://github.com/theosw/theosrenderpipeline/pull/3).
**1.7.104 remains unsupported and rejected by the plugin.** The renderer still
admits exactly 1.5.97, 1.6.640 and 1.6.1170. The separate GOG draft is not a
dependency of this work.

## Target files

The target is Steam `SkyrimSE.exe` **1.7.104.0**, paired with
[SKSE64 2.3.1](https://skse.silverlock.org/) and
[Address Library All in One v13](https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files).
The executable and its address table are pending local acquisition. No target
executable hash, verified instruction RVAs or game acceptance is claimed yet.

For an owned Steam copy, the August 27, 2026
[executable manifest](https://steamdb.info/depot/489833/manifests/) is:

```text
download_depot 489830 489833 4886117324142477814
```

This downloads the executable depot into Steam's content directory. It does
not prepare a complete 1.7.104 test installation. Retain any previous target
executable before reusing that depot directory. Download Address Library v13
separately and extract `versionlib-1-7-104-0.bin` into ignored research storage;
neither file belongs in Git or the current mod installation.

## Work started

`tools/audit_address_library.py` reads formats 1, 2 and 5, records the file hash
and header version, and checks the existing renderer's 18 function/data
locations. Family selection is explicit; a 1.7 table must use `--family ae`.
It rejects malformed/truncated tables and a version mismatch, and treats dense
zero entries as missing IDs. It does not load a game or alter the input file.

Run with Python 3.10 or newer, from this checkout:

```powershell
python -m unittest discover -s tests -p test_address_library.py -v
python tools/audit_address_library.py out/research/versionlib-1-7-104-0.bin `
  --expect-version 1.7.104.0 --family ae > out/research/address-coverage.json
```

Exit codes are 0 for complete requested ID coverage, 1 for missing IDs, and 2
for a file/header/argument error. `--id <number>` can replace `--family` to
inspect individual IDs. Reported RVAs identify the containing function/data,
not a verified hook instruction. Even complete coverage leaves hook offsets,
callee contracts, layouts, artwork callers and game behavior unverified.

## Dependency and layout work

The pinned CommonLibSSE-NG 3.6.0 is insufficient: its runtime classifier selects
AE only for minor version 6 and its Address Library loader expects formats 1/2.
The offline tool does **not** add format-5 support to that runtime dependency.
A dependency update or a reviewed compatibility patch is still required.

Pinned upstream references are CommonLibSSE-NG
[`3c0f5a87`](https://github.com/alandtse/CommonLibSSE-NG/tree/3c0f5a87c3b166c9a6712d5c3bd180e9ac5ad0fd)
and SKSE64
[`25b72352`](https://github.com/ianpatt/skse64/tree/25b72352adb6543fa6d0bd3795780672b2e238e0).
Relevant findings to verify against the target executable:

- [Format 5](https://github.com/alandtse/CommonLibSSE-NG/blob/3c0f5a87c3b166c9a6712d5c3bd180e9ac5ad0fd/include/REL/IDDB.h)
  has a 96-byte header: format, four version components, a 64-byte image name,
  pointer size, data-format field and offset count. Its body is a dense array
  of 32-bit RVAs indexed by ID. The reader reports the reserved data-format
  field and follows the pinned implementation's uint32 interpretation.
- [Graphics state](https://github.com/alandtse/CommonLibSSE-NG/blob/3c0f5a87c3b166c9a6712d5c3bd180e9ac5ad0fd/include/RE/S/State.h)
  moves the runtime-data base from `+0x60` to `+0x70` and the frame counter
  from `+0x4C` to `+0x54` starting at 1.7.99. The renderer's custom wrapper
  and direct frame-counter read need independent changes and verification.
- [SKSE metadata](https://github.com/ianpatt/skse64/blob/25b72352adb6543fa6d0bd3795780672b2e238e0/skse64/PluginAPI.h)
  adds an Address Library v5 capability bit. Check the loader's exact-version
  path before changing metadata; this plugin deliberately uses explicit
  compatible runtimes rather than claiming version independence.

Before admitting 1.7.104, identify the executable and table, inspect all hook
instructions and callees, trace graphics/camera/input layouts, identify both
loading-artwork return sites, and cover old/new selection with offline tests.
Then rebuild Full and Standard in separate `out/` directories and verify their
Query/Load/metadata agreement, exports and retained feature scope.

## Verification

Ten Python tests pass, covering synthetic formats 1/2/5, all compressed value
kinds, pointer scaling, dense zero slots, sparse gaps, truncation at every byte,
invalid counts/versions, overflow, duplicates and CLI identity/coverage checks.
The tool also reproduces all **396 recorded location comparisons across 22
local format-1/2 tables**, checking their hashes and header identities. The
tables and raw reports stay outside Git. Format 5 currently has synthetic
coverage only; its real 1.7.104 table is still needed.

Plugin sources, build configuration and runtime dependencies are unchanged, so
this preparation did not rebuild or retest the renderer. Renderer dependency
changes, target instruction inspection, packaging, deployment and game testing
remain pending. Existing rendering/input acceptance does not transfer to 1.7.
