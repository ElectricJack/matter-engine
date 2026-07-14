#include "vk_resources.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

#include "matter/vulkan_device.h"
#include "vk_device_internal.h"

namespace matter {
namespace detail {

struct VkBufferAllocation final : DeviceLifetimeControl {
    explicit VkBufferAllocation(std::shared_ptr<DeviceAccessToken> device_access)
        : DeviceLifetimeControl(std::move(device_access)) {}

    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;

    ~VkBufferAllocation() override { release_device_objects(); }

protected:
    void release_device_objects() noexcept override {
        const VkDevice device = live_device();
        if (device != VK_NULL_HANDLE && mapped && memory != VK_NULL_HANDLE)
            vkUnmapMemory(device, memory);
        if (device != VK_NULL_HANDLE && buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, buffer, nullptr);
        if (device != VK_NULL_HANDLE && memory != VK_NULL_HANDLE)
            vkFreeMemory(device, memory, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        mapped = nullptr;
    }
};

struct VkImageAllocation final : DeviceLifetimeControl {
    explicit VkImageAllocation(std::shared_ptr<DeviceAccessToken> device_access)
        : DeviceLifetimeControl(std::move(device_access)) {}

    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;

    ~VkImageAllocation() override { release_device_objects(); }

protected:
    void release_device_objects() noexcept override {
        const VkDevice device = live_device();
        if (device != VK_NULL_HANDLE && view != VK_NULL_HANDLE)
            vkDestroyImageView(device, view, nullptr);
        if (device != VK_NULL_HANDLE && image != VK_NULL_HANDLE)
            vkDestroyImage(device, image, nullptr);
        if (device != VK_NULL_HANDLE && memory != VK_NULL_HANDLE)
            vkFreeMemory(device, memory, nullptr);
        image = VK_NULL_HANDLE;
        view = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    }
};

}  // namespace detail
namespace {

const char* result_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        default: return "VkResult error";
    }
}

bool fail_result(const char* operation, VkResult result, std::string& error) {
    error = std::string(operation) + " failed: " + result_name(result) +
            " (" + std::to_string(static_cast<int>(result)) + ")";
    return false;
}

VkDeviceSize align_down(VkDeviceSize value, VkDeviceSize alignment) {
    return value - value % alignment;
}

VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) {
    if (value > std::numeric_limits<VkDeviceSize>::max() - (alignment - 1)) {
        return std::numeric_limits<VkDeviceSize>::max();
    }
    return ((value + alignment - 1) / alignment) * alignment;
}

struct CopyBufferRecord {
    VkBuffer source;
    VkBuffer destination;
    VkBufferCopy region;
};

void record_copy_buffer(VkCommandBuffer command_buffer, void* user_data) {
    const auto& copy = *static_cast<const CopyBufferRecord*>(user_data);
    vkCmdCopyBuffer(command_buffer, copy.source, copy.destination, 1,
                    &copy.region);
    VkMemoryBarrier2 barriers[2]{};
    barriers[0].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barriers[0].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barriers[0].dstAccessMask =
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barriers[1].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barriers[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 2;
    dependency.pMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

struct TransitionRecord {
    VkImageResource* image;
    VkImageLayout new_layout;
    VkPipelineStageFlags2 source_stage;
    VkAccessFlags2 source_access;
    VkPipelineStageFlags2 destination_stage;
    VkAccessFlags2 destination_access;
    VkImageAspectFlags aspect;
};

void record_transition(VkCommandBuffer command_buffer, void* user_data) {
    auto& transition = *static_cast<TransitionRecord*>(user_data);
    record_image_transition(command_buffer, *transition.image,
                            transition.new_layout, transition.source_stage,
                            transition.source_access,
                            transition.destination_stage,
                            transition.destination_access, transition.aspect);
}

class ImmediateSubmissionRetention final
    : public detail::DeviceRetainedResource {
public:
    ImmediateSubmissionRetention(
        VkDevice device, std::vector<std::shared_ptr<void>> dependencies)
        : device_(device), dependencies_(std::move(dependencies)) {}

    ~ImmediateSubmissionRetention() override {
        dependencies_.clear();
        if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, fence_, nullptr);
        if (pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, pool_, nullptr);
    }

    void abandon(VkCommandPool& pool, VkFence& fence) noexcept {
        pool_ = std::exchange(pool, VK_NULL_HANDLE);
        fence_ = std::exchange(fence, VK_NULL_HANDLE);
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    std::vector<std::shared_ptr<void>> dependencies_;
};

}  // namespace

VkBufferResource::~VkBufferResource() { reset(); }

VkBufferResource::VkBufferResource(VkBufferResource&& other) noexcept {
    *this = std::move(other);
}

VkBufferResource& VkBufferResource::operator=(VkBufferResource&& other) noexcept {
    if (this == &other) return *this;
    reset();
    device = std::exchange(other.device, VK_NULL_HANDLE);
    buffer = std::exchange(other.buffer, VK_NULL_HANDLE);
    memory = std::exchange(other.memory, VK_NULL_HANDLE);
    size = std::exchange(other.size, 0);
    address = std::exchange(other.address, 0);
    mapped = std::exchange(other.mapped, nullptr);
    allocation_size = std::exchange(other.allocation_size, 0);
    non_coherent_atom_size = std::exchange(other.non_coherent_atom_size, 1);
    memory_properties = std::exchange(other.memory_properties, 0);
    lifetime = std::move(other.lifetime);
    return *this;
}

void VkBufferResource::reset() {
    if (lifetime) {
        lifetime.reset();
    } else {
        if (device != VK_NULL_HANDLE && mapped && memory != VK_NULL_HANDLE)
            vkUnmapMemory(device, memory);
        if (device != VK_NULL_HANDLE && buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, buffer, nullptr);
        if (device != VK_NULL_HANDLE && memory != VK_NULL_HANDLE)
            vkFreeMemory(device, memory, nullptr);
    }
    device = VK_NULL_HANDLE;
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    size = 0;
    address = 0;
    mapped = nullptr;
    allocation_size = 0;
    non_coherent_atom_size = 1;
    memory_properties = 0;
}

VkImageResource::~VkImageResource() { reset(); }

VkImageResource::VkImageResource(VkImageResource&& other) noexcept {
    *this = std::move(other);
}

VkImageResource& VkImageResource::operator=(VkImageResource&& other) noexcept {
    if (this == &other) return *this;
    reset();
    device = std::exchange(other.device, VK_NULL_HANDLE);
    image = std::exchange(other.image, VK_NULL_HANDLE);
    view = std::exchange(other.view, VK_NULL_HANDLE);
    memory = std::exchange(other.memory, VK_NULL_HANDLE);
    format = std::exchange(other.format, VK_FORMAT_UNDEFINED);
    extent = std::exchange(other.extent, VkExtent3D{});
    layout = std::exchange(other.layout, VK_IMAGE_LAYOUT_UNDEFINED);
    lifetime = std::move(other.lifetime);
    return *this;
}

void VkImageResource::reset() {
    if (lifetime) {
        lifetime.reset();
    } else {
        if (device != VK_NULL_HANDLE && view != VK_NULL_HANDLE)
            vkDestroyImageView(device, view, nullptr);
        if (device != VK_NULL_HANDLE && image != VK_NULL_HANDLE)
            vkDestroyImage(device, image, nullptr);
        if (device != VK_NULL_HANDLE && memory != VK_NULL_HANDLE)
            vkFreeMemory(device, memory, nullptr);
    }
    device = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    view = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    format = VK_FORMAT_UNDEFINED;
    extent = {};
    layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

bool find_memory_type(VkPhysicalDevice physical_device,
                      uint32_t allowed_type_bits,
                      VkMemoryPropertyFlags required,
                      VkMemoryPropertyFlags preferred,
                      uint32_t& memory_type,
                      VkMemoryPropertyFlags& selected_properties,
                      std::string& error) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((allowed_type_bits & (uint32_t{1} << i)) == 0) continue;
        const VkMemoryPropertyFlags flags =
            properties.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) continue;
        if (fallback == UINT32_MAX) fallback = i;
        if ((flags & preferred) == preferred) {
            memory_type = i;
            selected_properties = flags;
            return true;
        }
    }
    if (fallback != UINT32_MAX) {
        memory_type = fallback;
        selected_properties = properties.memoryTypes[fallback].propertyFlags;
        return true;
    }
    error = "no Vulkan memory type satisfies required flags " +
            std::to_string(required) + " for type mask " +
            std::to_string(allowed_type_bits);
    return false;
}

bool create_buffer(VulkanDevice& vulkan, VkDeviceSize size,
                   VkBufferUsageFlags usage,
                   VkMemoryPropertyFlags required_memory,
                   VkMemoryPropertyFlags preferred_memory,
                   VkBufferResource& output, std::string& error) {
    if (size == 0) {
        error = "create_buffer requires a nonzero size";
        return false;
    }
    VkBufferResource candidate;
    candidate.device = vulkan.device();
    candidate.size = size;

    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.size = size;
    create.usage = usage;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vkCreateBuffer(candidate.device, &create, nullptr,
                                     &candidate.buffer);
    if (result != VK_SUCCESS) return fail_result("vkCreateBuffer", result, error);

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(candidate.device, candidate.buffer,
                                  &requirements);
    uint32_t memory_type = 0;
    if (!find_memory_type(vulkan.physical_device(), requirements.memoryTypeBits,
                          required_memory, preferred_memory, memory_type,
                          candidate.memory_properties, error)) {
        return false;
    }
    candidate.allocation_size = requirements.size;
    VkPhysicalDeviceProperties physical_properties{};
    vkGetPhysicalDeviceProperties(vulkan.physical_device(),
                                  &physical_properties);
    candidate.non_coherent_atom_size =
        std::max<VkDeviceSize>(1, physical_properties.limits.nonCoherentAtomSize);

    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0) {
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.pNext = flags.flags ? &flags : nullptr;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type;
    result = vkAllocateMemory(candidate.device, &allocate, nullptr,
                              &candidate.memory);
    if (result != VK_SUCCESS) return fail_result("vkAllocateMemory", result, error);
    result = vkBindBufferMemory(candidate.device, candidate.buffer,
                                candidate.memory, 0);
    if (result != VK_SUCCESS) {
        return fail_result("vkBindBufferMemory", result, error);
    }
    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0) {
        VkBufferDeviceAddressInfo address_info{
            VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        address_info.buffer = candidate.buffer;
        candidate.address =
            vkGetBufferDeviceAddress(candidate.device, &address_info);
        if (candidate.address == 0) {
            error = "vkGetBufferDeviceAddress returned zero";
            return false;
        }
    }
    candidate.lifetime = std::make_shared<detail::VkBufferAllocation>(
        detail::DeviceLifetimeAccess::token(vulkan));
    candidate.lifetime->buffer = candidate.buffer;
    candidate.lifetime->memory = candidate.memory;
    candidate.lifetime->mapped = candidate.mapped;
    output = std::move(candidate);
    return true;
}

bool map_buffer(VkBufferResource& resource, std::string& error) {
    if ((resource.memory_properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
        error = "cannot map non-host-visible Vulkan memory";
        return false;
    }
    if (resource.mapped) return true;
    VkResult result = vkMapMemory(resource.device, resource.memory, 0,
                                  VK_WHOLE_SIZE, 0, &resource.mapped);
    if (result == VK_SUCCESS && resource.lifetime)
        resource.lifetime->mapped = resource.mapped;
    return result == VK_SUCCESS || fail_result("vkMapMemory", result, error);
}

bool flush_buffer(VkBufferResource& resource, VkDeviceSize offset,
                  VkDeviceSize size, std::string& error) {
    if (offset > resource.size || size > resource.size - offset) {
        error = "flush range exceeds buffer size";
        return false;
    }
    if (size == 0) return true;
    if (!resource.mapped) {
        error = "cannot flush Vulkan memory before it is mapped";
        return false;
    }
    if ((resource.memory_properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0)
        return true;
    const VkDeviceSize aligned_offset =
        align_down(offset, resource.non_coherent_atom_size);
    const VkDeviceSize end =
        std::min(resource.allocation_size,
                 align_up(offset + size, resource.non_coherent_atom_size));
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = resource.memory;
    range.offset = aligned_offset;
    range.size = end == resource.allocation_size ? VK_WHOLE_SIZE
                                                 : end - aligned_offset;
    const VkResult result = vkFlushMappedMemoryRanges(resource.device, 1, &range);
    return result == VK_SUCCESS ||
           fail_result("vkFlushMappedMemoryRanges", result, error);
}

bool invalidate_buffer(VkBufferResource& resource, VkDeviceSize offset,
                       VkDeviceSize size, std::string& error) {
    if (offset > resource.size || size > resource.size - offset) {
        error = "invalidate range exceeds buffer size";
        return false;
    }
    if (size == 0) return true;
    if (!resource.mapped) {
        error = "cannot invalidate Vulkan memory before it is mapped";
        return false;
    }
    if ((resource.memory_properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0)
        return true;
    const VkDeviceSize aligned_offset =
        align_down(offset, resource.non_coherent_atom_size);
    const VkDeviceSize end =
        std::min(resource.allocation_size,
                 align_up(offset + size, resource.non_coherent_atom_size));
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = resource.memory;
    range.offset = aligned_offset;
    range.size = end == resource.allocation_size ? VK_WHOLE_SIZE
                                                 : end - aligned_offset;
    const VkResult result =
        vkInvalidateMappedMemoryRanges(resource.device, 1, &range);
    return result == VK_SUCCESS ||
           fail_result("vkInvalidateMappedMemoryRanges", result, error);
}

bool submit_immediate(VulkanDevice& vulkan, ImmediateRecordFn record,
                      void* user_data, std::string& error,
                      ImmediateSubmitPhase phase,
                      std::vector<std::shared_ptr<void>> dependencies) {
    if (!record) {
        error = "submit_immediate requires a record callback";
        return false;
    }
    const VkDevice device = vulkan.device();
    auto retained = std::make_unique<ImmediateSubmissionRetention>(
        device, std::move(dependencies));
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    auto cleanup = [&]() {
        if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
        if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
    };

    VkCommandPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = vulkan.graphics_queue_family();
    VkResult result = vkCreateCommandPool(device, &pool_info, nullptr, &pool);
    if (result != VK_SUCCESS) {
        cleanup();
        return fail_result("vkCreateCommandPool", result, error);
    }
    VkCommandBufferAllocateInfo allocate{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(device, &allocate, &command_buffer);
    if (result != VK_SUCCESS) {
        cleanup();
        return fail_result("vkAllocateCommandBuffers", result, error);
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(command_buffer, &begin);
    if (result != VK_SUCCESS) {
        cleanup();
        return fail_result("vkBeginCommandBuffer", result, error);
    }
    record(command_buffer, user_data);
    result = vkEndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS) {
        cleanup();
        return fail_result("vkEndCommandBuffer", result, error);
    }
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    result = vkCreateFence(device, &fence_info, nullptr, &fence);
    if (result != VK_SUCCESS) {
        cleanup();
        return fail_result("vkCreateFence", result, error);
    }
    bool completion_proven = false;
    const char* phase_name = nullptr;
    switch (phase) {
        case ImmediateSubmitPhase::staging_upload:
            phase_name = "staging-upload";
            break;
        case ImmediateSubmitPhase::staging_readback:
            phase_name = "staging-readback";
            break;
        case ImmediateSubmitPhase::image_transition:
            phase_name = "image-transition";
            break;
        case ImmediateSubmitPhase::compute_dispatch:
            phase_name = "dispatch-moved-buffer";
            break;
        case ImmediateSubmitPhase::raster_submission:
            phase_name = "raster-submission";
            break;
    }
    const bool submitted = detail::DeviceSubmitAccess::submit_and_wait(
        vulkan, command_buffer, fence, completion_proven, phase_name, error);
    if (!completion_proven) {
        // Completion is ambiguous: atomically transfer the command objects and
        // every caller-registered dependency to device-lifetime ownership.
        retained->abandon(pool, fence);
        detail::DeviceRetentionAccess::retain(vulkan, std::move(retained));
    }
    bool result_ok = submitted;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    const char* force_completed_failure =
        std::getenv("MATTER_VK_TEST_FORCE_IMMEDIATE_COMPLETED_FAILURE");
    if (submitted && completion_proven && force_completed_failure &&
        std::strcmp(force_completed_failure, phase_name) == 0) {
        error = "forced completed immediate failure for raster fault test";
        result_ok = false;
    }
#endif
    cleanup();
    return result_ok;
}

bool upload_buffer(VulkanDevice& vulkan, VkBufferResource& destination,
                   const void* data, size_t byte_count, VkDeviceSize offset,
                   std::string& error) {
    if (!data || offset > destination.size ||
        byte_count > destination.size - offset) {
        error = "upload range exceeds destination buffer";
        return false;
    }
    if ((destination.memory_properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        if (!map_buffer(destination, error)) return false;
        std::memcpy(static_cast<std::byte*>(destination.mapped) + offset, data,
                    byte_count);
        return flush_buffer(destination, offset, byte_count, error);
    }
    VkBufferResource staging;
    if (!create_buffer(vulkan, byte_count, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, error) ||
        !map_buffer(staging, error)) {
        return false;
    }
    std::memcpy(staging.mapped, data, byte_count);
    if (!flush_buffer(staging, 0, byte_count, error)) return false;
    CopyBufferRecord copy{staging.buffer, destination.buffer,
                          {0, offset, byte_count}};
    std::vector<std::shared_ptr<void>> dependencies{staging.lifetime,
                                                    destination.lifetime};
    return submit_immediate(vulkan, record_copy_buffer, &copy, error,
                            ImmediateSubmitPhase::staging_upload,
                            std::move(dependencies));
}

bool readback_buffer(VulkanDevice& vulkan, VkBufferResource& source, void* data,
                     size_t byte_count, VkDeviceSize offset,
                     std::string& error) {
    if (!data || offset > source.size || byte_count > source.size - offset) {
        error = "readback range exceeds source buffer";
        return false;
    }
    if ((source.memory_properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        if (!map_buffer(source, error) ||
            !invalidate_buffer(source, offset, byte_count, error)) {
            return false;
        }
        std::memcpy(data, static_cast<const std::byte*>(source.mapped) + offset,
                    byte_count);
        return true;
    }
    VkBufferResource staging;
    if (!create_buffer(vulkan, byte_count, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                       VK_MEMORY_PROPERTY_HOST_CACHED_BIT, staging, error)) {
        return false;
    }
    CopyBufferRecord copy{source.buffer, staging.buffer,
                          {offset, 0, byte_count}};
    std::vector<std::shared_ptr<void>> dependencies{source.lifetime,
                                                    staging.lifetime};
    if (!submit_immediate(vulkan, record_copy_buffer, &copy, error,
                          ImmediateSubmitPhase::staging_readback,
                          std::move(dependencies)) ||
        !map_buffer(staging, error) ||
        !invalidate_buffer(staging, 0, byte_count, error)) {
        return false;
    }
    std::memcpy(data, staging.mapped, byte_count);
    return true;
}

bool create_image(VulkanDevice& vulkan, VkImageType type, VkFormat format,
                  VkExtent3D extent, VkImageUsageFlags usage,
                  VkImageAspectFlags aspect,
                  VkMemoryPropertyFlags required_memory,
                  VkImageResource& output, std::string& error) {
    if (extent.width == 0 || extent.height == 0 || extent.depth == 0) {
        error = "create_image requires a nonzero extent";
        return false;
    }
    VkImageResource candidate;
    candidate.device = vulkan.device();
    candidate.format = format;
    candidate.extent = extent;
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create.imageType = type;
    create.format = format;
    create.extent = extent;
    create.mipLevels = 1;
    create.arrayLayers = 1;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = usage;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result =
        vkCreateImage(candidate.device, &create, nullptr, &candidate.image);
    if (result != VK_SUCCESS) return fail_result("vkCreateImage", result, error);
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(candidate.device, candidate.image, &requirements);
    uint32_t memory_type = 0;
    VkMemoryPropertyFlags selected = 0;
    if (!find_memory_type(vulkan.physical_device(), requirements.memoryTypeBits,
                          required_memory, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          memory_type, selected, error)) {
        return false;
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type;
    result = vkAllocateMemory(candidate.device, &allocate, nullptr,
                              &candidate.memory);
    if (result != VK_SUCCESS) return fail_result("vkAllocateMemory", result, error);
    result = vkBindImageMemory(candidate.device, candidate.image,
                               candidate.memory, 0);
    if (result != VK_SUCCESS) return fail_result("vkBindImageMemory", result, error);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = candidate.image;
    view.viewType = type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D
                  : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                                            : VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = aspect;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    result = vkCreateImageView(candidate.device, &view, nullptr, &candidate.view);
    if (result != VK_SUCCESS) return fail_result("vkCreateImageView", result, error);
    candidate.lifetime = std::make_shared<detail::VkImageAllocation>(
        detail::DeviceLifetimeAccess::token(vulkan));
    candidate.lifetime->image = candidate.image;
    candidate.lifetime->view = candidate.view;
    candidate.lifetime->memory = candidate.memory;
    output = std::move(candidate);
    return true;
}

void record_image_transition(VkCommandBuffer command_buffer,
                             VkImageResource& image,
                             VkImageLayout new_layout,
                             VkPipelineStageFlags2 source_stage,
                             VkAccessFlags2 source_access,
                             VkPipelineStageFlags2 destination_stage,
                             VkAccessFlags2 destination_access,
                             VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = source_stage;
    barrier.srcAccessMask = source_access;
    barrier.dstStageMask = destination_stage;
    barrier.dstAccessMask = destination_access;
    barrier.oldLayout = image.layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command_buffer, &dependency);
    image.layout = new_layout;
}

bool transition_image(VulkanDevice& vulkan, VkImageResource& image,
                      VkImageLayout new_layout,
                      VkPipelineStageFlags2 source_stage,
                      VkAccessFlags2 source_access,
                      VkPipelineStageFlags2 destination_stage,
                      VkAccessFlags2 destination_access,
                      VkImageAspectFlags aspect, std::string& error) {
    const VkImageLayout old_layout = image.layout;
    TransitionRecord transition{&image, new_layout, source_stage, source_access,
                                destination_stage, destination_access, aspect};
    std::vector<std::shared_ptr<void>> dependencies{image.lifetime};
    if (!submit_immediate(vulkan, record_transition, &transition, error,
                          ImmediateSubmitPhase::image_transition,
                          std::move(dependencies))) {
        if (image.device != VK_NULL_HANDLE) image.layout = old_layout;
        return false;
    }
    return true;
}

}  // namespace matter
