#pragma once

#include <memory>

namespace matter_physx {

template <class T>
struct PxRelease {
    void operator()(T* value) const noexcept {
        if (value) value->release();
    }
};

template <class T>
using PxOwner = std::unique_ptr<T, PxRelease<T>>;

}  // namespace matter_physx
