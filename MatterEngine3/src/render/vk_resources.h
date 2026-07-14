#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "matter/vulkan_device.h"

namespace matter {

namespace detail {
struct VkBufferAllocation;
struct VkImageAllocation;
}  // namespace detail

struct VkBufferResource {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
    void* mapped = nullptr;

    VkDeviceSize allocation_size = 0;
    VkDeviceSize non_coherent_atom_size = 1;
    VkMemoryPropertyFlags memory_properties = 0;
    std::shared_ptr<detail::VkBufferAllocation> lifetime;

    VkBufferResource() = default;
    ~VkBufferResource();
    VkBufferResource(const VkBufferResource&) = delete;
    VkBufferResource& operator=(const VkBufferResource&) = delete;
    VkBufferResource(VkBufferResource&& other) noexcept;
    VkBufferResource& operator=(VkBufferResource&& other) noexcept;

    void reset();
};

struct VkImageResource {
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::shared_ptr<detail::VkImageAllocation> lifetime;

    VkImageResource() = default;
    ~VkImageResource();
    VkImageResource(const VkImageResource&) = delete;
    VkImageResource& operator=(const VkImageResource&) = delete;
    VkImageResource(VkImageResource&& other) noexcept;
    VkImageResource& operator=(VkImageResource&& other) noexcept;

    void reset();
};

bool find_memory_type(VkPhysicalDevice physical_device,
                      uint32_t allowed_type_bits,
                      VkMemoryPropertyFlags required,
                      VkMemoryPropertyFlags preferred,
                      uint32_t& memory_type,
                      VkMemoryPropertyFlags& selected_properties,
                      std::string& error);

bool create_buffer(VulkanDevice& vulkan, VkDeviceSize size,
                   VkBufferUsageFlags usage,
                   VkMemoryPropertyFlags required_memory,
                   VkMemoryPropertyFlags preferred_memory,
                   VkBufferResource& output, std::string& error);

bool map_buffer(VkBufferResource& resource, std::string& error);
bool flush_buffer(VkBufferResource& resource, VkDeviceSize offset,
                  VkDeviceSize size, std::string& error);
bool invalidate_buffer(VkBufferResource& resource, VkDeviceSize offset,
                       VkDeviceSize size, std::string& error);

bool upload_buffer(VulkanDevice& vulkan, VkBufferResource& destination,
                   const void* data, size_t byte_count, VkDeviceSize offset,
                   std::string& error);
bool readback_buffer(VulkanDevice& vulkan, VkBufferResource& source, void* data,
                     size_t byte_count, VkDeviceSize offset,
                     std::string& error);

bool create_image(VulkanDevice& vulkan, VkImageType type, VkFormat format,
                  VkExtent3D extent, VkImageUsageFlags usage,
                  VkImageAspectFlags aspect,
                  VkMemoryPropertyFlags required_memory,
                  VkImageResource& output, std::string& error);

void record_image_transition(VkCommandBuffer command_buffer,
                             VkImageResource& image,
                             VkImageLayout new_layout,
                             VkPipelineStageFlags2 source_stage,
                             VkAccessFlags2 source_access,
                             VkPipelineStageFlags2 destination_stage,
                             VkAccessFlags2 destination_access,
                             VkImageAspectFlags aspect);

bool transition_image(VulkanDevice& vulkan, VkImageResource& image,
                      VkImageLayout new_layout,
                      VkPipelineStageFlags2 source_stage,
                      VkAccessFlags2 source_access,
                      VkPipelineStageFlags2 destination_stage,
                      VkAccessFlags2 destination_access,
                      VkImageAspectFlags aspect, std::string& error);

using ImmediateRecordFn = void (*)(VkCommandBuffer, void*);
enum class ImmediateSubmitPhase {
    staging_upload,
    staging_readback,
    image_transition,
    compute_dispatch,
};
bool submit_immediate(VulkanDevice& vulkan, ImmediateRecordFn record,
                      void* user_data, std::string& error,
                      ImmediateSubmitPhase phase,
                      std::vector<std::shared_ptr<void>> dependencies = {});

}  // namespace matter
