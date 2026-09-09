# Task 11 amendment: conforming waterfall fairing

Status: **REJECTED / DEPRECATED** on 2026-08-30. This is historical experiment
documentation, not an implementation commitment. Scalar locking failed seam
gates; conforming fairing failed real frame-0 material-improvement gates.
All experimental product code, wiring, and tests were removed after archival
under `build/qa/task11-rejected/`. Stage 1 continuity remains accepted;
waterfall refinement remains open and controller integration proceeds.

## Decision

Task 11 will refine the accepted coarse waterfall surface by one deterministic
ratio-2 conforming red-green subdivision followed by constrained Taubin
fairing. It will not remesh a finer particle field at the coarse ownership
boundary and will not add a coarse-to-fine zipper strip.

The bounded patch follows existing coarse triangle edges. An ownership edge is
exactly `0 < selected uses < all uses`; it remains unsplit and its positions
and normals are fixed. Fully selected edges share one midpoint even if the
accepted source has more than two incident faces; original multiplicity is
preserved. A physical free water-silhouette edge is exactly
`all uses = 1, selected uses = 1`; it has no adjacent outside face and is
therefore split conformingly and may fair without creating a T-junction. Every
outside triangle remains byte-identical, and the ownership seam is structural:
no bridge triangle, alpha hide, or approximate boundary match is permitted.

Lip, plunge, and authored feature vertices are pinned. Intentional spray is
identified independently of physical boundary status: selected triangles are
partitioned by shared source vertices, the largest component by triangle count
(earliest-triangle tie break) is the main sheet, and every original vertex in a
smaller component is pinned. Thus the main free silhouette can change while
detached spray cannot be silently absorbed or faired away.

The frame-0 selector evaluates five fixed rows (`lambda`, `mu`, pair count):
`(.33, -.33/.67, 2)`, `(.20, 0, 1)`, `(.33, -.34, 2)`,
`(.45, -.55, 16)`, and `(.45, -.56, 24)`. The first is a uniform-scale
shrink-cancellation proof, the second is a constrained-Laplacian control,
and the remaining rows evaluate stronger Taubin fairing. Every row retains
the unchanged `0.075 m` displacement cap and inversion/normal-flip guards.

Production stencils derive only from the authored waterfall frame: lip-to-
plunge drop direction, river lateral direction, and their cross-product
normal. The canonical orthographic contour plus symmetric lateral obliques
contribute a deterministic union of contour neighbors; other vertices use
surface neighbors. Four canonical projected extrema are pinned (deduplicated
when extrema share a vertex). The retained screenshot camera is an independent
QA oracle, never a production stencil or row-selection input.

Selection is deterministic by lowest authored canonical scanline-envelope
roughness, then P95/RMS face-normal variation. It requires at least 30 percent
canonical contour improvement against both the exact coarse patch and its
unsmoothed subdivision, no more than five percent regression on either
oblique, and at least five percent/0.10-degree improvement in both P95 and RMS.
The metric samples left/right geometry contours at 1024 fixed coarse-height
scanlines and measures normalized second differences; ownership cuts are
excluded while physical free edges and closed-surface occluding contours are
included. Per-row normal variation, all view metrics, displacement, and
moved/fixed population are retained.

A deployable row must retain `0.85..1.05` of unsmoothed 3D patch area,
`0.90..1.05` of physical free-edge length, `0.90..1.05` of retained projected
coverage, and `0.95..1.05` of projected width/height, with nearest pinned
plunge coverage unchanged within `0.00001 m`. Projected contour length is
reported but is not an anti-shrink gate: removing jagged turns legitimately
shortens it, while coverage, width, height, and 3D-area gates prevent collapse.
The semantic revision is `waterfall-conforming-red-green-fairing-v4`.

The prior frame-0 candidate was rejected visually despite improved dihedrals.
Its packed-artifact retained-camera envelope improved only about 27.1 percent;
the next candidate must exceed a 30-percent retained-view comparison and
visibly improve normal and geometry-normal captures, including a second
gameplay-side view. The focused test executable's `--score-artifacts` mode
decodes MHWA frames and calls the same C++ projection/ownership/scanline code
used by bake diagnostics so the exact comparator is not a Python approximation.

The selected `0.075 m` Task 10a row now means one subdivision level of the
production `0.15 m` surface and its memory target. It no longer authorizes a
second particle-field evaluation. Particle radius `0.13 m`, blend width
`0.10 m`, and simulation spacing `0.20 m` remain properties of the accepted
coarse source bake.

## Why scalar locking is rejected

Real exact-row frame-0 evidence rejected the coarse/fine scalar-lock design.
The best locked mesh measured `0.009786 m` symmetric boundary Hausdorff,
`0.000411 m` above the unchanged `0.009375 m` gate. An authoritative normal
buffer was proven through validation, chunk cropping, descriptors, and RTX
mutation tests, but the real boundary still lacked a valid correspondence.

At the worst x-max sample all transition weights were zero and the stored,
analytic, and emitted refined normals agreed. The old `0.641867` dot came from
position-only matching against a different nearly coincident coarse fold.
A stricter one-to-one component proof did not rescue the result: x-max had
`2/2` components but only one compatible edge and no perfect assignment;
y-max had `1/1` components and zero compatible edges; only z-max completed its
assignment (`0.999970` minimum dot). This is a real self-overlap/normal-field
correspondence failure, not a descriptor bug or a threshold problem.

An exact coarse-to-fine seam strip is also rejected for this spike. It would
need the correspondence that the real proof could not establish, reintroduce
zipper/sliver/inversion risk, violate the current no-bridge decision, and spend
against only `6,259,294` projected bytes of RiverFloatLab network headroom.

## Frame-0 execution gate

The first spike stops after one real frame and retained-camera captures.
Before any 30-frame bake it must prove:

- conforming deterministic topology and index order;
- no split ownership edge or T-junction; physical free silhouette edges split
  conformingly and may fair;
- exact boundary positions and normals (`Hausdorff = 0`, normal dot `= 1`);
- byte-identical geometry outside the patch;
- lip, plunge, feature, and spray pins remain fixed;
- bounded selected Taubin/Laplacian displacement with no inversion or
  disallowed normal flip, plus a material same-frame improvement over both
  exact coarse and unsmoothed-subdivision baselines;
- unchanged component/hole/spray topology and accepted plunge coverage;
- semantic/cache identity changes when any fairing control changes;
- measured complete-animation and network projections remain below the
  existing `1 GiB` section and exclusive `700 MiB` network caps; and
- retained normal and silhouette captures visibly reduce the block-curtain
  faceting. Subdivision without genuine visual improvement is a failure.

Only a passing frame 0 authorizes the existing 30-frame, cache, memory, native
GPU, and screenshot acceptance sequence.
