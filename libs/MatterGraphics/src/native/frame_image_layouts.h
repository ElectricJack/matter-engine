#pragma once

#include "vk_resources.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace viewer {

// Renderer-owned attachment objects have stable addresses. Their allocations
// may change, so rollback checks allocation identity as well as the handle.
// Vulkan transitions update CPU layout tracking while recording; an abandoned
// command buffer must restore that tracking without issuing GPU barriers.
class FrameImageLayouts {
public:
    bool begin(uint64_t serial) {
        if (serial == 0 || serial_ == serial) return false;
        serial_ = serial;
        entries_.clear();
        return true;
    }

    void capture(matter::VkImageResource& image) {
        if (image.image != VK_NULL_HANDLE)
            entries_.push_back({&image, image.image, image.lifetime, image.layout});
    }

    void finish(uint64_t serial, bool submitted) {
        if (serial == 0 || serial != serial_) return;
        if (!submitted) {
            for (const auto& entry : entries_) {
                if (const auto allocation = entry.allocation.lock();
                    allocation && allocation == entry.resource->lifetime &&
                    entry.image == entry.resource->image)
                    entry.resource->layout = entry.layout;
            }
        }
        clear();
    }

    void clear() {
        entries_.clear();
        serial_ = 0;
    }

    void abandon() { finish(serial_, false); }

private:
    struct Entry {
        matter::VkImageResource* resource;
        VkImage image;
        std::weak_ptr<void> allocation;
        VkImageLayout layout;
    };
    uint64_t serial_ = 0;
    std::vector<Entry> entries_;
};

} // namespace viewer
