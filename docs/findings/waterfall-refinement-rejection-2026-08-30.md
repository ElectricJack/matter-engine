# Waterfall refinement — rejected experiment, 2026-08-30

Status: **Task 11 architecture rejected; waterfall quality remains open.**
Stage 1 animated-section continuity remains accepted. This is not acceptance
of overall raster-water appearance. Controller integration is the next
implementation priority.

## Decisive evidence

| Approach | Real result | Decision |
|---|---|---|
| `0.10 m` fine particle remesh with scalar locking | Boundary Hausdorff `0.019421–0.022446 m`; normal dot `0.904996–0.955015` | Failed unchanged `0.009375 m / 0.995` gates. |
| `0.075 m` fine particle remesh with authoritative normals | Locked Hausdorff `0.009786 m`, `0.000411 m` over limit; x/y faces lacked a complete component-consistent normal correspondence | Rejected; more local lock tuning was not justified. |
| Conforming subdivision/fairing, prior v6 | Exact coarse/candidate packed-MHWA scanline roughness `70.0936432 → 51.0947838` (27.1049678%); screenshots retained the block-curtain staircase | Visually rejected despite improved dihedrals. |
| Final authored-local multiview fairing, v7 | All five rows passed safety/preservation but failed material improvement. Best authored roughness `86.554810 → 86.551796`; retained-view `31.557835 → 31.557550`; max original movement `0.001527 m`, free-edge movement zero | Failed closed before publishing a refined artifact. No all-30 refinement bake or cache acceptance was run. |

V7 used fixed rows `(.33,-.33/.67,2)`, `(.20,0,1)`, `(.33,-.34,2)`,
`(.45,-.55,16)`, and `(.45,-.56,24)` with the unchanged 75 mm displacement
cap. Canonical/oblique silhouette stencils were derived from the authored
waterfall frame, not the screenshot camera. The strongest row worsened the
retained metric to `43.315849`. Synthetic MSVC tests passed, but the real
geometry did not materially improve. An indirect retained-camera dependency
also remained in an anti-shrink filter; the rejected implementation was
removed rather than promoted or tuned further.

The packed-artifact comparator used the same C++ projection, ownership-edge
classification, and 1024-scanline code as bake diagnostics. The v6 packed
comparison and v7 pre-packed measurements are separate baselines and must not
be compared as if they were identical geometry.

## Retained visuals and recovery

- [Exact coarse frame 0](../../build/qa/task11-rejected/evidence/rejected-v6/baseline-coarse-normal/waterfall-side.png)
  and [visually rejected v6](../../build/qa/task11-rejected/evidence/rejected-v6/candidate-normal/waterfall-side.png).
- [Coarse geometry-normal](../../build/qa/task11-rejected/evidence/rejected-v6/baseline-coarse-geometry-normal/waterfall-side.png)
  and [rejected v6 geometry-normal](../../build/qa/task11-rejected/evidence/rejected-v6/candidate-geometry-normal/waterfall-side.png).
- [V7 failed-publication view](../../build/qa/task11-rejected/evidence/v7-failed-publication/waterfall-side.png)
  and [opposite view](../../build/qa/task11-rejected/evidence/v7-failed-publication/waterfall-opposite.png)
  show an empty/failed-publication scene, **not accepted coarse fallback or
  refined water**. The editor screenshot driver exited zero, but the fluid
  bake explicitly rejected all candidates.

The complete recoverable task patch, pre-cleanup snapshots, logs, scorer, and
capture evidence are under [the rejection archive](../../build/qa/task11-rejected/README.md).
Experimental source, tests, CMake wiring, provider integration, probe variables,
and the uncommitted architecture-specific Task10a selector amendment were
removed. The committed Task10/10a measurement harness and all earlier accepted
continuity code remain unchanged. The rejected design is retained only in
[the deprecated archive](../deprecated/superpowers/specs/2026-08-30-waterfall-conforming-subdivision-amendment.md).

Reopening waterfall refinement requires a new bounded architecture decision;
it is not a prerequisite for integrating and testing the character controller
against the accepted coarse river.
