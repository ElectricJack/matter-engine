# Per-Cell Parallel Meshing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the per-cell CPU mesh build across multiple cores via a persistent worker pool (one `SurfaceScratch` per worker), keeping all GL/BLAS/TLAS work on the main thread in a deterministic order so output is byte-identical to the serial path.

**Architecture:** Split `Cell::generate_mesh_for_group` at the CPU/main boundary into `build_group_mesh` (pure CPU → `GroupMeshResult`) and `commit_group_mesh` (main-thread GL/BLAS). `Cluster::rebuild_dirty_cells` becomes three phases: PRE (serial: gather indices/carve, release old BLAS, build `CellJob`s), PARALLEL (`MeshWorkerPool` runs `build_cell_meshes` per cell, each worker on its own scratch), DRAIN (serial, fixed job order: `commit_cell_meshes` then one TLAS rebuild). A hand-rolled `std::thread` pool owns N scratches and is resized between rebuilds via an ImGui slider.

**Tech Stack:** C++14 (main Makefile), C marching-cubes mesher (surface.c), raylib PODs, pthread/std::thread, Dear ImGui.

**Working directory:** `/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib`

**Build/test reference:**
- Linux build: `cd MatterSurfaceLib && WSL_LINUX=1 make` → `./matter_surface_lib`.
- Headless suites (from `MatterSurfaceLib/tests`): `make run-cont` (must report `tris=1964`, "ALL EXPECTATIONS MET"), `make run-cell`, `make run-tint`, `make run-simp`, `make run-cull`.
- WSL/DrvFs caveat: a freshly-linked binary may be all-zero ("Permission denied"/"Exec format error"; `file <bin>` shows "data"). Fix: `rm -f <bin> && make run-<suite>` (intermittent, re-link succeeds).
- The 5 suites are the geometry guard: any geometry/material drift fails them. Tint may differ ONLY on exact distance ties (already accepted).

---

## File Structure

- `MatterSurfaceLib/src/surface.c` — flip `ENABLE_PERFORMANCE_TIMING` to 0 (silence per-mesh PERF spam on the threaded path).
- `MatterSurfaceLib/include/mesh_worker_pool.h` (new) — `GroupMeshResult`, `CellMeshResult`, `CellJob`, `MeshWorkerPool`.
- `MatterSurfaceLib/src/mesh_worker_pool.cpp` (new) — `MeshWorkerPool` impl.
- `MatterSurfaceLib/include/cell.h` — forward-declare result types; add public `build_group_mesh`/`commit_group_mesh`/`build_cell_meshes`/`commit_cell_meshes`; keep `rebuild_meshes`.
- `MatterSurfaceLib/src/cell.cpp` — split `generate_mesh_for_group`; rewire `rebuild_meshes`; strip CPU-path printf.
- `MatterSurfaceLib/include/cluster.h` — replace `SurfaceScratch* surface_scratch_` with `std::unique_ptr<MeshWorkerPool> mesh_pool_`; add `set_mesh_worker_count`/`get_mesh_worker_count`.
- `MatterSurfaceLib/src/cluster.cpp` — 3-phase `rebuild_dirty_cells`; drop `update_cell_meshes`; own the pool.
- `MatterSurfaceLib/main.cpp` — ImGui "Mesh workers" slider.
- `MatterSurfaceLib/Makefile` — add `mesh_worker_pool` to `SRC`/`OBJ` + build rule.
- `MatterSurfaceLib/tests/parallel_mesh_tests.cpp` (new) + `MatterSurfaceLib/tests/Makefile` `run-par` target — W=1 vs W=N determinism (GL-free).

---

## Task 1: Silence per-mesh PERF logging on the threaded path

**Files:**
- Modify: `MatterSurfaceLib/src/surface.c:24`

The PERF `printf` macros fire per mesh build. Under threads they interleave and the `printf` lock serializes workers. They are diagnostic-only; disabling them removes all per-mesh stdout at zero runtime cost. (No data race exists: the Linux timer is pure `clock_gettime`; only `_WIN32` has idempotent lazy statics.)

- [ ] **Step 1: Disable the timing macro**

In `MatterSurfaceLib/src/surface.c`, change line 24 from:

```c
#define ENABLE_PERFORMANCE_TIMING 1
```

to:

```c
#define ENABLE_PERFORMANCE_TIMING 0
```

- [ ] **Step 2: Build the GUI binary to confirm it still compiles**

Run: `cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib" && WSL_LINUX=1 make 2>&1 | tail -5`
Expected: `Built executable for linux` (no errors). The `#else` branch of the macro provides empty `TIMER_START`/`TIMER_END`, so all call sites still compile.

- [ ] **Step 3: Run the continuity suite to confirm geometry unchanged**

Run: `cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib/tests" && make run-cont 2>&1 | tail -20`
Expected: `tris=1964` and "ALL EXPECTATIONS MET". (If the binary is all-zero, `rm -f mesh_continuity_tests && make run-cont`.)

- [ ] **Step 4: Commit**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp"
git add MatterSurfaceLib/src/surface.c
git commit -m "perf: disable per-mesh PERF timing printf ahead of threaded meshing"
```

---

## Task 2: Add result types + split generate_mesh_for_group (serial, behavior-preserving)

This introduces the CPU/main split with **no threading yet**: `rebuild_meshes` calls the new
`build_cell_meshes` then `commit_cell_meshes` serially, so behavior is identical and the 5 suites
stay green. This is the foundational refactor.

**Files:**
- Create: `MatterSurfaceLib/include/mesh_worker_pool.h` (types only in this task)
- Modify: `MatterSurfaceLib/include/cell.h`
- Modify: `MatterSurfaceLib/src/cell.cpp`

- [ ] **Step 1: Create the result-types header**

Create `MatterSurfaceLib/include/mesh_worker_pool.h`. In this task only the result/job types are
needed; the `MeshWorkerPool` class is added in Task 3 (write the whole file now — the pool class is
appended in Task 3, so create the full skeleton below but the pool methods come later). For Task 2,
create the file with exactly this content:

```cpp
#ifndef MESH_WORKER_POOL_H
#define MESH_WORKER_POOL_H

#include "raylib.h"          // Mesh
#include "bvh.h"             // Tri, TriEx
#include <vector>
#include <cstdint>

// Forward declarations
struct Cell;
struct SurfaceScratch;

// CPU-only mesh build output for one merge group. Holds the raylib CPU Mesh
// (vertex/normal/index arrays, pre-UploadMesh) plus the BLAS-ready triangle
// arrays with per-triangle material/tint already resolved. Detached from any
// GL/BLAS state, so it can be produced on a worker thread and committed later
// on the main thread. The Mesh pointers are owned downstream by the Cell once
// committed; GroupMeshResult never frees them.
struct GroupMeshResult {
    uint32_t group_id = 0;
    Mesh mesh = {};                          // vertexCount == 0 => "no mesh, skip"
    std::vector<Tri> triangles;
    std::vector<TriEx> triangle_normals;     // materialId/tint filled during build
};

// CPU-only mesh build output for all merge groups in one cell.
struct CellMeshResult {
    std::vector<GroupMeshResult> groups;
};

#endif // MESH_WORKER_POOL_H
```

- [ ] **Step 2: Update cell.h — forward-declare result types and add the split methods**

In `MatterSurfaceLib/include/cell.h`, add forward declarations after the existing forward
declarations block (after line 13, `class CellRenderVisitor;`):

```cpp
struct GroupMeshResult;
struct CellMeshResult;
```

Then in `struct Cell`, add these PUBLIC methods just after the existing `rebuild_meshes`
declaration (after line 79). They must be public so `Cluster` can call them across the 3 phases:

```cpp
    // CPU-only mesh build for every merge group in this cell. Reentrant: uses the
    // caller-supplied per-thread SurfaceScratch and touches no GL/BLAS/global
    // state, so it is safe to run on a worker thread. Reads material_particle_indices
    // (populated by add_particle_index) and cluster_particles read-only.
    CellMeshResult build_cell_meshes(const std::vector<StaticParticle>& cluster_particles,
                                     SurfaceScratch* scratch,
                                     float simplification_ratio, float base_detail, int max_pow,
                                     float uniform_detail,
                                     const Particle* carveParticles, int carveCount) const;
    // Main-thread commit of a CellMeshResult: UploadMesh (GL), BLAS registration,
    // BVH report, and material_meshes/material_blas writes. Sets has_meshes.
    void commit_cell_meshes(CellMeshResult& result, BLASManager& blas_manager);
```

Then change the `private:` section: move `generate_mesh_for_group` out (delete its declaration at
lines 104-107) and replace with the two split helpers (keep them private):

```cpp
    // CPU-only build of one merge group's mesh + tagged triangles (no GL/BLAS).
    GroupMeshResult build_group_mesh(uint32_t group_id, const std::vector<StaticParticle>& cluster_particles,
                                     SurfaceScratch* scratch,
                                     float simplification_ratio, float base_detail, int max_pow, float uniform_detail,
                                     const Particle* carveParticles, int carveCount) const;
    // Main-thread commit of one group's result (UploadMesh + BLAS + BVH report).
    void commit_group_mesh(GroupMeshResult& result, BLASManager& blas_manager);
```

- [ ] **Step 3: cell.cpp — include the new header**

In `MatterSurfaceLib/src/cell.cpp`, add after line 7 (`#include "mesh_simplifier.hpp"`):

```cpp
#include "../include/mesh_worker_pool.h"
#include <utility>   // std::move
```

- [ ] **Step 4: cell.cpp — replace generate_mesh_for_group with the split**

Replace the entire `Cell::generate_mesh_for_group` function (lines 317-507) with the four functions
below. `build_group_mesh` is the CPU half (everything up to and including per-triangle tagging, no
`UploadMesh`); `commit_group_mesh` is the main-thread half; `build_cell_meshes`/`commit_cell_meshes`
loop groups.

```cpp
GroupMeshResult Cell::build_group_mesh(uint32_t group_id, const std::vector<StaticParticle>& cluster_particles,
                                       SurfaceScratch* scratch,
                                       float simplification_ratio, float base_detail, int max_pow, float uniform_detail,
                                       const Particle* carveParticles, int carveCount) const {
    GroupMeshResult result;
    result.group_id = group_id;

    auto group_it = material_particle_indices.find(group_id);
    if (group_it == material_particle_indices.end() || group_it->second.empty()) {
        return result;
    }

    const auto& particle_indices = group_it->second;

    Bounds bounds;
    bounds.center = center;
    bounds.size = Vector3{actual_size, actual_size, actual_size};

    float detail_min;
    if (uniform_detail > 0.0f) {
        detail_min = uniform_detail;
    } else {
        detail_min = base_detail;
        for (uint32_t idx : particle_indices) {
            if (idx >= cluster_particles.size()) continue;
            float ds = cluster_particles[idx].detail_size;
            if (ds > 0.0f && ds < detail_min) detail_min = ds;
        }
    }
    bounds.divisionPow = choose_division_pow(detail_min, base_detail, 4, max_pow);
    int gridSize = 1 << bounds.divisionPow;
    float voxel = actual_size / (float)(gridSize - 1);
    float blend_voxels = kBlendVoxels;
    if (const char* e = getenv("MSL_BLEND_VOXELS")) { float v = (float)atof(e); if (v >= 0.0f) blend_voxels = v; }
    float blend_width = blend_voxels * voxel;
    float carve_blend = blend_width;
    if (const char* e = getenv("MSL_CARVE_BLEND")) { float v = (float)atof(e); if (v > 0.0f) carve_blend = v; }
    float cull_radius = kFeatureCullVoxels * voxel;
    float vis_radius  = kFeatureVisVoxels  * voxel;

    std::vector<Particle> particles;
    std::vector<float4> particle_tints;
    particles.reserve(particle_indices.size());
    particle_tints.reserve(particle_indices.size());
    float max_radius = 0.0f;
    for (uint32_t idx : particle_indices) {
        if (idx >= cluster_particles.size()) continue;
        const StaticParticle& sp = cluster_particles[idx];
        if (sp.radius < cull_radius) continue;
        float r_eff = (sp.radius < vis_radius) ? vis_radius : sp.radius;

        Particle surface_particle;
        surface_particle.position = sp.position;
        surface_particle.radius = r_eff;
        surface_particle.materialId = static_cast<int>(sp.materialId);
        particles.push_back(surface_particle);
        particle_tints.push_back(make_float4(sp.tint.x, sp.tint.y, sp.tint.z, sp.tint.w));
        if (r_eff > max_radius) max_radius = r_eff;
    }

    if (particles.empty()) {
        return result;
    }

    bool group_transparent = MaterialIsTransparent(particles[0].materialId) != 0;
    std::vector<Particle> clip = build_clip_particles(
        group_id, material_particle_indices, cluster_particles,
        group_transparent, cull_radius, vis_radius);
    Particle* clipPtr = clip.empty() ? NULL : clip.data();
    int clipCount = static_cast<int>(clip.size());

    Mesh mesh = GenerateMeshWithScratch(scratch, particles.data(), max_radius, static_cast<int>(particles.size()),
                             bounds, blend_width, clipPtr, clipCount,
                             const_cast<Particle*>(carveParticles), carveCount, carve_blend);

    if (simplification_ratio < 1.0f && mesh.vertexCount > 0 && mesh.triangleCount > 0) {
        CellBounds cb;
        cb.min_bound = min_bound;
        cb.max_bound = max_bound;
        SimplifyOptions so;
        so.target_ratio = simplification_ratio;
        so.lock_boundary = true;
        Mesh simplified = simplify_mesh(mesh, so, &cb);
        if (simplified.vertexCount > 0 && simplified.triangleCount > 0) {
            ComputeSurfaceNormalsWithScratch(scratch, &simplified, particles.data(), max_radius,
                                  static_cast<int>(particles.size()), blend_width, clipPtr, clipCount,
                                  const_cast<Particle*>(carveParticles), carveCount, carve_blend);
            UnloadMesh(mesh);
            mesh = simplified;
        } else {
            UnloadMesh(simplified);
        }
    }

    if (mesh.vertexCount <= 0) {
        return result;
    }

    // CPU triangle conversion + per-triangle material/tint tag. MUST run here in
    // the same worker right after generation: it reuses the scratch's spatial
    // hash, which still holds exactly `particles` (same data ptr + count passed
    // to GenerateMeshWithScratch and ComputeSurfaceNormalsWithScratch), so
    // `nearest - particles.data()` is a valid index.
    std::vector<TriEx> triangle_normals;
    std::vector<Tri> triangles = convert_mesh_to_triangles(mesh, &triangle_normals);

    SpatialHash* tri_hash = SurfaceScratchHash(scratch);
    float tri_search = max_radius * 2.5f + blend_width * 4.0f;
    for (size_t t = 0; t < triangle_normals.size() && t < triangles.size(); ++t) {
        const float3& c = triangles[t].centroid;
        int bestIdx = 0;
        Particle* nearest = NULL;
        int nfound = tri_hash
            ? sh_query_radius_nearest(tri_hash, c.x, c.y, c.z, tri_search, (void**)&nearest, 1)
            : 0;
        if (nfound > 0 && nearest) {
            bestIdx = (int)(nearest - particles.data());
        }
        triangle_normals[t].materialId = particles[bestIdx].materialId;
        triangle_normals[t].tint = particle_tints[bestIdx];
    }

    result.mesh = mesh;
    result.triangles = std::move(triangles);
    result.triangle_normals = std::move(triangle_normals);
    return result;
}

void Cell::commit_group_mesh(GroupMeshResult& result, BLASManager& blas_manager) {
    if (result.mesh.vertexCount <= 0) {
        return;
    }
    uint32_t group_id = result.group_id;

    material_meshes[group_id] = result.mesh;
    UploadMesh(&material_meshes[group_id], false);

    try {
        std::vector<Tri>& triangles = result.triangles;
        std::vector<TriEx>& triangle_normals = result.triangle_normals;

        if (!triangles.empty()) {
            material_blas[group_id] = blas_manager.register_triangles(triangles, triangle_normals);

            BVH* bvh = blas_manager.get_bvh(material_blas[group_id]);
            BvhMesh* mesh_ptr = blas_manager.get_mesh(material_blas[group_id]);
            if (bvh && mesh_ptr) {
                std::string analysis_name = "Cell(" + std::to_string((int)coordinates.x) + "," +
                                           std::to_string((int)coordinates.y) + "," +
                                           std::to_string((int)coordinates.z) + ")_Mat" +
                                           std::to_string(group_id) + "_" +
                                           std::to_string(triangles.size()) + "tris";
                BVHReportManager::RegisterBVH(analysis_name, bvh, mesh_ptr);
                BVHReportManager::UpdateAnalysis(analysis_name);
            }
        } else {
            material_blas[group_id] = 0;
        }
    } catch (const std::exception& e) {
        printf("Error registering mesh with BLAS manager: %s\n", e.what());
        material_blas[group_id] = 0;
    } catch (...) {
        printf("Unknown error registering mesh with BLAS manager\n");
        material_blas[group_id] = 0;
    }
}

CellMeshResult Cell::build_cell_meshes(const std::vector<StaticParticle>& cluster_particles, SurfaceScratch* scratch,
                                       float simplification_ratio, float base_detail, int max_pow, float uniform_detail,
                                       const Particle* carveParticles, int carveCount) const {
    CellMeshResult cell_result;
    for (const auto& group_entry : material_particle_indices) {
        uint32_t group_id = group_entry.first;
        GroupMeshResult gr = build_group_mesh(group_id, cluster_particles, scratch, simplification_ratio,
                                              base_detail, max_pow, uniform_detail, carveParticles, carveCount);
        if (gr.mesh.vertexCount > 0) {
            cell_result.groups.push_back(std::move(gr));
        }
    }
    return cell_result;
}

void Cell::commit_cell_meshes(CellMeshResult& result, BLASManager& blas_manager) {
    for (auto& gr : result.groups) {
        commit_group_mesh(gr, blas_manager);
    }
    has_meshes = !material_meshes.empty();
}
```

- [ ] **Step 5: cell.cpp — rewire rebuild_meshes to use the split serially**

Replace the body of `Cell::rebuild_meshes` (lines 138-157) with:

```cpp
void Cell::rebuild_meshes(const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager,
                          SurfaceScratch* scratch,
                          float simplification_ratio, float base_detail, int max_pow, float uniform_detail,
                          const Particle* carveParticles, int carveCount) {
    clear_meshes(&blas_manager);

    if (material_particle_indices.empty()) {
        return;
    }

    CellMeshResult result = build_cell_meshes(cluster_particles, scratch, simplification_ratio,
                                              base_detail, max_pow, uniform_detail,
                                              carveParticles, carveCount);
    commit_cell_meshes(result, blas_manager);
}
```

- [ ] **Step 6: Build the cell suite and verify identical geometry**

Run: `cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib/tests" && make run-cell 2>&1 | tail -20`
Expected: PASS (no failed assertions). The cell suite links `cell.cpp`, exercising `rebuild_meshes`.
If the binary is all-zero: `rm -f cell_bounds_tests && make run-cell`.

- [ ] **Step 7: Run the full suite battery (geometry guard)**

Run: `cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib/tests" && for s in cont cell tint simp cull; do echo "== $s =="; make run-$s 2>&1 | tail -5; done`
Expected: `run-cont` shows `tris=1964` / "ALL EXPECTATIONS MET"; all five report pass with no failures.

- [ ] **Step 8: Build the GUI binary**

Run: `cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib" && WSL_LINUX=1 make 2>&1 | tail -5`
Expected: `Built executable for linux`.

- [ ] **Step 9: Commit**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp"
git add MatterSurfaceLib/include/mesh_worker_pool.h MatterSurfaceLib/include/cell.h MatterSurfaceLib/src/cell.cpp
git commit -m "refactor: split per-group meshing into CPU build + main-thread commit"
```

---

## Task 3: Add the MeshWorkerPool class

A persistent hand-rolled `std::thread` pool. Each worker owns one `SurfaceScratch` for its lifetime
and pulls jobs via an atomic index. `run` dispatches a batch and blocks until all jobs complete;
`resize` joins+respawns (legal only between rebuilds, no batch in flight).

**Files:**
- Modify: `MatterSurfaceLib/include/mesh_worker_pool.h`
- Create: `MatterSurfaceLib/src/mesh_worker_pool.cpp`

- [ ] **Step 1: Append CellJob + MeshWorkerPool to the header**

In `MatterSurfaceLib/include/mesh_worker_pool.h`, add these includes to the existing include block
(after `#include <cstdint>`):

```cpp
#include "surface.h"         // SurfaceScratch, Particle
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
```

Then, before the final `#endif`, add:

```cpp
// One unit of parallel work: build every merge group's mesh for a single cell.
// Carries an owned copy of the cell's carve subset so the worker reads no shared
// mutable cluster state beyond the read-only particle vector.
struct CellJob {
    Cell* cell = nullptr;
    std::vector<Particle> carve;      // gathered carve subset for this cell (owned)
    float simplification_ratio = 1.0f;
    float base_detail = 0.0f;
    int   max_pow = 6;
    float uniform_detail = 0.0f;
};

// Persistent worker pool for CPU mesh building. Spawns `worker_count` threads,
// each owning its own SurfaceScratch for its entire lifetime. `run` executes a
// batch of jobs across the workers and blocks until all complete. `resize` is
// only legal between rebuilds (no batch in flight).
class MeshWorkerPool {
public:
    using JobFn = std::function<void(const CellJob&, SurfaceScratch*, CellMeshResult&)>;

    explicit MeshWorkerPool(int worker_count);
    ~MeshWorkerPool();

    MeshWorkerPool(const MeshWorkerPool&) = delete;
    MeshWorkerPool& operator=(const MeshWorkerPool&) = delete;

    // Runs fn(jobs[i], worker_scratch, results[i]) for every i across the workers,
    // blocking until all jobs finish. `results` is resized to jobs.size().
    void run(std::vector<CellJob>& jobs, std::vector<CellMeshResult>& results, const JobFn& fn);

    // Join existing workers and respawn `worker_count` (clamped to >= 1). Only
    // call when no batch is in flight (e.g. between rebuilds).
    void resize(int worker_count);

    int size() const { return static_cast<int>(workers_.size()); }

private:
    void start(int worker_count);
    void stop();
    void worker_loop(int worker_index);

    std::vector<std::thread> workers_;
    std::vector<SurfaceScratch*> scratches_;   // one per worker, indexed by worker id

    std::mutex m_;
    std::condition_variable cv_start_;
    std::condition_variable cv_done_;
    bool stop_ = false;

    std::vector<CellJob>* jobs_ = nullptr;       // current batch (borrowed)
    std::vector<CellMeshResult>* results_ = nullptr;
    const JobFn* fn_ = nullptr;
    std::atomic<size_t> next_{0};                // shared cursor into jobs_
    size_t batch_id_ = 0;                        // bumped per run; workers detect new batch
    size_t active_workers_ = 0;                  // workers still draining the current batch
};
```

- [ ] **Step 2: Create the implementation**

Create `MatterSurfaceLib/src/mesh_worker_pool.cpp`:

```cpp
#include "../include/mesh_worker_pool.h"
#include <cstdio>
#include <cstdlib>

MeshWorkerPool::MeshWorkerPool(int worker_count) {
    start(worker_count);
}

MeshWorkerPool::~MeshWorkerPool() {
    stop();
}

void MeshWorkerPool::start(int worker_count) {
    if (worker_count < 1) worker_count = 1;
    stop_ = false;
    batch_id_ = 0;
    next_.store(0);
    active_workers_ = 0;

    scratches_.resize(worker_count, nullptr);
    for (int i = 0; i < worker_count; ++i) {
        scratches_[i] = CreateSurfaceScratch();
        if (!scratches_[i]) {
            fprintf(stderr, "FATAL: CreateSurfaceScratch failed for mesh worker %d (out of memory)\n", i);
            abort();
        }
    }

    workers_.reserve(worker_count);
    for (int i = 0; i < worker_count; ++i) {
        workers_.emplace_back(&MeshWorkerPool::worker_loop, this, i);
    }
}

void MeshWorkerPool::stop() {
    {
        std::unique_lock<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_start_.notify_all();
    for (std::thread& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();

    for (SurfaceScratch* s : scratches_) {
        if (s) DestroySurfaceScratch(s);
    }
    scratches_.clear();
}

void MeshWorkerPool::resize(int worker_count) {
    if (worker_count < 1) worker_count = 1;
    if (static_cast<int>(workers_.size()) == worker_count) return;
    stop();
    start(worker_count);
}

void MeshWorkerPool::worker_loop(int worker_index) {
    SurfaceScratch* scratch = scratches_[worker_index];
    size_t last_batch = 0;
    for (;;) {
        std::vector<CellJob>* jobs;
        std::vector<CellMeshResult>* results;
        const JobFn* fn;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_start_.wait(lk, [&]{ return stop_ || batch_id_ != last_batch; });
            if (stop_) return;
            last_batch = batch_id_;
            jobs = jobs_;
            results = results_;
            fn = fn_;
        }

        for (;;) {
            size_t i = next_.fetch_add(1);
            if (i >= jobs->size()) break;
            (*fn)((*jobs)[i], scratch, (*results)[i]);
        }

        {
            std::unique_lock<std::mutex> lk(m_);
            if (--active_workers_ == 0) {
                cv_done_.notify_one();
            }
        }
    }
}

void MeshWorkerPool::run(std::vector<CellJob>& jobs, std::vector<CellMeshResult>& results, const JobFn& fn) {
    results.clear();
    results.resize(jobs.size());
    if (jobs.empty()) return;

    {
        std::unique_lock<std::mutex> lk(m_);
        jobs_ = &jobs;
        results_ = &results;
        fn_ = &fn;
        next_.store(0);
        active_workers_ = workers_.size();
        ++batch_id_;
    }
    cv_start_.notify_all();

    {
        std::unique_lock<std::mutex> lk(m_);
        cv_done_.wait(lk, [&]{ return active_workers_ == 0; });
    }
}
```

- [ ] **Step 3: Compile the pool object standalone**

Run: `cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib" && g++ -c src/mesh_worker_pool.cpp -std=c++14 -Wall -Wextra -I../Libraries/raylib/src -I./include -o /tmp/mwp.o && echo OK`
Expected: `OK` (no errors/warnings). This confirms the header + impl compile under C++14 with the
same include paths the main Makefile uses.

- [ ] **Step 4: Commit**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp"
git add MatterSurfaceLib/include/mesh_worker_pool.h MatterSurfaceLib/src/mesh_worker_pool.cpp
git commit -m "feat: add MeshWorkerPool (persistent per-worker-scratch thread pool)"
```

---

## Task 4: Determinism test — W=1 vs W=N produce identical geometry (GL-free)

Proves worker count never perturbs CPU output. Builds a fixed scene of `StaticParticle`s into a
`Cell`, runs `build_cell_meshes` through `MeshWorkerPool(1)` and `MeshWorkerPool(4)`, and asserts the
`GroupMeshResult` geometry (per-group triangle count + every vertex position) is byte-identical. It
never calls `UploadMesh`/`commit_*`, so it links without a GL context.

**Files:**
- Create: `MatterSurfaceLib/tests/parallel_mesh_tests.cpp`
- Modify: `MatterSurfaceLib/tests/Makefile`

- [ ] **Step 1: Write the test**

Create `MatterSurfaceLib/tests/parallel_mesh_tests.cpp`:

```cpp
// Determinism harness: build_cell_meshes output must be identical regardless of
// MeshWorkerPool worker count. GL-free (no UploadMesh / no commit).
#include "../include/cell.h"
#include "../include/cluster.h"        // StaticParticle
#include "../include/mesh_worker_pool.h"
#include <cstdio>
#include <cmath>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++g_failures; } } while(0)

// Build one cell's worth of particle indices for a fixed scene.
static void make_scene(Cell& cell, std::vector<StaticParticle>& particles) {
    // A small cluster of same-material particles inside the cell, plus a couple
    // offset ones, so meshing produces a non-trivial multi-triangle surface.
    const float r = 0.6f;
    Vector3 c = cell.center;
    auto add = [&](float dx, float dy, float dz, uint32_t mat) {
        uint32_t idx = (uint32_t)particles.size();
        particles.push_back(StaticParticle(Vector3{c.x+dx, c.y+dy, c.z+dz}, r, mat));
        cell.add_particle_index(idx, mat);
    };
    add(-0.4f, 0.0f, 0.0f, 0);
    add( 0.4f, 0.0f, 0.0f, 0);
    add( 0.0f, 0.4f, 0.0f, 0);
    add( 0.0f,-0.4f, 0.0f, 0);
    add( 0.0f, 0.0f, 0.5f, 0);
}

// Run build_cell_meshes for the cell across `workers` threads; return the result.
static CellMeshResult run_build(Cell& cell, const std::vector<StaticParticle>& particles, int workers) {
    MeshWorkerPool pool(workers);
    std::vector<CellJob> jobs;
    CellJob job;
    job.cell = &cell;
    job.simplification_ratio = 1.0f;
    job.base_detail = 0.0f;
    job.max_pow = 6;
    job.uniform_detail = 0.0f;
    jobs.push_back(job);

    std::vector<CellMeshResult> results;
    pool.run(jobs, results, [&](const CellJob& j, SurfaceScratch* scratch, CellMeshResult& out) {
        out = j.cell->build_cell_meshes(particles, scratch, j.simplification_ratio,
                                        j.base_detail, j.max_pow, j.uniform_detail, nullptr, 0);
    });
    return std::move(results[0]);
}

static void compare(const CellMeshResult& a, const CellMeshResult& b) {
    CHECK(a.groups.size() == b.groups.size(), "group count differs between W=1 and W=N");
    if (a.groups.size() != b.groups.size()) return;
    for (size_t g = 0; g < a.groups.size(); ++g) {
        const GroupMeshResult& ga = a.groups[g];
        const GroupMeshResult& gb = b.groups[g];
        CHECK(ga.group_id == gb.group_id, "group_id differs");
        CHECK(ga.mesh.vertexCount == gb.mesh.vertexCount, "vertexCount differs");
        CHECK(ga.mesh.triangleCount == gb.mesh.triangleCount, "triangleCount differs");
        CHECK(ga.triangles.size() == gb.triangles.size(), "triangle array size differs");
        if (ga.mesh.vertexCount != gb.mesh.vertexCount) continue;
        if (!ga.mesh.vertices || !gb.mesh.vertices) { CHECK(false, "null vertices"); continue; }
        for (int v = 0; v < ga.mesh.vertexCount * 3; ++v) {
            if (ga.mesh.vertices[v] != gb.mesh.vertices[v]) {
                printf("FAIL: vertex float %d differs (%.9g vs %.9g)\n", v, ga.mesh.vertices[v], gb.mesh.vertices[v]);
                ++g_failures;
                break;
            }
        }
    }
}

int main() {
    // Cell at origin; size_power 0 -> actual_size == smallest_cell_size.
    const float cell_size = 4.0f;

    Cell cell_a(Vector3{0,0,0}, 0, cell_size);
    std::vector<StaticParticle> particles_a;
    make_scene(cell_a, particles_a);

    Cell cell_b(Vector3{0,0,0}, 0, cell_size);
    std::vector<StaticParticle> particles_b;
    make_scene(cell_b, particles_b);

    CellMeshResult serial   = run_build(cell_a, particles_a, 1);
    CellMeshResult parallel = run_build(cell_b, particles_b, 4);

    CHECK(!serial.groups.empty(), "serial build produced no groups (scene meshed empty)");
    compare(serial, parallel);

    if (g_failures == 0) {
        printf("PARALLEL DETERMINISM: PASS (groups=%zu)\n", serial.groups.size());
        return 0;
    }
    printf("PARALLEL DETERMINISM: FAIL (%d failures)\n", g_failures);
    return 1;
}
```

- [ ] **Step 2: Add the Makefile target**

In `MatterSurfaceLib/tests/Makefile`, add `run-par` to the `.PHONY` line (line 59) so it reads:

```make
.PHONY: clean run run-simp run-blas run-cell run-cont run-reg run-tint run-cull run-par
```

Then append at the end of the file:

```make
# Per-cell parallel-meshing determinism harness (headless, GL-free at runtime).
# Mirrors the CELL target's C-source handling (gcc for unmangled extern "C")
# plus the worker pool. Never calls UploadMesh, so no GL context is needed.
PAR_TARGET = parallel_mesh_tests
PAR_CPP = parallel_mesh_tests.cpp ../src/cell.cpp ../src/mesh_simplifier.cpp \
          ../src/blas_manager.cpp ../src/bvh.cpp ../src/bvh_analyzer.cpp \
          ../src/mesh_worker_pool.cpp
PAR_C = ../src/surface.c ../src/open_particle_surface.c \
        ../src/spatial_hash.c ../src/object_allocator.c ../src/material_registry.c

PAR_C_OBJ = surface.o open_particle_surface.o spatial_hash.o object_allocator.o material_registry.o

$(PAR_TARGET): $(PAR_CPP) $(PAR_C)
	gcc -c $(PAR_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(PAR_CPP) $(PAR_C_OBJ) -o $(PAR_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(PAR_C_OBJ)

run-par: $(PAR_TARGET)
	./$(PAR_TARGET)
```

Also add `$(PAR_TARGET)` to the `clean` rule's `rm -f` list (line 62).

- [ ] **Step 3: Run the determinism test**

Run: `cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib/tests" && make run-par 2>&1 | tail -20`
Expected: `PARALLEL DETERMINISM: PASS (groups=...)` and exit 0. If the binary is all-zero:
`rm -f parallel_mesh_tests && make run-par`.

- [ ] **Step 4: Commit**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp"
git add MatterSurfaceLib/tests/parallel_mesh_tests.cpp MatterSurfaceLib/tests/Makefile
git commit -m "test: add W=1-vs-W=N parallel meshing determinism harness"
```

---

## Task 5: Wire the pool into Cluster's 3-phase rebuild

Replace the single `surface_scratch_` with a `MeshWorkerPool` owned by the cluster, and restructure
`rebuild_dirty_cells` into PRE (serial gather + old-BLAS release), PARALLEL (`build_cell_meshes` per
cell on worker scratches), DRAIN (serial `commit_cell_meshes` in fixed job order + one TLAS rebuild).
Drop `update_cell_meshes`.

**Files:**
- Modify: `MatterSurfaceLib/include/cluster.h`
- Modify: `MatterSurfaceLib/src/cluster.cpp`
- Modify: `MatterSurfaceLib/Makefile`

- [ ] **Step 1: Add mesh_worker_pool to the GUI Makefile**

In `MatterSurfaceLib/Makefile` line 129 (`SRC = ...`), add `src/mesh_worker_pool.cpp` (place it
right after `src/cell.cpp`). In line 130 (`OBJ = ...`), add `$(OBJ_DIR)/mesh_worker_pool.o` (right
after `$(OBJ_DIR)/cell.o`). Then add a build rule after the `cell.o` rule (after line 259):

```make
$(OBJ_DIR)/mesh_worker_pool.o: src/mesh_worker_pool.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@
```

- [ ] **Step 2: cluster.h — swap the scratch for the pool**

In `MatterSurfaceLib/include/cluster.h`:

Add to the forward declarations (after line 16, `class BLASManager;` block) :

```cpp
class MeshWorkerPool;
```

Change the include block (after line 8, `#include <memory>` already present — confirm it is; it is at
line 8). No new include needed in the header (unique_ptr to incomplete type is fine because
`~Cluster` is defined in cluster.cpp).

Replace the `surface_scratch_` member (lines 139-142) with:

```cpp
    // Persistent worker pool for per-cell CPU meshing. Owns one SurfaceScratch
    // per worker thread (replaces the former single per-cluster scratch). Sized
    // to hardware concurrency at construction; resized between rebuilds via the
    // ImGui worker slider.
    std::unique_ptr<MeshWorkerPool> mesh_pool_;
```

Add public accessors after `get_max_division_pow()` (after line 113):

```cpp
    // Number of CPU mesh worker threads. Resizing is only applied between
    // rebuilds (call from the UI before the next rebuild_dirty_cells).
    void set_mesh_worker_count(int n);
    int  get_mesh_worker_count() const;
```

Remove the `update_cell_meshes` declaration (line 150).

- [ ] **Step 3: cluster.cpp — includes + construct/destruct the pool**

In `MatterSurfaceLib/src/cluster.cpp`:

Replace the include at line 6 (`#include "../include/surface.h"`) with:

```cpp
#include "../include/mesh_worker_pool.h"   // MeshWorkerPool + SurfaceScratch
```

Add after the existing includes block (after line 14):

```cpp
#include <thread>    // std::thread::hardware_concurrency
#include <chrono>
#include <memory>
```

In the constructor, replace the `surface_scratch_` creation block (lines 46-55) with:

```cpp
    // Persistent CPU mesh worker pool: default to all-but-one hardware thread so
    // the main thread (GL/BLAS/TLAS commit) stays responsive. One SurfaceScratch
    // per worker makes the mesh build re-entrant.
    unsigned hw = std::thread::hardware_concurrency();
    int default_workers = (hw > 1u) ? (int)(hw - 1u) : 1;
    mesh_pool_ = std::make_unique<MeshWorkerPool>(default_workers);
```

In the destructor, replace the `surface_scratch_` cleanup block (lines 69-73) with:

```cpp
    // Worker pool joins its threads and frees their scratches in its destructor.
    mesh_pool_.reset();
```

- [ ] **Step 4: cluster.cpp — add the worker-count accessors**

Add near the other simple accessors (e.g. just before `Cluster::rebuild_dirty_cells` at line 245):

```cpp
void Cluster::set_mesh_worker_count(int n) {
    if (mesh_pool_) mesh_pool_->resize(n);
}

int Cluster::get_mesh_worker_count() const {
    return mesh_pool_ ? mesh_pool_->size() : 0;
}
```

- [ ] **Step 5: cluster.cpp — rewrite rebuild_dirty_cells as 3 phases**

Replace the entire `Cluster::rebuild_dirty_cells` (lines 245-297) with:

```cpp
void Cluster::rebuild_dirty_cells() {
    // One resolution for every meshed cell: derived from the globally finest
    // detail so neighboring marching-cubes grids align and stay watertight.
    float uniform_detail = compute_finest_detail();

    auto t_start = std::chrono::steady_clock::now();

    // PHASE 1 - PRE (serial, main thread): per dirty non-interior cell, gather
    // particle indices + carve subset, release the old BLAS, and queue a CellJob.
    std::vector<CellJob> jobs;
    uint32_t processed = 0;   // non-interior dirty cells handled (gates TLAS rebuild)

    for (auto& cell : cells_) {
        if (!cell->is_dirty) continue;

        uint64_t key = pack_slot(SlotCoord{
            (int)lroundf(cell->coordinates.x),
            (int)lroundf(cell->coordinates.y),
            (int)lroundf(cell->coordinates.z)});
        if (no_mesh_cells_.find(key) != no_mesh_cells_.end()) {
            cell->clear_meshes(&blas_manager_);  // interior cell: never meshed
            cell->is_dirty = false;
            continue;
        }

        // Assign particles to this cell (read-only over particles_).
        cell->clear_particle_indices();
        for (uint32_t i = 0; i < particles_.size(); ++i) {
            const StaticParticle& particle = particles_[i];
            if (cell->intersects_sphere(particle.position, particle.radius)) {
                cell->add_particle_index(i, particle.materialId);
            }
        }

        // Release the previous build's BLAS on the main thread before re-meshing.
        cell->clear_meshes(&blas_manager_);
        cell->is_dirty = false;
        cell->mesh_version++;
        processed++;

        if (cell->material_particle_indices.empty()) {
            continue;  // nothing to mesh (already cleared)
        }

        CellJob job;
        job.cell = cell.get();
        // Gather carve particles whose influence overlaps this cell (mirrors the
        // additive intersects_sphere halo; slack covers the carve fillet reach).
        for (const Particle& cpart : carve_particles_) {
            if (cell->intersects_sphere(cpart.position, cpart.radius * 1.5f))
                job.carve.push_back(cpart);
        }
        job.simplification_ratio = simplification_ratio_;
        job.base_detail = base_detail_size_;
        job.max_pow = max_division_pow_;
        job.uniform_detail = uniform_detail;
        jobs.push_back(std::move(job));
    }

    // PHASE 2 - PARALLEL: build every job's cell mesh on the worker pool. Each
    // worker uses its own SurfaceScratch; particles_ is read-only here.
    std::vector<CellMeshResult> results;
    if (!jobs.empty()) {
        mesh_pool_->run(jobs, results,
            [this](const CellJob& job, SurfaceScratch* scratch, CellMeshResult& out) {
                const Particle* carvePtr = job.carve.empty() ? nullptr : job.carve.data();
                int carveCount = static_cast<int>(job.carve.size());
                out = job.cell->build_cell_meshes(particles_, scratch, job.simplification_ratio,
                                                  job.base_detail, job.max_pow, job.uniform_detail,
                                                  carvePtr, carveCount);
            });
    }

    // PHASE 3 - DRAIN (serial, fixed job order): commit GL/BLAS deterministically.
    uint32_t committed_groups = 0;
    for (size_t i = 0; i < jobs.size(); ++i) {
        jobs[i].cell->commit_cell_meshes(results[i], blas_manager_);
        committed_groups += static_cast<uint32_t>(results[i].groups.size());
    }

    // One TLAS rebuild if anything changed (meshed or cleared).
    if (processed > 0) {
        tlas_manager_.clear();
        add_to_tlas();
        tlas_manager_.build(blas_manager_);
    }

    auto t_end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    printf("REBUILD: %u cells processed, %zu meshed / %u groups, %.1f ms (%d workers)\n",
           processed, jobs.size(), committed_groups, ms, mesh_pool_->size());
}
```

- [ ] **Step 6: cluster.cpp — delete update_cell_meshes**

Delete the entire `Cluster::update_cell_meshes` function (lines 308-343). Its logic now lives inline
in the PRE phase. (Leave `compute_finest_detail` at lines 299-306 intact.)

- [ ] **Step 7: Build the GUI binary**

Run: `cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib" && WSL_LINUX=1 make 2>&1 | tail -8`
Expected: `Built executable for linux` with no errors. Threading links via `-lpthread` (already in
the Linux LDFLAGS).

- [ ] **Step 8: Run the full suite battery (geometry guard)**

Run: `cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib/tests" && for s in cont cell tint simp cull par; do echo "== $s =="; make run-$s 2>&1 | tail -5; done`
Expected: `run-cont` `tris=1964` / "ALL EXPECTATIONS MET"; cell/tint/simp/cull pass; `run-par`
`PARALLEL DETERMINISM: PASS`. (`cont`/`simp`/`cull` don't link cluster.cpp, but cell/tint validate
the shared path; the GUI build is the real cluster integration check.)

- [ ] **Step 9: Commit**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp"
git add MatterSurfaceLib/include/cluster.h MatterSurfaceLib/src/cluster.cpp MatterSurfaceLib/Makefile
git commit -m "feat: parallelize per-cell meshing via 3-phase pre/parallel/drain rebuild"
```

---

## Task 6: ImGui worker-count slider

A live slider so the user can A/B serial (W=1) vs parallel (W=N). Applied between rebuilds via
`set_mesh_worker_count` (the pool resizes only when no batch is in flight, which is always true at
UI time since rebuilds are synchronous within the frame).

**Files:**
- Modify: `MatterSurfaceLib/main.cpp`

- [ ] **Step 1: Add the slider near the simplification control**

In `MatterSurfaceLib/main.cpp`, after the simplification slider block (after line 1405, the closing
`}` of the Simplification block) and before the `ImGui::Separator();` at line 1407, insert:

```cpp
        // Mesh worker threads: 1 = serial oracle (single worker), up to hardware
        // concurrency. Applied immediately; the pool resizes between rebuilds.
        {
            int max_workers = (int)std::thread::hardware_concurrency();
            if (max_workers < 1) max_workers = 1;
            int workers = test_cluster_->get_mesh_worker_count();
            if (ImGui::SliderInt("Mesh workers", &workers, 1, max_workers)) {
                test_cluster_->set_mesh_worker_count(workers);
            }
        }

        ImGui::Separator();
```

- [ ] **Step 2: Ensure <thread> is included in main.cpp**

Check the top of `MatterSurfaceLib/main.cpp` for `#include <thread>`. If absent, add it with the
other standard headers.

Run: `grep -n "#include <thread>" "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib/main.cpp" || echo MISSING`
If it prints `MISSING`, add `#include <thread>` near the other `#include <...>` lines at the top.

- [ ] **Step 3: Build the GUI binary**

Run: `cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib" && WSL_LINUX=1 make 2>&1 | tail -5`
Expected: `Built executable for linux`, no errors.

- [ ] **Step 4: Commit**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp"
git add MatterSurfaceLib/main.cpp
git commit -m "feat: add ImGui mesh-worker-count slider (W=1 serial oracle)"
```

---

## Final Verification

- [ ] **Step 1: Full clean build + complete suite battery**

Run:
```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/MatterSurfaceLib" && WSL_LINUX=1 make 2>&1 | tail -3
cd tests && for s in cont cell tint simp cull par; do echo "== $s =="; make run-$s 2>&1 | tail -4; done
```
Expected: GUI builds; `cont` `tris=1964`/"ALL EXPECTATIONS MET"; all suites pass; `par`
`PARALLEL DETERMINISM: PASS`.

- [ ] **Step 2: Manual GUI smoke test — DEFER TO USER**

The GUI requires a display and human judgment, so do not attempt it headless. Leave a note for the
user: run `./matter_surface_lib`, drag the "Mesh workers" slider between 1 and N, apply
carve/lumpiness, and confirm the surface is visually identical at every worker count (modulo accepted
tint ties) and that rebuilds are faster at higher worker counts. Watch the `REBUILD: ... ms (W workers)`
log line for the speedup.

## Self-Review (controller, before execution)

- **Spec coverage:** split-phase (Task 2), MeshWorkerPool (Task 3), data structures (Task 2/3),
  ImGui slider (Task 6), deterministic drain (Task 5), logging strip (Task 1), GL-free determinism
  test (Task 4) — all covered.
- **Type consistency:** `GroupMeshResult`/`CellMeshResult`/`CellJob`/`MeshWorkerPool::JobFn` names
  are used identically across Tasks 2-5. `build_cell_meshes`/`commit_cell_meshes`/`build_group_mesh`/
  `commit_group_mesh`/`set_mesh_worker_count`/`get_mesh_worker_count` signatures match between header
  and impl tasks.
- **Placeholders:** none — every code step has full content.
```
