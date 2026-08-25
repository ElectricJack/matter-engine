#include "water_field_vk_resources.h"

#include <cstring>
#include <string>
#include <utility>

namespace viewer {
namespace {

bool fail(WaterFieldError& error, WaterFieldErrorCode code,
          std::string message) {
    error.code = code;
    error.message = std::move(message);
    return false;
}

struct UploadRecord {
    VkBuffer staging = VK_NULL_HANDLE;
    std::array<matter::VkImageResource*, 3> images{};
    std::array<VkDeviceSize, 3> offsets{};
    VkExtent3D extent{};
};

void record_upload(VkCommandBuffer command_buffer, void* user_data) {
    auto& upload = *static_cast<UploadRecord*>(user_data);
    for (matter::VkImageResource* image : upload.images) {
        matter::record_image_transition(
            command_buffer, *image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VkAccessFlags2(0),
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    for (std::uint32_t index = 0u; index != upload.images.size(); ++index) {
        VkBufferImageCopy copy{};
        copy.bufferOffset = upload.offsets[index];
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1u;
        copy.imageExtent = upload.extent;
        vkCmdCopyBufferToImage(
            command_buffer, upload.staging, upload.images[index]->image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
    }
    for (matter::VkImageResource* image : upload.images) {
        matter::record_image_transition(
            command_buffer, *image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }
}

bool create_sampler(VkDevice device, VkFilter filter, VkSampler& sampler,
                    WaterFieldError& error) {
    VkSamplerCreateInfo create{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    create.magFilter = filter;
    create.minFilter = filter;
    create.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    create.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    create.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    create.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    create.minLod = 0.0f;
    create.maxLod = 0.0f;
    const VkResult result = vkCreateSampler(device, &create, nullptr, &sampler);
    if (result == VK_SUCCESS) return true;
    return fail(error, WaterFieldErrorCode::UploadFailure,
                "vkCreateSampler for water field failed (VkResult " +
                    std::to_string(static_cast<int>(result)) + ")");
}

}  // namespace

WaterFieldVkResources::WaterFieldVkResources(
    matter::VulkanDevice& vulkan) noexcept
    : vulkan_(&vulkan) {}

WaterFieldVkResources::~WaterFieldVkResources() { destroy(); }

bool WaterFieldVkResources::initialize(WaterFieldError& error) {
    error = {};
    if (initialized_) return true;
    VkSampler linear = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE;
    if (!create_sampler(vulkan_->device(), VK_FILTER_LINEAR, linear, error) ||
        !create_sampler(vulkan_->device(), VK_FILTER_NEAREST, nearest, error)) {
        if (linear != VK_NULL_HANDLE)
            vkDestroySampler(vulkan_->device(), linear, nullptr);
        if (nearest != VK_NULL_HANDLE)
            vkDestroySampler(vulkan_->device(), nearest, nullptr);
        return false;
    }

    PackedWaterField zero;
    zero.layout.width = 1u;
    zero.layout.depth = 1u;
    zero.layout.cell_size_m = 1.0f;
    zero.runtime_digest = 1u;
    zero.presentation_digest = 1u;
    zero.image_a_rgba16f.assign(4u, 0u);
    zero.image_b_rgba16f.assign(4u, 0u);
    zero.image_c_rgba8.assign(4u, 0u);
    StagedImages dummy;
    if (!stage(zero, dummy, error)) {
        vkDestroySampler(vulkan_->device(), linear, nullptr);
        vkDestroySampler(vulkan_->device(), nearest, nullptr);
        return false;
    }
    linear_sampler_ = linear;
    nearest_sampler_ = nearest;
    dummy_ = std::move(dummy);
    initialized_ = true;
    return true;
}

void WaterFieldVkResources::destroy() noexcept {
    for (Slot& slot : slots_) slot = {};
    dummy_ = {};
    if (vulkan_) {
        if (linear_sampler_ != VK_NULL_HANDLE)
            vkDestroySampler(vulkan_->device(), linear_sampler_, nullptr);
        if (nearest_sampler_ != VK_NULL_HANDLE)
            vkDestroySampler(vulkan_->device(), nearest_sampler_, nullptr);
    }
    linear_sampler_ = VK_NULL_HANDLE;
    nearest_sampler_ = VK_NULL_HANDLE;
    initialized_ = false;
}

bool WaterFieldVkResources::stage(const PackedWaterField& field,
                                  StagedImages& output,
                                  WaterFieldError& error) {
    error = {};
    if (!packed_water_field_valid(field))
        return fail(error, WaterFieldErrorCode::InvalidInput,
                    "packed water field has invalid image sizes or metadata");

    const VkExtent3D extent{field.layout.width, field.layout.depth, 1u};
    const VkFormat formats[3] = {VK_FORMAT_R16G16B16A16_SFLOAT,
                                 VK_FORMAT_R16G16B16A16_SFLOAT,
                                 VK_FORMAT_R8G8B8A8_UNORM};
    StagedImages candidate;
    std::string vk_error;
    for (std::uint32_t index = 0u; index != candidate.images.size(); ++index) {
        if (!matter::create_image(
                *vulkan_, VK_IMAGE_TYPE_2D, formats[index], extent,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                candidate.images[index], vk_error)) {
            return fail(error, WaterFieldErrorCode::AllocationFailure,
                        "water field image allocation failed: " + vk_error);
        }
    }

    const VkDeviceSize byte_sizes[3] = {
        static_cast<VkDeviceSize>(field.image_a_rgba16f.size() *
                                  sizeof(std::uint16_t)),
        static_cast<VkDeviceSize>(field.image_b_rgba16f.size() *
                                  sizeof(std::uint16_t)),
        static_cast<VkDeviceSize>(field.image_c_rgba8.size())};
    const VkDeviceSize offsets[3] = {0u, byte_sizes[0],
                                     byte_sizes[0] + byte_sizes[1]};
    const VkDeviceSize total = offsets[2] + byte_sizes[2];
    matter::VkBufferResource staging;
    if (!matter::create_buffer(
            *vulkan_, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, vk_error) ||
        !matter::map_buffer(staging, vk_error)) {
        return fail(error, WaterFieldErrorCode::AllocationFailure,
                    "water field staging allocation failed: " + vk_error);
    }
    std::memcpy(static_cast<std::byte*>(staging.mapped) + offsets[0],
                field.image_a_rgba16f.data(),
                static_cast<std::size_t>(byte_sizes[0]));
    std::memcpy(static_cast<std::byte*>(staging.mapped) + offsets[1],
                field.image_b_rgba16f.data(),
                static_cast<std::size_t>(byte_sizes[1]));
    std::memcpy(static_cast<std::byte*>(staging.mapped) + offsets[2],
                field.image_c_rgba8.data(),
                static_cast<std::size_t>(byte_sizes[2]));
    if (!matter::flush_buffer(staging, 0u, total, vk_error)) {
        return fail(error, WaterFieldErrorCode::UploadFailure,
                    "water field staging flush failed: " + vk_error);
    }

    UploadRecord upload;
    upload.staging = staging.buffer;
    upload.extent = extent;
    for (std::uint32_t index = 0u; index != candidate.images.size(); ++index) {
        upload.images[index] = &candidate.images[index];
        upload.offsets[index] = offsets[index];
    }
    std::vector<std::shared_ptr<void>> dependencies{staging.lifetime};
    for (const auto& image : candidate.images)
        dependencies.push_back(image.lifetime);
    if (!matter::submit_immediate(
            *vulkan_, record_upload, &upload, vk_error,
            matter::ImmediateSubmitPhase::staging_upload,
            std::move(dependencies))) {
        return fail(error, WaterFieldErrorCode::UploadFailure,
                    "water field image upload failed: " + vk_error);
    }
    output = std::move(candidate);
    return true;
}

void WaterFieldVkResources::commit(WaterFieldBinding binding,
                                   StagedImages&& images) noexcept {
    if (!binding.valid() || binding.slot >= slots_.size()) return;
    Slot& slot = slots_[binding.slot];
    slot.images = std::move(images);
    slot.generation = binding.generation;
    slot.retire_after_serial = 0u;
    slot.occupied = true;
    slot.pending_release = false;
}

void WaterFieldVkResources::release(
    WaterFieldBinding binding, std::uint64_t retire_after_serial) noexcept {
    if (!binding.valid() || binding.slot >= slots_.size()) return;
    Slot& slot = slots_[binding.slot];
    if (!slot.occupied || slot.generation != binding.generation) return;
    slot.pending_release = true;
    slot.retire_after_serial = retire_after_serial;
}

void WaterFieldVkResources::collect(
    std::uint64_t completed_serial) noexcept {
    for (Slot& slot : slots_) {
        if (!slot.pending_release ||
            slot.retire_after_serial > completed_serial)
            continue;
        slot = {};
    }
}

VkImageView WaterFieldVkResources::image_view(
    WaterFieldBinding binding, std::uint32_t channel) const noexcept {
    if (!binding.valid() || binding.slot >= slots_.size() || channel >= 3u)
        return VK_NULL_HANDLE;
    const Slot& slot = slots_[binding.slot];
    return slot.occupied && slot.generation == binding.generation
               ? slot.images.images[channel].view
               : VK_NULL_HANDLE;
}

VkImageView WaterFieldVkResources::descriptor_view(
    std::uint32_t slot, std::uint32_t channel) const noexcept {
    if (!initialized_ || channel >= 3u) return VK_NULL_HANDLE;
    if (slot < slots_.size() && slots_[slot].occupied)
        return slots_[slot].images.images[channel].view;
    return dummy_.images[channel].view;
}

VkSampler WaterFieldVkResources::sampler(
    std::uint32_t channel) const noexcept {
    if (!initialized_ || channel >= 3u) return VK_NULL_HANDLE;
    return channel < 2u ? linear_sampler_ : nearest_sampler_;
}

void WaterFieldVkResources::append_frame_lifetimes(
    std::vector<std::shared_ptr<void>>& output) const {
    if (!initialized_) return;
    for (const auto& image : dummy_.images) output.push_back(image.lifetime);
    for (const Slot& slot : slots_) {
        if (!slot.occupied) continue;
        for (const auto& image : slot.images.images)
            output.push_back(image.lifetime);
    }
}

}  // namespace viewer
