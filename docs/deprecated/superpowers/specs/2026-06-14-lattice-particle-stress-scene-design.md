# Lattice Particle Stress Scene + Per-Particle Tint — Design

Date: 2026-06-14
Project: MatterSurfaceLib

## Goal

Add a new test scene that builds a large lattice (64 × 64 × 128) of jittered
particles in three material types — two opaque and one glass — where every
particle carries a slightly different tint. The scene is meant to push the ray
tracer: a lot of geometry plus a complex (glass) material. Visually it should
read as "a brick made of individual particles": particles that just touch and
fuse into a connected mass, with organic, non-gridded drift and varying overlap.

This requires one engine change (per-particle tint) plus the scene itself.

## Part 1 — Per-particle tint

Today a particle's color comes entirely from its material's `albedo`, looked up
by `materialId` from a 64-entry GPU material table. There is no per-particle
color. We add a per-particle `tint` (RGBA) that blends with the material albedo:

```
finalAlbedo = mix(material.albedo, tint.rgb, tint.a)
```

Default tint is `(1, 1, 1, 0)` → `mix` returns the material albedo unchanged, so
existing behavior is untouched everywhere the tint isn't set.

### Data path

1. **Particle structs.** Add `tint` (4 floats, RGBA) to `StaticParticle`
   (`include/cluster.h`) and to the cell-side particle list used during meshing.
   `Cluster::add_particle` gains an overload accepting a tint; the existing
   signature defaults to neutral `(1,1,1,0)`.

2. **Per-triangle carry.** Add `float4 tint` to `TriEx` (`include/bvh.h:25`).
   In `Cell::generate_mesh_for_group` (`src/cell.cpp` ~394–406), where each
   triangle is already tagged with its nearest particle's `materialId`, also copy
   that same nearest particle's `tint` into the triangle's `TriEx.tint`.

3. **GPU packing.** The triangle GPU texture uses 6 texel rows per triangle
   (`src/blas_manager.cpp:388`). Row 0's `.w` holds the packed material id
   (`pack_material_w`, `blas_manager.hpp:74`); rows 1–5 currently write `0.0f`
   into their `.w`. Pack the tint into the spare `.w` slots:
   - row 1 `.w` = tint.r
   - row 2 `.w` = tint.g
   - row 3 `.w` = tint.b
   - row 4 `.w` = tint.a
   - row 5 `.w` stays free.

   No new rows, no texture growth. Add a `pack_tint_w(triex, index, channel)`
   static helper mirroring `pack_material_w`, so packing/sentinel logic is
   unit-testable without a GL context (the neutral default when `triex` is null
   must reconstruct to `(1,1,1,0)` so untinted meshes are unchanged).

4. **Dedup identity.** Tint must participate in the BLAS hash and equality
   (`calculate_hash`, `triangles_equal` in `blas_manager.cpp`) alongside the
   existing per-triangle `materialId`, so two byte-identical meshes carrying
   different tints are not deduplicated into one.

5. **Shader.** In `shaders/raytrace_tlas_blas.fs`, where the shader already reads
   row-0 `.w` for the material id, also read rows 1–4 `.w` to rebuild the tint,
   then apply `albedo = mix(materialAlbedo, tint.rgb, tint.a)` before shading and
   before the transmission tint (`attenuation *= albedo`, currently fs:487).

### Why spare `.w` slots instead of a new texel row

Adding a 7th row grows every triangle's GPU footprint and the texture height for
all meshes, tinted or not. The existing rows already carry unused `.w` channels,
so reusing them is zero-cost and keeps the stride at 6.

## Part 2 — The lattice scene

A new method `setup_lattice_scene()` in `MatterSurfaceLib/main.cpp`. The current
`setup_matter_system()` call is commented out (prototype — no env-var gating, no
scene switch). Constants live at the top of the method for easy tweaking.

### Geometry

- Lattice dimensions: 64 × 64 × 128 (≈ 524k particles).
- `base_radius` and `spacing` chosen so `spacing ≈ 2 × base_radius` — neighbors
  just touch (hybrid look: visible individual sphere bumps on a connected mass).
- **Perlin-noise displacement:** each lattice point is offset by a noise-derived
  vector (sample a 3D noise field at the lattice coordinate to produce x/y/z
  offsets), giving smooth coherent drift, bent rows, and varying overlap rather
  than a perfect grid.
- **Per-particle white noise:** small additional random jitter on radius and a
  fine random position jitter on top of the Perlin drift.
- Deterministic seed (`SetRandomSeed`) so runs are reproducible for benchmarking.

Noise source: prefer raylib/stb_perlin if a 3D scalar noise call is available in
the build; otherwise a small self-contained gradient/value-noise helper local to
the scene file. Decided at implementation time.

### Materials

- Two **opaque** materials sharing one **merge group** → they fuse into a single
  connected brick mass, distinguished by their own albedo/roughness/metallic plus
  per-particle tint.
- One **glass** material in its **own** merge group → it carves clean pockets
  through the opaque mass via the existing transparent-carve path.
- If suitable shared-group opaque materials don't already exist in
  `src/material_registry.c`, add two (same `mergeGroup`, `translucency = 0`). The
  glass material (translucent, its own group) already exists (id 4).

### Tint and material assignment

- Every particle gets a small random RGBA tint: random `rgb` with **alpha ≈ 0.2**
  (subtle variation around the base material color).
- Material mix: ~**15% glass**, the rest split between the two opaque materials.
  Chosen per-particle from the deterministic RNG.

## Part 3 — Verification

- **Unit test** (headless, alongside existing `tests/` suites such as the BLAS
  and material-registry tests): cover `pack_tint_w` round-trip (including the
  neutral default for a null `triex`) and confirm tint participates in BLAS
  hash/equality (different tint ⇒ different hash / not equal).
- **Build:** `cd MatterSurfaceLib && make` builds the app; `./build-all.sh test`
  stays green.
- **Visual / benchmark:** run with `MSL_CAPTURE=<out.png>` for a headless
  screenshot to confirm tint variation and glass rendering; interactive run to
  exercise render-mode toggles (raytrace / solid / wireframe / debug BVH) and
  sanity-check performance under the full lattice.

## Out of scope

- No env-var scene switcher or separate executable (prototype: one method, the
  old setup commented out).
- No continuous/unbounded per-particle color beyond the RGBA-blend tint.
- No changes to the marching-cubes resolution or LOD system.
