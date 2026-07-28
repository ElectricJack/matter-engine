#include "image_preview.h"

#include <algorithm>
#include <cstring>

#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"
#include "matter/vulkan_device.h"

namespace viewer {

namespace {

constexpr VkFormat kPreviewFormat = VK_FORMAT_R8G8B8A8_UNORM;

bool linear_sampling_supported(VkPhysicalDevice physical) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physical, kPreviewFormat, &props);
    return (props.linearTilingFeatures &
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

uint32_t find_host_visible_memory(VkPhysicalDevice physical,
                                  uint32_t type_bits) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & wanted) == wanted)
            return i;
    }
    return UINT32_MAX;
}

} // namespace

ImagePreviewCache::~ImagePreviewCache() {
    // shutdown() should already have run while the device was alive. If it did
    // not, the device is gone and touching Vulkan here would fault — drop the
    // handles and let process teardown reclaim them.
    entries_.clear();
    pending_.clear();
}

void ImagePreviewCache::shutdown() {
    if (!vulkan_) {
        entries_.clear();
        pending_.clear();
        return;
    }
    // Immediate, not deferred: no more frames will be drawn, and the device and
    // the ImGui backend are still alive here (main.cpp calls this before
    // ui.shutdown()). Waiting for idle first covers anything still in flight.
    vulkan_->wait_idle();
    for (Entry& entry : entries_) destroy_entry(entry);
    entries_.clear();
    for (Pending& p : pending_) destroy_entry(p.entry);
    pending_.clear();
    const VkDevice device = vulkan_->device();
    if (sampler_) { vkDestroySampler(device, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
    if (pool_) { vkDestroyCommandPool(device, pool_, nullptr); pool_ = VK_NULL_HANDLE; }
    vulkan_ = nullptr;
}

bool ImagePreviewCache::ensure_pool_and_sampler(std::string& error) {
    const VkDevice device = vulkan_->device();
    if (pool_ == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                     VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = vulkan_->graphics_queue_family();
        if (vkCreateCommandPool(device, &info, nullptr, &pool_) != VK_SUCCESS) {
            error = "preview: failed to create command pool";
            return false;
        }
    }
    if (sampler_ == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samp.magFilter = VK_FILTER_LINEAR;
        samp.minFilter = VK_FILTER_LINEAR;
        samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(device, &samp, nullptr, &sampler_) != VK_SUCCESS) {
            error = "preview: failed to create sampler";
            return false;
        }
    }
    return true;
}

void* ImagePreviewCache::create(const std::vector<uint8_t>& rgba,
                                uint32_t width, uint32_t height,
                                std::string& error) {
    if (!vulkan_) { error = "preview: no device"; return nullptr; }
    if (width == 0 || height == 0) { error = "preview: empty image"; return nullptr; }
    if (rgba.size() < static_cast<size_t>(width) * height * 4) {
        error = "preview: pixel buffer smaller than width*height*4";
        return nullptr;
    }
    if (!linear_sampling_supported(vulkan_->physical_device())) {
        error = "preview: device cannot sample linear-tiled RGBA8";
        return nullptr;
    }
    if (!ensure_pool_and_sampler(error)) return nullptr;

    const VkDevice device = vulkan_->device();
    Entry entry{};

    VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    img.imageType = VK_IMAGE_TYPE_2D;
    img.format = kPreviewFormat;
    img.extent = {width, height, 1};
    img.mipLevels = 1;
    img.arrayLayers = 1;
    img.samples = VK_SAMPLE_COUNT_1_BIT;
    img.tiling = VK_IMAGE_TILING_LINEAR;
    img.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    img.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // PREINITIALIZED keeps the host writes below valid across the transition;
    // UNDEFINED would license the driver to discard them.
    img.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    if (vkCreateImage(device, &img, nullptr, &entry.image) != VK_SUCCESS) {
        error = "preview: vkCreateImage failed";
        return nullptr;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, entry.image, &req);
    const uint32_t type =
        find_host_visible_memory(vulkan_->physical_device(), req.memoryTypeBits);
    if (type == UINT32_MAX) {
        error = "preview: no host-visible memory type";
        destroy_entry(entry);
        return nullptr;
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &alloc, nullptr, &entry.memory) != VK_SUCCESS) {
        error = "preview: vkAllocateMemory failed";
        destroy_entry(entry);
        return nullptr;
    }
    vkBindImageMemory(device, entry.image, entry.memory, 0);

    // Row pitch is the driver's, not width*4 — copy row by row.
    VkImageSubresource subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout layout{};
    vkGetImageSubresourceLayout(device, entry.image, &subresource, &layout);

    void* mapped = nullptr;
    if (vkMapMemory(device, entry.memory, 0, req.size, 0, &mapped) !=
        VK_SUCCESS) {
        error = "preview: vkMapMemory failed";
        destroy_entry(entry);
        return nullptr;
    }
    uint8_t* dst = static_cast<uint8_t*>(mapped) + layout.offset;
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(dst + static_cast<size_t>(row) * layout.rowPitch,
                    rgba.data() + static_cast<size_t>(row) * width * 4,
                    static_cast<size_t>(width) * 4);
    }
    vkUnmapMemory(device, entry.memory);

    // One transition, host writes -> shader reads.
    VkCommandBufferAllocateInfo cmd_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_info.commandPool = pool_;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &cmd_info, &cmd) != VK_SUCCESS) {
        error = "preview: vkAllocateCommandBuffers failed";
        destroy_entry(entry);
        return nullptr;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = entry.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    vkEndCommandBuffer(cmd);

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
        error = "preview: vkCreateFence failed";
        vkFreeCommandBuffers(device, pool_, 1, &cmd);
        destroy_entry(entry);
        return nullptr;
    }
    bool proven = false;
    const bool submitted = vulkan_->submit_and_wait(cmd, fence, proven, error);
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, pool_, 1, &cmd);
    if (!submitted) {
        destroy_entry(entry);
        return nullptr;
    }

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = entry.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kPreviewFormat;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &view_info, nullptr, &entry.view) !=
        VK_SUCCESS) {
        error = "preview: vkCreateImageView failed";
        destroy_entry(entry);
        return nullptr;
    }

    entry.set = ImGui_ImplVulkan_AddTexture(
        sampler_, entry.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (entry.set == VK_NULL_HANDLE) {
        error = "preview: ImGui_ImplVulkan_AddTexture failed";
        destroy_entry(entry);
        return nullptr;
    }

    entries_.push_back(entry);
    return static_cast<void*>(entry.set);
}

void ImagePreviewCache::destroy_entry(Entry& entry) {
    if (!vulkan_) return;
    const VkDevice device = vulkan_->device();
    if (entry.set) { ImGui_ImplVulkan_RemoveTexture(entry.set); entry.set = VK_NULL_HANDLE; }
    if (entry.view) { vkDestroyImageView(device, entry.view, nullptr); entry.view = VK_NULL_HANDLE; }
    if (entry.image) { vkDestroyImage(device, entry.image, nullptr); entry.image = VK_NULL_HANDLE; }
    if (entry.memory) { vkFreeMemory(device, entry.memory, nullptr); entry.memory = VK_NULL_HANDLE; }
}

namespace {
// Frames a retired texture must survive before it can be freed. The caller is
// mid-UI-pass, so the CURRENT frame's draw list may reference it and has not
// been submitted yet; one full frame plus a margin covers that, and collect()
// waits for the device on top.
constexpr int kDeferFrames = 2;
} // namespace

void ImagePreviewCache::destroy(void* handle) {
    if (!handle || !vulkan_) return;
    const auto set = static_cast<VkDescriptorSet>(handle);
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [set](const Entry& e) { return e.set == set; });
    if (it == entries_.end()) return;
    pending_.push_back(Pending{*it, kDeferFrames});
    entries_.erase(it);
}

void ImagePreviewCache::destroy_all() {
    for (Entry& entry : entries_) pending_.push_back(Pending{entry, kDeferFrames});
    entries_.clear();
    // The pool and sampler deliberately survive: the sampler is baked into the
    // descriptor sets still queued in pending_, and both are cheap to keep for
    // the life of the editor. shutdown() releases them.
}

void ImagePreviewCache::collect() {
    if (!vulkan_ || pending_.empty()) return;
    bool any_due = false;
    for (Pending& p : pending_)
        if (--p.frames_remaining <= 0) any_due = true;
    if (!any_due) return;
    // Cheap because it only runs on the rare frame that actually retires a
    // texture, and it makes the free safe regardless of frames-in-flight.
    vulkan_->wait_idle();
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->frames_remaining <= 0) {
            destroy_entry(it->entry);
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace viewer
