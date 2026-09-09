// libs/MatterSurfaceLib/src/mesh_worker_pool.cpp
//
// Persistent worker pool for CPU mesh building. See `include/mesh_worker_pool.h`
// for the job/result types. Threads live for the pool's whole lifetime — they
// are not spawned per batch — and each owns one `SurfaceScratch` allocated at
// `start()` and destroyed at `stop()`, so the mesher's per-thread scratch never
// has to be reallocated.
//
// Batch protocol. There is exactly ONE batch in flight at a time:
//
//   run() on the owner thread            worker_loop() on each worker
//   ----------------------------------   ----------------------------------
//   resize `results` to jobs.size()
//   under m_: publish jobs_/results_/fn_,
//     next_ = 0, active_workers_ = N,
//     ++batch_id_                        wakes when batch_id_ != last_batch,
//   notify_all(cv_start_)                  snapshots the three pointers under m_
//   wait on cv_done_ until               races on the atomic next_ cursor,
//     active_workers_ == 0                 calls fn(job, scratch, result)
//                                        under m_: --active_workers_, and the
//                                          last one notifies cv_done_
//
// Consequences worth knowing:
//   - `jobs_`, `results_` and `fn_` are BORROWED raw pointers valid only for
//     the duration of `run()`. Because `run()` blocks until the batch drains,
//     the caller's objects (including the `JobFn` itself) stay alive — but
//     nothing else may be dispatched onto the pool meanwhile.
//   - Workers write only `results[i]` for the `i` they claimed from `next_`,
//     and `results` is sized before dispatch, so result writes need no lock.
//     The mutex guards only the batch handoff and the completion count.
//   - `run()` is NOT reentrant and must not be called from a worker or from two
//     threads at once. `resize()` and the destructor still should not run while
//     a batch is in flight — the batch's results are abandoned either way — but
//     a worker that observes `stop_` before claiming the current batch releases
//     its share of `active_workers_` on the way out, so an in-flight `run()`
//     returns rather than deadlocking.
//   - Job order is not deterministic across workers, but each job writes its
//     own slot, so `results` is always in `jobs` order.
//
// Failure policy: a `SurfaceScratch` allocation failure at `start()` is treated
// as unrecoverable and calls `abort()` — there is no degraded single-threaded
// fallback.
#include "../include/mesh_worker_pool.h"
#include <cstdio>
#include <cstdlib>

MeshWorkerPool::MeshWorkerPool(int worker_count) {
    start(worker_count);
}

MeshWorkerPool::~MeshWorkerPool() {
    stop();
}

// Allocates one SurfaceScratch per worker BEFORE spawning any thread, so
// `worker_loop` can index `scratches_` without synchronization. A scratch
// allocation failure aborts the process (see the file header). Not safe to
// call on a live pool — `resize()` stops first.
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

// Signals `stop_`, joins every worker, then destroys the scratches. Safe to
// call on an already-stopped pool (both vectors are empty, so it is a no-op).
// Must NOT be called while a batch is in flight — see the file header.
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

// One worker's whole life: park on `cv_start_` until either `stop_` is set or
// `batch_id_` moves off the batch this worker last drained, snapshot the batch
// pointers under the lock, then pull job indices off the shared atomic cursor
// until it runs past the end. `last_batch` starting at 0 matches `start()`
// resetting `batch_id_` to 0, so a freshly started worker does not see a
// phantom batch.
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
            if (stop_) {
                // A batch published but never claimed by this worker still
                // counts it in active_workers_ (run() sets the count to the
                // whole worker set up front). Give the count back before
                // leaving, or the owner's cv_done_ wait never releases. When
                // batch_id_ == last_batch this worker already decremented for
                // the batch it drained, so there is nothing to release.
                if (batch_id_ != last_batch && active_workers_ > 0) {
                    if (--active_workers_ == 0) cv_done_.notify_one();
                }
                return;
            }
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

// Blocks the calling thread until the whole batch is done. `results` is cleared
// and resized to `jobs.size()` first, so anything the caller had in it is lost.
// An empty `jobs` returns immediately without waking the workers.
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
