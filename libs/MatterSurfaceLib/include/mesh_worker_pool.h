#ifndef MESH_WORKER_POOL_H
#define MESH_WORKER_POOL_H

// libs/MatterSurfaceLib/include/mesh_worker_pool.h
//
// The persistent thread pool that MatterSurfaceLib meshes cells on, plus the
// plain-data job and result structs that cross the thread boundary.
//
// Where it sits: MatterSurfaceLib. `Cluster` owns the pool and calls `run`
// once per rebuild with one `CellJob` per cell; the job function is
// `Cell::build_group_mesh`, which drives the `MeshingAlgorithm`
// implementations declared in `meshing_algorithm.h`. Everything produced here
// is CPU-side — the results are committed to GPU/BLAS state later, on the
// main thread — which is why the pool sits below any Vulkan code.
//
// Lifecycle:
//   MeshWorkerPool pool(n);        // spawns n threads + n SurfaceScratch
//   pool.run(jobs, results, fn);   // blocks until every job is done
//   pool.resize(m);                // only between rebuilds
//   // destructor signals stop, joins every worker, destroys the scratches
//
// Threading rules:
//   - `run` blocks the calling thread, and is single-caller: one `run` at a
//     time, from one thread. There is no queue, no future, no way to poll
//     progress.
//   - Jobs are pulled off a shared atomic cursor, so distribution is dynamic.
//     A job's index does not determine which worker executes it, and cheap
//     and expensive cells balance out on their own — but do not rely on any
//     execution order between jobs.
//   - Each worker owns one `SurfaceScratch` for its entire lifetime and hands
//     it to `fn`. `fn` therefore runs concurrently on N threads and must
//     touch nothing shared and mutable beyond the scratch it is given and its
//     own `results[i]` slot. `CellJob` carries an owned copy of the cell's
//     carve subset for exactly this reason.
//   - `resize` joins every worker and respawns from scratch, destroying and
//     recreating all `SurfaceScratch` state. It is not safe while a batch is
//     in flight.
//
// Gotchas:
//   - Construction allocates one `SurfaceScratch` per worker and `abort()`s
//     the process if any allocation fails; there is no failure return and no
//     degraded mode.
//   - `worker_count` is clamped up to 1 in both the constructor and `resize`,
//     so a pool is never empty and `run` always makes progress.
//   - Non-copyable and non-assignable — it owns threads.
//   - `raylib.h` is included only for the POD `Mesh` type; nothing here
//     touches GL.

#include "raylib.h"          // Mesh
#include "tri.h"             // Tri, TriEx
#include <vector>
#include <cstdint>
#include "surface.h"         // SurfaceScratch, Particle
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>

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
    //
    // Single-caller: one `run` at a time on a pool, from one thread. `fn` is
    // invoked concurrently on every worker, so the only per-job state it may
    // write is its own `results[i]`. Job execution order is unspecified —
    // work is claimed off a shared atomic cursor. An empty `jobs` returns
    // immediately with `results` cleared and the workers left asleep.
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

#endif // MESH_WORKER_POOL_H
