// tbb/blocked_range.h — shim replacement. See tbb_shim.h for rationale.
//
// Instantiated with size_t (autoremesher pipeline) and GEO::index_t /
// unsigned int (geogram). Only the two-argument constructor is used; the
// grainsize overload is kept because parallel_for's splitter honours it.

#ifndef AUTOREMESHER_TBB_SHIM_BLOCKED_RANGE_H
#define AUTOREMESHER_TBB_SHIM_BLOCKED_RANGE_H

#include <cstddef>

namespace tbb {

template <typename Value>
class blocked_range {
public:
    using const_iterator = Value;
    using size_type = std::size_t;

    blocked_range(Value begin_, Value end_, size_type grainsize_ = 1)
        : begin_v(begin_), end_v(end_), grain(grainsize_ ? grainsize_ : 1)
    {
    }

    Value begin() const { return begin_v; }
    Value end() const { return end_v; }
    size_type grainsize() const { return grain; }
    bool empty() const { return !(begin_v < end_v); }

    size_type size() const
    {
        return empty() ? size_type(0)
                       : static_cast<size_type>(end_v - begin_v);
    }

private:
    Value begin_v;
    Value end_v;
    size_type grain;
};

} // namespace tbb

#endif // AUTOREMESHER_TBB_SHIM_BLOCKED_RANGE_H
