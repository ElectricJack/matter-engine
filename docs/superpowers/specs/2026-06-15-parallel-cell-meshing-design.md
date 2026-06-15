# Per-Cell Parallel Meshing Design

**Date:** 2026-06-15
**Status:** Approved, ready for planning

## Problem

`Cluster::rebuild_dirty_cells` (MatterSurfaceLib/src/cluster.cpp) rebuilds every dirty cell's
mesh on the main thread, one cell at a time. Each cell rebuild does a marching-cubes field eval,
mesh simplification, normal recompute, and per-triangle material tagging — all pure CPU work that
dominates rebuild latency (carve/lumpiness edits regenerate hundreds of cell/groups). The mesher
was just made re-entrant via `SurfaceScratch` (one caller-owned context per call site, no shared
mutable global on the explicit-scratch path), so the CPU-heavy portion can now run on multiple
cores: one scratch per worker thread.

The blocker is that the rebuild interleaves CPU work with main-thread-only operations:
GL uploads (`UploadMesh`), shared-BLAS registration (`BLASManager::register_triangles`), shared
global reporting (`BVHReportManager`), `material_meshes`/`material_blas` map writes, old-BLAS
release (`clear_meshes`), and the final TLAS rebuild. These cannot run off the main thread.

## Goals

- Run the per-cell CPU mesh build (`GenerateMeshWithScratch`, `simplify_mesh`,
  `ComputeSurfaceNormalsWithScratch`, `convert_mesh_to_triangles`, per-triangle hash tagging)
  across multiple cores using a persistent worker pool, one `SurfaceScratch` per worker.
- Keep all GL/BLAS/TLAS/map/report operations on the main thread, executed in a deterministic
  order so the parallel result is identical to the serial result (same BLAS/TLAS handles, same
  geometry).
- Make worker count user-tunable at runtime via an ImGui slider, with `W=1` acting as a serial
  oracle for A/B verification.
- Keep mesh **geometry byte-identical** to the current serial path (verified by the five headless
  suites; continuity stays `tris=1964`). Tint may differ only on the already-accepted exact
  distance ties.

## Non-Goals

- Parallelizing the GL upload, BLAS build, or TLAS build (main-thread-only by design).
- Changing marching-cubes / SDF math, carve/clip fields, or simplification.
- Cross-cluster parallelism (this targets per-cell within a cluster rebuild).
- Work-stealing or task-graph schedulers — a simple job queue suffices for the per-cell grain.

## Architecture: split-phase rebuild

`generate_mesh_for_group` is split at the CPU/main boundary into two functions:

- **`build_group_mesh(...) -> GroupMeshResult`** — pure CPU, takes a `SurfaceScratch*`, runs the
  particle/clip/carve vector build, `GenerateMeshWithScratch`, `simplify_mesh`,
  `ComputeSurfaceNormalsWithScratch`, `convert_mesh_to_triangles`, and the per-triangle hash
  material/tint tagging (which must run in the same worker right after generation because it reads
  the live scratch hash). Returns a `GroupMeshResult` (CPU mesh + triangle arrays + per-triangle
  material/tint, fully detached from any GL/BLAS state). No GL, no shared globals, no logging.
- **`commit_group_mesh(const GroupMeshResult&, ...)`** — main-thread only, takes the result and
  performs `UploadMesh`, `material_meshes`/`material_blas` map writes,
  `BLASManager::register_triangles`, and `BVHReportManager` Register/UpdateAnalysis.

The cluster rebuild runs in three phases:

1. **PRE (serial, main thread).** For each dirty cell: handle interior-cull (`no_mesh_cells_` via
   `clear_meshes`), gather per-cell particle indices (`intersects_sphere`, read-only on
   `particles_`) and the per-cell carve subset, release the old BLAS (`clear_meshes`), and build a
   `CellJob` capturing the `Cell*`, the gathered carve vector, and scalar params. Append to an
   ordered `std::vector<CellJob>` (order = `cells_` iteration order).
2. **PARALLEL (N workers).** Submit all `CellJob`s to the `MeshWorkerPool`. Each worker pulls a
   job, runs `build_group_mesh` for every group in that cell using **its own** `SurfaceScratch`,
   and writes a `CellMeshResult` into `results[jobIndex]` (each job owns a distinct slot — no
   write contention). `pool.wait()` blocks until all jobs finish.
3. **DRAIN (serial, main thread).** Iterate `results` in job order (= deterministic cell order);
   for each `CellMeshResult`, `commit_group_mesh` every `GroupMeshResult` in fixed group order.
   Then one TLAS rebuild and one summary log line.

Determinism: because commits happen strictly in job-index order on the main thread, BLAS/TLAS
handle assignment and map insertion order are identical to the serial path regardless of worker
count. The parallel phase only produces CPU data; it cannot affect handle ordering.

## Data structures

```cpp
// New header: MatterSurfaceLib/include/mesh_worker_pool.h (result types live here or in cell.h)

struct GroupMeshResult {
    uint32_t group_id;
    Mesh mesh;                              // CPU mesh (vertices/normals/indices), pre-UploadMesh
    std::vector<Tri> triangles;            // from convert_mesh_to_triangles
    std::vector<TriEx> triangle_normals;   // per-triangle extended data
    // per-triangle material/tint already resolved during build (hash tagging)
};

struct CellMeshResult {
    std::vector<GroupMeshResult> groups;
};

struct CellJob {
    Cell* cell;
    std::vector<Particle> carve;           // gathered carve subset for this cell (owned copy)
    const Particle* carve_ptr;             // points into carve (or null)
    int carve_count;
    float simplification_ratio;
    float base_detail;
    int max_pow;
    float uniform_detail;
};
```

Exact field set on `Tri`/`TriEx` and how material/tint ride along is whatever the current
`generate_mesh_for_group` already produces — the split must preserve it verbatim, just relocating
the GL/BLAS half into `commit_group_mesh`.

## MeshWorkerPool

Hand-rolled persistent pool (header `mesh_worker_pool.h`, impl `src/mesh_worker_pool.cpp`):

```cpp
class MeshWorkerPool {
public:
    explicit MeshWorkerPool(int worker_count);   // spawns worker_count threads, one scratch each
    ~MeshWorkerPool();                            // joins all threads, destroys scratches

    // Run fn(job, scratch) for each job across workers; blocks until all complete.
    void run(std::vector<CellJob>& jobs,
             std::vector<CellMeshResult>& results,
             const std::function<void(const CellJob&, SurfaceScratch*, CellMeshResult&)>& fn);

    void resize(int worker_count);   // only legal between rebuilds (no jobs in flight)
    int  size() const;
};
```

Internals:
- A `std::vector<std::thread>` workers, each owning a `SurfaceScratch* = CreateSurfaceScratch()`
  for its entire lifetime (created in the worker, destroyed on join).
- A shared atomic job index (`std::atomic<size_t> next`) into the current `jobs` vector; each
  worker does `i = next.fetch_add(1)` and processes `jobs[i]` until `i >= jobs.size()`. This is
  the simplest correct dispatch for the per-cell grain (no per-job condition_variable signaling).
- `run` sets up the batch (jobs/results/fn pointers), wakes workers via a `condition_variable`,
  and blocks on a completion counter / second `condition_variable` until all workers report idle.
- `resize` joins existing workers and respawns; only called between rebuilds (enforced by calling
  it from the main loop outside `rebuild_dirty_cells`). Asserts no batch is active.
- A worker count of 1 spawns one worker — functionally a serial oracle (still off the main
  thread, but single-stream so geometry/commit order is trivially identical).

Ownership: the pool replaces the single `surface_scratch_` currently on `Cluster`
(cluster.cpp:47). The legacy `g_defaultScratch` in surface.c remains for the legacy
single-threaded API and headless tests; the parallel path never touches it.

## ImGui worker slider

A slider in main.cpp near the existing carve/lumpiness knobs:
`ImGui::SliderInt("Mesh workers", &worker_count, 1, hardware_concurrency())`. Default
`max(1, hardware_concurrency() - 1)`. The value is applied by calling `pool.resize(worker_count)`
**only when it changed and no rebuild is in flight** (i.e., at the top of the frame before
`rebuild_dirty_cells`). `W=1` is the serial oracle for visual A/B.

## Logging

Per the approved decision, strip per-mesh logging from the parallel path:
- Set `ENABLE_PERFORMANCE_TIMING` to `0` in surface.c (the PERF `printf` macros are diagnostic
  spam; under threads they interleave and the `printf` lock serializes workers). This removes all
  per-mesh PERF output at zero runtime cost.
- Remove the per-group `printf` lines in `generate_mesh_for_group` (cell.cpp lines ~439/473/475/503)
  from the CPU `build_group_mesh` path. `build_group_mesh` must produce **no stdout**.
- Keep exactly one main-thread summary line at the end of `rebuild_dirty_cells`
  (e.g. `rebuilt N cells / M groups in T ms (W workers)`).

## Thread-safety boundary (verified)

Parallel-safe with per-worker scratch: particle/clip/carve vector build,
`GenerateMeshWithScratch`, `simplify_mesh`, `ComputeSurfaceNormalsWithScratch`,
`convert_mesh_to_triangles`, per-triangle hash tag (`SurfaceScratchHash` +
`sh_query_radius_nearest`, run in-worker right after generation). Material registry reads
(`MaterialIsTransparent`/`GetMaterialColor`) are pure reads of `static const` tables — safe.

Main-thread-only / serial drain: `UploadMesh` (GL), `BLASManager::register_triangles` (shared),
`BVHReportManager` Register/UpdateAnalysis (shared global), `material_meshes`/`material_blas`
writes, `clear_meshes` old-BLAS release (done in PRE), TLAS rebuild.

## Error handling

- `CreateSurfaceScratch` failure in a worker: fatal — `fprintf(stderr, ...)` + `abort()`, matching
  the existing cluster ctor idiom. A worker without a scratch cannot mesh.
- Empty job list: `run` returns immediately; DRAIN does nothing; no TLAS rebuild (matches current
  `rebuilt_count == 0` short-circuit).
- A cell that meshes to zero groups produces an empty `CellMeshResult`; DRAIN commits nothing for
  it (same as serial).
- Exceptions are not expected in the CPU path (C mesher + POD vectors); the pool does not add a
  try/catch — a crash in a worker is a real bug to surface, not to swallow.

## Testing

- **Determinism test (new, headless, GL-free):** `MatterSurfaceLib/tests/parallel_mesh_tests.cpp`
  + a `run-par` Makefile target. Build a fixed particle scene, run `build_group_mesh` for all
  groups via a `MeshWorkerPool(1)` and again via `MeshWorkerPool(N)`, and assert the
  `GroupMeshResult` geometry (triangle/vertex counts and positions) is byte-identical between the
  two. This proves worker count doesn't perturb CPU output. `build_group_mesh` is GL-free
  (`convert_mesh_to_triangles` works on CPU mesh arrays before `UploadMesh`), so this links
  without raylib rendering objects.
- **Existing five suites** (cont/cell/tint/simp/cull) must still pass; continuity stays
  `tris=1964`. The serial commit path is unchanged in behavior, so these guard the split.
- **Manual (deferred to user):** run the GUI, exercise carve/lumpiness regeneration at W=1 and
  W=N, confirm the surface is visually identical (modulo accepted tint ties) and rebuilds are
  faster at higher W.

## Risks

- **Split drops state.** Relocating the GL/BLAS half into `commit_group_mesh` could miss a field
  that `convert_mesh_to_triangles`/tagging needs. Mitigated by the determinism test + the five
  suites (any geometry/tag drift fails them).
- **Resize during a rebuild.** Guarded by only calling `resize` at frame top before
  `rebuild_dirty_cells`, with an assert that no batch is active.
- **False sharing on `results[jobIndex]`.** Each job writes a distinct vector slot; the vectors
  hold heap-allocated `std::vector` payloads, so contention is negligible. Not optimized further.
- **WSL/DrvFs binary corruption** (known): freshly-linked binaries may all-zero; fix is
  `rm -f <bin> && make run-<suite>`.
