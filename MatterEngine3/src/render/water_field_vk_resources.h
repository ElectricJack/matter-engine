#pragma once

#include "vk_resources.h"
#include "water_field_vk.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace viewer {

// Viewer-only Vulkan ownership for the immutable CPU slot table in
// WaterFieldVk. Uploads are staged before CPU publication and committed with
// no allocation, preserving the last valid field when any upload fails.
class WaterFieldVkResources {
public:
    struct StagedImages {
        std::array<matter::VkImageResource, 3> images{};
    };

    explicit WaterFieldVkResources(matter::VulkanDevice& vulkan) noexcept;
    ~WaterFieldVkResources();
    WaterFieldVkResources(const WaterFieldVkResources&) = delete;
    WaterFieldVkResources& operator=(const WaterFieldVkResources&) = delete;

    bool initialize(WaterFieldError& error);
    void destroy() noexcept;

    bool stage(const PackedWaterField& field, StagedImages& output,
               WaterFieldError& error);
    void commit(WaterFieldBinding binding, StagedImages&& images) noexcept;
    void release(WaterFieldBinding binding,
                 std::uint64_t retire_after_serial) noexcept;
    void collect(std::uint64_t completed_serial) noexcept;

    VkImageView image_view(WaterFieldBinding binding,
                           std::uint32_t channel) const noexcept;
    VkImageView descriptor_view(std::uint32_t slot,
                                std::uint32_t channel) const noexcept;
    VkSampler sampler(std::uint32_t channel) const noexcept;
    void append_frame_lifetimes(
        std::vector<std::shared_ptr<void>>& output) const;

private:
    struct Slot {
        StagedImages images;
        std::uint32_t generation = 0;
        std::uint64_t retire_after_serial = 0;
        bool occupied = false;
        bool pending_release = false;
    };

    matter::VulkanDevice* vulkan_ = nullptr;
    std::array<Slot, kWaterFieldBindingSlots> slots_{};
    StagedImages dummy_{};
    VkSampler linear_sampler_ = VK_NULL_HANDLE;
    VkSampler nearest_sampler_ = VK_NULL_HANDLE;
    bool initialized_ = false;
};

}  // namespace viewer
