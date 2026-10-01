# Extraction validation, 2026-09-23

Canonical source: TRP `89ddf3807dbb03757c49e9a87e5b6fd5e837938f`.
All 37 imported source, test and notice files match their Git blobs at that
commit. The extraction's CMake wrapper, README and source checker are separate.

- Windows x64 / Visual Studio 2022 Release static library and test tools build.
- All 40 registered CTests pass. These use CPU fixtures; no GPU inference or
  frame presentation is performed.
- The exact DLSS-G 310.9.1 provider identified in the README passes the
  production audit for SM75 and SM86: 70 containers plus the temporal program,
  publication and rollback, 158 and 208 transaction writes respectively.
- All 71 final containers for each architecture match the earlier
  volunteer-candidate audit byte for byte.
- CPU emulation of the production-patched provider selects both prepared PTX
  endpoint networks on Turing. Ampere factory bytes remain unchanged. The two
  networks retain their object fields, weights, allocations, other dispatch
  entries and 25/14 kernel launch contracts.

The provider audits load vendor files as data and do not initialize NGX,
Streamline or a graphics device. Their exported vendor programs are private
local test inputs/outputs and are not included in this repository.

The [RTX 2060 volunteer evidence](RTX20_COMPATIBILITY.md) belongs to the earlier
TRP candidate containing this compatibility implementation. It is not an
in-game test of a new application using this extracted library, nor of the
combined TRP 0.3.0 build. Those integration checks remain necessary.
