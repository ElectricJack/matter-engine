#pragma once

#include "vk_resources.h"
#include "vt_types.h"
#include <algorithm>
#include <memory>
#include <new>
#include <vector>

namespace vt {

// Sparse, immutable R16 page factors. Pages share device allocations, but each
// lease keeps its subrange occupied until every GPU reader has retired.
class VtOcclusionPages {
public:
    static constexpr uint32_t kPagesPerSlab=32;
    static constexpr uint32_t kPageBytes=kVtPageStride*kVtPageStride*2;
    static_assert(kPageBytes%16==0,"occlusion page addresses stay 16-byte aligned");
    struct Slab {
        matter::VkBufferResource buffer;
        uint32_t free_bits=UINT32_MAX,used=0;
    };
    struct Page {
        std::shared_ptr<Slab> slab;
        uint32_t index;
        Page(std::shared_ptr<Slab> owner,uint32_t slot):slab(std::move(owner)),index(slot) {
            slab->free_bits&=~(1u<<index);++slab->used;
        }
        ~Page(){slab->free_bits|=1u<<index;--slab->used;}
        Page(const Page&)=delete;Page& operator=(const Page&)=delete;
        VkDeviceSize offset() const {return VkDeviceSize(index)*kPageBytes;}
        VkDeviceAddress address() const {return slab->buffer.address+offset();}
    };
    explicit VtOcclusionPages(uint32_t max_pages):max_slabs_((max_pages+kPagesPerSlab-1)/kPagesPerSlab) {}
    std::shared_ptr<Page> allocate(matter::VulkanDevice& vk,std::string& error) {
        try {
            for(auto& slab:slabs_)if(slab->free_bits) return take(slab);
            if(slabs_.size()>=max_slabs_){error="receiver occlusion retirement capacity exhausted";return {};}
            auto slab=std::make_shared<Slab>();
            if(!matter::create_buffer(vk,VkDeviceSize(kPagesPerSlab)*kPageBytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0,slab->buffer,error))return {};
            if(!slab->buffer.address || (slab->buffer.address&15u)) {
                error="unaligned receiver occlusion device address";return {};
            }
            slabs_.push_back(slab);return take(slab);
        } catch(const std::bad_alloc&) {error="receiver occlusion allocation failed";return {};}
    }
    // No descriptor points to an unused slab. A retired Page still increments
    // used, so it is impossible to free/reuse its bytes through this collection.
    void collect() {
        slabs_.erase(std::remove_if(slabs_.begin(),slabs_.end(),
            [](const auto& slab){return !slab->used;}),slabs_.end());
    }
    uint64_t allocated_bytes() const {
        uint64_t bytes=0;for(const auto& slab:slabs_)bytes+=slab->buffer.allocation_size;return bytes;
    }
    uint32_t retained_pages() const {
        uint32_t pages=0;for(const auto& slab:slabs_)pages+=slab->used;return pages;
    }
private:
    static std::shared_ptr<Page> take(const std::shared_ptr<Slab>& slab) {
        for(uint32_t i=0;i<kPagesPerSlab;++i)if(slab->free_bits&(1u<<i))return std::make_shared<Page>(slab,i);
        return {};
    }
    uint32_t max_slabs_;
    std::vector<std::shared_ptr<Slab>> slabs_;
};
} // namespace vt
