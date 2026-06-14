# Material-Aware Surfacing Design

Date: 2026-06-14
Project: MatterSurfaceLib
Status: Approved (pending spec review)

## Goal

Support distinct materials that do **not** fuse into one blob, while still letting
appearance variants of the *same* material merge. The motivating case: a channel of
glass running through a metal surface, rendered correctly in the ray tracer (the metal
forms a clean cavity, the glass fills it and refracts), versus several shades of stone
that should merge into one continuous surface.

This builds directly on the existing per-material meshing in `cell.cpp` and the
per-particle-radius + smooth-min metaball field in `surface.c`.

## Current behavior (baseline)

- `cell.cpp` buckets particles by `materialId` into `material_particle_indices` and
  meshes each material separately (`generate_mesh_for_material`, cell.cpp:261): its own
  SDF field, its own isosurface, its own BLAS. The metaball smooth-min blend only runs
  *within* one material.
- Consequence 1 — **different material types already do not merge.** But because each is
  an independent closed metaball surface, where two types meet their surfaces bulge into
  each other and **overlap** (no clean shared wall) — the ray tracer hits whichever is
  nearer, producing z-fighting / double surfaces at the boundary.
- Consequence 2 — **no concept of shade.** Materials are keyed by exact `materialId`, so
  two shades of stone with different ids would be split into separate non-merging
  surfaces.
- Material reaches the ray tracer **per-instance**: the shader reads `inst.materialId`
  (`raytrace_tlas_blas_processed.fs:451,690`); each cell's per-material mesh is one BLAS =
  one instance = one material. The per-vertex colors `surface.c` writes via
  `GetMaterialColor` are not consumed by the ray tracer.
- Material properties (albedo, roughness, metallic, emission, **translucency**, **ior**,
  flatShading) are hardcoded in an `if/else` chain in `materials.glsl` keyed by
  `materialId`. `isMaterialTranslucent` already exists. A `materialId >= 1000000`
  convention is used to flag smooth shading.

## Design overview

Four pieces, the first two of which are reusable foundations:

1. **Single-source material registry** — one authored table, consumed by both CPU
   (meshing decisions) and GPU (shading).
2. **Per-triangle material** — lets a single merged mesh carry multiple appearance rows.
3. **Cross-group clip field (the carve)** — makes two groups meet on a shared wall
   instead of overlapping, gated by transparency.
4. **Meshing flow + render wiring** to tie it together.

### Identity model

- A particle stores exactly one id: its **registry row** (`materialId`, already present on
  both `StaticParticle` in `cluster.h` and the surfacing `Particle` in `particle.h`). No
  new per-particle field is added; `materialId` is reinterpreted as "registry row".
  `stone_light` and `stone_dark` are simply two rows.
- The **registry row carries a `mergeGroup`** field. Rows sharing a `mergeGroup` blend
  together (metaballs); rows in different groups never blend. `stone_light` and
  `stone_dark` share `mergeGroup = stone`; `steel` and `glass` each have their own group.
- **Meshing groups by `mergeGroup`** (resolved through the registry), not by raw
  `materialId`.
- **Transparency** for the carve decision is read per group from the registry
  (`translucency > 0`). A merge group is assumed to be a single optical class.

## Component 1 — Single-source material registry

One authored definition; two consumers; the GLSL `if/else` table is removed.

**Authoring source (CPU):** a material table. Each row holds the fields
`MaterialProperties` already defines — `albedo`, `roughness`, `metallic`, `emission`,
`translucency`, `ior`, `flatShading` — plus a new `mergeGroup` id. This is the only place
materials are defined.

**CPU consumer:** meshing reads `translucency > 0` to decide carving
(`material_type_is_transparent(mergeGroup)` / per-row lookup) and resolves
`mergeGroup` from each particle's `materialId`.

**GPU consumer:** the table is uploaded to the shader as a fixed-size
`MaterialProperties` uniform array (`MAX_MATERIALS`), refreshed when dirty — consistent
with how the renderer already pushes data, and simpler than a texture for a small bounded
count. `getMaterialProperties(id)` becomes an array index instead of a branch chain.

**Cleanup enabled:** the `materialId >= 1000000` smooth/flat-shading hack is removed —
`flatShading` is just a table field.

**Sync note:** because the table now lives in one place and is uploaded to the GPU, the
duplicated GLSL definitions are deleted (not kept in sync — eliminated).

## Component 2 — Per-triangle material

A single merge-group mesh can contain triangles of different rows (light vs dark stone),
but the ray tracer reads material per-instance. To render merged shades, material must be
carried per triangle.

- During marching cubes, assign each output triangle the `materialId` of the **nearest
  particle at the triangle centroid**. This reuses the dormant `materialField` path in
  `surface.c`, resolved once per triangle (a single lookup) rather than per vertex.
- Carry that id into the BLAS: add a `materialId` to the per-triangle data (`TriEx`, or a
  parallel array) so it survives into the GPU triangle buffer.
- Shader: on hit, set `result.material` from the **triangle's** id and index the registry
  with it, instead of `inst.materialId`. The per-instance id remains the
  fallback/default when a mesh carries no per-triangle data.
- The per-triangle nearest-row lookup uses only the group's own particles, never the clip
  set (Component 3).
- Cross-*group* materials (steel vs glass) are already separate BLAS instances, so
  per-instance would have sufficed for them; per-triangle is specifically what unlocks
  *within-group* shade variation.

## Component 3 — Cross-group clip field (the carve)

The carve makes two groups meet on a shared wall instead of overlapping. It is a clip
applied to one group's SDF using the other groups' fields.

**Per cell, when meshing merge-group `G`:**

1. Gather **clip particles** = particles of every *other* group present in (or within
   blend-reach of) this cell that is **carving-relevant** to `G`. A pair is relevant iff
   `G` is transparent **or** the other group is transparent. Opaque↔opaque pairs are
   skipped → no clip → harmless hidden overlap.
2. Compute `G`'s own field `f_G(p)` exactly as today (per-particle radius + smooth-min
   metaball blend, within the group).
3. Compute the foreign field `f_O(p)` = **hard min** over clip particles of
   `dist − radius` (no cross-group blending — different materials must not fuse).
4. **Clip:** where `f_O < f_G` (a foreign surface is nearer), force `G` outside by
   raising its sample, `f_G ← max(f_G, −f_O)`, so `G`'s isosurface terminates exactly on
   the locus `f_G == f_O`.

Because both `G` and its neighbor clip to the *same* equidistant locus, their walls land
**coincident** — glass's back-face and steel's cavity front-face occupy the same surface
with opposite normals. Glass, clipped on all foreign sides, closes into a watertight
manifold (front + back), so refraction works. Steel gets a clean front facing the glass.

**LOD consistency:** clip particles use the same LOD-effective radii (`r_eff`,
taper/cull) the group's own particles use, so the interface stays stable across LOD.

**Coincident-triangle handling:** truly coincident triangles can z-fight under ray
tracing. Start with the existing ray-spawn epsilon (`ro + N*1e-4`). If artifacts appear,
add a per-side clip bias (`f_O ± ε`) so glass sits microscopically inside steel's cavity.

## Component 4 — Meshing flow and API

**`cell.cpp`:**

- Bucket particles by `mergeGroup` (via the registry) instead of raw `materialId`.
  `material_particle_indices` becomes keyed by group; `add_particle_index` /
  `remove_particle_index` look up the group.
- `generate_mesh_for_material` → `generate_mesh_for_group`: builds the group's `Particle`
  list as today (with `r_eff` taper/cull), **plus** a second list of foreign clip
  particles from carving-relevant neighbor groups in this cell, each at its `r_eff`.
- Everything downstream — per-group BLAS, boundary seam-locking, `ComputeSurfaceNormals`
  reapply for simplified meshes — is unchanged.

**`surface.c` API:**

- Extend `GenerateMesh` (and `GenerateMeshWithConfig`) with an optional clip set:
  `Particle* clipParticles, int clipCount`. When `clipCount == 0`, behavior is identical
  to today (zero change for single-group cells — preserves the metaball + LOD work).
- `CalculateScalarAndMaterial` computes `f_G` as now, then, if clip particles exist,
  computes `f_O` (hard-min) and applies `f_G ← max(f_G, −f_O)` before returning.
- `ComputeSurfaceNormals` takes the same clip set so the simplified-mesh normal reapply
  matches the clipped surface.

## Component 5 — Render-side interface & transparency

- Per-triangle material flows into the BVH triangle fetch; on hit,
  `result.material = triangleMaterialId`. `getMaterialProperties` indexes the uploaded
  registry. Per-instance id stays as the fallback.
- Refraction through the shared wall is handled by the existing translucency/IOR path,
  now sourced from the registry: a ray refracting through glass and hitting the coincident
  steel front-face shades as steel ("see the metal through the glass channel").
- Coincident-surface handling: ray-spawn epsilon first, optional clip bias second.
- No new transparency data: `translucency`/`ior` already exist per row.

## Watertightness

- **Within-group:** unchanged — a single continuous clipped scalar field per group keeps
  marching cubes manifold; same-level seam locking still applies.
- **Cross-group interface:** both sides sample the same equidistant locus → coincident,
  gap-free.

## Test plan

Extend `MatterSurfaceLib/tests/`:

1. **Shade merge:** two rows, same `mergeGroup` → one connected mesh; triangles carry both
   row ids (per-triangle material populated).
2. **Transparent carve:** glass + steel adjacency → glass forms a closed manifold; steel
   terminates at the interface; no overlap beyond ε; interface coincident.
3. **Opaque/opaque:** two opaque groups adjacent → no carve; each full union (overlap
   allowed, hidden).
4. **Registry single-source:** CPU `material_type_is_transparent` agrees with the uploaded
   table; the `clipCount == 0` path is byte-identical to today (regression guard for the
   metaball/LOD work).
5. Existing `mesh_continuity_tests` and `mesh_simplifier_tests` still pass.

## Out of scope (future work)

- Per-vertex (barycentric) shade blending — this design resolves material once per
  triangle.
- Cross-LOD-level interface stitching — carve correctness is specified for same-level
  neighbor cells (consistent with the existing seam guarantee).
- Mixing transparent and opaque rows inside one merge group.
