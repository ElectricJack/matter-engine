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
parts/<resolved_hash>.flat.part                  same v2 format, empty child table
   │
   ▼
viewer: PartStore flat-preferred load → WorldComposer (1 instance per flat root,
        child expansion only as fallback) → TLAS (rebuilt only on change) → shader RT
```

## Stage responsibilities

| Stage | Files | Job |
|---|---|---|
| ScriptHost | `include/script_host.h:35`, `src/script_host.cpp` | QuickJS sandbox; evaluates `build()`; hash authority (`compute_resolved_hash` = FNV-1a over folded source + canonical params + child hashes) |
| DslState | `include/dsl_state.h:66` | Transform stack, material/tint cursors, session state, RNG seeding, child-placement recording |
| CSG lowering | `src/csg_lowering.cpp` | Flat brush list → mesher field: additive spheres → particles, boxes → typed iso-prims, ordered Union/Difference/Intersection stages |
| Triangle emit | `include/triangle_emit.hpp:48` | Direct-mesh path: line tubes, spheres/boxes, capped cones, capsules, polygon extrude (parallel-transport frames, miter/bevel/round joins) |
| LOD bake | `src/lod_bake.cpp` | QEM edge-collapse. Ratio pyramid (`bake_lods`, keep {1.0, 0.1, 0.01}) for per-part bakes; error-bounded `decimate_to_error(ε)` + `reproject_triex` (TriEx carried via nearest-centroid re-projection) for the flatten ladder |
| Asset v2 | `src/part_asset_v2.cpp:86` | Atomic serialize: materials, BLAS table, `ChildInstance[]` (hash + 4x4, 72 B), LOD levels |
| Flatten | `src/part_flatten.cpp` | Merge a root's whole subtree (transforms applied, TriEx carried, LOD0 of each part) into ONE mesh; build ε ladder (ε = radius/{256,64,16,4}, stop < 2000 tris); save as `<root>.flat.part` with an empty child table |
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
ε-bounded LOD ladder, saved as `parts/<root>.flat.part`. The flat artifact shares the
root's resolved hash, so invalidation is free — any subtree change changes the hash and
orphans the stale flat file. The viewer's PartStore prefers the flat artifact (stored
ladder used directly, empty child table); a flattened Tree is therefore ONE TLAS
instance per frame instead of hundreds. Per-frame recursive child expansion
(`viewer/world_composer.cpp`, depth cap 8, 200k instance cap) remains only as the
fallback for parts without a flat artifact, and the TLAS rebuild is skipped entirely
when the instance set fingerprint is unchanged frame-over-frame.

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
