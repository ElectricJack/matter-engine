// tbb/parallel_for.h — shim replacement. See tbb_shim.h for rationale.
//
// Only the `parallel_for(const blocked_range<T>&, Body)` overload is provided,
// because that is the only form the vendored sources call. Body is invoked
// with a sub-range, either as a lambda or as a functor with
// `void operator()(const blocked_range<T>&) const` (autoremesher's
// IsotropicPhase / SurfaceParameterizer classes take the latter shape).

#ifndef AUTOREMESHER_TBB_SHIM_PARALLEL_FOR_H
#define AUTOREMESHER_TBB_SHIM_PARALLEL_FOR_H

#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include <tbb/blocked_range.h>
#include <tbb/tbb_shim.h>

namespace tbb {

template <typename Value, typename Body>
void parallel_for(const blocked_range<Value>& range, const Body& body)
{
    using Size = std::size_t;

    const Size n = range.size();
    if (n == 0)
        return;

    // Never hand a thread fewer than `grainsize` elements.
    const Size grain = range.grainsize();
    const Size max_chunks = (n + grain - 1) / grain;

    Size want = static_cast<Size>(shim::max_threads());
    if (want > max_chunks)
        want = max_chunks;

    const int helpers = want > 1 ? shim::reserve_helpers(static_cast<int>(want) - 1) : 0;
    if (helpers <= 0) {
        // Budget exhausted (we are nested inside another parallel_for) or the
        // range is too small to be worth splitting: run inline.
        body(range);
        return;
    }

    const Size parts = static_cast<Size>(helpers) + 1;

    // Exact, overflow-free chunk boundaries: the first `rem` chunks get one
    // extra element.
    const Size base = n / parts;
    const Size rem = n % parts;
    const Value origin = range.begin();

    std::exception_ptr first_error;
    std::mutex error_mutex;

    auto run_chunk = [&](Size k) {
        const Size lo = base * k + (k < rem ? k : rem);
        const Size hi = base * (k + 1) + (k + 1 < rem ? k + 1 : rem);
        if (lo >= hi)
            return;
        try {
            body(blocked_range<Value>(static_cast<Value>(origin + lo),
                                      static_cast<Value>(origin + hi),
                                      grain));
        } catch (...) {
            std::lock_guard<std::mutex> guard(error_mutex);
            if (!first_error)
                first_error = std::current_exception();
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(helpers));
    for (Size k = 1; k < parts; ++k)
        workers.emplace_back(run_chunk, k);

    run_chunk(0);

    for (std::thread& t : workers)
        t.join();
    shim::release_helpers(helpers);

    if (first_error)
        std::rethrow_exception(first_error);
}

} // namespace tbb

#endif // AUTOREMESHER_TBB_SHIM_PARALLEL_FOR_H
