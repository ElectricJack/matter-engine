# Verified Bug Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix 5 verified bug groups (each duplicated across the project copies that share code) found during a full-repo bug audit on 2026-06-11.

**Architecture:** This is a C/C++ monorepo where several projects (`SurfaceLib`, `OpenParticleSurfaceLib`, `MatterSurfaceLib`, `GPURayTraceExample`) carry forked copies of the same source files. Each bug must be fixed in **every copy** listed. All 5 tasks touch disjoint file sets and can be executed by parallel subagents.

**Tech Stack:** C (gnu11) / C++ (raylib, OpenGL), per-project Makefiles, top-level `./build-all.sh`.

**Testing note:** The affected code paths live in interactive raylib applications with no unit-test harness. Strict TDD is not practical here; each task instead verifies by (a) clean compile of every affected project and (b) the repo-wide `./build-all.sh test` regression run in the final task. Build commands below are exact and must succeed before commit.

**Recommended subagent models per task:**

| Task | Description | Model |
|------|-------------|-------|
| 1 | Memory-pool double-free on error path (3 files) | sonnet |
| 2 | `cleanup()` array desync → OOB read after reset (1 file) | sonnet |
| 3 | Uninitialized `axis`/`splitPos` UB in BVH + traversal stack slot (2 files) | opus |
| 4 | TLAS `BVHInstance[]` leak every rebuild (2 .cpp + 2 headers) | opus |
| 5 | Unchecked `realloc` → NULL deref / leak (2 files) | sonnet |

All tasks are independent (disjoint files) — dispatch in parallel if desired.

**Build environment:** Linux/WSL. Repo root: `/mnt/d/Shared With Desktop/AI/matter-engine-cpp` (path contains spaces — always quote). Per CLAUDE.md, each project builds from its own directory with `make`. `MatterSurfaceLib`, `OpenParticleSurfaceLib`, `GPURayTraceExample` need `make WSL_LINUX=1`; `ParticleDynamicsExample` needs `make TARGET=linux`; `SurfaceLib` plain `make`.

---

### Task 1: Fix memory-pool double-free on spatial-hash failure (3 copies)

**Bug:** In `GenerateSurfaceMesh`, when `config.enableMemoryReuse` is true, `data.scalarField` / `data.materialField` are *aliases into the global memory pool* (`g_memoryPool.scalarField` / `.materialField`), not owned allocations. The `sh_create()` failure path frees them unconditionally, corrupting the pool for every later call (use-after-free / double-free). Severity: critical. Verified — the aliasing happens ~25 lines above each error path.

**Files:**
- Modify: `MatterSurfaceLib/src/surface.c:264-269`
- Modify: `OpenParticleSurfaceLib/src/surface.c:263-268`
- Modify: `SurfaceLib/src/surface.c:264-269`

All three copies contain the identical block (line numbers shift by ±1 per file):

```c
    if (!spatialHash) {
        printf("Failed to create spatial hash\n");
        free(data.scalarField);
        free(data.materialField);
        return mesh;
    }
```

- [ ] **Step 1: Apply the guarded fix to all three files**

Replace the block in each of the three files with:

```c
    if (!spatialHash) {
        printf("Failed to create spatial hash\n");
        if (!config.enableMemoryReuse) {
            free(data.scalarField);
            free(data.materialField);
        }
        return mesh;
    }
```

Locate each instance by searching for `Failed to create spatial hash` in the file. Note: `config` is the local `SurfaceConfig` already in scope in this function in all three copies (the same variable consulted at the `enableMemoryReuse` allocation branch ~25 lines above — read that branch first to confirm the variable name in each copy).

- [ ] **Step 2: Build all three affected projects**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/SurfaceLib" && make
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/OpenParticleSurfaceLib" && make WSL_LINUX=1
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib" && make WSL_LINUX=1
```

Expected: each completes with exit code 0, no new warnings about `config`.

- [ ] **Step 3: Commit**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp"
git add SurfaceLib/src/surface.c OpenParticleSurfaceLib/src/surface.c MatterSurfaceLib/src/surface.c
git commit -m "fix: don't free memory-pool aliases on spatial hash failure"
```

---

### Task 2: Fix gamified-array desync after ParticleSystem::reset()

**Bug:** `ParticleSystem::cleanup()` (`ParticleDynamicsExample/src/particle_system.cpp:72-103`) clears the main particle SoA arrays (`pos_x_` … `active_`, `free_indices_`) but **not** the gamified-physics arrays (`heat_energy_`, `electric_energy_`, `chemical_energy_`, `kinetic_energy_`, `device_states_`) nor `electrical_connections_`. After `reset()` (cleanup + initialize, line 105-109), adding particles pushes onto *both* sets, so `device_states_.size()` becomes `old_count + new_count` while `active_.size()` is only `new_count`. `update_device_states()` (line 1910) then iterates `device_states_.size()` and reads `active_[i]` out of bounds — undefined behavior. `update_energy_flow()` (line 1871) also walks stale `electrical_connections_` entries referencing pre-reset indices. Severity: critical. Verified.

**Files:**
- Modify: `ParticleDynamicsExample/src/particle_system.cpp:72-103` (the `cleanup()` function)

- [ ] **Step 1: Inventory per-particle containers**

Open `ParticleDynamicsExample/include/particle_system.h` and list every `std::vector`/container member that grows per-particle or references particle indices (at minimum: `heat_energy_`, `electric_energy_`, `chemical_energy_`, `kinetic_energy_`, `device_states_`, `electrical_connections_`; also check for arcing/connection containers used by `update_arcing_effects` and `update_electrical_network`). Any such member not already cleared in `cleanup()` must be added in Step 2.

- [ ] **Step 2: Clear them in cleanup()**

In `cleanup()`, directly after `free_indices_.clear();` (line 91), add:

```cpp
    // Clear gamified physics data (must stay in sync with the main SoA arrays)
    heat_energy_.clear();
    electric_energy_.clear();
    chemical_energy_.clear();
    kinetic_energy_.clear();
    device_states_.clear();
    electrical_connections_.clear();
```

…plus `.clear()` for any additional per-particle containers found in Step 1.

- [ ] **Step 3: Build**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/ParticleDynamicsExample" && make TARGET=linux
```

Expected: exit code 0.

- [ ] **Step 4: Commit**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp"
git add ParticleDynamicsExample/src/particle_system.cpp
git commit -m "fix: clear gamified physics arrays in ParticleSystem::cleanup to prevent OOB after reset"
```

---

### Task 3: Fix uninitialized axis/splitPos UB in BVH build + traversal stack slot (2 copies)

**Bug A (high):** `BVH::FindBestSplitPlane(node, axis, splitPos, ...)` only assigns `axis`/`splitPos` when it finds a valid split. If every axis is degenerate (`boundsMin == boundsMax` — all centroids identical, e.g. duplicate triangles) it returns `1e30f` without writing them. The caller `Subdivide` declares `int axis, splitPos;` uninitialized and can then use them:
- `GPURayTraceExample/src/bvh.cpp:170-186` — when `subdivToOnePrim` is true and `triCount > 1`, execution reaches line 186 (`scale = BINS / (centroidMax.cell[axis] - centroidMin.cell[axis])`) with garbage `axis`, and divides by zero → `inf`, then `(int)(0.0f * inf)` = `(int)NaN` — UB.
- `MatterSurfaceLib/src/bvh.cpp:180-208` — worse: the `forceSplit` path (`triCount > MAX_TRIS_PER_LEAF`, line 181/196) proceeds into the partition with garbage `axis` even when no valid split exists. Its `FindBestSplitPlane` (line 241-348) can also finish with `numCandidates == 0` on every axis (line 342), leaving outputs unset.

**Bug B (low):** `TLAS::Intersect` declares `TLASNode* stack[64]` but pushes with `stackPtr < 63`, wasting the last slot and silently dropping the far child one level earlier than necessary (missed intersections at depth 63). `GPURayTraceExample/src/bvh.cpp:470`, `MatterSurfaceLib/src/bvh.cpp:616`.

**Files:**
- Modify: `GPURayTraceExample/src/bvh.cpp:170-171` (Subdivide), `:216-218` (FindBestSplitPlane), `:470` (Intersect)
- Modify: `MatterSurfaceLib/src/bvh.cpp:184-185` (Subdivide), `:241-243` (FindBestSplitPlane), `:616` (Intersect)

- [ ] **Step 1: Initialize the out-params in both FindBestSplitPlane implementations**

In `GPURayTraceExample/src/bvh.cpp`, at the top of `FindBestSplitPlane` (line 217, just after the opening brace):

```cpp
float BVH::FindBestSplitPlane( BVHNode& node, int& axis, int& splitPos, float3& centroidMin, float3& centroidMax )
{
	axis = 0; splitPos = 0; // defensible defaults; cost stays 1e30f when no valid split exists
	float bestCost = 1e30f;
```

Same edit in `MatterSurfaceLib/src/bvh.cpp` at line 242 (`float bestCost = 1e30f;` is line 243 there).

- [ ] **Step 2: Bail out of Subdivide when no valid split exists**

In `GPURayTraceExample/src/bvh.cpp`, immediately after the `FindBestSplitPlane` call (line 171), add:

```cpp
	float splitCost = FindBestSplitPlane( node, axis, splitPos, centroidMin, centroidMax );
	if (splitCost == 1e30f) return; // no valid split (all centroids identical) - keep as leaf
```

In `MatterSurfaceLib/src/bvh.cpp`, immediately after its `FindBestSplitPlane` call (line 185), add the same guard:

```cpp
	float splitCost = FindBestSplitPlane( node, axis, splitPos, centroidMin, centroidMax );
	if (splitCost == 1e30f) return; // no valid split (all centroids identical) - keep as leaf
```

This guard must come **before** the `subdivToOnePrim`/`forceSplit` logic so the force-split path can never run with a degenerate split. (When no axis can separate the centroids, partitioning is impossible regardless of leaf-size policy, so returning is correct.)

- [ ] **Step 3: Fix the traversal stack bound in both copies**

`GPURayTraceExample/src/bvh.cpp:470` (becomes ~471 after Step 2) and `MatterSurfaceLib/src/bvh.cpp:616` (becomes ~617):

```cpp
				if (distRight != 1e30f && stackPtr < 64)
```

(The push is `stack[stackPtr++]` into `TLASNode* stack[64]`; checking `< 64` before the push writes at most index 63 — in bounds.)

- [ ] **Step 4: Build both projects**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/GPURayTraceExample" && make WSL_LINUX=1
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib" && make WSL_LINUX=1
```

Expected: exit code 0 for both.

- [ ] **Step 5: Commit**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp"
git add GPURayTraceExample/src/bvh.cpp MatterSurfaceLib/src/bvh.cpp
git commit -m "fix: guard BVH subdivide against degenerate splits with uninitialized axis; use full TLAS traversal stack"
```

---

### Task 4: Fix TLAS BVHInstance array leak on every rebuild (2 copies)

**Bug:** `TLASManager::build()` allocates `BVHInstance* simple_array = new BVHInstance[n]` (line 319 in both copies) and hands the raw pointer to `TLAS`, which does **not** own or delete it (the code's own comment at line 327: *"Note: This creates a memory leak, but let's test if it works first"*). Animated scenes rebuild the TLAS every frame → unbounded leak. Severity: high. Verified in both copies.

**Files:**
- Modify: `GPURayTraceExample/src/tlas_manager.cpp:316-328` and `GPURayTraceExample/include/tlas_manager.hpp`
- Modify: `MatterSurfaceLib/src/tlas_manager.cpp:316-328` and `MatterSurfaceLib/include/tlas_manager.hpp`

- [ ] **Step 1: Confirm TLAS does not free the array**

Read the `TLAS` class in `GPURayTraceExample/include/bvh.h` (and the MatterSurfaceLib copy). Confirm the destructor does not `delete[] blas` — it must not, or the fix below would double-free. If a destructor *does* free it, stop and re-plan this task.

- [ ] **Step 2: Add owned storage to both TLASManager headers**

In each `include/tlas_manager.hpp`, next to the existing `tlas_` / `instances_` members, add:

```cpp
    std::vector<BVHInstance> instance_storage_; // backing array owned by the manager; TLAS holds a raw pointer into it
```

(`<vector>` is already included for the existing members; verify and add the include if not.)

- [ ] **Step 3: Replace the leaked allocation in both build() implementations**

In each `src/tlas_manager.cpp`, replace lines 316-328:

```cpp
    // Create and build TLAS
    if (!instance_ptrs.empty()) {
        // Allocate a simple array for TLAS - this is a temporary fix
        BVHInstance* simple_array = new BVHInstance[instance_ptrs.size()];
        for (size_t i = 0; i < instance_ptrs.size(); i++) {
            simple_array[i] = *instance_ptrs[i]; // Copy construct
        }
        
        tlas_ = std::make_unique<TLAS>(simple_array, static_cast<int>(instance_ptrs.size()));
        tlas_->Build();
        
        // Note: This creates a memory leak, but let's test if it works first
    }
```

with:

```cpp
    // Create and build TLAS
    if (!instance_ptrs.empty()) {
        tlas_.reset(); // old TLAS points into instance_storage_; drop it before mutating
        instance_storage_.clear();
        instance_storage_.reserve(instance_ptrs.size());
        for (BVHInstance* p : instance_ptrs) {
            instance_storage_.push_back(*p);
        }
        tlas_ = std::make_unique<TLAS>(instance_storage_.data(), static_cast<int>(instance_storage_.size()));
        tlas_->Build();
    }
```

The vector is sized once before `TLAS` captures `.data()` and is never resized while `tlas_` is alive (every rebuild resets `tlas_` first), so the raw pointer stays valid.

- [ ] **Step 4: Build both projects**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/GPURayTraceExample" && make WSL_LINUX=1
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib" && make WSL_LINUX=1
```

Expected: exit code 0 for both.

- [ ] **Step 5: Commit**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp"
git add GPURayTraceExample/src/tlas_manager.cpp GPURayTraceExample/include/tlas_manager.hpp MatterSurfaceLib/src/tlas_manager.cpp MatterSurfaceLib/include/tlas_manager.hpp
git commit -m "fix: own TLAS instance array in TLASManager to stop per-rebuild leak"
```

---

### Task 5: Fix unchecked realloc of cellParticleBuffer (2 copies)

**Bug:** The mesh-update loop grows a persistent static buffer with `cellParticleBuffer = realloc(cellParticleBuffer, ...)`. On realloc failure this NULLs the only pointer (leaking the old buffer) and the very next loop writes `cellParticleBuffer[validCount]` → NULL deref. Worse, `cellParticleBufferSize` is updated *before* the realloc, so even a survivable failure leaves the recorded size larger than the actual buffer. Severity: medium-high. Verified in both copies.

**Files:**
- Modify: `MatterSurfaceLib/src/open_particle_surface.c:740-745`
- Modify: `OpenParticleSurfaceLib/src/open_particle_surface.c:821-826`

Both copies contain (inside `for (int idx = 0; idx < dirtyFound; idx++) { ... }`):

```c
        // Ensure the buffer is large enough
        if (spatialHashCells[cellIndex].particleCount > cellParticleBufferSize) {
            cellParticleBufferSize = spatialHashCells[cellIndex].particleCount * 1.5;
            cellParticleBuffer = (Particle*)realloc(cellParticleBuffer, 
                                               cellParticleBufferSize * sizeof(Particle));
        }
```

- [ ] **Step 1: Apply the safe-realloc fix to both files**

Replace the block in each file with:

```c
        // Ensure the buffer is large enough
        if (spatialHashCells[cellIndex].particleCount > cellParticleBufferSize) {
            int newSize = spatialHashCells[cellIndex].particleCount * 1.5;
            Particle* newBuffer = (Particle*)realloc(cellParticleBuffer,
                                               newSize * sizeof(Particle));
            if (!newBuffer) {
                printf("[ERROR] Failed to grow cell particle buffer to %d entries\n", newSize);
                continue; // keep old buffer/size; skip this cell
            }
            cellParticleBuffer = newBuffer;
            cellParticleBufferSize = newSize;
        }
```

(`continue` advances the enclosing `for (int idx ...)` cell loop — confirm you're inside that loop at both sites; the surrounding `[DEBUG] Processing cell` printf is the landmark.)

- [ ] **Step 2: Build both projects**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib" && make WSL_LINUX=1
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/OpenParticleSurfaceLib" && make WSL_LINUX=1
```

Expected: exit code 0 for both.

- [ ] **Step 3: Commit**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp"
git add MatterSurfaceLib/src/open_particle_surface.c OpenParticleSurfaceLib/src/open_particle_surface.c
git commit -m "fix: check realloc result before replacing cell particle buffer"
```

---

### Task 6: Full-repo regression build and test (run after Tasks 1-5)

- [ ] **Step 1: Build everything and run headless test suites**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && ./build-all.sh test
```

Expected: summary table shows `OK` for all 8 projects, exit code 0 (ObjectAllocatorLib and SpatialQueryLib test suites pass).

- [ ] **Step 2: Verify all five fix commits are present**

```bash
git log --oneline -6
```

Expected: the five `fix:` commits from Tasks 1-5 on top of the previous HEAD.

---

## Appendix: Audit findings investigated and rejected (do NOT "fix" these)

Recorded so future audits don't rediscover them:

1. **`GetEdgeKey` negative-coordinate / zero-key claims** (all surface.c copies): cell coords come from loops over `0..gridSize-2`; endpoints are `coord` or `coord+1`, never negative. A key of 0 would require both endpoints at (0,0,0), impossible for a real edge (endpoints always differ by +1 in one axis, and the sorted larger endpoint lands in the high bits). The `edgeKey == 0` skip is dead code, not a bug.
2. **Spatial-hash `dx/dy/dz` "shadowing" in `sh_query_radius` / `sh_query_first`** (all spatial_hash.c copies): the inner `float dx = entry->x - x;` declarations are new locals used immediately and correctly for the distance test. Benign style issue, zero functional impact.
3. **Triangle winding "reversal"** (`triangle.indices[2/1/0] = edge1/2/3`): identical in all three surface.c copies — a deliberate winding convention, with vertex normals derived from the same winding. Consistent, not a bug.
4. **ParticleDynamicsExample "array desync on add/remove"**: `add_particle` pushes to every array including the gamified set; slot reuse goes through `initialize_particle_gamified_data`, which fully resets energies and device state. In-sync during normal operation. (The *real* desync is the `cleanup()` path — Task 2.)
5. **`build-all.sh` unquoted `make $args`**: `$args` only ever holds space-free literals (`WSL_LINUX=1`, `TARGET=linux`); the word-splitting is intentional. `"$proj"` is quoted where it matters.
6. **TLAS traversal "buffer overflow" at `stackPtr < 63`**: not an overflow (push writes at most index 62); it's a wasted slot + silent node drop, fixed as the low-severity part of Task 3.
7. **Various "add bounds checks to every array access" suggestions** (particle_system.cpp:320-363, 1851-1853, 1871-1906, etc.): defensive checks against states that cannot occur once Task 2 lands; rejected per project convention of trusting internal invariants.
