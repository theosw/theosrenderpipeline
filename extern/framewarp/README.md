# FrameWarp subset for the native TRP experiment

Source: [xPoiler/Reshade-Asynchronous-Reprojection](https://github.com/xPoiler/Reshade-Asynchronous-Reprojection),
FrameWarp 1.9.1, commit `0cd62c1b8c59519435fee8fab355da89c056a370` (2026-10-08).
Upstream code is GPL-3.0; see [LICENSE](LICENSE). This subset retains that license.
TRP's modding exceptions do not grant additional permissions over upstream code.
The XPAR engine and camera/input model are upstream work, not a TRP invention.

Only the presenter, pure camera algorithms, and their CPU tests are included.
No ReShade add-on, hook/producer implementation, Vulkan dependencies, NVIDIA
Latewarp DLL, or NGX runtime is bundled. TRP supplies its own native producer.

Local changes:

- Build through TRP's opt-in CMake option and name the helper `TRPFrameWarpPresenter`.
- Use `Local\TRPFrameWarp_*` shared objects and native protocol version 55.
- Add an atomic producer heartbeat; hide the window after 500 ms without a source
  or one second without the helper render thread's heartbeat.
- Allow first-frame rendering while the window stays hidden until its image and
  output heartbeat are ready; visibility and rendering eligibility are separate.
- Keep logs/calibration/captures under `%LOCALAPPDATA%/TheosRenderPipeline/FrameWarp/<producer-pid>`.
- Replace Latewarp initialization/evaluation with an own-engine-only adapter.
- Request an unprivileged high-priority queue, falling back to normal.
- Check helper GPU submissions/list operations and all retirement waits; exit on
  failure without returning GPU-owned Reading slots to the producer.
- Report camera/motion disagreement as a native-bridge suspension, not an
  estimated-camera fallback.
- Keep the estimator's 1.5 ms CPU budget as an explicit `FW_CPU_BUDGET_TEST=1`
  benchmark; ordinary CPU tests assert numerical correctness. Native TRP camera
  inputs do not use the estimated-camera path.
- Split long HLSL strings into adjacent literals for MSVC; shader text is unchanged.

The native integration is described in [FRAMEWARP-EXPERIMENT.md](../../FRAMEWARP-EXPERIMENT.md).
