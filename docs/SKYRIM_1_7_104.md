# Steam Skyrim 1.7.104

Both Full and Standard include an experimental Steam 1.7.104 profile alongside
1.5.97, 1.6.640 and 1.6.1170. Use matching SKSE64 2.3.1 and Address Library v13
for 1.7.104. See the [README](../README.md) for compatibility and
[build instructions](BUILD.md) for dependencies and optional tests.

The port uses CommonLibSSE-NG 8.1.0 for the format-5 Address Library and updated
engine accessors. Its profile supplies the changed graphics/frame-counter
layout, five hook offsets and loading-artwork callers. The render-extent hook
preserves the engine's separate window-resize query and UI projection fields.
Exact hook instructions and relevant layouts were checked against the target
Steam executable.

Full retains Neural Rendering and the Ada MFG unlock. Standard retains native
NVIDIA capabilities. Settings and public companion APIs are unchanged. The
updated dependency and decoder notices are in [THIRD-PARTY.md](../THIRD-PARTY.md).

Full and Standard builds pass. Each edition passes five offline checks covering
runtime admission, artwork caller isolation, graphics/input layouts and linked
Address Library loading. These checks do not execute game hooks or establish
visual/input acceptance. The compatibility candidate received scoped Full and
Standard regression checks on 1.6.1170. Skyrim 1.5.97, 1.6.640 and 1.7.104 remain
experimental and untested in-game; the final combined 0.1.3 build has not had a
separate game run.
