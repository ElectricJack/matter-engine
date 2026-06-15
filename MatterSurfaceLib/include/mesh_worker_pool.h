#ifndef MESH_WORKER_POOL_H
#define MESH_WORKER_POOL_H

#include "raylib.h"          // Mesh
#include "bvh.h"             // Tri, TriEx
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
