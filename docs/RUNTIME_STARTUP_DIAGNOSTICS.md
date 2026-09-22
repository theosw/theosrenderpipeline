# NVIDIA runtime startup diagnostics

Universal logs `[SourceDLSSG Modules]` records when checking the loaded DLSS-G
wrapper and provider for Ada/Ampere compatibility. Include these records and
the surrounding startup log when reporting a failure. These are startup-only
observations; they do not change runtime selection or enable unsupported files.

For each configured `sl.dlss_g.dll` and `nvngx_dlssg.dll`, the records distinguish:

- `fileAccessible`: Windows could query the configured disk path. `fileWin32`
  records the query error when it could not. File accessibility does not prove
  the module is loaded or that its bytes match a release package.
- `retained`: the existing configured-path lookup found and retained a loaded
  module. A file can be accessible while this is false.
- `reported` / `normalized` / `configuredMatch`: the loaded module's reported
  filename, its normalized path, and the existing path comparison result. MO2
  may expose virtual and physical spellings; different spellings alone do not
  establish a mismatch.
- `diskVersion`: version metadata read through the reported path, with
  `versionWin32` when unavailable. This identifies the file's metadata, not a
  hash of loaded code; a missing version resource is not a compatibility failure.
- `same-name-candidate`: when lookup failed, the diagnostic lists loaded modules
  with the same basename. These modules are never selected as fallback runtimes.
  Enumeration failure or nonzero `retainFailures` / `pathFailures` means the
  diagnostic candidate list may be incomplete.

On the Ada route, each existing export requirement also records `present`,
`expectedPresent`, `passed` and its immediate Win32 lookup error. The provider's
DirectSR export is expected to be absent; an absent export is not always an
error. If the configured module was not retained, exports are explicitly marked
`not-checked`, rather than reported as missing. Ampere only logs module identity
here; its existing bridge and publication checks remain authoritative.

The original admission, patch verification and terminal failure behavior remain
unchanged. Diagnostic collection is best effort, preserves the caller's Win32
last-error value, and neither loads DLLs nor keeps extra references alive. A
successful diagnostic record does not establish successful frame generation or
gameplay. The offline module-path tests cover direct/virtual paths, both load
orders, missing/foreign modules, required/forbidden exports and reference lifetime.
