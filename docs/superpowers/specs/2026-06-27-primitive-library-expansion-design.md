# Primitive Library Expansion — Design

**Status:** Draft for review (2026-06-27)
**Context:** Two specs already define the machinery this builds on:
`2026-06-27-typed-iso-primitives-design.md` (the `primitive_sdf` dispatch + fat-primitive
array + ordered CSG stages for the voxel path) and
`2026-06-27-stateful-dsl-completeness-design.md` (the session-polymorphism rule:
voxel → iso-primitive SDF, mesh → triangles, mid-`beginShape` → error). With `line()` now a
real swept tube (`triangle_emit.cpp`) and the oriented box becoming a first-class iso-primitive,
the primitive set is `sphere`, `box`, `line`. This doc adds **extrude** — a general mesh sweep
of an arbitrary 2D profile (concave, with holes) along a segment or polyline — and then defines
**capsule**, **cylinder**, and **cone** as circular specializations of that one new machine
(plus analytic SDF cases on the voxel side).

Like the specs it depends on, this is **entirely a bake-time concern**: every change is
authoring/lowering/mesh-emit state. Nothing alters the serialized `.part` format (`save_v2`
writes baked triangle geometry only). Cache-clear once after landing.

## Goal

Grow the primitive vocabulary with **one new general mesh machine — `extrude`** — and express
the round primitives as specializations of it, plus analytic SDF cases on the voxel side:

- **Extrude is the general mesh sweep**: an arbitrary 2D profile (concave, and **with holes**)
  swept along a segment or polyline. The profile is authored with the existing
  `beginShape`/`endShape` verbs (plus `beginContour`/`endContour` for holes), so authors reuse
  the interface they already know.
- **Capsule / cylinder / cone reuse the extrude machinery** on the mesh side: a circular
  profile swept along the segment, differing only in cap style (flat disc vs hemisphere) and
  taper. They are **not** a separate swept-tube family — they call the same extrude core.
- **Capsule / cylinder / cone are also polymorphic**: in a voxel session they lower to one
  analytic SDF case each in `primitive_sdf` (capsule = `sdSegment − r`, cylinder/cone =
  `sdCappedCone`). Voxel extrude is **deferred** (an SDF of a swept arbitrary prism is hard;
  do it by voxelization later, if ever).

Explicitly **out of scope**: torus, generalized revolve, boolean-on-mesh, and any voxel
representation of `extrude`.

---

## The machines this builds on

| Machine | Lives | What a new primitive adds |
|---|---|---|
| **Voxel iso-primitive** | `primitive_sdf(prim, p)` (MSL) + a `BuildOp` kind + a lowering case | one `case` (an analytic SDF) + one fat-primitive param layout |
| **Mesh extrude (NEW, general)** | `TriangleBuildBuffer::extrude(...)` + profile capture in `beginShape`/`endShape` | the round primitives become a generated circular profile + a cap-style flag |

`line()`'s landed swept tube proved the wall-sweep mechanics (orthonormal cross-section frame
from the axis, ring per path sample, quad wall band, end caps, outward winding). **Extrude
generalizes that**: the ring is an arbitrary profile instead of a circle, the path is a
polyline instead of one segment, and the caps are a real polygon triangulation instead of a
fan. Once extrude exists, `cylinder`/`cone`/`capsule` are *defined in terms of it*.

---

## P1 — Extrude (the general mesh sweep)

Sweep a **2D profile** (outer contour, optionally with holes) along a **path** (one segment or
a polyline). This is the new foundational mesh primitive.

### Profile authoring — reuse `beginShape`/`endShape`

Add a **polygon profile mode** to the existing shape verbs (a new `beginShape` mode alongside
`triangles`/`strip`/`fan`). Authors build the profile exactly like a shape:

```
beginShape(POLYGON)        // outline mode — a closed, fillable contour
  vertex(x, y)             // outer contour points, CCW; the X/Y are the cross-section
  vertex(x, y)             //   (u,v) plane, Z ignored
  beginContour()           // optional hole(s)
    vertex(x, y)           //   inner contour, wound opposite (CW)
  endContour()
endShape()                 // finalizes and RETAINS the profile as the "current shape"
extrude(path)              // sweeps the retained profile along the path → closed solid
```

- **`POLYGON` mode is a true outline**, distinct from `triangles`/`strip`/`fan` (which are
  explicit tessellations where the vertices already *are* triangles). A `POLYGON` shape is a
  boundary to be filled/swept.
- **`endShape()` on a `POLYGON` retains the contour set** as the current profile. If nothing
  consumes it, it emits as a **flat filled face** (see triangulation below) — so `POLYGON` mode
  also upgrades 2D fills to support concave outlines and holes, which `fan` cannot express.
  `extrude(path)` **consumes** the retained profile and emits the swept solid **instead of**
  the flat face. (Implementation: `endShape` finalizes + retains; the flat fill is emitted lazily
  only if no consumer verb claims the profile before the next `beginShape`/session change.)
- `extrude` outside a retained `POLYGON` profile → error ("extrude with no profile; build one
  with beginShape(POLYGON)…endShape first").

### Concave profiles and holes — general triangulation

Caps and flat fills triangulate the profile with a **constrained ear-clipping triangulator
that supports holes** (hole contours bridged into the outer contour, then ear-clipped — the
standard earcut approach). This is required, not optional: **concave profiles and holes
(e.g. a custom tube cross-section) are a primary reason for `extrude`.** The same triangulator
serves three callers: `POLYGON` flat fills, extrude end caps, and the disc/annulus caps of the
round primitives.

### Path, frame, walls

- **Path:** one segment `(a,b)` or a polyline `[p0,p1,…,pn]`.
- **Frame:** a **rotation-minimizing frame** (parallel transport) carried along the path so the
  profile does not twist at bends. (`line`'s one-shot Gram-Schmidt frame suffices for a single
  segment; a polyline needs transport to avoid a discontinuous flip at each joint.)
- **Walls:** ring-to-ring quad bands between consecutive path samples, one quad per profile
  edge, outward-wound from the profile's CCW order; hole edges wind inward so hole walls face
  into the cavity.
- **Caps:** triangulate the profile (with holes) at the two open ends of an open path. A closed
  loop path has no caps.

### Joints — author-selectable, set before extrude

The wall stitch at an **interior polyline vertex** is controlled by a **join-type cursor** set
*before* the extrude call (a stateful cursor, like `fill`/`smoothing`):

```
joinType(MITER | BEVEL | ROUND)   // default: MITER
extrude(path)
```

- **MITER** — extend the two adjacent wall sections to their intersection (sharp corner;
  falls back to bevel past a miter-limit angle to avoid spikes).
- **BEVEL** — flat chamfer across the corner.
- **ROUND** — arc fillet across the corner.

### Voxel session → deferred

In a voxel session `extrude()` **errors** with a clear "not supported in voxel session yet"
(not a silent gap). A voxel extrude would require voxelizing the swept prism — its own
sub-project. This is the one deliberate exception to the polymorphism rule.

New emitter:
`TriangleBuildBuffer::extrude(const Profile& profile, const float3* path, int path_n, JoinType join, int material_id, const mat4& transform)`,
where `Profile` holds the outer contour + hole contours captured from the `POLYGON` shape.

## P2 — Capsule / cylinder / cone (circular specializations)

On the **mesh side** these are *not* a separate emitter family — each **generates a circular
profile** (N-gon) and calls the **extrude core** along the single segment `a→b`, differing only
in cap style and taper:

- **cylinder** `(a, b, r)` → constant-radius circle profile, **flat disc caps** (triangulated
  by the same triangulator; a tube radius pair → annular caps via a hole contour).
- **cone** `(a, b, r0, r1)` → authoring sugar for a cylinder whose end radii differ (`r1` may be
  0 for a point). **Cone is never its own primitive** — it lowers to the tapered cylinder.
- **capsule** `(a, b, r)` → circle profile swept wall, but **hemisphere caps** instead of flat
  discs (the one non-triangulated cap; rings from the end profile down to a pole along `±axis`).
  Capsule is the only round primitive whose cap is not a profile triangulation.

`line` mesh (landed, flat-capped swept tube) is the same shape as `cylinder` mesh; it can
optionally be folded onto the cylinder/extrude path later, but the landed emitter is fine and
folding is **not** required by this spec.

On the **voxel side** each lowers to one analytic SDF case (independent of the mesh extrude
machinery):

- **capsule / voxel `line`** → `sdSegment(p) − r` (this *is* the polymorphic voxel `line` from
  stateful-DSL spec G1 — same primitive, explicit name).
- **cylinder / cone** → `sdCappedCone(p)` reading `r0`/`r1` from the param blob; equal radii
  degenerate to a straight capped cylinder.

All evaluate in brush-local space via the fat-primitive `invTransform`, picking up the
transform-stack scale exactly like sphere/box (iso-primitives G2 rule).

---

## Polymorphism summary

| Verb | Voxel session | Mesh session (None) | Mid-`beginShape` |
|---|---|---|---|
| `sphere` | `sdSphere` brush | UV-sphere tris | error |
| `box` | `sdBox` brush | 12 tris | error |
| `line` | capsule SDF (`sdSegment − r`) | flat-capped tube (landed) | error |
| `capsule` | capsule SDF (= voxel `line`) | circle profile, **hemisphere** caps | error |
| `cylinder` | `sdCappedCone` (equal radii) | circle profile, **flat** caps (via extrude) | error |
| `cone` | `sdCappedCone` taper | tapered circle profile (via extrude) | error |
| `extrude` | **error — deferred** | arbitrary profile (concave + holes) swept | error |
| `beginShape(POLYGON)`…`endShape` | error (mesh-only) | concave/holed flat fill | n/a (is the shape) |

Mesh `cylinder`/`cone` are **extrude with a generated circle profile**. `capsule` shares the
wall sweep but uses hemisphere caps. `extrude` is the one true new machine.

---

## Where the edits land

| Change | File(s) |
|---|---|
| `POLYGON` shape mode + profile retention in `beginShape`/`endShape`; `beginContour`/`endContour` | `dsl_triangle.cpp`, `dsl_state.{h,cpp}` |
| Constrained ear-clipping triangulator (holes + concave) | `triangle_emit.{cpp,hpp}` (or a small `polygon_triangulate.{hpp,cpp}`) |
| `extrude` emitter (profile sweep + transport frame + join handling + caps) | `triangle_emit.{cpp,hpp}` |
| `cylinder`/`cone`/`capsule` mesh emitters (circle profile → extrude core + cap style) | `triangle_emit.{cpp,hpp}` |
| `joinType` cursor (MITER/BEVEL/ROUND) | `dsl_state.{h,cpp}`, `dsl_bindings.cpp`, `part_base.js.h` |
| `BrushKind::{Capsule, Cylinder}` + param layout (segment + r0/r1) | `dsl_state.h` |
| voxel lowering for capsule/cylinder/cone fat-primitives | `csg_lowering.cpp` |
| `primitive_sdf` cases `sdSegment` / `sdCappedCone` | MSL `surface.c` / iso-primitive header |
| DSL dispatch (`capsule`/`cylinder`/`cone`/`extrude` on `session_`) | `dsl_state.cpp` / `dsl_triangle.cpp`, `dsl_bindings.cpp` |
| JS surface (`capsule`/`cylinder`/`cone`/`extrude`/`beginContour`/`endContour`/`joinType`/`POLYGON`) | `part_base.js.h` |
| On-disk `.part` format | **unchanged** |

Depends on the iso-primitives spec landing first (the fat-primitive array + `primitive_sdf`
dispatch must exist before the capsule/cylinder/cone voxel cases can be added). The mesh
extrude work depends only on the already-landed swept-tube `line()` and the new triangulator.

---

## Goals / Non-goals

**Goals**
- `extrude` as the general mesh sweep: arbitrary profile (concave + holes) along a segment or
  polyline, twist-free, with author-selectable join type.
- Profile authored via the existing `beginShape`/`endShape` (`POLYGON` mode) plus
  `beginContour`/`endContour` for holes — no new bespoke profile interface.
- A constrained ear-clipping triangulator (holes + concave) shared by flat fills, extrude caps,
  and round-primitive caps.
- Capsule/cylinder/cone mesh defined as circular profiles through the extrude core (cone =
  tapered cylinder; never its own primitive).
- Capsule/cylinder/cone voxel cases via analytic `sdSegment`/`sdCappedCone`.
- Takes capsule/cylinder/cone off the iso-primitives spec's "deferred shapes" non-goal list.

**Non-goals (deferred)**
- Voxel-mode `extrude` (needs mesh→SDF voxelization — separate sub-project).
- Torus, generalized surface-of-revolution, mesh-on-mesh boolean.
- Per-segment profile morphing/scaling along a polyline (constant profile for now).
- Self-intersection cleanup at very tight polyline angles beyond the miter-limit fallback.

---

## Open questions (resolve during planning)

- **Profile retention semantics:** lazy flat-fill emission (emit only if no consumer claims the
  profile before the next `beginShape`/session change) vs. `extrude` as a strict alternative
  *terminator* to `endShape` (build with `beginShape(POLYGON)`…`vertex`…, then `extrude(path)`
  closes+sweeps, no `endShape`). The lazy model is more flexible (same profile can flat-fill or
  extrude); the terminator model is simpler to implement. **Recommend** the lazy model since the
  user explicitly wants `beginShape`+`endShape` to build the profile.
- **`POLYGON` winding / hole detection:** require outer CCW + holes CW (explicit), or auto-detect
  hole nesting by containment? Recommend explicit winding (cheaper, predictable).
- **Miter limit:** the angle at which `MITER` falls back to `BEVEL` to avoid spikes — pick a
  default (e.g. ~150°) and whether it's author-tunable.
- **Cap-style encoding for round primitives:** one shared circular-sweep emitter taking a
  `CapStyle{Flat, Hemisphere}`, or capsule kept as its own emitter? (Leaning: shared emitter +
  `CapStyle`.)
- **Voxel cone math:** `sdCappedCone` vs `sdRoundCone` for the tapered case — pick based on
  whether tapered ends should be flat or rounded in the field.
