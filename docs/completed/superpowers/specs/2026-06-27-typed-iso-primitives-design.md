# Typed Iso-Primitives & Ordered CSG — Design

**Status:** Draft for review (2026-06-27)
**Context:** The voxel/SDF authoring path (`beginVoxels` … `sphere`/`box` … `endVoxels`)
currently lowers every brush into **sphere particles** for a metaball/SDF mesher that
only understands spheres. A box has no native representation: `stamp_box`
(`MatterEngine3/src/csg_lowering.cpp`) fakes one by filling its volume with a cubic grid
of overlapping spheres. The direct-triangle `line()` primitive had the same disease — it
built a tapered tube as a chain of UV spheres (the "beaded" look). This doc defines a
**typed iso-primitive** model so boxes (and future shapes) are first-class SDF brushes,
plus the **ordered-CSG** evaluation needed to make add/subtract/add sequences correct.

Related: `2026-06-24-dsl-procedural-geometry-design.md` (the authoring DSL),
`2026-06-15-organic-surface-carving-design.md` (carve/clip field model),
`2026-06-24-part-artifact-v2-design.md` (the on-disk `.part` format this does **not**
touch).

## Goal

Represent non-sphere brushes (starting with an **oriented box**) as native iso-primitives
the mesher evaluates with a real per-shape SDF — instead of stamping sphere soup. The model
must:

- Leave the sphere hot path (hundreds of thousands of particles) **byte-for-byte untouched**.
- Extend to future shapes (cylinder, cone, capsule, …) without combinatorial code growth.
- Preserve **CSG operation order** (`add → subtract → add` must keep the later adds).
- Keep allocation predictable (no per-shape array explosion; single-pass, `reserve`-able).
- Require **no change to the on-disk `.part` format** and no part-file migration.

---

## Why this is entirely a bake-time concern

The system has three layers; only the last layer's **output** is serialized.

| Layer | Lives where | Owns | Lifetime |
|---|---|---|---|
| **1. Authoring ops** | `DslState::buffer_` — `std::vector<BuildOp>` (`dsl_state.h`) | one ordered, kind-tagged op list | one bake |
| **2. Lowering** | `LoweredField` (`csg_lowering.h`) → mesher inputs | particle/primitive arrays | one bake |
| **3. Meshing** | MSL `surface.c` field eval + `SurfaceScratch` | spatial hash, scratch buffers | reused per worker |

`save_v2` (`part_asset_v2.h`) writes the **baked triangle geometry** (`BLASManager` + BVH),
the `TLASManager`, the `ChildInstance` table, and the LOD levels — keyed by resolved hash.
By the time anything hits disk, spheres and boxes have already been dissolved into a
marching-cubes triangle mesh. **No primitive layer is ever serialized.**

Consequence: this entire redesign changes only in-memory bake structures. It needs **no
format-version bump and no `.part` migration**. The one caveat is the cache key —
`compute_resolved_hash(source, params, child_hashes)` hashes the *script source*, not the
mesher version — so after landing these changes, an unchanged schema still hashes to its old
key but would bake different geometry. **Clear the cache (`rm -rf viewer/cache`) once after
landing** to force re-bake; this matches the existing schema-edit workflow.

---

## Layer 1 already solves ordering and allocation

`BuildBuffer.ops` is a single `std::vector<BuildOp>`; every `sphere()`/`box()` appends one
op (amortized geometric growth). `BuildOp` already carries a `BrushKind kind` tag plus a
`CsgOp op` and *both* `radius` (sphere) and `halfExtents` (box) inline. So at the authoring
layer:

- **Emission order is the source of truth** — the ordered op list preserves
  `add → subtract → add` exactly.
- **Inline param-union is fine here** — the op count is one-per-`build()`-call, not the
  exploded particle count, so a few dozen bytes per op is irrelevant.

The split into separate per-shape arrays is purely a **Layer-2 derivation** of this ordered
list. Adding a future primitive type = append a new kind-tagged `BuildOp`; no new authoring
structures.

---

## Decision 1 — `line()` is a swept tube (landed)

`TriangleBuildBuffer::line()` (`MatterEngine3/src/triangle_emit.cpp`) no longer beads UV
spheres. It now emits a real swept tube: a ring of `segments` vertices at each end (radii
`r0`/`r1`), a quad band for the wall, and fan caps — hollow, smooth-tapered, outward-wound.
This is the direct-triangle path and is independent of the iso-primitive work below, but it
was the same "shape made of spheres" anti-pattern and is fixed.

---

## Decision 2 — typed iso-primitive with SDF dispatch

Give iso brushes a `kind` discriminator and a single `primitive_sdf(prim, point)` that
switches on kind. Sphere stays the implicit common case (`|p − c| − r`); box is the first
explicit case (`sdBox` of the point mapped into box-local space). Adding a shape = one enum
value + one `case`. Carve and clip (today sphere-only linear scans) call the *same* dispatch,
unifying three field paths.

`sdBox` already exists in `csg_lowering.cpp` (used only by the `field_is_solid` test oracle);
the math moves into MSL's `primitive_sdf`.

---

## Decision 3 — two arrays, sphere stream left alone

The mesher splits its inputs into:

- **The existing sphere array** — unchanged struct (`StaticParticle`/`Particle`), unchanged
  spatial hash, unchanged smooth-min union. Zero regression risk on the 99% path.
- **A new small "fat-primitive" array** — entries carry a `kind` tag, a bounding sphere
  (`center`, `boundRadius`) for bounds/queries, `materialId`, `tint`, and a **param blob**
  sized to the largest shape (box: `halfExtents` + `invTransform`).

Rationale for *not* unifying into one tagged array:

- Inlining a transform onto every sphere particle would **double the bandwidth of the
  hottest loop** for a feature spheres never use.
- A side table indexed from the sphere stream scatters cache lines for the common case.
- Fat primitives are **few by construction** (one box op = one entry), so a separate small
  array stays resident in cache and its union-sized param blob is negligible waste.

The fat array is **not** inserted into the spatial hash. Following the existing carve/clip
idiom, the field eval does the sphere hash-union exactly as today, then **linear-scans the
small fat array** and folds each `primitive_sdf` into the same combinator. Fat primitives
also expand the group's mesh bounds so marching cubes covers them.

If some future shape ever became both huge *and* numerous, split that one kind into its own
dense array then — not worth pre-solving.

---

## Decision 4 — ordered CSG stages

**Today's limitation (pre-existing):** `lower_build_buffer` sorts ops into two buckets by
type — `Union` → additive, `Difference` → carve — and the field eval runs a *fixed* pipeline:
union all additive (hash + smooth-min) → subtract all carve (`ApplySubtractField`) → clip.
The original interleaving is discarded, so `add → subtract → add` already subtracts the box
from the later adds. This is wrong for the workflow we want and is **not** caused by the
two-array split — flat "bag of adds / bag of subtracts" arrays inherently throw order away
(CSG is non-commutative: `(A − B) ∪ C ≠ (A ∪ C) − B`).

**Fix:** replace the two buckets with an **ordered list of CSG stages** derived from the
ordered `BuildOp` list. Each stage carries its `op` and a content set (a batch of spheres
and/or fat primitives). Per sample:

```
field = +inf
for each stage in order:
    d = stage.sdf(p)              // smooth-min over the stage's spheres + fat prims
    Union:        field = smin(field, d)
    Difference:   field = smax(field, -d)
    Intersection: field = smax(field, d)
```

Performance is preserved two ways:

1. **Consecutive same-op ops merge into one stage** at lowering time (union is commutative
   within a run), so a big additive blob with a few carves is only a handful of stages.
2. **One spatial-hash query, bucketed by stage** — tag each hashed particle with its stage
   index; query once, accumulate per stage, fold stages in order. Cost over today is a few
   per-sample stage accumulators instead of one min + one max.

Carve and clip become special cases of a stage, so this *unifies* the three field paths
rather than adding a fourth.

---

## Decision 5 — allocation model

- **Layer 1:** one amortized `std::vector<BuildOp>`; append per emit. (unchanged)
- **Layer 2:** lowering is a **single pass** over the ordered ops. Because an oriented box is
  now **one** fat primitive (not a cubic sphere stamp), the lowered array sizes are
  **predictable (≈ op count)** — lowering can `reserve()` the sphere and fat arrays up front
  and never reallocate. (Today `stamp_box` makes the additive vector's final size unknowable,
  so it churns through reallocations.)
- **Layer 3:** the mesher **borrows** the arrays (`MeshContext` holds
  `const std::vector<Particle>&` and raw `const Particle*` + counts) and owns only the
  **reused per-worker `SurfaceScratch`** (spatial hash + buffers). Arrays are immutable during
  meshing — lowering completes before meshing, so nothing grows mid-mesh.

No new growable allocator is introduced anywhere.

---

## Where the edits land

| Change | File(s) |
|---|---|
| `line()` swept tube (landed) | `MatterEngine3/src/triangle_emit.cpp`, `include/triangle_emit.hpp` |
| `PrimKind` enum + fat-primitive struct + `primitive_sdf()` | MSL `surface.c` / new header |
| Fat-primitive array + counts threaded through `MeshContext` | MSL `meshing_algorithm.h`, `cell.cpp` |
| Stage loop in field eval (replaces union-then-subtract) | MSL `surface.c` (`CalculateScalarAndMaterial`, `ApplySubtractField`) |
| Bounds expansion for fat primitives | MSL `cell.cpp` |
| Emit one oriented box per brush; delete `stamp_box`; `reserve` arrays | `MatterEngine3/src/csg_lowering.cpp` |
| On-disk format | **unchanged** (`part_asset_v2`) |

**Scope note:** the field evaluator lives in **MatterSurfaceLib**, which we treat as
read-only. This is a genuine feature (the mesher cannot represent a box otherwise), so the
MSL edits are justified — flagged here as a deliberate scope decision rather than a silent
change.

---

## Goals / Non-goals

**Goals**
- Oriented box as a native iso-primitive with a real `sdBox`, additive and carve.
- Sphere hot path unchanged (struct, hash, union math).
- `primitive_sdf` dispatch that extends to new shapes with one `case` each.
- Ordered CSG stages so `add → subtract → add` is correct.
- Single-pass, `reserve`-able lowering; reused per-worker scratch.
- No `.part` format change; cache-clear-once on landing.

**Non-goals (deferred)**
- Shapes beyond box (cylinder/cone/capsule/torus) — slot in later as new `primitive_sdf`
  cases + `BuildOp` kinds. Capsule/cylinder/cone are now specced in
  `2026-06-27-primitive-library-expansion-design.md` (which builds on this model); torus
  remains deferred.
- Putting fat primitives in the spatial hash (linear scan is enough while counts are small).
- Splitting a fat kind into its own dense array (only if one ever gets huge *and* numerous).
- Per-stage independent spatial hashes (single bucketed query is sufficient).

---

## Open questions (resolve during planning)

- **Box orientation encoding** in the param blob: full inverse `Matrix`, or inverse-rotation
  3×3 + center (smaller, reconstruct translation from `center`)? Both avoid per-sample
  inversion; pick on size vs. simplicity.
- **`boundRadius` for a scaled/sheared box:** bounding sphere of the transformed half-extents
  — confirm the transform stack only ever applies rotation+uniform-ish scale here, else the
  bound must account for shear.
- **Stage granularity vs. smoothing:** the whole-expression `smoothing` k currently applies
  to the additive union; define how `k` behaves *across* stage boundaries (per-stage k vs.
  global) so smooth-min/΄max stay registered.
- **Material/tint on carve stages:** carve currently only adjusts the scalar; confirm whether
  an oriented-box carve needs to participate in material selection or stays scalar-only.
- **Stage count cap / diagnostics:** worst-case authoring (alternating add/subtract) defeats
  stage merging — decide whether to cap stages or just document the per-sample cost.
