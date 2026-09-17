# Steam Skyrim 1.6.640

Both Full and Standard include an experimental Steam 1.6.640 profile.
Use SKSE64 and Address Library matching that game version. See the
[README](../README.md) for the current supported-version list and
[build instructions](BUILD.md) for dependencies and optional tests.

The profile keeps loader admission, plugin metadata and loading-artwork caller
selection together. Steam 1.6.640 uses the existing AE graphics layout and hook
offsets, with its own loading-artwork callers. Exact hook instructions and the
relevant graphics/input layouts were checked against the target executable.

Full retains Neural Rendering and the Ada MFG unlock. Standard retains native
NVIDIA capabilities. Settings and public companion APIs are unchanged.

Full and Standard builds and offline profile/artwork checks pass. These checks
do not execute game hooks. The compatibility candidate received scoped Full
and Standard regression checks on 1.6.1170. Skyrim 1.6.640 remains experimental
and untested in-game; the final combined 0.1.3 build has no separate game run.
