# Animated-water section continuity acceptance — 2026-08-29

Status: **Stage 1 executable and visual gates pass.**

`handoffVisualGate: pass`

Task 10/10a measurement tooling is retained. Task 11 waterfall refinement was
rejected on 2026-08-30 and its experimental implementation removed; see
[the decisive failures](waterfall-refinement-rejection-2026-08-30.md).
Stage 1 remains accepted, while waterfall quality and overall raster-water
acceptance remain open. Controller integration proceeds next.

The accepted repair treats the temporary dam as
simulation collision only. Every finite captured particle inside the canonical
boundary crop remains authoritative visual-field input, even when its support
touches the former dam.

## Accepted result

The strict comparator reports `passed=true` with no failures in
[stage1-summary.json](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/wmc9a/stage1-summary.json).
The unchanged Task 8 gates retain these worst values:

| Metric | Worst accepted value | Gate |
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

Both cuts contain all 30 frames, feature labels are deterministic, and
`loopFrame29To0Synchronized=true`. The cold handoff retained `1,959,842`
temporary-dam-support contributor uses across its 30 frame builds; that number
is diagnostic, not an exclusion target.

## Cold, unchanged-warm, and downstream-edit locality

All accepted editor runs reached `Ready` with zero bake errors:

| Run | Hydrology wall time | Upper | Lower | Dependent handoff |
|---|---:|---|---|---|
| Cold | `473708.216 ms` | static/animation miss, `simulateMs=66053.340` | static/animation miss, `simulateMs=48528.491` | static/animation miss |
| Unchanged warm retry | `16263.146 ms` | static/animation hit, `simulateMs=0` | static/animation hit, `simulateMs=0` | static/animation hit |
| Fixture-only downstream edit | `290221.896 ms` | static/animation hit, `simulateMs=0` | static/animation miss, `simulateMs=52885.626` | static/animation miss |

The cold and unchanged-warm keys/digests are byte-identical and nonzero:

- upper animation `74aa703a7f084480 / 5b50d1e476422ce8` and boundary source
  `405091341bfb62b7 / a752ae3f2eeee52c`;
- lower animation `da1da916192c49db / 219ff1ff50a1a924` and boundary source
  `8b044112e9ff5619 / 66e66a47c117c81d`;
- handoff static `284de73798b064dd / 34abf7d8a518c489` and animation
  `363089902f6f8765 / 305e6792c879e9a3`.

The edit leaves the upper identities unchanged while changing only the lower
animation to `3d184cedb48ee6d0 / af311bd7c03c5aa9`, lower boundary source to
`4b4109fb96b0c3de / fd5e91b8ee60bd91`, handoff static to
`44d495f0149278ee / fbb662f88f4b7add`, and handoff animation to
`c9c0b95d7b97bee7 / cb70401835f42039`.

The immutable traces are
[cold](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/wmc9a/cold/trace/timings.json),
[unchanged warm](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/wmc9a/cache-retry/trace/timings.json), and
[downstream edit](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/wmc9a/edit/trace/timings.json).
The original automatic warm launch was manually interrupted during delayed
startup output and is excluded; `cache-retry` is the clean successful replay.

The boundary-source domain is now
`water-boundary-animation-source-v2-retained-dam-support`, the handoff
animation semantic revision is `4`, and dependent section animations use
`water-mesh-animation-v4-retained-boundary-support`. The previous `final5`
identities therefore cannot address the repaired products. A first repaired
run must resimulate accepted sections because the v1 sidecar physically omitted
the dam-touching particles and the cache does not persist a separate complete
30-frame raw capture. The unchanged repaired replay then hits every product,
as the evidence above shows.

## Memory and raster-only rendering

The cold handoff animation is `21,491,312` bytes, retained boundary sources are
`11,265,642` bytes, and peak handoff/network build CPU payload is
`76,136,896` bytes. The editor activated three disjoint raster draws at about
`651.3 MiB` compressed with a `21.8 MiB` active GPU slot. During the cold build
the process working set rose while frame products accumulated and fell from
about `2.30 GiB` to `1.51 GiB` as the queue drained.

All four native modes (`gpu-mesher`, `water-forward`, `water-animation`, and
default) passed with zero Vulkan validation errors. The parsed
[native gates](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/wmc9a/native/native-gates.json)
are:

```text
rasterDirectDraws=1
waterDecodeDispatches=0
waterBlasBuilds=0
waterTlasInstances=0
waterRtRecords=0
validationErrors=0
```

## Required 20 phase captures and strict visual verdict

Every native `1280x720` PNG and nonempty `.done` sidecar was inspected at
frames `0,7,15,22,29` in normal, geometry-normal, foam-driver, and owner-
identity views. No phase contains the old terrain/background band at either
ownership cut. Normal views show a continuous water sheet; geometry-normal
shows a coherent folded spillway ridge rather than a dry opening; foam-driver
coverage is not separated; and frames `29 -> 0` remain visually synchronized.

The deterministic screen-space gate classifies the cyan upstream, green
handoff, and blue downstream owners and requires one 8-connected component to
contain at least half of every owner. The accepted dominant fractions are:

| Frame | Cyan | Green | Blue |
|---:|---:|---:|---:|
| 0 | `1.0` | `1.0` | `1.0` |
| 7 | `1.0` | `1.0` | `1.0` |
| 15 | `1.0` | `1.0` | `1.0` |
| 22 | `1.0` | `1.0` | `1.0` |
| 29 | `1.0` | `1.0` | `0.999284` |

The complete capture tree is
[screenshots](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/wmc9a/screenshots).
Retaining authoritative particles reveals a real energetic splash/fold at the
former dam. It is visible physical evidence, not a synthetic bridge or a
reason to restore broad AABB field erasure. A future splash-classification
change would require physical/provenance evidence of its own.

## Retained prior failure and RCA

The old `final5` failure evidence remains under
[final5](C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp/build/qa/water-mesh-continuity-2026-08-29/stage1/final5).
Its frame-0 identity capture classified owner pixels
`[224611,44638,53142]`, but the dominant component contained only
`[224611,9,0]`; the new deterministic gate rejects it as
`background band separates water owners`.

That failure came from testing the `0.725 m` visual support sphere against the
world AABB of a rotated `0.5 m` temporary wall. The upstream boundary sidecar
and handoff builder both repeated the same deletion, producing a shared dry
notch whose remaining contours still agreed. The repair removes both filters,
keeps bounded outer/canonical ownership cuts, and proves the partitioned
upper + handoff + lower field-derived coverage against an unpartitioned
reference. No static legacy collar, synthetic bridge geometry, or shader hide
participates in the accepted animated raster presentation.

## Reproducibility boundary

This run exercised the current integration worktree, including accepted but
unrelated RiverFloatLab authoring/provider work that remains deliberately
unstaged under the dirty-worktree constraint. Its evidence is valid for that
integration state; cache identities and timings are not promised to reproduce
byte-for-byte from the Task 9a commit alone until those prerequisites land.

## Native cold-bake validation follow-up — 2026-08-30

The pre-merge native integration gate now uses the production Vulkan visual
mesher with the actual adapter identity, rather than substituting the CPU
mesher into the full animated-river test. A fresh bake exposed a validator
defect at downstream frame 13, not a new visible gap: its two original
contours differed by 0.00183885 m within the unchanged 0.009375 m tolerance,
but independent short-edge contractions moved the compared contours enough
to report 0.00962739 m and reject the handoff.

Topology normalization now supplies component membership only. Distance,
normal, and whole-segment coverage checks use the original geometry of each
surviving component, including attached short edges. Entirely collapsed
disconnected slivers retain their prior degenerate treatment; one-to-one
component matching, genuine-gap rejection, and tolerances are unchanged.

A small deterministic regression failed before the fix and passes afterward.
The exact captured GPU frame also replays as weldable on CPU: 0.00183885 m
distance, minimum normal dot 0.999997, no unmatched boundary edges or
duplicate triangles. The opt-in capture/replay control is documented in
[the agent control surface](../agent/control-surface.md). Run evidence is
retained under `build/qa/main-integration-2026-08-30/`.

The fresh native full two-section cold-bake test passed in 459.51 seconds:
both sections reached Ready, both fill sensors completed, particles remained
finite and within escape budgets, and visual/query/gameplay products and
separated timing fields were published. The five final native gates passed
in 464.19 seconds, including the animation, boundary-source, handoff and
registration checks (`final-feature-gates.log`). These are feature-tree
results; combined-main verification follows the merge. This validator
correction does not extend visual acceptance to the still-open waterfall/foam
quality work.
