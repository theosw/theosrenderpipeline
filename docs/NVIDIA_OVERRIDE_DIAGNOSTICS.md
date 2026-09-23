# NVIDIA FG override diagnostics

NVIDIA can initialize DLSS frame generation from a cached replacement provider
while TRP still requires the configured nvngx_dlssg.dll. If module retention then
fails, the error now names the NVIDIA App override only when the vendor log
explicitly reported `Feature dlssg override enabled`. It identifies the relevant
per-program model and frame-generation override settings to restore to application
control. Generic app-name messages or another DLSS feature do not establish that
observation.

The failure log includes the configured path, whether a file exists there, any
same-named loaded module path and the observed-override flag. These distinguish
available disk files from actual module ownership. The vendor log retains the
cached provider path/version when NVIDIA supplies it.

No runtime admission or provider patch checks change. The diagnostic does not
claim that every retention failure is an override, and does not enable cached
providers on native, Ada or Ampere hardware. RuntimeModuleDiagnostics verifies
positive/negative vendor messages, concurrent callbacks, existing counters and
failure-message selection. Its focused Release run passes; the reporter's restart
with application-controlled overrides remains separate game evidence.
