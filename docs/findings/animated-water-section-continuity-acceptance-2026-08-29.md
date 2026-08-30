# Animated-water section continuity acceptance — 2026-08-29

Status: **Stage 1 executable gates pass; visual gate fails.**

`handoffVisualGate: fail`

Task 10 and the rest of Stage 2 remain blocked. The retained run proves the
new handoff geometry, fields, cache locality, loop metadata, memory accounting,
and raster-only render contract. The corrected captures prove that all three
animated/direct raster owners are presented, but also expose a persistent
coverage opening between the cyan upstream owner and green handoff strip.

## Result

The previous blocked visual baseline had a `3.871 m` section gap. The final
Stage 1 comparator passes against the unchanged `0.15 / 16 = 0.009375 m`
quantization gate:

| Metric | Worst retained value | Gate |
|---|---:|---:|
| Symmetric Hausdorff | `0.009 m` | `<= 0.009375 m` |
| RMS boundary distance | `0.001 m` | diagnostic |
| Geometry minimum normal dot | `1.0` | `>= 0.995` |
| Unmatched open edges | `0` | `0` |
| Duplicate coplanar triangles | `0` | `0` |
| Field height delta | `0.005 m` | `<= 0.009375 m` |
| Field minimum normal dot | `1.0` | `>= 0.995` |
| Turbulence delta | `0.001` | `<= 0.01` |
| Aeration delta | `0.0` | `<= 0.01` |
| Foam delta | `0.0` | `<= 0.01` |

Both cuts contain all 30 frames, `loopFrame29To0Synchronized=true`, feature
labels are deterministic, and `excludedDamContributors=0`.

The executable summary is
[stage1-summary.json](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/stage1-summary.json).

## Reproducibility boundary

This run exercised the current integration worktree, not base `496d913c` plus
this Task 9 patch in isolation. The runner copied pre-existing accepted but
unstaged RiverFloatLab authoring and provider changes, including the revised
fill-sensor width/surface behavior and removal of the authored visual-particle
cap. Those hunks remain deliberately unstaged under Task 9's dirty-worktree
constraint. The retained evidence is valid for this integration state, but its
cache identities and timings are not expected to be byte-for-byte reproducible
from the Task 9 commit alone until those prerequisite accepted changes land.

## Cold, unchanged-warm, and downstream-edit locality

All three real editor runs reached `Ready`:

| Run | Wall time | Upper | Lower | Dependent handoff |
|---|---:|---|---|---|
| Cold | `460305.262 ms` | static/animation miss, `simulateMs=66132.690` | static/animation miss, `simulateMs=48658.416` | static/animation miss |
| Unchanged warm | `16016.080 ms` | static/animation hit, `simulateMs=0` | static/animation hit, `simulateMs=0` | static/animation hit |
| Fixture-only downstream edit | `276322.582 ms` | static/animation hit, `simulateMs=0` | static/animation miss, `simulateMs=53047.726` | static/animation miss |

The upper animation key/digest remained
`9c480bb9e8e7e092 / cdd133de173671bd`, and its boundary-source key/digest
remained `f46d62ad5b3911b3 / 71d4db5265daed64` across all three runs. The edit
changed only the lower endpoint and dependent handoff identities:

- lower animation: `23a15a9008c3a255 / adfe5be62ebfcabd` to
  `22cc311102b7da8c / 8148c3e91e812b94`;
- lower boundary source: `ccf998244cb9fa59 / 24d27bbae93d5507` to
  `3c15a53db64bac78 / ca4ae26bb57bd399`;
- handoff static: `ca0483313cfce7e5 / 6cf73d69ffa7f315` to
  `f8b4d0ca4e5104b4 / 95154fd2c21b0455`;
- handoff animation: `401b8a647adcb3c9 / 939e1efba67710b2` to
  `4237588692f5dda7 / 4c63ad227e1541de`.

The immutable traces are
[cold](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/cold/trace/timings.json),
[unchanged warm](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/cache/trace/timings.json), and
[downstream edit](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/edit/trace/timings.json).

## Build time and residency

The cold handoff's 30 GPU animation-frame build times, in milliseconds, were:

```text
3330.050, 3432.138, 3641.168, 3387.861, 3350.088,
3527.426, 3449.125, 3433.386, 3397.544, 3389.042,
3265.937, 3350.497, 3685.116, 3693.048, 3601.621,
3388.532, 3849.324, 3337.952, 3552.060, 3369.278,
3280.297, 3394.453, 3468.239, 3396.475, 3278.204,
3463.364, 3630.340, 3286.665, 3388.968, 3496.439
```

Minimum/mean/maximum were `3265.937 / 3450.488 / 3849.324 ms`. The immutable
handoff animation is `13,842,344` bytes, retained boundary sources are
`5,386,050` bytes, and both peak handoff and network build CPU payload are
`54,521,896` bytes. The editor activated three animated raster draws at
approximately `639.1 MiB` compressed and `21.4 MiB` per GPU slot.

## Raster-only native gates

All four native modes (`gpu-mesher`, `water-forward`, `water-animation`, and
default) reported `ALL PASS` and zero Vulkan validation errors. The native
water-animation telemetry reports:

```text
water animation raster direct draws: 1
water animation RT counters: decode=0 blas=0 tlas_before=0 tlas_after=0 records=0
```

The parsed gate is
[native-gates.json](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/native/native-gates.json),
with the complete mode logs in its containing directory.

## Required 20 phase captures

Every PNG and `.done` pair is nonempty and was inspected. Normal,
geometry-normal, foam-driver, and identity views use the same corrected
`section-handoff` camera at every retained phase. The identity captures prove
disjoint cyan upstream, green handoff-strip, and blue downstream ownership.
They also show a large missing band between the cyan and green owners at every
phase. The same opening is visible in normal and geometry-normal views, while
the foam-driver view has no water coverage across that band. Real temporal
surface and turbulence variation remains visible in the upper section.

| Frame | Normal | Geometry normal | Foam driver | Identity |
|---:|---|---|---|---|
| 0 | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-00-normal/section-handoff.png) | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-00-geometry-normal/section-handoff.png) | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-00-foam-driver/section-handoff.png) | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-00-identity/section-handoff.png) |
| 7 | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-07-normal/section-handoff.png) | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-07-geometry-normal/section-handoff.png) | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-07-foam-driver/section-handoff.png) | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-07-identity/section-handoff.png) |
| 15 | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-15-normal/section-handoff.png) | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-15-geometry-normal/section-handoff.png) | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-15-foam-driver/section-handoff.png) | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-15-identity/section-handoff.png) |
| 22 | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-22-normal/section-handoff.png) | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-22-geometry-normal/section-handoff.png) | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-22-foam-driver/section-handoff.png) | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-22-identity/section-handoff.png) |
| 29 | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-29-normal/section-handoff.png) | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-29-geometry-normal/section-handoff.png) | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-29-foam-driver/section-handoff.png) | [image](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/screenshots/frame-29-identity/section-handoff.png) |

## Visual-gate RCA and blocker

The first 20-image run produced a false-negative diagnostic: all identity
captures appeared cyan because the acceptance timeline issued its FIFO `cam`
command immediately after the early `bake.finished` event. On the next frame,
the editor consumed the queued bake-finished state and restored the authored
world camera, overwriting the commanded pose. The retained failed probe is
[identity-far.png](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final4/camera-probe/identity-far.png).

A regression now requires one rendered frame after `bake.finished` before the
FIFO camera command. No renderer production code changed. The synchronized
probe
[identity-synchronized.png](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final4/camera-probe/identity-synchronized.png)
and all 20 corrected captures prove that the camera spans three distinct direct
draw owners.

The first `final5` recapture also exposed an older control-surface contract
mismatch: the editor intentionally touched a zero-byte `.done` sentinel while
Task 9 requires every completion sidecar to be nonempty. The runner rejected it
without overwriting it. The original PNG, empty marker, timeline, and log remain
under
[completion-marker-failure](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5/completion-marker-failure/).
The engine now writes `captured\n` at capture completion; the retained 20
sidecars are each 10 bytes and the strict comparator confirms all pairs.

That corrected framing exposes the actual Stage 1 blocker: the cyan upstream
owner terminates before the green strip, leaving an open,
background-visible band at the upstream cut. The green strip meets the blue
downstream owner without a visible separation. The upstream gap persists in
frames `0,7,15,22,29` across normal,
geometry-normal, foam-driver, and identity diagnostics. The executable
boundary metrics pass because they compare the recorded handoff-cut products;
they do not prove visible coverage between the handoff strip and the separately
drawn upstream surface.

The retained RCA traces that common missing coverage to the current temporary
dam filter. With `particleRadius=0.13 m` and `visualBlendWidth=0.10 m`, the
visual field support radius is `0.725 m`. Both the upstream boundary sidecar
and handoff builder erase contributors whenever that support intersects the
temporary dam's axis-aligned bounds. Those bounds conservatively enclose a
rotated `0.5 m` wall, so the filter removes a much wider dam-shaped region from
both products. Their remaining contours agree and pass the cut comparator,
while their shared scalar-field hole remains visible. The corrective work must
preserve accepted upstream water support and replace blanket AABB/support
erasure with a targeted solid-interior or contributor-provenance rule.

Stage 2 must not start until the upstream cut's raster ownership/coverage is
fixed and a fresh 20-image run shows:

- distinct upper/strip/lower ownership with no missing band;
- no coverage opening, bridge triangle, or normal/foam jump at either cut;
- synchronous frame 29 to frame 0 presentation across all three draws.
