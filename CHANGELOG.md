# 0.2.2

- Enabled Neural Rendering with DLAA, before or after anti-aliasing.
- Improved experimental RTX 30-series compatibility in Universal, tested with
  Community Shaders on an RTX 3060 Laptop.
- Fixed the compatibility default when upgrading older configurations with a
  missing key, while respecting explicit opt-outs.
- Improved startup diagnostics for renderer edition, settings, GPU and
  initialization failures.

RTX 30 requires Universal; Standard can supply its NVIDIA runtimes. RTX 30
support remains experimental, with artifacting and intermittent hitches reported.
HDR remains unsupported. Existing features, packaged defaults and runtimes are
retained; the NR optimization experiments are not included.
