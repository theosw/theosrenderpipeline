# Startup hook compatibility

The renderer now accepts the six-byte indirect GetClientRect call when an earlier
plugin has replaced its pointer slot with an executable external target. This
covers SSE Display Tweaks BorderlessUpscale while retaining the named engine
import, instruction-form, readable-slot and executable-target checks. The existing
call installer preserves the predecessor. Rejected sites log observed bytes,
resolved targets and module ownership before refusing startup.

Entry hooks also preserve the destination of an existing E9 or FF25 jump. The
pinned detour library copies displaced instructions without relocating relative
addresses, so executing its copied jump can fault. Native interface/inventory
hooks and the CS postprocessing detour use the shared preservation helper.
Unmodified entries retain the original trampoline path. Malformed targets and
self-hooks are refused.

HookChains executes unmodified, E9 and FF25 entries through the production helper
with full-width arguments and return values. It also patches and calls a replaced
GetClientRect-style slot through the same CommonLib six-byte call mechanism used
by the renderer. HookSafety retains its named-IAT, repeat-installation and
malformed-memory checks. Both pass in the initial focused Release run.

These are process-local fixtures. They do not establish startup or gameplay with
an installed Display Tweaks/CS combination. A combined candidate still needs a
user-requested game test before merging. This branch also carries the previously
local 0.2.4 hardening baseline: configured-path persistence, versioned hook
preflight and repeat-safe device/import hooks.
