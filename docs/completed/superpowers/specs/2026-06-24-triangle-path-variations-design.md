# SP-6 — Direct-Triangle Path & Variations — Design

**Status:** Approved for planning (2026-06-24)
**Project:** MatterEngine3
**Parent:** `2026-06-24-procedural-part-authoring-design.md` (sub-project SP-6)
**Consumes:** SP-2 (`ScriptHost`, build buffer, lowering → BLAS), SP-1 (`.part`),
SP-3 (params→hash dedup).
**Implements (subset of):** `2026-06-24-dsl-procedural-geometry-design.md` — the
direct-triangle session and `instance(child, variation)`.

## Goal

Add the **direct-triangle emission session** to the script host and wire up **variations**.
Two capabilities:

1. **Direct triangles** — `beginShape(type)` / `vertex(...)` / `endShape()` and `line(a,b)`
   with a **skinning radius** on 1D primitives — emitting triangles straight into the
   part's geometry, **merged into the same baked per-cell BLAS** as the voxel path, carrying
   **per-triangle materials**.
2. **Variations** — a part instanced with different params yields a different baked
   artifact, deduped by content hash (consistent with SP-3).

## All geometry → one part BLAS (the unifying decision)

The settled model: **every emission mode is converted to meshes and cached in the part's
BLAS.** There is **no separate triangle BLAS and no parallel triangle render path.** Direct
triangles **merge into the part's baked per-cell BLAS geometry** alongside whatever the
voxel session produced, and **materials are expressed per-triangle** (`TriEx`), so one part
BLAS can hold many materials.

Why this (correcting an earlier separate-BLAS idea): the renderer is a single raytraced
triangle-BLAS pipeline (per the sector-LOD design's "single render pipeline"). Keeping all
of a part's geometry in one BLAS means one instance = one TLAS leaf regardless of how the
geometry was authored, LOD decimation operates on the whole part uniformly, and there's no
second shader path to maintain. Per-triangle materials are already how the mesher tags
surface material (`TriEx`), so triangles just feed the same machinery.

## Direct-triangle session

Surface form (from the DSL spec):

- `beginShape(type)` / `vertex(x,y,z)` / `endShape()` — emit triangles/strips/etc. for the
  given primitive type, transformed by the current matrix, tagged with the current material
  cursor (per-triangle).
- `line(a, b)` and `LINES`-type shapes carry a **skinning radius** (`lineThickness(r)` or a
  radius arg) — **not** a stroke color. At emission the host **tubes** the 1D primitive into
  solid geometry (stepped spheres along the segment, radius lerpable end-to-end), which is
  how concise L-system trunk/branch authoring stays solid. This produces triangles that go
  into the same BLAS.

### Thin surfaces, no field interaction

- Direct triangles are **thin surfaces**: they are emitted as authored, with **per-triangle
  material**, and **do not participate in SDF/field CSG**. They don't blend, carve, or
  smooth-min against voxel brushes — they're literal geometry.
- Overlap between a direct-triangle pass and a voxel pass resolves at **surface time** by
  the material/`mergeGroup` rules (per the DSL spec's sequential-session merge), but the
  triangles themselves are not re-fielded — they are placed triangles in the same BLAS.

### Merge into the build buffer

- The direct-triangle session writes into the **same C++ build buffer** the voxel session
  uses (SP-2). At bake, the buffer's triangles + the voxel-lowered surface mesh are combined
  into the part's per-cell BLAS via the existing surfacer/BLAS path.
- Sessions remain **mutually exclusive at any instant** but **sequential** within a part
  (DSL spec): a part may run a voxel pass then a triangle pass; both flush into the one
  artifact.

## Variations

- `instance(childPart, variation)` records a **child-instance entry** (child reference +
  current transform) — already the SP-1 child-instance table; SP-2/SP-3 handle the
  reference, SP-4 expands it.
- **A "variation" is a parameterization.** `variation` selects params (e.g. a seed or a
  named variant) passed to the child part. Because the child's `resolved_hash` folds its
  params (SP-2/SP-3), **each distinct variation is a distinct cached artifact, deduped by
  content hash** — identical variation params across many instances collapse to one baked
  part; different variations bake separately.
- **Params are bound at instance time** (the parent supplies them when recording the child
  instance), consistent with SP-3's params→hash dedup. This is the same mechanism for "100
  rocks, 8 unique shapes": 8 variation param-sets → 8 baked artifacts → many instances.
- **Variation is independent of LOD** (SP-4): every variation gets the same ~3 LOD levels,
  selected by the same screen-size rule. Variation chooses *which* geometry; LOD chooses
  *how detailed*.

## Data flow

```
build():
  beginVoxels(...) ... endVoxels()     → CSG → lowered surface mesh ─┐
  beginShape(...) vertex... endShape() → triangles (per-tri material)├→ one build buffer
  line(a,b) w/ radius                  → tubed triangles ────────────┘
  instance(child, variation)           → child-instance record (params→hash)
            │
            ▼
  bake: build buffer → surfacer → ONE per-cell part BLAS (many materials via TriEx)
        + child-instance table → save_v2 (SP-1)
```

## Testing

Headless `tests/triangle_variation_tests`:

- **Triangle emission:** a `beginShape`/`vertex`/`endShape` triangle appears in the baked
  BLAS with the current material as its per-triangle material; transform stack applied.
- **One BLAS:** a part mixing a voxel sphere + a direct triangle quad produces a **single**
  part BLAS containing both, with distinct per-triangle materials (no second BLAS).
- **Skinned line:** `line(a,b)` with radius r tubes into solid geometry (stepped spheres);
  lerped radius produces a tapered tube; output is triangles in the BLAS.
- **No field interaction:** a direct triangle overlapping a voxel brush is **not** smoothed/
  carved into it (the triangle survives as authored).
- **Variation dedup:** instancing a child with the same variation params N times → one
  cached artifact, N child-instance records; different variation params → distinct
  artifacts.
- **Variation/LOD independence:** two variations get identical LOD level structure (with
  SP-4 present) — or, GL-free, identical LOD-array shape after SP-4 bake.

## Goals / Non-goals

**Goals**
- Direct-triangle session (`beginShape`/`vertex`/`endShape`, `line` + skinning radius),
  per-triangle materials, merged into the part's single per-cell BLAS.
- Thin surfaces with no SDF/field interaction; sequential-session merge into one artifact.
- `instance(child, variation)` → child-instance records; variation = params bound at
  instance time, content-hash deduped; independent of LOD.

**Non-goals (deferred)**
- A separate triangle BLAS or parallel triangle render path (explicitly rejected).
- Re-fielding triangles into the SDF (they stay literal geometry).
- Lattice session (later sub-project) and its scatter (`filledSlots`/`slotsMatching`).
- `pointsOnSurface` mesh scatter (can land with lattice/scatter work).
- LOD generation/selection (SP-4); world flatten (SP-4).
- Per-fill `mergeGroup` override (DSL-spec non-goal).

## Open questions (resolve in planning)

- Skinning-radius units/encoding: constant vs. per-end lerp (prototype's stepped-sphere
  width) and step density vs. cost.
- How per-triangle materials from direct triangles coexist with voxel-surface materials in
  the same cell BLAS at surface time (cross-group carve vs. coexist) — confirm against the
  material-aware-surfacing rules referenced by the DSL spec.
- Whether tubed lines reuse the voxel sphere-brush path (stepped spheres = particles) or a
  dedicated triangle tuber — the former unifies more but pulls lines toward the field path.
- `variation` argument shape: raw params object vs. a named-variant indirection resolving to
  params.
