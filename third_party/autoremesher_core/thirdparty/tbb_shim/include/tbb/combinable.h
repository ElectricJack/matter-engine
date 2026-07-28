// tbb/combinable.h — shim replacement. See tbb_shim.h for rationale.
//
// Per-thread accumulators, used by param_hdc.cpp and geogram's
// mesh_global_param.cpp to scatter-accumulate into per-vertex arrays without
// contention, then fold the partials. Only local() and combine_each() are
// called.
//
// Keyed on std::thread::id. Two live threads never share an id, so local()
// hands out an exclusive reference. Ids may be recycled after a thread exits,
// which at worst means a later thread inherits an earlier one's partial — the
// fold still visits every contribution exactly once, so the result is
// unchanged. std::map is node-based, so references handed out by local()
// remain valid as further threads register.

#ifndef AUTOREMESHER_TBB_SHIM_COMBINABLE_H
#define AUTOREMESHER_TBB_SHIM_COMBINABLE_H

#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace tbb {

template <typename T>
class combinable {
public:
    combinable() : init([]() { return T(); }) {}

    template <typename Finit>
    explicit combinable(Finit finit) : init(std::move(finit))
    {
    }

    // Call sites invoke this once per sub-range, not per element, so the
    // mutex is not on a hot path.
    T& local()
    {
        const std::thread::id id = std::this_thread::get_id();
        std::lock_guard<std::mutex> guard(mutex);
        typename std::map<std::thread::id, T>::iterator it = slots.find(id);
        if (it == slots.end())
            it = slots.emplace(id, init()).first;
        return it->second;
    }

    template <typename Func>
    void combine_each(Func f)
    {
        std::lock_guard<std::mutex> guard(mutex);
        for (typename std::map<std::thread::id, T>::iterator it = slots.begin();
             it != slots.end(); ++it)
            f(it->second);
    }

    void clear()
    {
        std::lock_guard<std::mutex> guard(mutex);
        slots.clear();
    }

private:
    std::function<T()> init;
    std::mutex mutex;
    std::map<std::thread::id, T> slots;
};

} // namespace tbb

#endif // AUTOREMESHER_TBB_SHIM_COMBINABLE_H
