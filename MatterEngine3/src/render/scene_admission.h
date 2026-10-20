#pragma once

// Internal scene-admission protocol. The public render API remains bool/string;
// only this producer may tag a predictable, pre-allocation capacity rejection.
// Vulkan failures never receive this tag.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace viewer::admission {
enum class Reason { none, storage_range, addressing, arithmetic_overflow };
enum class Buffer : size_t {
    triangles, bricks, cells, instances, roots, levels, visible, commands,
    texture_mips, texels, texture_pages, children, placements, work, addresses,
    count
};
inline const char* name(Buffer buffer) {
    constexpr const char* names[]={"surface triangles", "voxel bricks", "voxel cells",
        "local instances", "roots", "levels", "visible indices", "indirect commands",
        "texture mips", "texture texels", "texture pages", "hierarchy children",
        "world placements", "hierarchy work", "assembly addresses"};
    return names[size_t(buffer)];
}
struct Failure {
    Reason reason=Reason::none;
    Buffer buffer=Buffer::triangles;
    uint64_t requested_bytes=0, allowed_bytes=0;
    size_t asset=0;
    explicit operator bool() const { return reason!=Reason::none; }
    std::string message() const {
        if(!*this) return {};
        return "[scene-admission:"+std::to_string(int(reason))+"] "+name(buffer)+
            " catalog asset="+std::to_string(asset)+" requested_bytes="+
            std::to_string(requested_bytes)+" allowed_bytes="+std::to_string(allowed_bytes)+
            ". Requested scene rejected; reduce the catalog or select a smaller world.";
    }
};
inline bool rejected(const std::string& error) {
    return error.rfind("[scene-admission:",0)==0;
}

// Count-only planner used before flattened CPU arrays or GPU uploads. Division
// precedes multiplication; even synthetic SIZE_MAX counts cannot wrap to fit.
class Plan {
public:
    explicit Plan(uint64_t storage_limit):limit_(storage_limit) {}
    bool add(Buffer buffer,uint64_t count,uint64_t stride,size_t asset,
             uint64_t address_limit=UINT32_MAX) {
        if(failure) return false;
        auto& total=counts_[size_t(buffer)];
        const auto maximum=std::numeric_limits<uint64_t>::max();
        if(count>maximum-total || (stride && total+count>maximum/stride))
            return fail(Reason::arithmetic_overflow,buffer,maximum,limit_,asset);
        const uint64_t next=total+count,bytes=next*stride;
        if(next>std::numeric_limits<size_t>::max()/std::max(uint64_t(1),stride))
            return fail(Reason::arithmetic_overflow,buffer,bytes,limit_,asset);
        if(bytes>limit_) return fail(Reason::storage_range,buffer,bytes,limit_,asset);
        if(next>address_limit)
            return fail(Reason::addressing,buffer,bytes,address_limit*stride,asset);
        total=next;
        return true;
    }
    bool bound(Buffer buffer,uint64_t count,uint64_t allowed,uint64_t stride,size_t asset) {
        if(failure) return false;
        if(count<=allowed) return true;
        const auto maximum=std::numeric_limits<uint64_t>::max();
        return fail(Reason::addressing,buffer,stride && count>maximum/stride?maximum:count*stride,
                    stride && allowed>maximum/stride?maximum:allowed*stride,asset);
    }
    uint64_t count(Buffer buffer) const {return counts_[size_t(buffer)];}
    Failure failure;
private:
    bool fail(Reason reason,Buffer buffer,uint64_t requested,uint64_t allowed,size_t asset) {
        failure={reason,buffer,requested,allowed,asset};return false;
    }
    uint64_t limit_;
    std::array<uint64_t,size_t(Buffer::count)> counts_{};
};

#ifdef MATTER_VK_TEST_FAULT_INJECTION
inline thread_local uint64_t test_storage_limit=UINT64_MAX;
enum class Preparation { none, primary, shadow };
inline thread_local Preparation test_failure=Preparation::none;
struct ScopedPreparationFailure {
    Preparation previous=test_failure;
    explicit ScopedPreparationFailure(Preparation failure) {test_failure=failure;}
    ~ScopedPreparationFailure() {test_failure=previous;}
    ScopedPreparationFailure(const ScopedPreparationFailure&)=delete;
    ScopedPreparationFailure& operator=(const ScopedPreparationFailure&)=delete;
};
struct ScopedLimit {
    uint64_t previous=test_storage_limit;
    explicit ScopedLimit(uint64_t limit) {test_storage_limit=limit;}
    ~ScopedLimit() {test_storage_limit=previous;}
    ScopedLimit(const ScopedLimit&)=delete;
    ScopedLimit& operator=(const ScopedLimit&)=delete;
};
#endif
inline uint64_t storage_limit(uint64_t physical) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    return std::min(physical,test_storage_limit);
#else
    return physical;
#endif
}
} // namespace viewer::admission
