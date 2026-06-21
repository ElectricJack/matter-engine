# Part Serialization (`.part` deep cache) — Design

**Date:** 2026-06-20
**Status:** Approved, ready for implementation planning
**Project:** MatterSurfaceLib

## Problem

We are building a generic **part** generation system. A part is a procedurally
generated object reduced to baked acceleration structures (BLAS geometry + BVH,
TLAS instances, materials). The brick is the first concrete part, and it is now
complex enough that generating it is expensive: occupancy grid → particle
emission → parallel marching-cubes meshing → per-cell BVH build → GPU texture
packing. This runs on every launch.

We want to **serialize a fully baked part to disk and load it instead of
regenerating**, so iteration is fast and a future imposter-research sibling
project can consume a ready-to-traverse part. The format must be part-kind
agnostic: it stores baked acceleration structures, not anything brick-specific,
so any current or future part type serializes through the same path.

This spec covers serialization only. Imposter rendering is a separate sub-project
with its own spec; it will `-I../MatterSurfaceLib/include` and call the loader
defined here.

## Terminology

- **Part** — any procedurally generated object serialized through this system.
  The brick is the first part; nothing in the format is brick-specific.
- **Part asset / `.part` file** — the on-disk baked representation of a part.
- **Gen params** — the parameters that drive a part's generation and form its
  cache key. Each part kind supplies its own params; the brick's are the lattice
  tunables (see §3).

## Goals

- Skip the expensive regeneration on load (marching cubes + per-cell BVH build)
  for any part kind.
- Auto-cache keyed on the part's generator parameters; no manual save/load steps.
- Robust regenerate-on-mismatch via a format version, struct-layout guard, and
  content hash. A bad/old/corrupt file is never trusted — it is silently
  regenerated and overwritten.
- Expose a clean, part-kind-agnostic loader the imposter sub-project can reuse.

## Non-Goals

- Editable loaded parts. A loaded part is **render-only** (see §5).
- Cross-architecture / cross-endian portability. We are only ever on WSL/Windows
  x86-64 little-endian. Layout changes are caught and trigger regeneration, not
  conversion.
- Serializing the scene-level TLAS that instances many parts together (different
  structure; out of scope, door left open — see §4).

## Decisions (settled during brainstorming)

- **Depth: Deep.** Serialize final CPU-side acceleration structures, not source
  params or intermediate meshes. Load skips all generation.
- **Trigger: Auto cache.** Hash gen params; load if a matching file exists,
  else generate + write.
- **Code location: Option A.** Loader lives in `MatterSurfaceLib/include` +
  `src`, reused by siblings via `-I../MatterSurfaceLib/include`.
- **TLAS: rebuilt on load, not serialized** (§4).
- **GPU data textures: re-packed on load, not serialized** (§4).

## 1. Code Layout

New files in `MatterSurfaceLib/`:

- `include/part_asset.h` — `PartGenParams`, save/load API, format constants.
- `src/part_asset.cpp` — implementation.

Dependencies are only `bvh.h`, `blas_manager.hpp`, `tlas_manager.hpp`,
`material_registry.h`, all already in `MatterSurfaceLib/include`. The API is
part-kind agnostic: it takes a `BLASManager` + `TLASManager` + the gen-param hash
and knows nothing about how the part was generated.

## 2. On-Disk Format

Single binary file: `parts/<param_hash>.part`. Raw POD array dumps, little-endian.

```
Header
  magic            u32  'PART' (0x50415254)
  format_version   u32
  param_hash       u64                       // cache key, see §3
  sizeof_Tri       u32                        // layout guards
  sizeof_TriEx     u32
  sizeof_BVHNode   u32
  content_hash     u64                        // FNV-1a over all bytes after header

Materials
  count            u32
  MaterialDef[count]                          // snapshot for validation

BLAS table
  blas_count       u32
  per BLAS:
    hash           u32                        // BLASEntry geometry hash
    ref_count      u32
    tri_count      u32
    nodes_used     u32
    Tri[tri_count]
    TriEx[tri_count]
    BVHNode[nodes_used]
    triIdx         u32[tri_count]

Instances
  inst_count       u32
  per instance:
    blas_index     u32                        // index into BLAS table above
    material_id    u32
    transform      f32[16]                     // row-major, DrawRecord.transform
```

We do **not** store the GPU data textures or the TLAS node tree. Nothing in the
format encodes the part kind — it is purely baked acceleration data.

### Validation on load (any failure → ignore file, regenerate, overwrite)

- `magic` mismatch
- `format_version` != current
- any `sizeof_*` != current `sizeof(...)` (struct layout changed)
- recomputed `content_hash` != stored (corruption)
- `param_hash` in header != requested hash (defense in depth; filename already
  encodes it)

## 3. Cache Key

Each part kind supplies a gen-params struct that exists in one hashable place and
drives both generation and the cache key. For the brick, the lattice tunables
currently live as locals inside `setup_lattice_scene()` (main.cpp:579–597);
extract them into `PartGenParams` (brick variant):

- DIM_X/Y/Z, SPACING, BASE_RADIUS
- POS_JITTER, RADIUS_VAR, VOID_AMT, vein params
- material ids (MAT_OPAQUE_A/B, MAT_GLASS)
- simplification ratio
- RNG seed

`param_hash = FNV1a(PartGenParams bytes) ^ format_version`.

**Startup flow (per part):**

1. Build the part's gen params.
2. Compute `param_hash`; path = `parts/<param_hash>.part`.
3. If the file exists and passes all §2 validation → load it, skip generation.
4. Otherwise → generate via the existing pipeline, then write the file.

A "force regenerate" path is not required for normal use (changing params changes
the hash → new file), but the generate-and-write branch is the same code the
fallback uses, so regeneration is always available by deleting the file or
bumping `format_version`.

## 4. What We Skip vs. Rebuild on Load

- **BLAS geometry + BVH: fully restored, no rebuild.** This is the expensive part.
  `register_triangles()` today *builds* the BVH internally, which we must bypass.
  Add:

  ```cpp
  BLASHandle BLASManager::register_prebuilt(
      const Tri* tris, const TriEx* triex, int tri_count,
      const BVHNode* nodes, uint nodes_used, const uint* tri_idx,
      uint32_t hash, uint32_t ref_count);
  ```

  It constructs a `BLASEntry` with a `BvhMesh` (tri + triEx) and a `BVH` whose
  `bvhNode`, `triIdx`, and `nodesUsed` are filled directly from the file —
  skipping `BVH::Build()`. It registers the entry in `entries_` and
  `hash_to_entry_` and marks textures dirty, mirroring the existing register path
  minus the build.

- **TLAS tree: rebuilt on load (cheap).** The `TLASNode` tree is just indices into
  the `BVHInstance` backing array, and every `BVHInstance` holds a raw `BVH*` plus
  cached `invTransform`/`bounds` that are invalid across runs and must be
  re-linked regardless. So we serialize instance records only and replay them via
  `TLASManager::draw_batch()` (mapping saved `blas_index` → freshly registered
  `BLASHandle`) followed by `build()`. Even an O(N²) agglomerative build over a
  part's instances is tens of ms vs. seconds of meshing.

- **GPU data textures: re-packed on load (ms).** Call the managers'
  `ensure_gpu_textures_ready()` from the restored CPU structs, exactly as the
  normal pipeline does.

**Door left open:** the only case where serializing a TLAS earns its keep is a
huge instance count where build time dominates — that is a future **scene-level**
TLAS that instances many parts together, a different structure from a single
part's internal TLAS. If it ever bites, an optional TLAS section can be added
behind the same `format_version` guard without disturbing this format.

## 5. Scope of a Loaded Part (render-only)

Load reconstructs only the **render state**: `BLASManager` + `TLASManager` +
material table. It does **not** rebuild the source generation layer (for the
brick, the `Cluster`/`Cell`/particle layer). A loaded part therefore renders
instantly but the live tuning sliders (for the brick: lumpiness/carve/
simplification) do not operate on it. To tune, delete the cache file (or change
params, which changes the hash) and let the normal pipeline regenerate and
re-save. This is the intended tradeoff: load to view fast, regenerate to edit.
(Confirmed acceptable.)

## 6. Save / Load Walkthrough

**Save** (after a normal generation completes):

1. Walk `BLASManager::get_entries()`. For each entry emit hash, ref_count,
   `triangles` (Tri), `mesh->triEx` (TriEx), `bvh->bvhNode[0..nodesUsed)`,
   `bvh->triIdx[0..tri_count)`. Record `handle → blas_index`.
2. Walk `TLASManager::get_draw_records()`. For each emit
   `{blas_index(map[handle]), material_id, transform.m[16]}`.
3. Snapshot the material table (`MaterialRegistryCount()` + `MaterialRegistryGet`).
4. Compute `content_hash`, write header + sections to `parts/<param_hash>.part`
   (temp file + rename for atomicity).

**Load:**

1. Read + validate header (§2). On any failure return false → caller regenerates.
2. Read materials; validate against the live registry (count + bytes). Mismatch →
   fail (materials are code-defined; a snapshot mismatch means the file is stale).
3. For each BLAS, read arrays and call `register_prebuilt(...)`; build
   `blas_index → new BLASHandle` map.
4. Read instances; `TLASManager::draw_batch()` with mapped handles + transforms +
   material ids; then `build(blas_manager)`.
5. `ensure_gpu_textures_ready()` on both managers.

## 7. Testing

A headless `tests/part_asset_tests` mirroring the existing `tests/*` suites:

- **Round-trip:** build a small synthetic `BLASManager` (a couple of registered
  triangle sets) + `TLASManager` (a few instances) + a material snapshot, save,
  load into fresh managers, assert Tri / TriEx / BVHNode / triIdx / instance bytes
  are identical and `content_hash` matches.
- **Layout guard:** write a file, then corrupt a `sizeof_*` / `format_version`
  field and assert load rejects it (so the caller regenerates).
- **Corruption guard:** flip a byte in a data section and assert `content_hash`
  rejects it.
- **prebuilt vs built parity:** register the same geometry via `register_triangles`
  (builds BVH) and via `register_prebuilt` (loads saved BVH) and assert identical
  node/triIdx arrays, proving the bypass installs an equivalent BVH.

## Open Items

None. All decisions settled in brainstorming.
