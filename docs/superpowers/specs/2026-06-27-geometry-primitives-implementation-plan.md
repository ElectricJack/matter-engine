# Geometry Primitives — Implementation Plan

**Status:** Draft for review (2026-06-27)
**Scope:** A single sequenced plan to land all three design specs:

1. `2026-06-27-typed-iso-primitives-design.md` — typed iso-primitives + ordered CSG (voxel path).
2. `2026-06-27-stateful-dsl-completeness-design.md` — DSL completeness gaps G1–G8.
3. `2026-06-27-primitive-library-expansion-design.md` — extrude + capsule/cylinder/cone.

**Already landed (Phase 0):** `TriangleBuildBuffer::line()` rewritten from beaded UV spheres to
a real swept tube (`triangle_emit.cpp`).

This plan sequences the work by **dependency**, not by spec. Each phase is independently
testable and leaves the tree green. Every phase that changes baked geometry ends with
`rm -rf MatterEngine3/viewer/cache` so unchanged schemas re-bake (the resolved hash keys on
script source, not engine version). No phase changes the on-disk `.part` format.

**MSL scope note:** Phases 1 and 4 edit MatterSurfaceLib (the field evaluator). MSL is treated
as read-only, but representing a box / capsule / cylinder in the field is a genuine feature the
mesher cannot otherwise express — flagged as a deliberate scope decision per the per-project
exception, not a silent change.

---

## Dependency graph

```
Phase 0 (landed: swept-tube line)
   │
Phase 1  Typed iso-primitives foundation (voxel)         ── enables G1, P2-voxel
   │        primitive_sdf dispatch, fat array, ordered CSG, box, G2 scale fix
   ├────────────────────────────┐
Phase 2  DSL completeness        Phase 4  Round-primitive voxel cases
   │   (G3 G4 G5 G6 G7 G8)          (capsule/cylinder/cone SDF; satisfies G1)
   │   mostly independent
Phase 3  Extrude mesh machine ───┘
   │   triangulator, POLYGON mode, joins
Phase 5  Round-primitive mesh + DSL wiring (needs Phase 3 core + Phase 4 voxel)
```

Phases 2 and 3 can proceed in parallel after Phase 1. Phase 5 needs both 3 and 4.

---

## Phase 1 — Typed iso-primitives foundation (voxel path)

**Spec:** typed-iso-primitives. **Why first:** everything voxel-side (box, capsule, cylinder,
ordered CSG) depends on `primitive_sdf` dispatch + the fat-primitive array existing.

**Work**
1. **MSL `primitive_sdf` dispatch** — add a `PrimKind` enum + fat-primitive struct
   (`kind`, `center`, `boundRadius`, `materialId`, `tint`, union-sized param blob with box
   `halfExtents` + `invTransform`). `primitive_sdf(prim, p)` switches on kind: sphere
   (`|p−c|−r`) + box (`sdBox` of `invTransform·p`). Move `sdBox` math out of
   `csg_lowering.cpp` into MSL.
2. **Thread the fat array through `MeshContext`** (`meshing_algorithm.h`, `cell.cpp`): a
   `const FatPrim*` + count, borrowed (not in the spatial hash). Expand mesh bounds to cover
   fat prims (`cell.cpp`).
3. **Ordered CSG stages** — replace the fixed union-then-subtract pipeline in `surface.c`
   (`CalculateScalarAndMaterial`, `ApplySubtractField`) with an ordered stage list derived from
   the `BuildOp` order: per sample `field = smin/smax/…` folded in op order. Merge consecutive
   same-op ops into one stage; tag hashed particles with stage index, single bucketed query.
4. **`csg_lowering.cpp`** — emit **one oriented box** fat-primitive per `box` op; **delete
   `stamp_box`** (the cubic sphere stamp); `reserve()` the sphere + fat arrays from op counts.
5. **G2 scale fix (folds in here)** — the sphere brush carries its `invTransform`; SDF is
   `sdSphere(invTransform·p, r)` so `scale(2,2,2); sphere()` meshes at the scaled radius and
   matches the `field_is_solid` oracle (which already respects scale). Box already does this.

**Tests (Phase 4 of systematic-debugging — failing test first)**
- Box meshes to a box, not sphere-soup: compare meshed occupancy against the `field_is_solid`
  analytic oracle at sample points (oracle already exists in `csg_lowering.cpp`).
- `scale(2,2,2); sphere(c, r)` occupancy matches radius `2r` (the G2 regression — assert it
  fails before the fix).
- Ordered CSG: `add A → subtract B → add C` keeps `C` where it overlaps `B`
  (`(A−B)∪C ≠ (A∪C)−B`); assert the new geometry survives the earlier subtract.

**Done when** box is a real box, sphere respects scale, add/subtract/add ordering holds, no
`stamp_box`. `rm -rf viewer/cache`, re-bake a box-bearing part, eyeball in viewer.

---

## Phase 2 — DSL completeness (authoring correctness)

**Spec:** stateful-DSL-completeness, gaps that don't need the round primitives. Largely
independent of Phase 1; can run in parallel after it. Each gap is a small, separately-tested
change.

- **G3 — session-scoped op verbs.** `set_last_op` (`dsl_state.cpp`) gates to an open voxel
  session and the current session's brush range (`session_start_`); `difference()` with no
  brush in this session is a clear error. *Test:* stray `difference()` after `endVoxels` errors;
  in-session re-tag still works.
- **G4 — `tint(r,g,b,a)` cursor.** New cursor in `dsl_state.{h,cpp}`, binding in
  `dsl_bindings.cpp`, `part_base.js.h`; capture onto each brush/triangle (mirrors `material_`).
  Default `(1,1,1,0)` = neutral (unchanged behavior). *Test:* a tinted fill carries tint into
  the `BuildOp`/`TriEx`; default is unchanged.
- **G5 — `lookAt()` transform verb.** Matrix-stack helper orienting the current frame at a
  target. *Test:* `lookAt` aims +Z (or the chosen forward axis) at the target; composes with
  the stack.
- **G6 — `placeChild(module, params?)`.** Optional params folded into the child's resolved
  hash (canonical bytes). Backwards compatible (no params = today). *Test:* same module + same
  params → same resolved hash (dedup); different params → different hash.
- **G7 — build-end stack-balance check.** `script_host.cpp` finalizer: `stack_.size() != 1` at
  build end is a misuse error (same fail-closed path as open-session). *Test:* a `pushMatrix`
  without `popMatrix` fails the build with a clear message.
- **G8 — `sphere`/`box` mesh emitters + dispatch.** In mesh/None session, `sphere` emits a
  UV-sphere and `box` emits 12 triangles into the `TriangleBuildBuffer`, baking the transform
  matrix into vertices; in Voxels they stay SDF brushes; mid-`beginShape` errors. Add
  `TriangleBuildBuffer::sphere(center, r)` / `box(center, halfExtents)`. *Test:* `sphere()` in
  None session produces a closed triangulated sphere of the right radius; in Voxels still a
  brush; mid-shape errors.

*(G1 — voxel `line` → capsule — is **deferred to Phase 4**, since it needs the capsule SDF.)*

**Done when** all gaps land with tests; the polymorphism rule holds for sphere/box across
sessions. `rm -rf viewer/cache` (G8 changes meshed geometry for any mesh-mode sphere/box).

---

## Phase 3 — Extrude mesh machine

**Spec:** primitive-library, P1. The general mesh sweep. Independent of Phases 1/4 (pure mesh).

**Work**
1. **Constrained ear-clipping triangulator** (`triangle_emit` helper or
   `polygon_triangulate.{hpp,cpp}`): outer contour + hole contours → triangle indices. Holes
   bridged into the outer loop, then ear-clipped. Pure function, unit-testable in isolation.
2. **`POLYGON` shape mode + profile retention** (`dsl_triangle.cpp`, `dsl_state.{h,cpp}`): new
   `beginShape` mode = outline; `beginContour`/`endContour` push hole contours;
   `endShape` finalizes + retains the `Profile` (outer + holes). Lazy flat-fill: emit the
   triangulated face only if no consumer claims the profile before the next
   `beginShape`/session change. *(Resolve the lazy-vs-terminator open question here.)*
3. **`extrude` emitter** (`triangle_emit.{cpp,hpp}`): profile + path → solid. Rotation-
   minimizing (parallel-transport) frame along the path; ring-to-ring quad walls (outer
   outward, holes inward); triangulated end caps (via the triangulator) on open paths.
4. **`joinType(MITER|BEVEL|ROUND)` cursor** (`dsl_state.{h,cpp}`, `dsl_bindings.cpp`,
   `part_base.js.h`): controls the interior-vertex wall stitch; MITER default with a
   miter-limit fallback to bevel.
5. **DSL dispatch + JS surface:** `extrude(path)` consumes the retained profile in None/mesh;
   errors in Voxels ("not supported in voxel session yet") and mid-`beginShape`.
   `beginContour`/`endContour`/`joinType`/`POLYGON` bound in `part_base.js.h`.

**Tests**
- Triangulator: convex square, concave L, square-with-square-hole — correct triangle count,
  no degenerate/overlapping tris, covers the polygon area.
- Extrude a square profile along one segment → a closed box-like prism (watertight: every edge
  shared by exactly two tris).
- Extrude an annulus (profile with hole) → a tube with capped ends (a true hollow tube).
- Extrude along an L-shaped polyline → no twist at the joint; MITER vs BEVEL change the corner.
- `extrude` in Voxels and mid-`beginShape` both error.

**Done when** extrude produces watertight solids for concave + holed profiles along polylines
with selectable joins. `rm -rf viewer/cache` if any committed part uses it.

---

## Phase 4 — Round-primitive voxel cases (+ G1)

**Spec:** primitive-library P2 (voxel side) + stateful-DSL G1. Needs Phase 1's dispatch.

**Work**
1. **`primitive_sdf` cases** in MSL: `sdSegment` (capsule) and `sdCappedCone` (cylinder/cone,
   reads `r0`/`r1`; equal radii = straight capped cylinder). Brush-local via `invTransform`.
2. **`BrushKind::{Capsule, Cylinder}`** + param layout (segment `a`,`b` + `r0`,`r1`) in
   `dsl_state.h`; voxel lowering in `csg_lowering.cpp` emits these as fat-primitives.
3. **G1 — voxel `line` → capsule.** `line()` in a voxel session now lowers to the capsule
   fat-primitive (`sdSegment − r`) instead of erroring. So voxel `line` and voxel `capsule` are
   the same primitive.

**Tests**
- Capsule/cylinder voxel occupancy vs. a new analytic oracle (extend `field_is_solid`-style
  check) including under `scale`.
- Ordered CSG still holds with a capsule/cylinder in the stage list.
- Voxel `line()` no longer errors and meshes to a round-capped tube (matches the capsule SDF).

**Done when** capsule/cylinder/cone bake correctly in voxel mode and voxel `line` is a capsule.
`rm -rf viewer/cache`.

---

## Phase 5 — Round-primitive mesh + full DSL wiring

**Spec:** primitive-library P2 (mesh side). Needs Phase 3 (extrude core) and Phase 4 (voxel
cases + BrushKinds).

**Work**
1. **Mesh emitters via the extrude core:** `cylinder(a,b,r)` / `cone(a,b,r0,r1)` generate a
   circular N-gon profile and call the extrude core along the segment with **flat (triangulated)
   caps**; tube radii → annular caps via a hole contour. `capsule(a,b,r)` reuses the wall sweep
   but emits **hemisphere caps** (the one non-triangulated cap). Optionally a shared
   circular-sweep emitter taking `CapStyle{Flat, Hemisphere}`.
2. **DSL dispatch** (`dsl_state.cpp`/`dsl_triangle.cpp`): `capsule`/`cylinder`/`cone` dispatch
   on `session_` — voxel → the Phase-4 fat-primitive; None → the mesh emitter; mid-`beginShape`
   → error.
3. **JS surface** (`part_base.js.h`): `capsule`/`cylinder`/`cone` verbs (`cone` is sugar that
   lowers to a tapered cylinder).
4. *(Optional, not required)* fold the landed `line()` mesh onto the cylinder/extrude path.

**Tests**
- `cylinder()` in None session → watertight flat-capped solid; tube variant → hollow with
  annular caps.
- `capsule()` in None → round-capped solid; `cone()` → tapered, `r1=0` closes to a point.
- Each of capsule/cylinder/cone: voxel session bakes the SDF, None bakes mesh, mid-shape errors
  (the full polymorphism matrix).

**Done when** the polymorphism summary table in the primitive-library spec holds end-to-end.
`rm -rf viewer/cache`, re-bake, eyeball capsule/cylinder/cone/extrude parts in the viewer.

---

## Cross-cutting checklist (every phase)

- Failing test **before** the fix (systematic-debugging Phase 4 / TDD).
- No `.part` format change; if baked geometry changes, `rm -rf MatterEngine3/viewer/cache`.
- MSL edits flagged as deliberate scope decisions (Phases 1, 4).
- Keep each phase green before starting the next; phases 2 & 3 may overlap.

## Sequencing summary

| Phase | Lands | Depends on | Cache clear |
|---|---|---|---|
| 0 | swept-tube `line` | — | (done) |
| 1 | iso-primitives, ordered CSG, box, G2 | 0 | yes |
| 2 | DSL gaps G3–G8 | 1 (for G8 emitters seam) | yes (G8) |
| 3 | extrude + triangulator + joins + POLYGON | 0 | if used |
| 4 | capsule/cylinder/cone voxel SDF + G1 | 1 | yes |
| 5 | capsule/cylinder/cone mesh + dispatch | 3, 4 | yes |

## Open questions rolled up (resolve before coding the relevant phase)

- **P1/Phase 3** — profile retention: lazy flat-fill vs. `extrude`-as-terminator (recommend
  lazy). `POLYGON` winding: explicit outer-CCW/holes-CW vs. auto-detect (recommend explicit).
  Miter-limit default + tunability.
- **Phase 1** — box orientation encoding in the param blob (full inverse `Matrix` vs.
  inverse-rotation 3×3 + center); `boundRadius` for scaled/sheared box; per-stage vs. global
  smoothing `k`; carve-stage material participation; stage-count cap/diagnostics.
- **Phase 4** — `sdCappedCone` vs `sdRoundCone` for tapered ends (flat vs rounded in field).
- **Phase 2** — G4 tint vs. material-default-tint combine rule; G6 params encoding (canonical
  JSON vs typed struct) for a stable resolved hash.
- **Phase 5** — shared circular-sweep emitter + `CapStyle` vs. separate capsule emitter.
