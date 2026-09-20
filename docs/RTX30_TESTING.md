# RTX 30 testing for 0.2.2

RTX 30 support remains experimental. An RTX 3060 Laptop volunteer using
Community Shaders has successfully run frame generation and NR with the 0.2.2
compatibility changes; retained logs record x2/x3/x4 outputs. Later feedback
reports grass-edge artifacting and occasional one-to-two-second hitches.
Physical frame pacing, broader hardware coverage and RTX 30 DLAA/NR remain
unverified. The separate DLAA/NR correction was tested locally on ENB/RTX 4080 SUPER.

Install matching Standard first, then Universal after it in MO2 so Universal
wins `TheosRenderPipeline.dll`. The NVIDIA runtime files can come from Standard.
Keep `Experimental/SourceDLSSGMFGUnlock=true` in the effective INI, including
any copy in Overwrite. A missing key now uses that packaged default. An explicit
false remains an opt-out; Universal explains the incompatible configuration on
RTX 30 instead of reaching a generic vendor adapter rejection. The NVIDIA host
is still required with interpolation off, so Standard alone is not an RTX 30
DLSS/NR fallback. With Community Shaders, disable its frame generation and Reflex.

For a first test on your setup, keep NR off. Confirm x2, then x4 in the same scene;
check a menu/loading transition and turning frame generation off and back on.
Then test NR separately, before and after upscaling. Keep the startup log from
each run and report GPU, driver, game version, shading setup and visible issues.
NR is expensive on the tested laptop GPU. Start with one pass before upscaling
and reduce NR input resolution if needed. Keep CS HDR off; HDR is not supported.

The log now records edition, source revision, loaded renderer path, INI key
presence/value, physical rendering adapter, selected compatibility route, and
startup stages. On RTX 30 Universal with compatibility enabled, expect
`selected route=Ampere compatibilityRequested=true` before provider preparation
and `slInit`. A Native route does not exercise the Ampere implementation.

Offline checks cover missing-key migration, explicit opt-outs, adapter routes,
NGX wrapper isolation, failed prerequisites and repeated feature-create calls.
They use vendor doubles for architecture exposure and recreation: they do not
execute Ampere GPU programs, destroy real NGX features or measure presentation.
No cached Streamline support override is added; Windows, driver and HAGS checks
remain authoritative. Existing NR, Ada MFG and native NVIDIA features are retained.
