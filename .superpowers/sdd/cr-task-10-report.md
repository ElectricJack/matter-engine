# Task 10 Report: MatterSurfaceLib Genuine Bug Fixes

## Status: COMPLETE

All fixes applied, MatterSurfaceLib compiles cleanly (only pre-existing `primitive_sdf` linker error
remains — `fat_primitive.c` is absent from the MSL standalone Makefile, unrelated to our changes).
MatterEngine3 test suite passes in full (both `run-partv2` and `run-script` targets).

---

## Bugs Fixed

### B1 — Dangling pointer after `realloc` in spatial hash (`open_particle_surface.c`)

`GetCellIndex` stored a raw `SpatialHashCell*` into the hash table, then grew the cell array with
`realloc`, invalidating the stored pointer. Fixed by storing the 1-based index as
`(void*)(intptr_t)(idx+1)` so that `idx==0` is distinguishable from NULL, and recovering the index
via `(int)((intptr_t)stored - 1)` at lookup time.

### B3 — Fixed 10 000-element `malloc` with no bounds check (`open_particle_surface.c`)

Every cell's particle array was allocated as `malloc(10000 * sizeof(int))` with no guard against
overflow. Added `particleCapacity` field to `SpatialHashCell` and a `CellParticleArrayEnsure`
helper that doubles capacity on demand using `realloc` with the safe temp-pointer pattern.

### B4 — TLAS draw-record / instance desync (`tlas_manager.cpp`, `tlas_manager.hpp`)

`build()` used `continue` to skip records whose BLAS was missing, leaving `draw_records_[i]` out of
sync with the compacted `tlas_->blas[i]` array. Added `active_draw_records_` member (cleared at the
top of `build()` and `clear()`); each successfully-added instance also pushes its record into
`active_draw_records_`. `generate_instance_texture_data` now indexes `active_draw_records_[i]`
instead of `draw_records_[i]`.

### B5 — `matrix_inverse` placeholder (`tlas_manager.cpp`)

The function simply returned `*m` (the input unchanged). Replaced with a full 4×4 cofactor/adjugate
inverse.

### B6 — Edge-hash corruption on probe exhaustion (`surface.c`)

After 100 failed probes `hashPos` still pointed to the original (occupied) slot, which was silently
overwritten, corrupting a foreign edge entry. A `foundSlot` boolean now tracks whether any empty slot
was found during probing; writes only occur when `foundSlot == true`. Downstream triangle emission
additionally guards against any `cellEdgeVertexIndices` value of `-1`, skipping those triangles.

### B8 — Unchecked `realloc` returns same pointer on OOM (`surface.c`)

`EnsureFieldCapacity`, `EnsureMeshCapacity`, and `EnsureHashTableCapacity` all wrote the `realloc`
result back into the original pointer, leaking the original block on OOM. All three now use the
temp-pointer pattern: `T* tmp = realloc(ptr, newSize); if (!tmp) { log error; return; } ptr = tmp;`.

### B11 — BVH analyzer registry leak with dangling pointers (`bvh_analyzer.cpp`, `bvh_analyzer.h`, `cell.cpp`)

`RegisterBVH` accumulated entries under names that included the triangle count
(`"Cell(x,y,z)_MatN_NNNtris"`), so every rebuild added a new entry while the previous BLAS pointer
became dangling after `release_blas`. Added `BVHReportManager::UnregisterBVH(name)`. In `cell.cpp`:
`commit_group_mesh` now uses a stable key (`"Cell(x,y,z)_MatN"`) and calls `UnregisterBVH` before
`RegisterBVH`; `clear_meshes` calls `UnregisterBVH` for each cell before calling `release_blas`.

### T4 — Silent 16-bit index truncation (`surface.c`, `mesh_simplifier.cpp`)

When `vertexCount > 65535` the `unsigned short` index buffer silently wraps, producing garbage
geometry. Both sites now print a `[ERROR]` message to `stderr` and return an empty mesh, making the
failure loud and obvious.

### Spatial-hash dedup guard (`spatial_hash.c`)

`sh_query_radius` and `sh_query_first` could return duplicate results when multiple grid coordinates
hashed to the same bucket. Ported the `unsigned char visited[HASH_TABLE_SIZE]` guard from the
existing `sh_query_radius_nearest` to both functions; each bucket is only visited once per query.

### `enableEdgeDeduplication` default mismatch (`surface.c`)

The comment above the field in `GetDefaultMeshConfig` said "Default: enabled" but the value was
`false`. Changed to `true` to match both the comment and the equivalent Task 7 fix.

### `triangle_count == 3` heuristic removed (`blas_manager.cpp`, `blas_manager.hpp`)

The implicit heuristic `if (triangle_count == 3) { bvh->subdivToOnePrim = true; bvh->Build(); }`
changed production behaviour for any real 3-triangle mesh and triggered a redundant second build.
Replaced with an explicit `bool force_subdiv_one_prim = false` parameter on all three
`register_triangles` overloads, threaded through to the BVH build. Default is `false` (no behaviour
change for callers that do not opt in). No MatterEngine3 test relied on the old heuristic.

### 16-bit node-packing asserts (`tlas_manager.cpp`)

Child indices are packed into a 32-bit `leftRight` field as two 16-bit halves. Added
`assert(leftChild <= 0xFFFFu)` and `assert(rightChild <= 0xFFFFu)` in
`generate_node_texture_data` to catch overflow early.

### `include/mc_tables.h` ODR hazard deleted

`MatterSurfaceLib/include/mc_tables.h` was an unused duplicate of `src/mc_tables.h` that declared
`int edgeTable[256]` without `static`, creating an ODR violation when included alongside the real
`src/mc_tables.h`. Removed via `git rm`.

### `object_allocator.c` — alignment fix (companion fix in stash)

`calculate_object_size` now rounds the stride up to `_Alignof(max_align_t)` so that objects within
each page are always naturally aligned, preventing undefined behaviour when the object type has
stricter alignment than `sizeof(ObjectHeader)`.

---

## Build Result

```
make WSL_LINUX=1 clean && make WSL_LINUX=1
```

All 28 source files compiled without errors. Warnings are pre-existing (unused parameters in
`bvh_analyzer.cpp`, `memset`/`memcpy` on non-trivial type in vendored BVH code). The sole link
failure (`undefined reference to 'primitive_sdf'`) is pre-existing: `fat_primitive.c` is not listed
in the MSL standalone Makefile and was absent before any of our changes (verified by `git stash` +
clean rebuild).

---

## Test Results

```
cd MatterEngine3 && GALLIUM_DRIVER=d3d12 make test
```

- `run-partv2`: **All part_asset_v2 tests passed**
- `run-script`: **ALL PASS**

No regressions introduced. The `enableEdgeDeduplication` change to `true` and removal of the
`triangle_count == 3` heuristic produced no test failures.

---

## Files Modified

| File | Changes |
|------|---------|
| `MatterSurfaceLib/src/open_particle_surface.c` | B1 (index-as-pointer), B3 (dynamic particle array), `#include <stdint.h>` |
| `MatterSurfaceLib/src/surface.c` | B6 (edge-hash probe guard + -1 skip), B8 (checked reallocs ×3), T4 (>65535 loud fail), dedup flag |
| `MatterSurfaceLib/src/spatial_hash.c` | Dedup guard for `sh_query_radius` and `sh_query_first` |
| `MatterSurfaceLib/src/tlas_manager.cpp` | B4 (active_draw_records_), B5 (matrix_inverse), 16-bit asserts |
| `MatterSurfaceLib/include/tlas_manager.hpp` | B4 — `active_draw_records_` member |
| `MatterSurfaceLib/src/blas_manager.cpp` | Remove `triangle_count==3` heuristic, add `force_subdiv_one_prim` |
| `MatterSurfaceLib/include/blas_manager.hpp` | `force_subdiv_one_prim` parameter on all three overloads |
| `MatterSurfaceLib/src/bvh_analyzer.cpp` | B11 — `UnregisterBVH` implementation |
| `MatterSurfaceLib/include/bvh_analyzer.h` | B11 — `UnregisterBVH` declaration |
| `MatterSurfaceLib/src/cell.cpp` | B11 — stable key, unregister-before-register, unregister in clear_meshes |
| `MatterSurfaceLib/src/mesh_simplifier.cpp` | T4 (>65535 loud fail) |
| `MatterSurfaceLib/src/object_allocator.c` | Alignment rounding in `calculate_object_size` |
| `MatterSurfaceLib/include/mc_tables.h` | **Deleted** (ODR hazard) |
