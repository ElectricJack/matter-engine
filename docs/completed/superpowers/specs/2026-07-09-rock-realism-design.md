# Rock Realism: In-Session `raycast()` Probe + Surface-Relative Facet Cuts

**Date:** 2026-07-09
**Status:** Approved design, pending implementation plan

## Problem

`Rock.js` boulders read as blobs of primitives. The facet cuts are axis-aligned
boxes placed at a fixed distance from the origin (0.75–1.0) regardless of where
the blob surface actually is, so cuts gouge deep wrong chunks (e.g. an L-shaped
rock: a cube with a huge corner missing) instead of shaving facets off the
surface.

## Goals

1. Cuts land where the surface actually is, at controlled depths and angles.
2. A richer blob body (ellipsoids, dominant axis) so the underlying mass reads
   as a settled boulder, not overlapping spheres/boxes.
3. The mechanism is a native, generic DSL capability (not a rock-specific or
   JS-side workaround), reusable by other parts.

## Non-Goals

- True shatter-into-chunks (multi-chunk clusters). Possible later layering on
  this work (chunk = blob ∩ half-spaces, one baked variant per chunk).
- Multiple voxel sessions baking to separate meshes. Explicitly backlogged;
  Jack has an implementation idea — ask him before designing it.
- Post-retopo mesh queries (re-entering JS after bake).
- Pebble.js changes (candidate follow-up using the same recipe).
- New `plane()` primitive — a large transformed box brush already is one.

## Design

### 1. Native DSL verb: `raycast(origin, dir)` (C++)

An in-session probe of the solid being sculpted. During `build()` no mesh
exists — the build emits a brush list and meshing happens at bake — so the
honest primitive is an analytic query of the session's SDF field, which is the
surface the mesher will produce (within ~a voxel).

```js
this.beginVoxels(0.10);
this.smoothing(0.06);
// ...brushes...
const hit = this.raycast(origin, dir); // { point:[x,y,z], normal:[x,y,z] } | null
```

Semantics:

- **Scope:** valid only inside an open voxel session; calling it elsewhere is a
  fail-closed `set_error`, same pattern as `difference()` (`dsl_state.h:220`).
  One session = one solid = one mesh; no cross-session merge semantics implied.
- **What it queries:** the brushes emitted **so far** in the current session's
  range of `buffer_.ops`, with the smoothing cursor at call time. Emitting a
  cut and probing again sees the cut (later facets can chip earlier facet
  corners for free).
- **Return:** first surface hit along the ray as `{ point, normal }` (normal =
  outward unit vector via central-difference gradient), or `null` on miss.
- **Determinism:** pure function of the brush list; no GL; headless-testable.

Implementation:

- New `field_distance(buf, opBegin, opEnd, worldPoint)` in
  `csg_lowering.cpp` — signed-distance sibling of the existing
  `field_is_solid` oracle (`csg_lowering.cpp:128`), evaluated over the current
  session's op range and applying the same smooth-min/max the mesher uses
  (mirror MSL's smin formula; MSL itself stays untouched) so hits land on the
  actual iso surface.
- `DslState::raycast(...)` sphere-traces that field with a conservative step
  factor (smin fields are not exact SDFs), returns hit + gradient normal.
- Bind `__dsl_raycast` in `dsl_bindings.cpp`; expose
  `raycast(o,d)` in `part_base.js.h`.

### 2. Rock body: ellipsoid blobs (pure DSL, no engine changes)

Ellipsoids already work natively: the transform stack is stored per brush
(`dsl_state.cpp:86`) and non-uniformly scaled spheres lower to ellipsoid
FatPrims (`csg_lowering.cpp:193`).

- Pick a dominant horizontal axis (random yaw) and global anisotropy: squash Y
  (~0.65–0.9), stretch along the axis (~1.1–1.5) — settled/bedded, not round.
- 4–7 blobs (up from 3–5) placed along the dominant axis with jitter:
  `pushMatrix → rotate (per-blob orientation) → scale → sphere → popMatrix`.
  Occasional capsule for an elongated mass. No box brushes in the body (they
  were the main "primitive blob" tell); boxes appear only as cuts.

### 3. Facet cuts driven by `raycast()`

For each of 5–9 cuts (count varies by seed), inside the same voxel session:

1. Sample direction **d**, weighted to the upper hemisphere and sides (rock
   bottoms are sunk ~15% by Meadow).
2. Probe `raycast(center + d*3, -d)` → surface point **p**, normal **n**.
   Miss → skip the cut.
3. Cut-plane normal **m** = **n** jittered up to ~25° around a random tangent
   axis (facets not always tangent to the blob).
4. Depth `t = range(0.03, 0.12)` (absolute; rock is ~1 unit). Emit a large box
   (half-extent ~2) whose inner face lies on the plane through `p − m·t`:
   build the orthonormal frame from **m** in JS, `pushMatrix →
   applyMatrix(frame) → box → difference() → popMatrix`.
5. Guard: clamp `t` so the plane never passes closer than ~55% of the hit
   distance to the centroid — structurally impossible to gouge an L-shape.

### 4. Tuning

- `Rock.js` voxel spacing 0.15 → 0.10 (facets read crisply); smoothing 0.12 →
  ~0.06.
- Smoothing is whole-part (per-session stamp at `dsl_state.cpp:76`, lowering
  collapses to max), so body and cuts share one value — slightly beveled
  fracture edges, reads as weathered rock.
- Retopo modifier stack (`endModifier` wrapping) and Meadow instancing
  (yaw/scale/15% sink) unchanged.

## Testing

- **C++ headless unit tests** (script_host/dsl suites): raycast a known sphere
  → point/normal within tolerance; miss → `null`; call outside a voxel session
  → fail-closed error; probe after a `difference()` cut sees the cut surface.
- **Regression:** Meadow bake gates stay green.
- **Visual loop:** `make windows` after the engine change (standing rule), then
  FIFO-driven viewer shots (`GALLIUM_DRIVER=d3d12`, `tools/viewer_shots.sh`)
  with the camera parked at rocks; sweep several seeds; compare before/after.
  Windows viewer must not be left open by scripted runs.

## Risks / Open Points

- **smin mirror accuracy:** `field_distance` must track the mesher's smoothing
  formula closely enough that hits sit within ~a voxel of the baked surface.
  Covered by the sphere unit test plus visual validation.
- **Bake cost:** spacing 0.10 on a ~1-unit rock is ~10× the voxel count of
  0.15 per variant. ROCK_VARIANTS is small; if bake time regresses noticeably,
  spacing is the first knob to revisit.
- **In-session smoothing cursor:** raycast uses the smoothing value at call
  time; `Rock.js` sets smoothing once before the blobs and never changes it.
