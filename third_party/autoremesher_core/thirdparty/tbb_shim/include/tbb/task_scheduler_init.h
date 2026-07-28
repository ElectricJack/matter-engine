// tbb/task_scheduler_init.h — shim replacement. See tbb_shim.h for rationale.
//
// In real TBB this class starts and anchors the scheduler. Here there is no
// scheduler to start — parallel_for spawns and joins its own threads — so the
// only job left is recording the thread budget from remesh()'s `opts.threads`.
//
// remesh.cpp constructs one of these under std::call_once and deliberately
// leaks it, because wjakob's 2017 TBB segfaulted if the scheduler was ever
// destroyed and re-inited. That constraint is gone: this type is inert, safe
// to construct and destroy any number of times, and setting the budget is
// idempotent. remesh.cpp is left as-is so the include stays guarded by its
// existing __has_include check.

#ifndef AUTOREMESHER_TBB_SHIM_TASK_SCHEDULER_INIT_H
#define AUTOREMESHER_TBB_SHIM_TASK_SCHEDULER_INIT_H

#include <tbb/tbb_shim.h>

namespace tbb {

class task_scheduler_init {
public:
    static constexpr int automatic = -1;
    static constexpr int deferred = -2;

    explicit task_scheduler_init(int max_threads = automatic)
    {
        if (max_threads > 0)
            shim::configured_threads().store(static_cast<unsigned>(max_threads),
                                             std::memory_order_relaxed);
        else
            shim::configured_threads().store(0, std::memory_order_relaxed);
    }

    ~task_scheduler_init() = default;

    task_scheduler_init(const task_scheduler_init&) = delete;
    task_scheduler_init& operator=(const task_scheduler_init&) = delete;

    static int default_num_threads() { return static_cast<int>(shim::max_threads()); }
};

} // namespace tbb

#endif // AUTOREMESHER_TBB_SHIM_TASK_SCHEDULER_INIT_H
