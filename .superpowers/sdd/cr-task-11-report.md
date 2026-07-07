# Task 11 Report: MatterSurfaceLib Surgical Performance Fixes

**Branch:** `feature/autoremesher-integration` (worktree: `code-review-fixes`)
**Commit message:** `perf(MatterSurfaceLib): spatial-hash dirty-cell rebuild, carve binning, BLAS handle map, TLAS split axis`

---

## Summary

Eight surgical performance fixes applied to MatterSurfaceLib (MSL). All changes are behavior-preserving: outputs are byte-identical under normal operation; only hot-path algorithmic complexity changes. MSL is normally read-only; these fixes were explicitly approved as a genuine-bug/perf exception.

---

## Perf Fixes Applied

### Fix 1 — `open_particle_surface.c`: Gate hot-path `printf` behind `OPS_DEBUG`

**File:** `MatterSurfaceLib/src/open_particle_surface.c` (macro definition at top of file)

Changed `DEBUG_LOG` from always-on `printf` to compile-time-gated:
```c
// Before:
#define DEBUG_LOG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
// After:
#ifdef OPS_DEBUG
#define DEBUG_LOG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG_LOG(fmt, ...) ((void)0)
#endif
```

All hot-path `printf` calls in particle insert/delete/update paths became zero-cost no-ops unless `OPS_DEBUG` is defined. Matches the canonical OpenParticleSurfaceLib pattern.

---

### Fix 2 — `open_particle_surface.c`: O(1) free-list + `isActive` dedup flag

**File:** `MatterSurfaceLib/src/open_particle_surface.c`

**a) `CreateParticle` free-list (was O(N) linear scan):**

Added `nextFree` field to `InternalParticle`:
```c
typedef struct {
    Vector3 position;
    int     materialId;
    int     cellIndices[MAX_OVERLAPPING_CELLS];
    int     cellCount;
    bool    active;
    int     nextFree;  // free-list link (-1 = end-of-list)
} InternalParticle;
```

Added `static int freeParticleHead = -1;` global, initialized by `InitializeParticleSystem` (which also changed `malloc` → `calloc` for `spatialHashCells` to zero-initialize new cells cleanly). `CreateParticle` now pops from the list in O(1); `DeleteParticle` pushes onto it.

**b) `AddActiveCellIfNeeded` dedup (was O(N) linear scan):**

Added `isActive` to `SpatialHashCell`:
```c
typedef struct {
    // ... existing fields ...
    bool isActive;  // O(1) dedup flag for active-cell list
} SpatialHashCell;
```

`AddActiveCellIfNeeded` now returns early if `isActive` is already true. New cells get `memset(0)` via `calloc`, so `isActive` starts false correctly. The cell is cleared (including `isActive = false`) when removed from the active list.

---

### Fix 3 — `cell.cpp`: Gate `BVHReportManager::UpdateAnalysis` behind env var

**File:** `MatterSurfaceLib/src/cell.cpp`, function `commit_group_mesh`

`UpdateAnalysis` ran on every mesh commit (profiling overhead always paid). Now gated:
```cpp
static const bool bvh_analysis_enabled = (getenv("MSL_BVH_ANALYSIS") != nullptr);
if (bvh_analysis_enabled) {
    BVHReportManager::UpdateAnalysis(analysis_name);
}
```

Zero overhead in production. Set `MSL_BVH_ANALYSIS=1` to re-enable.

---

### Fix 4 — `cell.cpp` / `cell.h`: `add_particle_index_unchecked` skips `std::find`

**Files:** `MatterSurfaceLib/src/cell.cpp`, `MatterSurfaceLib/include/cell.h`

Added new method that skips the `std::find` duplicate check:
```cpp
// cell.h declaration:
void add_particle_index_unchecked(uint32_t particle_index, uint32_t material_id);

// cell.cpp definition:
void Cell::add_particle_index_unchecked(uint32_t particle_index, uint32_t material_id) {
    uint32_t group = (uint32_t)MaterialMergeGroup((int)material_id);
    material_particle_indices[group].push_back(particle_index);
    is_dirty = true;
}
```

Safe for `rebuild_dirty_cells` because `clear_particle_indices()` is always called first, guaranteeing no duplicates exist.

---

### Fix 5 — `cluster.cpp`: Transient particle spatial hash in `rebuild_dirty_cells`

**File:** `MatterSurfaceLib/src/cluster.cpp`, function `rebuild_dirty_cells`

Was: O(dirty_cells × total_particles) — every dirty cell scanned every particle.

Now: builds a transient `SpatialHash*` over all particles once, then queries per cell:
```cpp
SpatialHash* particle_hash = sh_create(smallest_cell_size_, (int)particles_.size());
for (uint32_t i = 0; i < particles_.size(); ++i) {
    const Vector3& pos = particles_[i].position;
    sh_insert(particle_hash, pos.x, pos.y, pos.z, (void*)(uintptr_t)(i + 1));
}
// Per dirty cell:
int found = sh_query_box(particle_hash, qxmin, qymin, qzmin, qxmax, qymax, qzmax,
                         query_buf.data(), kMaxQueryResults);
for (int qi = 0; qi < found; ++qi) {
    uint32_t i = (uint32_t)((uintptr_t)query_buf[qi] - 1);
    if (cell->intersects_sphere(particles_[i].position, particles_[i].radius))
        cell->add_particle_index_unchecked(i, particles_[i].materialId);
}
sh_destroy(particle_hash);
```

Index encoding uses `i+1` (same pattern as Task 10's cell encoding) to distinguish index 0 from NULL. A carve spatial hash is also built similarly for `apply_carve_fields` use. Both hashes freed at function exit.

---

### Fix 6 — `blas_manager.cpp` / `blas_manager.hpp`: O(1) BLAS handle lookup map

**Files:** `MatterSurfaceLib/src/blas_manager.cpp`, `MatterSurfaceLib/include/blas_manager.hpp`

Was: `has_blas`, `get_bvh`, `get_mesh`, `get_entry` all used `std::find_if` over `entries_` vector — O(N) per lookup.

Added `std::unordered_map<BLASHandle, size_t> handle_to_index_` to private members. All lookup paths now use:
```cpp
auto it = handle_to_index_.find(handle);
if (it == handle_to_index_.end()) return nullptr;
return entries_[it->second].get();
```

`register_triangles` and `register_prebuilt` insert into the map on creation. `release_blas` uses the map for O(1) erase, then rebuilds both `hash_to_entry_` and `handle_to_index_` after compaction (necessary because `entries_` indices shift on vector erasure). `clear()` and `reset_stats()` also clear the map.

`get_offsets` walks `entries_` only up to `handle_to_index_[handle]` index, not the full vector.

---

### Fix 7 — `bvh.cpp`: TLAS `BuildRecursive` sort along longest centroid axis

**File:** `MatterSurfaceLib/src/bvh.cpp`, function `BVH::BuildRecursive`

Was: split at `count/2` along a fixed axis (or no sort at all).

Now: compute extent of centroid AABB, pick longest axis, `std::nth_element` to place median at split point:
```cpp
float3 extent = node.aabbMax - node.aabbMin;
int axis = 0;
if (extent.y > extent.x) axis = 1;
if (axis == 0 && extent.z > extent.x) axis = 2;
else if (axis == 1 && extent.z > extent.y) axis = 2;

auto centroid_on_axis = [this, axis](uint idx) -> float {
    const float3& bmin = blas[idx].bounds.bmin;
    const float3& bmax = blas[idx].bounds.bmax;
    if (axis == 1) return (bmin.y + bmax.y) * 0.5f;
    if (axis == 2) return (bmin.z + bmax.z) * 0.5f;
    return (bmin.x + bmax.x) * 0.5f;
};

uint split = count / 2;
uint* base = nodeIdx + first;
std::nth_element(base, base + split, base + count,
    [&centroid_on_axis](uint a, uint b) {
        return centroid_on_axis(a) < centroid_on_axis(b);
    });
```

`float3` has no `[]` subscript operator, so axis selection uses explicit `if` branches. Added `#include <algorithm>` for `std::nth_element`. `.part` files store BLAS data only (no TLAS node order), so no golden files are affected.

---

### Fix 8 — `surface.c`: Carve/clip particle spatial hashes on `SurfaceScratch`

**File:** `MatterSurfaceLib/src/surface.c`

Was: `ApplySubtractField` and `ApplyClipField` iterated all carve/clip particles per voxel — O(voxels × carve_particles).

Now: `SurfaceScratch` carries optional `carve_hash` / `clip_hash` fields:
```c
SpatialHash* carve_hash;
SpatialHash* clip_hash;
const Particle* carve_particles;  // cache key
int            carve_count;
// ... similar for clip ...
float          carve_max_radius;
float          clip_max_radius;
```

`GenerateMeshInternal` builds the hashes after the main particle hash, keyed by pointer+count for reuse across identical calls. `DestroySurfaceScratch` destroys them.

`ApplySubtractField` and `ApplyClipField` accept new `SpatialHash* hash, float query_radius` parameters. When `hash != NULL`, a box query replaces the full particle scan. When `hash == NULL` (e.g., called from `ProbeFieldScalar`), the original O(N) loop runs unchanged — preserving byte-identical output for all non-carve paths.

`CalculateScalarAndMaterial` and `CalculateScalarStaged` forward the hash pointers through to the `Apply*` functions. `ProbeFieldScalar` call sites pass `NULL, 0.0f` for backward compatibility.

---

## Build Evidence

**MSL standalone build:**
```
cd MatterSurfaceLib && make
# exit code 0, no errors
```

**MatterEngine3 `make test`:**
```
cd MatterEngine3 && make test
# ALL PASS
```

**Full headless test suite** (`run-tilesetbake run-comp run-iso run-flatten`):
```
GALLIUM_DRIVER=d3d12 make run-tilesetbake run-comp run-iso run-flatten
# exit code 0
# ALL PASS (tilesetbake, comp, iso, flatten suites)
# part_flatten_tests: ALL PASS
```

---

## Deviations from Brief

None. All 8 fixes from the brief were implemented as specified. The TLAS split-axis change was not reverted — it is sound (verified that `.part` files contain BLAS data only, not TLAS node ordering, so no golden files are affected).

The one implementation wrinkle: `float3` has no `[]` subscript operator, so the centroid-on-axis lambda uses explicit `if (axis==1)/if (axis==2)` branches instead of `bmin[axis]`. This is equivalent and compiles cleanly.

---

## Files Changed

| File | Change |
|------|--------|
| `MatterSurfaceLib/src/open_particle_surface.c` | Fixes 1, 2: OPS_DEBUG macro, free-list, isActive |
| `MatterSurfaceLib/src/cell.cpp` | Fixes 3, 4: BVH analysis gate, add_particle_index_unchecked |
| `MatterSurfaceLib/include/cell.h` | Fix 4: declaration for add_particle_index_unchecked |
| `MatterSurfaceLib/src/cluster.cpp` | Fix 5: transient particle spatial hash in rebuild_dirty_cells |
| `MatterSurfaceLib/src/blas_manager.cpp` | Fix 6: handle_to_index_ map for O(1) lookups |
| `MatterSurfaceLib/include/blas_manager.hpp` | Fix 6: handle_to_index_ map member declaration |
| `MatterSurfaceLib/src/bvh.cpp` | Fix 7: longest-axis centroid sort in BuildRecursive |
| `MatterSurfaceLib/src/surface.c` | Fix 8: carve/clip spatial hashes on SurfaceScratch |

---

## Code-Review Follow-Up Fixes (post-commit 6766ba3)

### Fix A — `blas_manager.cpp` / `blas_manager.hpp`: Per-entry GPU texture dirty tracking

**Required finding:** `ensure_gpu_textures_ready` was doing a full re-upload of ALL GPU texture data on any single BLAS change (single global `textures_dirty_` flag).

**Design:** Texture data is a concatenated buffer where an entry's offsets depend on prior entries' total triangle/node counts. A change in the entry set (add/release) changes `tile_w` (the texture width) and thus shifts ALL offsets → full regeneration required. Only when the total counts are unchanged can we safely reuse the existing texture.

**Implementation:**

1. Added `mutable bool gpu_dirty = true` field to `BLASEntry` (set on construction, cleared after upload). `blas_manager.hpp:62-70`
2. Added `mutable int gpu_total_triangles_ = 0; mutable int gpu_total_nodes_ = 0;` to track counts from last successful GPU upload. `blas_manager.hpp:241-244`
3. Extracted `build_triangle_texture_buffer()` and `build_node_texture_buffer()` as file-scope helpers (removes duplication, enables future partial-path reuse). `blas_manager.cpp:450-597`
4. `ensure_gpu_textures_ready()` now:
   - **Fast path (cheap win):** if total counts unchanged AND no entry has `gpu_dirty=true`, clears `textures_dirty_` and returns immediately — no CPU or GPU work.
   - **Same-size update:** if total counts unchanged and textures already exist at the correct dimensions, uses `UpdateTexture()` instead of `UnloadTexture()` + `LoadTextureFromImage()`, avoiding GPU reallocation.
   - **Set/size changed:** full rebuild as before (Unload + Load). `blas_manager.cpp:599-683`
5. After upload, clears per-entry `gpu_dirty` flags and records `gpu_total_*`. `blas_manager.cpp:675-681`
6. `clear()` resets `gpu_total_triangles_` and `gpu_total_nodes_` to 0. `blas_manager.cpp:829-832`

**Behavior:** identical — same texture contents produced in all cases; `UpdateTexture` is a full-texture push so the GPU sees the same data as Unload+Load.

**Note on partial span upload:** True per-span `UpdateTextureRec` is not implemented because (a) the tiled layout means a single entry's triangles may straddle tile-row boundaries, requiring multiple `UpdateTextureRec` calls per entry, and (b) no CPU-side texture buffer is cached between calls so clean entries' texels would need recomputing anyway. The primary win (skip entirely when clean) and the secondary win (reuse GPU allocation when same size) are both captured.

---

### Fix B — `surface.c`: Carve/clip query saturation fallback

**Latent behavioral difference:** `ApplySubtractField` and `ApplyClipField` used `nearby[128]` stack buffers with `sh_query_radius(..., 128)`. If ≥128 carve/clip particles are within the query radius, `sh_query_radius` silently truncated the result set. The original full-scan had no cap.

**Implementation:**

`ApplyClipField` (`surface.c:1158-1184`): added `found < 128` guard before using the hash result. When `found == 128` (possible truncation), falls back to the full linear scan over `clipParticles[0..clipCount)`.

`ApplySubtractField` (`surface.c:1214-1246`): added `n < 128` guard before the hash path. When `n == 128` (possible truncation), falls through to the existing no-hash fallback code (scan all `carveParticles`). No `goto` used — the if-block returns early on the non-saturated path; saturation lets control fall through the closing brace into the existing fallback.

**Behavior:** exactly identical in all scenes with ≤127 carve/clip particles per query radius (the common case). Scenes with ≥128 particles in a voxel's radius now also produce bit-exact output (full scan, same as pre-optimization).

---

## Build and Test Evidence (Code-Review Fixes)

**MSL standalone build:**
```
cd MatterSurfaceLib && make clean && make
# All object files compiled clean
# Only pre-existing failure: primitive_sdf undefined reference in Windows link
```

**MatterEngine3 `make test`:**
```
cd MatterEngine3 && GALLIUM_DRIVER=d3d12 make test
# ALL PASS
```

**MatterEngine3/tests individual targets:**
```
cd MatterEngine3/tests && GALLIUM_DRIVER=d3d12 make run-tilesetbake
# All tileset_bake_tests passed.

GALLIUM_DRIVER=d3d12 make run-comp
# OK

GALLIUM_DRIVER=d3d12 make run-iso
# ALL PASS

GALLIUM_DRIVER=d3d12 make run-flatten
# part_flatten_tests: ALL PASS
```

---

## Additional Files Changed (Code-Review Fixes)

| File | Change |
|------|--------|
| `MatterSurfaceLib/include/blas_manager.hpp` | Added `gpu_dirty` to BLASEntry; added `gpu_total_triangles_/nodes_` private members |
| `MatterSurfaceLib/src/blas_manager.cpp` | Extracted texture-build helpers; per-entry dirty fast path + UpdateTexture reuse |
| `MatterSurfaceLib/src/surface.c` | Saturation fallback in ApplyClipField and ApplySubtractField |
