# Rasterization Switch: Clustered Instanced Meshes + Baked Probe Lighting — Design

**Status:** Approved for planning (2026-07-02)
**Builds on:** bake-time subtree flattening + error-bounded LOD ladder (shipped on
`feat/voxel-box-imposter`); supersedes the ray tracer as the primary renderer.
**Parks (does not delete):** the voxel-box imposter path
(`2026-06-22-voxel-box-imposter-design.md`) — baked `.vxi` assets and tests stay on the
branch, unused.

## Goal

Make forward **instanced rasterization** the viewer's primary renderer and demote the
fragment-shader ray tracer to a **baker and reference oracle**. Everything on screen is
one representation: **instanced, clustered, vertex-colored mesh** from the flatten
ladder. Lighting is **fully baked** into a world-space probe volume traced offline:
sky irradiance, sun visibility, emissive **mesh lights**, and static **spotlights** —
evaluated per pixel with SH-L1, modulated by per-vertex self-AO. No runtime shadow work,
no per-pixel marching, no BVH traversal in the frame loop.

## Architecture (one paragraph)

At bake time, each placed root's subtree is flattened (existing), then **spatially split
into clusters** (~16k tris each); each cluster gets its own error-bounded LOD ladder with
**locked boundary edges** so adjacent clusters at different levels stay watertight. The
flat artifact (format v3) stores the cluster table. A GL-free CPU **probe bake** traces
the flattened world with the same BVH structures the tracer uses: per grid cell it
accumulates SH-L1 irradiance (sky + emissive surfaces + spotlights from a world light
list) and a sun-visibility scalar, serialized as `cache/<world>.probes`. At runtime the
viewer builds one raylib `Mesh` per (cluster, level), and each frame culls clusters
against the frustum, selects a ladder level per cluster by projected size, and issues one
`DrawMeshInstanced` per (cluster, level) batch. The forward shader looks up the material
table, samples the probe volume at the world position (SH-L1 vs. the surface normal),
applies analytic sun × baked sun visibility, and multiplies by per-vertex AO and tint.
Instancing survives fully baked lighting because the lighting lives in **world space**,
not on the shared surface.

## Tech stack

C++17, raylib/rlgl + OpenGL 3.3 core. `DrawMeshInstanced` + custom forward shader
(`LoadShader`). Probe volume uploaded via a ~30-line direct-GL 3D-texture shim
(`glTexImage3D`; rlgl has no 3D-texture API). CPU probe bake is GL-free and
multithreaded. Existing GPU ray tracer retained behind a toggle.

---

## Goals / Non-goals

**v1 goals**
- Rasterization is the default renderer; ray tracer available via `MATTER_RT=1` for A/B.
- Cluster split + per-cluster ε ladders + flat artifact v3.
- CPU probe bake: SH-L1 irradiance (sky + mesh lights + spotlights) + sun visibility.
- World-level **light list** as the single source of truth for sun/sky/spotlights,
  shared by baker, raster shader, and reference tracer.
- Mesh lights via the existing material `emission` channel (materials.glsl slot [5]).
- Per-vertex self-AO (existing TriEx bake) modulating the ambient term.
- Frustum culling + per-cluster LOD selection + batch reuse on static frames.
- Headless A/B screenshot parity workflow (same `MATTER_CAM`/FIFO tooling).

**Explicit non-goals for v1**
- Deleting the ray tracer or the voxel-imposter code (parked, not removed).
- Texture baking of any kind (ground textures, imposter cards, high→low-poly appearance
  bakes) — deferred; the probe-bake seam and material model don't block them.
- Crisp dynamic shadows (CSM) and *dynamic* local lights — baked lights are static;
  crisp spotlight cones blur to probe-cell resolution by design in v1.
- GPU probe baking (the bake sits behind a narrow interface; a GPU producer is a
  drop-in later — worth it for interactive relighting during live-edit).
- Nanite-style cluster DAG / hierarchical cluster LOD (v1 = flat cluster list,
  boundary-locked ladders).
- MatterSurfaceLib changes beyond genuine bug fixes (read-only convention).

---

## Components

Each GL-free and unit-testable unless noted.

### 1. Cluster split — `MatterEngine3/src/part_cluster.cpp` (new)

- In: flattened `Tri[] + TriEx[]`. Recursive spatial median split (longest AABB axis)
  until each cluster ≤ `target_tris` (default 16k, tunable); out: per-cluster triangle
  ranges + AABBs. Small meshes → exactly 1 cluster (degenerate case ≡ current behavior).
- Per cluster: ε ladder via existing `decimate_to_error` + `reproject_triex`, with
  `lock_boundary = true`. Locked borders give watertight seams across mismatched
  neighbor LODs (and incidentally stop open-mesh QEM edge erosion).
- Invariants: every input triangle in exactly one cluster; boundary vertices
  bit-identical across a cluster's levels; deterministic re-split.

### 2. Flat artifact v3 — extend `part_asset_v2.{h,cpp}`

- `.flat.part` gains a cluster table: per cluster `{aabb, per-level blas_entry ranges,
  per-level screen_size_thresholds}`. Version guard bumped; a v2 flat fails the guard
  and regenerates (content-addressing unchanged — same `<hash>.flat.part` path).
- Compositional `.part` v2 untouched.

### 3. World light list — manifest extension

- `WorldManifest` gains lights: sun (direction, color/intensity), sky (color/model
  params), spotlights (position, direction, cone inner/outer, color, range).
- Single source of truth consumed by: probe baker, raster shader uniforms, and the
  reference tracer (replacing its hardcoded sun/sky) so A/B comparisons are meaningful.
- Folded into the probe-cache fingerprint: light edits → probe re-bake.

### 4. Probe bake — `MatterEngine3/src/probe_bake.cpp` (new)

- Interface (the GPU-swappable seam):
  in = world BVH (CPU TLAS/BLAS) + grid spec + light list;
  out = `ProbeVolume { bounds, nx, ny, nz, sh_l1_rgb[], sun_vis[] }`.
- Per cell: cosine-weighted hemisphere rays accumulate SH-L1 RGB irradiance from the
  procedural sky **and emissive surface hits** (mesh lights, via material `emission`);
  spotlights evaluated analytically (cone falloff × one shadow ray per cell per light);
  sun-disk rays → scalar visibility.
- Storage (v1 compact encoding — full SH-L1 RGB is 12 coefficients, too fat for two
  RGBA8 textures): texture A = ambient `c0.rgb` + `sun_vis` in .a; texture B = dominant
  incoming-light direction (luminance-weighted L1, xyz remapped to [0,1]) + directional
  intensity in .a. Shader evaluates ambient + directional·max(N·dir, 0). If colored
  directionality or banding proves visually necessary, the fallback is three RGBA8
  textures (true SH-L1 RGB) — a bake/shader-local change behind the same interface.
- Multithreaded; fixed-seed deterministic. Serialize `cache/<world>.probes` keyed by
  FNV-1a over (placed instance set, grid spec, light list); atomic write.
- Grid: world AABB padded by one cell; default cell size 1.0 unit (tunable).

### 5. RasterMeshStore — extend `viewer/part_store.{h,cpp}`

- `LoadedPart` gains `clusters[]`, each holding one raylib `Mesh` per ladder level,
  built non-indexed from that level's BLAS entries. TriEx → standard Mesh channels:
  `normals` ← shading normals, `colors` ← tint RGBA, `texcoords` ← (materialId,
  vertex AO). Owns Mesh GPU lifetime (upload/unload).
- BLAS-texture registration retained (reference tracer + probe bake still consume it).
- v2 flats (single ladder) load as one cluster; non-flattened parts keep the
  compositional fallback (whole-mesh LOD0 per node).

### 6. RasterComposer — `viewer/raster_composer.{h,cpp}` (new)

- Per frame: resolver roots → per instance × per cluster: transform cluster AABB →
  frustum test → projected size → ladder level → append instance transform to the
  (cluster, level) batch.
- Batch reuse: fingerprint over (mesh id, transform) set; unchanged frame → reuse
  transform arrays without rebuild (same trick as the TLAS skip).
- Caps: 200k instances, warning on overflow (parity with TLAS path).

### 7. Forward shader — `viewer/shaders/raster.vs` / `raster.fs` (new)

- VS: instanced `mat4` attribute; world position/normal out; passthrough color +
  (materialId, AO).
- FS: material from the existing 64×12 uniform table; probe sample at world position
  (two trilinear 3D-texture fetches, SH-L1 evaluated against N); ambient × AO × albedo
  + sun N·L × baked sun visibility + material emission; same tonemap as the tracer.
- Fallback uniform path when no probe volume is loaded: flat sky ambient (never black).

### 8. Integration — `viewer/main.cpp`, `renderer.cpp`

- Raster default; `MATTER_RT=1` renders the old fullscreen traced pass instead.
- 3D-texture shim (direct GL) for probe upload. HUD: batch count, drawn tris,
  per-level cluster counts, both paths' frame times.
- `LocalProvider::connect`: flatten (now +clusters) as today, then probe bake on
  fingerprint miss. Live reload: changed hashes re-flatten; manifest/light changes
  re-bake probes; MeshStore rebuilds only changed parts.

Dependency order: 1→2→5→6; 3→4→7; 8 last.

---

## Data flow

**Load:** bake/reconcile parts → per placed root missing a v3 flat: flatten → cluster
split → per-cluster ladders → save. Probe fingerprint miss → CPU bake → save →
upload 3D textures. MeshStore builds cluster meshes.

**Frame:** resolver roots → cull/select per cluster → batches (reused when
fingerprint-unchanged) → one `DrawMeshInstanced` per batch → forward shading →
procedural-sky clear color from the light list.

## Error handling / edge cases

- Missing/failed `.probes` → flat-ambient fallback uniform, warning printed; never black.
- Part without flat artifact → compositional fallback drawn through the same batch path.
- Degenerate cluster or eroded ladder level → skip level, clamp to finest available.
- Probe sampling outside grid → clamp-to-edge; grid padded one cell beyond world AABB.
- Empty light list → sky-only bake (today's look).
- Overflowing instance cap → truncate + warn (parity with TLAS path).

## Performance expectations

Tree world @1280×720: ~84 ms traced → fill-rate-bound instanced raster (sub-ms GPU at
this scale). Cost scales with *drawn* (LOD-bounded) triangles, not TLAS breadth or
per-pixel ray steps. The ~60s traced-shader warm-up leaves the default path — restarts
become cheap; the FIFO workflow remains useful for camera scripting. Probe bake:
~64×32×64 cells × ~64 rays ≈ 8M rays, seconds on multithreaded CPU, cached thereafter.
Memory: probe volume is two RGBA8 3D textures (couple of MB); cluster meshes duplicate
BLAS geometry on GPU as vertex buffers (accepted in v1; BLAS textures still resident for
the reference tracer).

## Testing strategy

**GL-free unit tests:**
- Cluster split: exact partition, AABB containment, size caps, locked-boundary
  bit-identity across levels, determinism.
- Flat v3: round-trip; v2 file fails version guard → regenerate path exercised.
- Probe bake: cell under occluder plane → sun_vis ≈ 0; open sky → ≈ 1; inside closed
  box → near-black; emissive quad lights a neighboring cell; spotlight cone in/out
  cells; fixed-seed determinism; `.probes` round-trip.
- RasterComposer (synthetic fixture): near/far level selection, behind-camera cull,
  unchanged-frame batch reuse.

**Visual/headless:** raster vs tracer A/B screenshots (identical `MATTER_CAM`), Tree
world: silhouettes + materials match; lighting soft-vs-traced judged by eye. HUD perf
recorded. Windows build rebuilt + smoke-run after engine changes.

## Phasing (each independently shippable; tracer A/B throughout)

1. **Raster MVP** — existing single-cluster flats via Mesh + DrawMeshInstanced, flat
   ambient + unshadowed sun. Proves the pipeline and the perf claim.
2. **Probe volume** — light list, CPU bake, 3D textures, SH-L1 shading, mesh lights +
   spotlights. Full v1 lighting.
3. **Clusters** — split, v3 format, per-cluster LOD selection.
4. **Ground content** — dense generated ground part exercising clusters at scale.

## Open questions (non-blocking)

- Probe cell size default (start 1.0 unit; judge leaking/softness in phase 2).
- RGBA8 vs RGBA16F probe textures (start RGBA8; revisit if banding).
- Cluster target size (start 16k tris; tune in phase 3).
- Whether coarse cluster borders (locked = undecimated) need the cluster-DAG follow-on
  sooner than expected — measured in phase 3/4.
- If crisp spotlight cones matter, a runtime spot + shadow map is the follow-on; not v1.
