# RTX 30 testing for 0.2.2

RTX 30 execution remains experimental and unverified on hardware. The 0.2.2
candidate corrects a missing compatibility key and improves startup diagnosis;
it does not establish successful frame generation or NR on RTX 30.

Install matching Standard first, then Universal after it in MO2 so Universal
wins `TheosRenderPipeline.dll`. The NVIDIA runtime files can come from Standard.
Keep `Experimental/SourceDLSSGMFGUnlock=true` in the effective INI, including
any copy in Overwrite. A missing key now uses that packaged default. An explicit
false remains an opt-out; Universal explains the incompatible configuration on
RTX 30 instead of reaching a generic vendor adapter rejection. The NVIDIA host
is still required with interpolation off, so Standard alone is not an RTX 30
DLSS/NR fallback. With Community Shaders, disable its frame generation and Reflex.

For the first hardware run, keep NR off. Confirm x2, then x4 in the same scene;
check a menu/loading transition and turning frame generation off and back on.
Then test NR separately, before and after upscaling. Keep the startup log from
each run and report GPU, driver, game version, shading setup and visible issues.

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
