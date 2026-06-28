# Stateful DSL Completeness — Design

**Status:** Draft for review (2026-06-27)
**Context:** The procedural-part DSL is a stateful, Processing-style interface: a transform
stack, cursors (material, smoothing, simplify), and three mutually-exclusive *sessions*
(`None` / `Voxels` / `Triangles`). An audit of the implementation (`dsl_state.cpp`,
`dsl_triangle.cpp`, `dsl_bindings.cpp`, `part_base.js.h`, host finalizer in
`script_host.cpp`) against the authoring spec (`2026-06-24-dsl-procedural-geometry-design.md`)
turned up a set of gaps where the stateful model is incomplete: verbs that error in a
session where they *should* adapt, cursors the backend supports but the DSL never exposes,
and a transform-stack correctness bug. This doc collects those gaps and proposes a single
pass to close the tractable ones.

Depends on / pairs with `2026-06-27-typed-iso-primitives-design.md` (the capsule and
oriented-box primitives referenced below).

## Goal

Make the stateful interface *complete and consistent*: every cursor the backend honors is
authorable, every primitive verb behaves correctly under the active session (or fails with a
clear reason), and the transform stack applies uniformly to all emitted geometry. Explicitly
**out of scope**: the Lattice session (its own sub-project) and anything requiring a new
backend mesher.

---

## Current state (what the audit found)

| Capability | Session gating today | Notes |
|---|---|---|
| transform stack, `fill`, `simplify`, `placeChild`, `position` | any session | shared cursors — correct |
| `sphere` / `box` | **Voxels only** (error otherwise) | default op `Union`; postfix `union/difference/intersection` re-tag the **global** last op |
| `union/difference/intersection` | mutate `buffer_.ops.back()` | not session-scoped |
| `smoothing` | stamped over the whole voxel session at `endVoxels` | correct |
| `beginShape`/`vertex`/`endShape` | **Triangles** (proper guards) | mesh path |
| `line` | **None only** — errors in Voxels *and* Triangles | mesh-only swept tube (just landed) |

Host finalizer (`script_host.cpp:652-660`) already fail-closes on (a) any `set_error` during
build and (b) a **session left open** at build end. It does **not** check the transform stack
balance.

---

## Gaps and decisions

### G1 — `line()` is not session-polymorphic *(in scope)*

Today `line()` errors inside a voxel session. With a stateful interface the verb should adapt:

- **Triangles / None** → swept triangle tube (current behavior).
- **Voxels** → emit a **capsule iso-primitive** (`sdSegment(p) − r`) into the field, as an
  additive/carve brush like `sphere`/`box`.

**Decision:** make `line()` dispatch on `session_`. The voxel branch emits a capsule, which
is the second concrete `primitive_sdf` case defined in the typed-iso-primitives spec (one
analytic capsule, not N stamped spheres). This is the cleanest voxel "tube" and removes a
hard error that has no good reason to exist.

### G2 — brush radius/extents ignore transform-stack scale *(in scope — correctness bug)*

`lower_build_buffer` emits a sphere as `StaticParticle{ xf(transform, center), radius }` —
the center is transformed but **`radius` stays a raw scalar**. So `scale(2,2,2); sphere(c, r)`
meshes at radius `r`, not `2r`. Worse, the analytic oracle `field_is_solid` *does* respect
scale (it maps the sample point through the inverse transform, yielding an ellipsoid), and
`stamp_box` *does* respect scale (it transforms each stamped center). So the test oracle, the
box path, and the sphere path **disagree** about whether the transform stack scales a brush.

**Decision:** define one rule — the transform stack applies fully to brushes. Concretely, with
the typed-iso-primitive model every brush carries its `invTransform`, and `primitive_sdf`
evaluates the SDF in brush-local space (already how the box works). The sphere becomes
`sdSphere(invTransform·p, r)` — naturally picking up scale (and, for non-uniform scale, an
ellipsoid that matches the oracle). This folds the fix into the iso-primitive work rather than
special-casing radius scaling. **Open sub-question:** do we want true non-uniform ellipsoids,
or clamp to a uniform-scale factor on `radius`? Recommend honoring the full transform
(ellipsoid) so oracle and mesher agree by construction.

### G3 — CSG op verbs are not session-scoped *(in scope — robustness)*

`set_last_op` mutates `buffer_.ops.back().op` with only an empty-list guard. Across a session
boundary (e.g. a stray `difference()` after `endVoxels`, or before any brush in a new session)
it can mis-tag a brush from a *previous* session or fire a confusing error.

**Decision:** gate op verbs to an open voxel session and to the current session's brush range
(`session_start_`). `difference()` with no brush *in this session* is a clear error.

### G4 — no `tint()` cursor *(in scope)*

`TriEx`, `StaticParticle`, and the variation recorder all carry a `tint` (`Vector4`, alpha =
blend strength), but the DSL has no way to set it. Authors can pick a material id via `fill`
but cannot author per-fill color/tint variation (e.g. varying leaf/bark hue), even though the
whole pipeline already plumbs tint through to bake.

**Decision:** add a `tint(r,g,b,a)` cursor alongside `fill`, captured at emit onto each
brush/triangle (mirrors `material_`). Default `(1,1,1,0)` = neutral/no tint (unchanged
behavior). This is purely additive — a new cursor + capture, no backend change.

### G5 — `lookAt()` transform verb missing *(in scope, small)*

The authoring spec lists `lookAt()` among the transform-stack verbs; it is never bound. Cheap
to add as a matrix-stack helper (orient the current frame toward a target). **Decision:** bind
it.

### G6 — `placeChild` cannot pass variation params *(in scope)*

`placeChild(module)` records a child by name + current transform only. The variation system
(`VariationRecorder::instance`, the triangle-path-variations spec) folds *variation params*
into the child's resolved hash so parametric children dedup correctly — but the DSL exposes no
way to pass them. Parametric child instancing is unreachable from authored parts.

**Decision:** extend to `placeChild(module, params?)` where `params` is an optional plain
object/byte range folded into the child's resolved hash. Backwards compatible (no params =
today's behavior). **Open sub-question:** params serialization format (canonical JSON bytes vs.
a typed struct) so the resolved hash is stable across runs.

### G7 — build-end transform-stack balance not checked *(in scope, small)*

The host catches an open session at build end but not an unbalanced matrix stack (a `pushMatrix`
without `popMatrix` leaves `stack_.size() > 1`). A leaked push silently offsets nothing today
but masks authoring bugs.

**Decision:** add a build-end check — `stack_.size() != 1` at end of build is a misuse error,
same fail-closed path as the open-session check.

### G8 — `sphere`/`box` are not session-polymorphic *(in scope)*

Like `line`, these verbs should adapt to the active session instead of erroring outside the
voxel path:

- **Voxels** → SDF brush (current behavior — one iso-primitive, smooth-min unioned).
- **None (mesh mode)** → emit a **triangulated solid** directly into the `TriangleBuildBuffer`
  (sphere = a UV sphere; box = 12 triangles), captured under the current transform/material
  exactly like `line`.
- **Triangles (mid `beginShape`)** → error, same as `line` (a solid is its own primitive, not
  loose vertices for an open shape).

**Decision:** make `sphere`/`box` dispatch on `session_`, mirroring the `line` rule. The mesh
branch adds `TriangleBuildBuffer::sphere(center, r)` / `box(center, halfExtents)` emitters
(the UV-sphere tessellation already existed in the old `line` implementation and can be
reused). Because mesh-mode solids bake the transform-stack matrix into their vertices, they
pick up scale/rotation for free — which keeps them consistent with the G2 transform rule. This
means the three solid-ish primitives (`sphere`, `box`, `line`) all share one polymorphism
rule: voxel → iso-primitive, mesh → triangles, mid-shape → error.

### G9 — Lattice session *(deferred — separate sub-project)*

The third promised session (`beginLattice`/slot fills/read/mutate/`forEach`/`filledSlots`/
mesh-stencil fill/scatter) is entirely unimplemented. It is a substantial sub-project with its
own design surface and is **explicitly out of scope** for this completeness pass. Listed here so
the gap is tracked, not forgotten.

---

## Where the edits land

| Gap | File(s) |
|---|---|
| G1 polymorphic `line` (voxel→capsule) | `dsl_triangle.cpp` (dispatch), MSL capsule `primitive_sdf` (iso-primitive spec) |
| G2 brush transform/scale | `csg_lowering.cpp` + MSL `primitive_sdf` (iso-primitive spec) |
| G3 session-scoped op verbs | `dsl_state.cpp` (`set_last_op`), uses `session_`/`session_start_` |
| G4 `tint()` cursor | `dsl_state.{h,cpp}`, `dsl_bindings.cpp`, `part_base.js.h`; capture into `BuildOp`/triangles |
| G5 `lookAt()` | `dsl_state.{h,cpp}`, `dsl_bindings.cpp`, `part_base.js.h` |
| G6 `placeChild(module, params)` | `dsl_state.{h,cpp}`, `dsl_bindings.cpp`, `part_base.js.h`; fold into resolved hash |
| G7 stack-balance check | `script_host.cpp` finalizer |
| G8 polymorphic `sphere`/`box` (voxel SDF / mesh tris) | `dsl_state.cpp`/`dsl_triangle.cpp` (dispatch), `triangle_emit.{hpp,cpp}` (`sphere`/`box` emitters) |
| On-disk `.part` format | **unchanged** |

This is the same "bake-time only" story as the iso-primitives spec: every change is authoring/
lowering state. Nothing alters the serialized artifact format. After landing, **clear the cache
once** (`rm -rf viewer/cache`) so unchanged schemas re-bake with the new semantics (the resolved
hash keys on script source, not the engine version).

---

## Goals / Non-goals

**Goals**
- `line()` session-polymorphic (mesh tube / voxel capsule).
- Transform stack applies uniformly to all brushes (sphere matches box matches oracle).
- CSG op verbs scoped to the current voxel session.
- `tint()` cursor; `lookAt()` transform; `placeChild(module, params)` variation.
- Build-end transform-stack balance check.
- `sphere`/`box`/`line` share one polymorphism rule: voxel → iso-primitive, mesh → triangles,
  mid-shape → error.

**Non-goals (deferred)**
- Lattice session and all its sub-features (separate sub-project).
- Per-fill `mergeGroup` override (additive, later — per the DSL spec's own non-goals).
- New shape primitives beyond the capsule/box from the iso-primitives spec.

---

## Open questions (resolve during planning)

- **G2 scale semantics:** full non-uniform ellipsoid (oracle-consistent) vs. uniform-scale
  factor on `radius`? Recommend the former.
- **G6 params encoding:** canonical-JSON bytes vs. typed struct for the variation-hash fold;
  must be deterministic across runs to preserve content-addressing.
- **G4 tint vs. material default:** when both a material default tint and an authored `tint()`
  are present, which wins / how do they combine (alpha-as-blend already implies override)?
- **G1 capsule caps/material:** does a voxel `line` carry per-end radius taper (like the mesh
  tube) into the capsule SDF, or a single radius? Tapered capsule = a rounded cone SDF — decide
  whether to support taper now or start with a constant-radius capsule.
- **G5 `lookAt` convention:** up-vector handling and whether it composes with or replaces the
  current frame's rotation.
