// tbb_shim.h — shared plumbing for the header-only TBB replacement.
//
// Why this exists
// ---------------
// autoremesher_core's pipeline (and the two geogram files it drags in,
// parameterization/mesh_global_param.cpp and exploragram/hexdom/quad_cover.cpp)
// use exactly four things from TBB:
//
//     tbb::blocked_range<T>       tbb::parallel_for(range, body)
//     tbb::combinable<T>          tbb::task_scheduler_init
//
// Upstream satisfied those with a full vendored copy of wjakob's 2017 TBB
// fork, which has to be built as a shared library. That library only ever
// built on Linux, so every consumer Makefile carried a
// `thirdparty/tbb/build/linux_*_release` wildcard, a `-ltbb`, and an $ORIGIN
// rpath — and retopo was silently compiled out on Windows.
//
// Four constructs do not justify a runtime dependency. This header implements
// them on <thread>, so libautoremesher_core.a is self-contained and retopo
// builds identically on every platform.
//
// Scope discipline: implement only what the vendored sources actually call.
// If an extraction sync (see UPSTREAM.md) introduces new TBB API, the build
// fails loudly with a missing declaration — extend the shim then, rather than
// speculatively mirroring TBB's surface here.

#ifndef AUTOREMESHER_TBB_SHIM_H
#define AUTOREMESHER_TBB_SHIM_H

#include <atomic>
#include <cstddef>
#include <thread>

namespace tbb {
namespace shim {

// Thread budget, set once by task_scheduler_init (see task_scheduler_init.h).
// 0 means "auto" -> hardware_concurrency.
inline std::atomic<unsigned>& configured_threads()
{
    static std::atomic<unsigned> n{0};
    return n;
}

inline unsigned max_threads()
{
    unsigned n = configured_threads().load(std::memory_order_relaxed);
    if (n == 0) {
        n = std::thread::hardware_concurrency();
        if (n == 0)
            n = 1;
    }
    return n;
}

// Live helper threads spawned by parallel_for, process-wide.
inline std::atomic<int>& live_helpers()
{
    static std::atomic<int> n{0};
    return n;
}

// Reserve up to `want` helper threads against the global budget, returning how
// many were granted (possibly 0).
//
// This bounded-spawn scheme is what makes nesting safe. autoremesher nests
// parallel_for three deep -- islands -> parameterize -> geogram's per-facet
// loops -- and a fixed worker pool would deadlock there the moment an outer
// task occupied a pool thread and then waited on inner tasks queued behind it.
// Here every parallel_for owns and joins the threads it creates, so a thread
// never waits on work that cannot start. When the budget is exhausted the
// caller simply runs its range inline, which is always forward-progress-safe.
// The calling thread does a share of the work, hence the -1.
inline int reserve_helpers(int want)
{
    if (want <= 0)
        return 0;
    const int budget = static_cast<int>(max_threads()) - 1;
    int cur = live_helpers().load(std::memory_order_relaxed);
    for (;;) {
        int grant = budget - cur;
        if (grant <= 0)
            return 0;
        if (grant > want)
            grant = want;
        if (live_helpers().compare_exchange_weak(cur, cur + grant,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed))
            return grant;
    }
}

inline void release_helpers(int n)
{
    if (n > 0)
        live_helpers().fetch_sub(n, std::memory_order_relaxed);
}

} // namespace shim
} // namespace tbb

#endif // AUTOREMESHER_TBB_SHIM_H
