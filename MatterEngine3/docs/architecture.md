# MatterEngine3 Architecture

**Premise:** procedural "parts" are authored in a JavaScript DSL, generated at very high
resolution offline, and baked into content-addressed, instanceable triangle assets that
should render at interactive rates. Generation runs once at full resolution (any
technique); everything downstream is mesh processing — bake-time subtree flattening plus
an error-bounded LOD ladder turn a whole part hierarchy into one instance per LOD.

## Pipeline at a glance

```
JS schema (.js)                                  authoring   (examples/*/schemas)
   │  ScriptHost (QuickJS, isolated ctx per bake)
   ▼
DslState ── voxel session ──► BuildBuffer (flat SDF brush ops)
        └── mesh session  ──► TriangleBuildBuffer (Tri + TriEx)
   │
   ▼  csg_lowering: BuildBuffer → LoweredField (particles/prims/ordered CSG stages)
   ▼  meshing (MatterSurfaceLib marching cubes) → triangles
   ▼  lod_bake: QEM decimation → LOD pyramid, each level a BLAS
   ▼
part_asset_v2: parts/<resolved_hash>.part        content-addressed artifact
   │  { materials, BLAS table (tris + TriEx), child-instance table, LOD levels }
   ▼  part_flatten (placed roots): merge whole subtree → ε-bounded LOD ladder
   ▼  part_cluster::split_clusters: spatial k-d split → ~16k-tri clusters
   ▼  per-cluster ε-ladder (decimate_to_error per cluster)
parts/<resolved_hash>.flat.part                  v3 format: cluster table + per-cluster LOD
   │
   ▼  world_lights: `light sun/sky/spot` lines in world.manifest → WorldLights
   ▼  probe_bake: CPU SH-L1 probes (sky + sun + spots) → cache/<world>.probes (PRB1)
   ▼  probe_texture: 2× RGBA8 3D textures (units 4/5) uploaded to raster.fs
   │
   ▼
viewer: PartStore flat-preferred load (v3 cluster ladders or v2 whole-part)
        → GpuCuller (cull.comp) per-cluster frustum + HiZ + LOD select
        → RasterComposer::draw_gpu_driven (glMultiDrawArraysIndirect, SSBO instancing)
        → probe-sampled forward lighting  (default; requires GL 4.6)
        fallback: WorldComposer → TLAS → raytrace (MATTER_RT=1, ~60s warm-up)
```

## Stage responsibilities

| Stage | Files | Job |
|---|---|---|
| ScriptHost | `include/script_host.h:35`, `src/script_host.cpp` | QuickJS sandbox; evaluates `build()`; hash authority (`compute_resolved_hash` = FNV-1a over folded source + canonical params + child hashes) |
| DslState | `include/dsl_state.h:66` | Transform stack, material/tint cursors, session state, RNG seeding, child-placement recording |
| CSG lowering | `src/csg_lowering.cpp` | Flat brush list → mesher field: additive spheres → particles, boxes → typed iso-prims, ordered Union/Difference/Intersection stages |
| Triangle emit | `include/triangle_emit.hpp:48` | Direct-mesh path: line tubes, spheres/boxes, capped cones, capsules, polygon extrude (parallel-transport frames, miter/bevel/round joins) |
| LOD bake | `src/lod_bake.cpp` | QEM edge-collapse. Ratio pyramid (`bake_lods`, keep {1.0, 0.1, 0.01}) for per-part bakes; error-bounded `decimate_to_error(ε)` + `reproject_triex` (TriEx carried via nearest-centroid re-projection) for the flatten ladder |
| Asset v2/v3 | `src/part_asset_v2.cpp:86` | Atomic serialize: materials, BLAS table, `ChildInstance[]` (hash + 4x4, 72 B), LOD levels. v3 extends this with a cluster table: each cluster carries its own AABB, radius, and per-level LOD indices |
| Flatten | `src/part_flatten.cpp` | Merge a root's whole subtree (transforms applied, TriEx carried, LOD0 of each part) into ONE mesh; build ε ladder (ε = radius/{256,64,16,4}, stop < 2000 tris); then invoke `split_clusters` to spatially partition into ~16k-tri clusters, bake a per-cluster ladder, and save as `<root>.flat.part` (v3) |
| Clusters | `src/part_cluster.cpp` | k-d median spatial split of a flat merged mesh → `ClusterSet` (cluster AABB, mesh slice, per-cluster ε-ladder); basis for per-cluster frustum cull + projected-size LOD in the raster path |
| World lights | `src/world_lights.cpp` | Parse `light sun/sky/spot` lines from `world.manifest`; produce `WorldLights` (sun dir/color, sky color, spot list). Defaults reproduce the Phase-1 hardcoded look for worlds without light lines |
| Probe bake | `src/probe_bake.cpp` | CPU SH-L1 probe volume: traces ray bundles from a grid of probes across the scene to accumulate sky ambient and sun visibility per cell; cached as `cache/<world>.probes` (PRB1 format). Invalidated when the lights fingerprint changes. Uploaded as two RGBA8 3D textures (ambient + dominant) to the raster shader (units 4/5) |
| PartGraph | `src/part_graph.cpp:96` | Dependency DAG: `static requires` discovery → memoized DFS with cycle detection → topo sort → children-first bake with cache hits |
| Live edit | `src/live_edit.cpp`, `src/inotify_watcher.cpp` | Debounced file watch → changed parts → upward ancestor cone → topo re-bake → re-flatten roots. Fail-closed with last-good artifact |

## Sessions: the two geometry modes

`Session::Voxels` and `Session::Triangles` are mutually exclusive (`dsl_state.h:14`).

- **Voxel session** (`beginVoxels(spacing)` … `endVoxels()`): sphere/box/capsule/cylinder
  brushes with postfix CSG retagging (`difference()` retags the last brush). Ordered,
  staged CSG — not bag-of-adds. Voxel `spacing` is the resolution floor. Meshed via
  marching cubes.
- **Mesh session** (`beginShape`/`vertex`/`endShape`, `extrude(path)`): direct triangles.
  No CSG. Carries **TriEx** per-triangle data.
- Verbs are session-polymorphic where sensible: `sphere()`/`box()` work in both modes.

## TriEx: the per-triangle detail channel

`TriEx` (MatterSurfaceLib `bvh.h`, 96 bytes, layout pinned by static_assert at
`part_asset_v2.cpp:20`) carries materialId, RGBA tint, 3 shading normals, packed AO.
This is where "hyper-detailed" lives: per-triangle material variation and baked AO.

On the flatten path TriEx survives **every** ladder level: `reproject_triex` copies
materialId/tint/uv/AO from the nearest source triangle (spatial hash over centroids) and
re-derives geometric normals. Only the legacy per-part ratio pyramid (`bake_lods`) still
drops TriEx on decimated levels — that path remains for non-flattened parts.

## Identity & determinism

- Everything is content-addressed: `resolved_hash = fnv1a(folded_source, canonical_params,
  sorted child_hashes)`. Shared-lib imports are transitively folded into the hash
  (`module_resolver::fold_sources`), so editing a library invalidates all importers.
- Seeded `Math.random`, canonical param ordering, NUL-separated source folds, and TriEx
  padding zeroing make re-bakes byte-identical.
- Cache: `parts/<16-hex>.part`; `PartGraph::install` skips cached hashes.

## Composition model

Compositional `.part`s still store a child-instance table (child hash + transform) — the
authoring/bake side stays hierarchical and content-addressed. At world-load time,
however, every **placed root** is flattened (`LocalProvider::connect` →
`part_flatten::flatten_part`): the whole subtree is merged into one mesh with an
ε-bounded LOD ladder, then spatially partitioned into ~16k-tri clusters
(`part_cluster::split_clusters`). Each cluster gets its own AABB, radius, and per-level
LOD mesh indices, and the result is saved as `parts/<root>.flat.part` (v3 format) with
an empty child table. The flat artifact shares the root's resolved hash, so invalidation
is free — any subtree change changes the hash and orphans the stale flat file.

**Raster path (default, GPU-driven):** PartStore loads the v3 flat artifact's
cluster table into GPU SSBOs (`GpuClusterMeta` for per-cluster bounds + LOD
metadata, and per-part vertex buffers). Each frame `GpuCuller::cull` uploads the
resolved instance stream (`GpuInstanceRec` SSBO), then a compute shader
(`shaders_gpu/cull.comp`) walks each instance × cluster, frustum-culls,
HiZ-culls against the previous-frame max-pyramid (`MATTER_HIZ`, default on),
picks a LOD by projected-size, and atomically appends the transform to that
(part, cluster, LOD) bucket's `DrawXforms` slice + increments its
`DrawArraysCmd::instance_count`. The CPU then issues a single
`glMultiDrawArraysIndirect`; the vertex shader (`raster_gpu_driven.vs`) reads
per-instance transforms via `gl_BaseInstance + gl_InstanceID`. There is no CPU
batch walk, no per-frame `UploadMesh`, and no batch-fingerprint cache. GL 4.6
is a hard requirement (compute + SSBO + indirect + `gl_BaseInstance` in
GLSL 460); `MATTER_GPU_CULL=0` opts out but is only sensible paired with
`MATTER_RT=1` (there is no CPU raster fallback). HUD shows GPU-emitted instances,
per-cluster frustum/HiZ kills, and drawn triangle total.

**RT fallback:** `WorldComposer` emits one TLAS leaf per flat root (empty child table →
no recursive expansion) or expands compositional parts recursively (depth cap 8, 200k
instance cap). TLAS rebuild is skipped when the instance fingerprint is unchanged.

## Known constraints (spotted in code)

- Params are flat (number/bool/string) — no nested objects (`part_graph.h:11`).
- PartGraph has no persistence; the DAG is recomputed per `install()`.
- Legacy ratio-pyramid LODs (non-flattened parts) still lose TriEx on decimated levels.
- QEM error-only decimation erodes OPEN mesh outlines for free (coplanar boundary
  collapses cost ~0); closed marching-cube surfaces are unaffected, and an
  eroded-to-empty result falls back to the input (`lod_bake.cpp`).
- QuickJS-ng v0.10.0 vendored; TriEx layout pinned to MatterSurfaceLib's bvh.h.
- TLAS is rebuilt from scratch when it rebuilds, but the rebuild is skipped when the
  (blas_handle, transform) instance set is unchanged (`world_composer.cpp`).
