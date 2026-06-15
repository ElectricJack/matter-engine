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
