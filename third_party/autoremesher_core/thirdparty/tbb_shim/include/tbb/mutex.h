// tbb/mutex.h — shim replacement. See tbb_shim.h for rationale.
//
// autoremesher.cpp includes this header but never names tbb::mutex (it locks
// with std::mutex / std::lock_guard). Provided so the include resolves, with
// TBB's scoped_lock spelling in case a future extraction sync starts using it.

#ifndef AUTOREMESHER_TBB_SHIM_MUTEX_H
#define AUTOREMESHER_TBB_SHIM_MUTEX_H

#include <mutex>

namespace tbb {

class mutex : public std::mutex {
public:
    using scoped_lock = std::lock_guard<std::mutex>;
};

} // namespace tbb

#endif // AUTOREMESHER_TBB_SHIM_MUTEX_H
