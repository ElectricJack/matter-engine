# Task 6 Report: Final Vulkan RTX and DLSS Viewer Gates

## Outcome

Task 6 is complete. The final gates now make the viewer report and verify actual
DLSS mode/extents, Vulkan RT state, history-reset stability, fallback reason,
steady upload/submit behavior, and validation count. Viewer evidence plumbing
and minimal renderer observation state were changed so those gates report
executed behavior; the rendering algorithm itself is unchanged.

Live DLSS is explicitly unavailable on this machine because legal Streamline
artifacts are absent. `STREAMLINE_PATH` is unset and neither the repository root
nor `MatterViewer` contains `sl.interposer.dll`. The verified path is therefore
Native/Native fallback with the runtime reason:

`Streamline SDK not found: build with HAVE_STREAMLINE=1 to enable DLSS`

## TDD Evidence

1. RED: the expanded static checker failed for every absent RTX/DLSS performance
   field and caught the stale `end_frame` evidence signature.
2. GREEN: viewer JSON production and perf assertions were added; the static
   checker passed.
3. RED: viewer smoke rejected resize because DLSS extents were only logged when
   mode/reset changed.
4. GREEN: extent changes now trigger a fresh truthful DLSS report; 960x540 resize
   passed.
5. RED: the first RT toggle case used a test-only device capability variable that
   is unavailable in the production viewer.
6. GREEN: `MATTER_DISABLE_VK_RT` now toggles viewer render options without changing
   device capability or renderer behavior; enabled and disabled cases passed.
7. RED: the performance harness could not start validation because it did not set
   `VK_LAYER_PATH`.
8. GREEN: the harness now discovers, installs, and restores the validation-layer
   environment; the Cornell performance/evidence sample passed.

## Final Gate Evidence

- Adapter: NVIDIA GeForce RTX 4090
- Driver: NVIDIA 610.74 (`0x98928000`)
- Vulkan API: 1.4.341
- Streamline runtime: unavailable; Native fallback verified
- Active mode/extent: selected Native, active Native, 1280x720 -> 1280x720
- Resize evidence: selected Native, active Native, 960x540 -> 960x540
- RT: available=true; enabled=true and enabled=false viewer cases both verified
- Renderer-observed RT: available=true, effective=true, trace dispatches=1,
  fallback reason empty; disabled/unavailable executable cases observe zero
  dispatches and explicit reasons
- Persistent DLSS resets during stable sample: 0
- Static vertex/cluster/stable-instance upload deltas: 0/0/0
- Immediate-submit delta: 0
- Cornell cadence: 180 frames, 60.01 FPS, median 16.66 ms, p95 16.70 ms
- Vulkan validation errors: 0

Commands and results:

```text
make -C MatterViewer windows HAVE_CUDA=1 CUDA_PATH=/c/PROGRA~1/NVIDIA~2/CUDA/v13.3 -j1
PASS (CUDA=1, OptiX=1, Vulkan-only Windows viewer)

make -C MatterViewer vulkan-smoke HAVE_CUDA=1 CUDA_PATH=/c/PROGRA~1/NVIDIA~2/CUDA/v13.3 -j1
PASS (six interop fault modes plus RT enabled/disabled/unavailable; every
process bounded, exit 0, ALL PASS, validation errors 0)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File MatterViewer/tools/check_vulkan_viewer.ps1
PASS

powershell.exe -NoProfile -ExecutionPolicy Bypass -File MatterViewer/tools/smoke_vulkan_viewer.ps1
PASS (five viewer cases, Native fallback, resize, RT toggle, validation errors 0)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File MatterViewer/tools/perf_vulkan_instancing.ps1 -World CornellBox -WarmupSeconds 1 -SampleSeconds 3 -MinimumFps 0
PASS (180 frames; renderer-observed RT JSON evidence above)
```

The requested StressForest50k command reaches the known pre-existing content
failure before warmup/sampling:

`FATAL: render: VkSceneCluster LOD count must be in [1, kVkMaxLod]`

Per user direction, StressForest is excluded from Task 6 demo sign-off. No
StressForest performance number is claimed.

## Whole-Branch Review Package

Review scope: base `41b5a5e` through current Task 6 working tree. The final static
checker binds the following evidence to source or executable coverage:

- Streamline bootstrap precedes Vulkan object initialization.
- Instance/device/swapchain/images/acquire/common-present/queue-present/destroy
  operations remain in the Streamline proxy funnel.
- One common-present and one proxied queue-present site exist, in order.
- Static and rigid-motion velocity tests remain present.
- DLSS evaluation uses an output image distinct from HDR/depth/velocity and
  per-frame output replacement/lifetime tests remain present.
- RT geometry remains pinned and BLAS candidate publication is transactional.
- Production `record_cull_and_render` contains no immediate submission.
- Runtime/performance gates reject mislabeled fallback, invalid Native extents,
  persistent reset, impossible RT state, steady uploads/submits, and validation
  errors.

Independent reviewer dispatch was attempted but the agent thread limit was
already reached. The initial self-review of `git diff 41b5a5e` and the Task 6
working tree did not identify the evidence gaps found by the later independent
review. The required review fix pass is recorded below.

## Review Fix Pass

The subsequent independent review found two Important evidence gaps. Both are
fixed with regression coverage:

1. `make vulkan-smoke` previously launched only the six CUDA/Vulkan interop
   fault modes. Its PowerShell aggregate now also launches `rt`, `rt-disabled`,
   and `rt-unavailable` as separate bounded processes and requires exit 0,
   `ALL PASS`, and `validation errors: 0` from each. The RT executable tests
   assert a real trace dispatch for enabled RT, no dispatch plus the disabled
   reason, and no dispatch plus the forced-unavailable reason.
2. Viewer/performance RT evidence previously serialized requested settings.
   `VkSceneRenderer` now publishes per-frame observed availability, effective
   execution, trace-dispatch count, samples/debug state, and fallback reason
   through `FrameStats`. Runtime and performance gates require effective RT to
   have at least one trace dispatch and no fallback reason; inactive RT must
   have zero dispatches and an explicit reason.

## Re-review Fix Pass

The final re-review found one Important launcher defect and three small evidence
accuracy issues. All are fixed:

1. The aggregate executable smoke no longer uses `Start-Process`, which can
   throw when the inherited Windows environment contains both `Path` and
   `PATH`. It now launches with `ProcessStartInfo` without accessing either
   managed environment dictionary, allowing Windows to pass the raw environment
   block through unchanged. The per-case mode is set on the current process and
   restored in `finally`; redirected streams and the bounded timeout remain. A
   Windows PowerShell 5.1 native-process regression with deliberate raw `Path`
   and `PATH` entries passed all nine modes.
2. Disconnected and missing-store clear-only frames now reset all renderer-
   observed RT statistics through the same per-frame helper used by empty-scene
   frames, with explicit unavailable reasons and zero trace dispatches.
3. `VkSceneRenderer` now resets observed samples/debug state from the current
   settings on every recording call, preventing stale values after a toggle.
4. This report now accurately records the minimal renderer observation changes
   made for Task 6.
