#include "vk_cloud_shadows.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <utility>

#include "matter/world_definition.h"
#include "../../shaders_gen/embedded_spirv.h"

namespace viewer {
namespace {

struct ClearImagesRecord {
    const std::vector<matter::VkImageResource*>* images = nullptr;
};

struct ReadTauRecord {
    matter::VkImageResource* image = nullptr;
    VkBuffer destination = VK_NULL_HANDLE;
    uint32_t x = 0, y = 0, z = 0;
};

struct GenerationRecord {
    VkCloudShadows* shadows = nullptr;
    float frame_time = 0.0f;
    bool success = false;
    std::string error;
};

struct alignas(16) GenerationConstants {
    float current_uvw_to_world[16]{};
    float previous_world_to_uvw[16]{};
    uint32_t dimensions_level[4]{};
    float scheduling[4]{};
    uint32_t controls[4]{};
    float density_override[4]{};
};
static_assert(sizeof(GenerationConstants) == 192);
static_assert(sizeof(matter::GpuCloudLayer) == 96);

bool vk_fail(const char* operation, VkResult result, std::string& error) {
    error = std::string(operation) + " failed with VkResult " +
            std::to_string(static_cast<int>(result));
    return false;
}

VkDescriptorSetLayoutBinding make_binding(uint32_t binding,
                                           VkDescriptorType type) {
    VkDescriptorSetLayoutBinding result{};
    result.binding = binding;
    result.descriptorType = type;
    result.descriptorCount = 1;
    result.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    return result;
}

bool create_compute_pipeline(VkDevice device, const char* name,
                             VkPipelineLayout layout, VkPipeline& pipeline,
                             std::string& error) {
    const matter::EmbeddedSpirvView spirv = matter::find_spirv(name);
    if (!spirv.words || spirv.word_count == 0) {
        error = std::string("embedded SPIR-V not found: ") + name;
        return false;
    }
    VkShaderModuleCreateInfo module_info{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = spirv.word_count * sizeof(uint32_t);
    module_info.pCode = spirv.words;
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult result = vkCreateShaderModule(device, &module_info, nullptr,
                                           &module);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateShaderModule(cloud shadow)", result, error);
    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";
    VkComputePipelineCreateInfo pipeline_info{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.stage = stage;
    pipeline_info.layout = layout;
    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                      &pipeline_info, nullptr, &pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    return result == VK_SUCCESS ||
           vk_fail("vkCreateComputePipelines(cloud shadow)", result, error);
}

void pack_mat4_column_major(float out[16], const matter::Mat4f& matrix) {
    for (uint32_t row = 0; row < 4; ++row)
        for (uint32_t column = 0; column < 4; ++column)
            out[column * 4 + row] = matrix.m[row * 4 + column];
}

float half_to_float(uint16_t value) {
    const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16;
    uint32_t exponent = (value >> 10) & 0x1fu;
    uint32_t mantissa = value & 0x3ffu;
    uint32_t bits = sign;
    if (exponent == 0) {
        if (mantissa != 0) {
            exponent = 127 - 14;
            while ((mantissa & 0x400u) == 0) { mantissa <<= 1; --exponent; }
            bits |= exponent << 23;
            bits |= (mantissa & 0x3ffu) << 13;
        }
    } else if (exponent == 31) {
        bits |= 0x7f800000u | (mantissa << 13);
    } else {
        bits |= (exponent + 127 - 15) << 23;
        bits |= mantissa << 13;
    }
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

void record_clear_images(VkCommandBuffer command_buffer, void* user_data) {
    const auto& record = *static_cast<ClearImagesRecord*>(user_data);
    const VkClearColorValue clear_tau{{0.0f, 0.0f, 0.0f, 0.0f}};
    const VkImageSubresourceRange range{
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (matter::VkImageResource* image : *record.images) {
        matter::record_image_transition(
            command_buffer, *image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        vkCmdClearColorImage(command_buffer, image->image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear_tau, 1, &range);
        matter::record_image_transition(
            command_buffer, *image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }
}

void record_read_tau(VkCommandBuffer command_buffer, void* user_data) {
    const auto& record = *static_cast<ReadTauRecord*>(user_data);
    matter::record_image_transition(
        command_buffer, *record.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {static_cast<int32_t>(record.x),
                          static_cast<int32_t>(record.y),
                          static_cast<int32_t>(record.z)};
    region.imageExtent = {1, 1, 1};
    vkCmdCopyImageToBuffer(command_buffer, record.image->image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           record.destination, 1, &region);
    matter::record_image_transition(
        command_buffer, *record.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
}

bool same_desc(const matter::CloudShadowLevelDesc& a,
               const matter::CloudShadowLevelDesc& b) {
    return a.width == b.width && a.height == b.height &&
           a.depth == b.depth && a.coverage_m == b.coverage_m;
}

float frame_angle_degrees(const matter::CloudShadowFrame& a,
                          const matter::CloudShadowFrame& b) {
    const float dot = std::fmax(-1.0f, std::fmin(
        matter::detail::cloud_shadow_dot(a.incoming_light_axis,
                                         b.incoming_light_axis),
        1.0f));
    return std::acos(dot) * (180.0f / 3.14159265358979323846f);
}

}  // namespace

VkCloudShadows::~VkCloudShadows() { destroy(); }

bool VkCloudShadows::init(matter::VulkanDevice& vulkan, std::string& error) {
    error.clear();
    if (initialized_) return true;
    vulkan_ = &vulkan;
    device_ = vulkan.device();
    requested_settings_.enabled = false;
    requested_levels_ = matter::resolve_cloud_shadow_levels(requested_settings_);
    if (!create_generation_resources(error) || !create_emergency_images(error)) {
        destroy();
        return false;
    }
    initialized_ = true;
    return true;
}

bool VkCloudShadows::create_generation_resources(std::string& error) {
    const VkDeviceSize cloud_bytes =
        sizeof(matter::GpuCloudLayer) * matter::kMaxCloudLayers;
    for (auto& buffer : cloud_layer_ssbo_) {
        if (!matter::create_buffer(
                *vulkan_, cloud_bytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                buffer, error) || !matter::map_buffer(buffer, error))
            return false;
        std::memset(buffer.mapped, 0, static_cast<size_t>(cloud_bytes));
        if (!matter::flush_buffer(buffer, 0, cloud_bytes, error)) return false;
    }

    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sampler_info.maxLod = 0.0f;
    VkResult result = vkCreateSampler(device_, &sampler_info, nullptr,
                                      &generation_sampler_);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateSampler(cloud shadow)", result, error);

    const VkDescriptorSetLayoutBinding bindings[] = {
        make_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        make_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        make_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        make_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        make_binding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
    };
    VkDescriptorSetLayoutCreateInfo set_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 5;
    set_info.pBindings = bindings;
    result = vkCreateDescriptorSetLayout(device_, &set_info, nullptr,
                                         &generation_set_layout_);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateDescriptorSetLayout(cloud shadow)", result,
                       error);
    VkPipelineLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &generation_set_layout_;
    result = vkCreatePipelineLayout(device_, &layout_info, nullptr,
                                    &generation_pipeline_layout_);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreatePipelineLayout(cloud shadow)", result, error);
    return create_compute_pipeline(device_, "cloud_shadow_reproject.comp.spv",
                                   generation_pipeline_layout_,
                                   reproject_pipeline_, error) &&
           create_compute_pipeline(device_, "cloud_shadow_density.comp.spv",
                                   generation_pipeline_layout_,
                                   density_pipeline_, error) &&
           create_compute_pipeline(device_, "cloud_shadow_integrate.comp.spv",
                                   generation_pipeline_layout_,
                                   integrate_pipeline_, error);
}

void VkCloudShadows::request_settings(
    const matter::CloudShadowSettings& settings) {
    matter::CloudShadowSettings clean = settings;
    const auto resolved = matter::resolve_cloud_shadow_levels(settings);
    clean.near_coverage_m = resolved[0].coverage_m;
    clean.far_coverage_m = resolved[1].coverage_m;
    if (!std::isfinite(clean.filter_scale) || clean.filter_scale < 0.0f)
        clean.filter_scale = 1.0f;
    if (!std::isfinite(clean.update_fraction) ||
        clean.update_fraction <= 0.0f)
        clean.update_fraction = 0.0625f;
    clean.update_fraction = std::fmax(0.0625f,
                                      std::fmin(clean.update_fraction, 1.0f));
    const bool changed = clean.enabled != requested_settings_.enabled ||
        !same_desc(resolved[0], requested_levels_[0]) ||
        !same_desc(resolved[1], requested_levels_[1]) ||
        clean.filter_scale != requested_settings_.filter_scale ||
        clean.update_fraction != requested_settings_.update_fraction;
    requested_settings_ = clean;
    requested_levels_ = resolved;
    if (changed) {
        request_failed_ = false;
        allocation_error_.clear();
    }
}

void VkCloudShadows::request_cloud_layers(const matter::FogSettings& fog) {
    std::array<matter::GpuCloudLayer, matter::kMaxCloudLayers> packed{};
    const uint32_t count = static_cast<uint32_t>(matter::active_cloud_count(fog));
    for (uint32_t index = 0; index < count; ++index)
        matter::pack_cloud_layer(fog.clouds[index], static_cast<int>(index),
                                 packed[index]);
    if (count != cloud_layer_count_ ||
        std::memcmp(packed.data(), packed_cloud_layers_.data(),
                    sizeof(packed)) != 0) {
        packed_cloud_layers_ = packed;
        cloud_layer_count_ = count;
        force_history_invalidation_ = true;
    }
}

bool VkCloudShadows::create_emergency_images(std::string& error) {
    std::vector<matter::VkImageResource*> images;
    std::vector<std::shared_ptr<void>> lifetimes;
    for (auto& image : emergency_) {
        if (!matter::create_image(
                *vulkan_, VK_IMAGE_TYPE_3D, VK_FORMAT_R16_SFLOAT, {1, 1, 1},
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                image, error)) {
            for (auto& partial : emergency_) partial.reset();
            return false;
        }
        images.push_back(&image);
        lifetimes.push_back(image.lifetime);
    }
    return clear_images(images, error);
}

bool VkCloudShadows::clear_images(
    const std::vector<matter::VkImageResource*>& images, std::string& error) {
    ClearImagesRecord record{&images};
    std::vector<std::shared_ptr<void>> lifetimes;
    lifetimes.reserve(images.size());
    for (const auto* image : images) lifetimes.push_back(image->lifetime);
    return matter::submit_immediate(
        *vulkan_, record_clear_images, &record, error,
        matter::ImmediateSubmitPhase::staging_upload, std::move(lifetimes));
}

bool VkCloudShadows::create_level_descriptors(
    CloudShadowLevelBundle& level, std::string& error) {
    for (auto& constants : level.generation_constants) {
        if (!matter::create_buffer(
                *vulkan_, sizeof(GenerationConstants),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                constants, error) || !matter::map_buffer(constants, error))
            return false;
    }
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4},
    };
    VkDescriptorPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 4;
    pool_info.poolSizeCount = 4;
    pool_info.pPoolSizes = sizes;
    VkResult result = vkCreateDescriptorPool(device_, &pool_info, nullptr,
                                             &level.generation_descriptor_pool);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateDescriptorPool(cloud shadow)", result, error);
    VkDescriptorSetLayout layouts[4]{generation_set_layout_,
        generation_set_layout_, generation_set_layout_, generation_set_layout_};
    VkDescriptorSet sets[4]{};
    VkDescriptorSetAllocateInfo allocate{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = level.generation_descriptor_pool;
    allocate.descriptorSetCount = 4;
    allocate.pSetLayouts = layouts;
    result = vkAllocateDescriptorSets(device_, &allocate, sets);
    if (result != VK_SUCCESS)
        return vk_fail("vkAllocateDescriptorSets(cloud shadow)", result, error);

    for (uint32_t frame_slot = 0; frame_slot < 2; ++frame_slot) {
        for (uint32_t destination = 0; destination < 2; ++destination) {
            const uint32_t set_index = frame_slot * 2 + destination;
            level.generation_sets[frame_slot][destination] = sets[set_index];
            VkDescriptorImageInfo density{VK_NULL_HANDLE, level.density.view,
                                          VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorImageInfo previous{generation_sampler_,
                level.cumulative[destination ^ 1u].view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo output{VK_NULL_HANDLE,
                level.cumulative[destination].view, VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorBufferInfo clouds{cloud_layer_ssbo_[frame_slot].buffer,
                0, cloud_layer_ssbo_[frame_slot].size};
            VkDescriptorBufferInfo constants{
                level.generation_constants[frame_slot].buffer, 0,
                sizeof(GenerationConstants)};
            VkWriteDescriptorSet writes[5]{};
            const VkDescriptorType types[] = {
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
            for (uint32_t binding = 0; binding < 5; ++binding) {
                writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[binding].dstSet = sets[set_index];
                writes[binding].dstBinding = binding;
                writes[binding].descriptorCount = 1;
                writes[binding].descriptorType = types[binding];
            }
            writes[0].pImageInfo = &density;
            writes[1].pImageInfo = &previous;
            writes[2].pImageInfo = &output;
            writes[3].pBufferInfo = &clouds;
            writes[4].pBufferInfo = &constants;
            vkUpdateDescriptorSets(device_, 5, writes, 0, nullptr);
        }
    }
    return true;
}

bool VkCloudShadows::create_level_pair(
    const std::array<matter::CloudShadowLevelDesc, 2>& descs,
    LevelPair& pair, std::string& error) {
    failed_candidate_lifetimes_.clear();
    uint32_t allocation_count = 0;
    const auto fail = [&]() {
        for (auto& level : pair) {
            if (level.density.lifetime)
                failed_candidate_lifetimes_.push_back(level.density.lifetime);
            for (auto& image : level.cumulative)
                if (image.lifetime)
                    failed_candidate_lifetimes_.push_back(image.lifetime);
        }
        destroy_level_pair(pair);
        return false;
    };
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    std::vector<matter::VkImageResource*> images;
    for (uint32_t level_index = 0; level_index < 2; ++level_index) {
        auto& level = pair[level_index];
        level.desc = descs[level_index];
        const VkExtent3D extent{level.desc.width, level.desc.height,
                                level.desc.depth};
        if (!matter::create_image(
                *vulkan_, VK_IMAGE_TYPE_3D, VK_FORMAT_R16_SFLOAT, extent,
                usage, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, level.density, error))
            return fail();
        images.push_back(&level.density);
        ++allocation_count;
        for (auto& cumulative : level.cumulative) {
            if (!matter::create_image(
                    *vulkan_, VK_IMAGE_TYPE_3D, VK_FORMAT_R16_SFLOAT, extent,
                    usage, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, cumulative, error))
                return fail();
            images.push_back(&cumulative);
            ++allocation_count;
            if (fail_next_bundle_creation_for_test_ && allocation_count == 2) {
                fail_next_bundle_creation_for_test_ = false;
                error = "injected cloud-shadow bundle allocation failure after partial candidate creation";
                return fail();
            }
        }
        if (!create_level_descriptors(level, error)) return fail();
    }
    if (!clear_images(images, error)) return fail();
    return true;
}

void VkCloudShadows::destroy_level_pair(LevelPair& pair) {
    for (auto& level : pair) {
        if (level.generation_descriptor_pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device_, level.generation_descriptor_pool,
                                    nullptr);
        level.generation_descriptor_pool = VK_NULL_HANDLE;
        for (auto& constants : level.generation_constants) constants.reset();
        level.density.reset();
        level.cumulative[0].reset();
        level.cumulative[1].reset();
        level = {};
    }
}

void VkCloudShadows::retire_active(uint32_t completed_frame_slot) {
    if (!active_) return;
    retired_bundles_.push_back(
        {std::move(active_levels_), completed_frame_slot ^ 1u});
    active_levels_ = {};
    active_ = false;
}

void VkCloudShadows::collect_retired(uint32_t completed_frame_slot) {
    for (auto it = retired_bundles_.begin(); it != retired_bundles_.end();) {
        if (it->protected_slot == completed_frame_slot) {
            destroy_level_pair(it->levels);
            it = retired_bundles_.erase(it);
        } else {
            ++it;
        }
    }
}

bool VkCloudShadows::requested_layout_matches_active() const {
    return active_ && same_desc(active_levels_[0].desc, requested_levels_[0]) &&
           same_desc(active_levels_[1].desc, requested_levels_[1]);
}

std::string VkCloudShadows::allocation_diagnostic(
    const std::string& detail) const {
    char diagnostic[384]{};
    const double mib = static_cast<double>(
        3ull * 2ull *
        (static_cast<uint64_t>(requested_levels_[0].width) *
             requested_levels_[0].height * requested_levels_[0].depth +
         static_cast<uint64_t>(requested_levels_[1].width) *
             requested_levels_[1].height * requested_levels_[1].depth)) /
        (1024.0 * 1024.0);
    std::snprintf(
        diagnostic, sizeof(diagnostic),
        "cloud-shadow allocation failed for near %ux%ux%u and far %ux%ux%u (%.2f MiB): %s",
        requested_levels_[0].width, requested_levels_[0].height,
        requested_levels_[0].depth, requested_levels_[1].width,
        requested_levels_[1].height, requested_levels_[1].depth, mib,
        detail.c_str());
    return diagnostic;
}

bool VkCloudShadows::prepare_frame(
    uint32_t frame_slot, const matter::Float3& camera,
    const matter::Float3& sun_direction, float sun_angular_diameter_deg,
    std::string& error) {
    error.clear();
    if (!initialized_) {
        error = "cloud-shadow resources are not initialized";
        return false;
    }
    if (frame_slot >= 2) {
        error = "cloud-shadow descriptor frame slot is out of range";
        return false;
    }
    collect_retired(frame_slot);
    prepared_frame_slot_ = frame_slot;
    last_generation_dispatch_count_ = 0;
    sun_angular_radius_ = std::isfinite(sun_angular_diameter_deg)
        ? std::fmax(0.0f, sun_angular_diameter_deg) *
              (3.14159265358979323846f / 360.0f)
        : 0.0f;
    if (!requested_settings_.enabled) {
        retire_active(frame_slot);
        direct_sun_visible_ = false;
        request_failed_ = false;
        allocation_error_.clear();
        return true;
    }
    if (request_failed_) return true;

    if (!requested_layout_matches_active()) {
        LevelPair candidate{};
        std::string detail;
        if (!create_level_pair(requested_levels_, candidate, detail)) {
            retire_active(frame_slot);
            request_failed_ = true;
            allocation_error_ = allocation_diagnostic(detail);
            return true;
        }
        retire_active(frame_slot);
        active_levels_ = std::move(candidate);
        active_ = true;
        force_history_invalidation_ = true;
    }

    direct_sun_visible_ = cloud_shadow_direct_sun_visible(sun_direction);
    if (!direct_sun_visible_) {
        for (auto& level : active_levels_) level.history_valid = false;
        force_history_invalidation_ = true;
        return true;
    }

    for (uint32_t index = 0; index < 2; ++index) {
        auto& level = active_levels_[index];
        const matter::CloudShadowFrame next = matter::make_cloud_shadow_frame(
            level.desc, camera, sun_direction);
        if (!matter::detail::cloud_shadow_frame_is_valid(next)) {
            retire_active(frame_slot);
            request_failed_ = true;
            allocation_error_ = allocation_diagnostic(
                "non-finite sun-space coordinate frame");
            return true;
        }
        const float sun_delta = level.current_frame.valid
            ? frame_angle_degrees(level.current_frame, next) : 180.0f;
        level.previous_frame = level.current_frame;
        level.current_frame = next;
        level.history_valid = level.history_valid &&
            !force_history_invalidation_ &&
            !matter::cloud_shadow_requires_full_invalidation(
                level.previous_frame, level.current_frame, sun_delta);
    }
    return true;
}

bool VkCloudShadows::record(VkCommandBuffer command_buffer, float frame_time,
                            std::string& error) {
    error.clear();
    last_generation_dispatch_count_ = 0;
    if (!initialized_) {
        error = "cloud-shadow resources are not initialized";
        return false;
    }
    if (!active_ || !direct_sun_visible_) return true;

    auto& cloud_buffer = cloud_layer_ssbo_[prepared_frame_slot_];
    std::memcpy(cloud_buffer.mapped, packed_cloud_layers_.data(),
                sizeof(packed_cloud_layers_));
    if (!matter::flush_buffer(cloud_buffer, 0,
                              sizeof(packed_cloud_layers_), error))
        return false;

    for (uint32_t level_index = 0; level_index < 2; ++level_index) {
        auto& level = active_levels_[level_index];
        const uint32_t destination = level.active_index ^ 1u;
        GenerationConstants constants{};
        pack_mat4_column_major(constants.current_uvw_to_world,
                               level.current_frame.uvw_to_world);
        pack_mat4_column_major(constants.previous_world_to_uvw,
                               level.previous_frame.world_to_uvw);
        constants.dimensions_level[0] = level.desc.width;
        constants.dimensions_level[1] = level.desc.height;
        constants.dimensions_level[2] = level.desc.depth;
        constants.dimensions_level[3] = level_index;
        constants.scheduling[0] = requested_settings_.update_fraction;
        constants.scheduling[1] = frame_time;
        constants.scheduling[2] = level.current_frame.voxel_depth_m;
        constants.scheduling[3] = level.history_valid ? 1.0f : 0.0f;
        constants.controls[0] = frame_index_;
        constants.controls[1] = static_cast<uint32_t>(density_override_mode_);
        constants.controls[2] = static_cast<uint32_t>(density_override_nan_slice_);
        constants.controls[3] = cloud_layer_count_;
        constants.density_override[0] = density_override_even_sigma_;
        constants.density_override[1] = density_override_odd_sigma_;
        auto& constant_buffer =
            level.generation_constants[prepared_frame_slot_];
        std::memcpy(constant_buffer.mapped, &constants, sizeof(constants));
        if (!matter::flush_buffer(constant_buffer, 0, sizeof(constants), error))
            return false;

        matter::record_image_transition(
            command_buffer, level.density, VK_IMAGE_LAYOUT_GENERAL,
            level.density.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            level.density.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                : VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
        auto& output = level.cumulative[destination];
        matter::record_image_transition(
            command_buffer, output, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

        const VkDescriptorSet set =
            level.generation_sets[prepared_frame_slot_][destination];
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                generation_pipeline_layout_, 0, 1, &set,
                                0, nullptr);
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          reproject_pipeline_);
        vkCmdDispatch(command_buffer, (level.desc.width + 3u) / 4u,
                      (level.desc.height + 3u) / 4u,
                      (level.desc.depth + 3u) / 4u);
        ++last_generation_dispatch_count_;

        matter::record_image_transition(
            command_buffer, output, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          density_pipeline_);
        vkCmdDispatch(command_buffer, (level.desc.width + 7u) / 8u,
                      (level.desc.height + 7u) / 8u, 1);
        ++last_generation_dispatch_count_;
        matter::record_image_transition(
            command_buffer, level.density, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          integrate_pipeline_);
        vkCmdDispatch(command_buffer, (level.desc.width + 7u) / 8u,
                      (level.desc.height + 7u) / 8u, 1);
        ++last_generation_dispatch_count_;
        matter::record_image_transition(
            command_buffer, output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }
    for (auto& level : active_levels_) {
        level.active_index ^= 1u;
        level.history_valid = true;
    }
    force_history_invalidation_ = false;
    ++frame_index_;
    return true;
}

const CloudShadowLevelBundle& VkCloudShadows::level(uint32_t index) const {
    static const CloudShadowLevelBundle empty{};
    return index < active_levels_.size() ? active_levels_[index] : empty;
}

uint64_t VkCloudShadows::persistent_bytes() const {
    if (!active_) return 0;
    uint64_t voxels = 0;
    for (const auto& level : active_levels_) {
        voxels += static_cast<uint64_t>(level.desc.width) *
                  level.desc.height * level.desc.depth;
    }
    return voxels * 3ull * 2ull;
}

const matter::VkImageResource& VkCloudShadows::environment_image(
    uint32_t index) const {
    if (active_ && direct_sun_visible_ && index < 4) {
        const uint32_t level_index = index / 2;
        const uint32_t ping = index % 2;
        return active_levels_[level_index].cumulative[ping];
    }
    return emergency_[std::min(index, 3u)];
}

std::array<float, 40> VkCloudShadows::environment_block() const {
    std::array<float, 40> block{};
    for (uint32_t matrix = 0; matrix < 2; ++matrix) {
        for (uint32_t i = 0; i < 4; ++i)
            block[matrix * 16 + i * 5] = 1.0f;
    }
    if (!active_ || !direct_sun_visible_) return block;
    for (uint32_t matrix = 0; matrix < 2; ++matrix) {
        const auto& source = active_levels_[matrix].current_frame.world_to_uvw;
        for (uint32_t row = 0; row < 4; ++row)
            for (uint32_t column = 0; column < 4; ++column)
                block[matrix * 16 + column * 4 + row] =
                    source.m[row * 4 + column];
    }
    block[32] = 1.0f;
    block[33] = static_cast<float>(active_levels_[0].active_index);
    block[34] = static_cast<float>(active_levels_[1].active_index);
    block[36] = active_levels_[0].current_frame.voxel_xy_m;
    block[37] = active_levels_[1].current_frame.voxel_xy_m;
    block[38] = sun_angular_radius_;
    block[39] = requested_settings_.filter_scale;
    return block;
}

bool VkCloudShadows::failed_candidate_destroyed_for_test() const {
    return !failed_candidate_lifetimes_.empty() &&
        std::all_of(failed_candidate_lifetimes_.begin(),
                    failed_candidate_lifetimes_.end(),
                    [](const std::weak_ptr<void>& lifetime) {
                        return lifetime.expired();
                    });
}

bool VkCloudShadows::environment_image_is_clear_for_test(
    uint32_t index, std::string& error) {
    error.clear();
    if (!vulkan_ || index >= 4) {
        error = "cloud-shadow clear readback index is invalid";
        return false;
    }
    matter::VkImageResource& image = active_ && direct_sun_visible_ ?
        active_levels_[index / 2].cumulative[index % 2] : emergency_[index];
    matter::VkBufferResource readback;
    if (!matter::create_buffer(
            *vulkan_, sizeof(uint16_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, readback, error) ||
        !matter::map_buffer(readback, error)) return false;
    ReadTauRecord request{&image, readback.buffer, 0, 0, 0};
    if (!matter::submit_immediate(
            *vulkan_, record_read_tau, &request, error,
            matter::ImmediateSubmitPhase::staging_readback,
            {image.lifetime, readback.lifetime}) ||
        !matter::invalidate_buffer(readback, 0, sizeof(uint16_t), error))
        return false;
    uint16_t bits = 1;
    std::memcpy(&bits, readback.mapped, sizeof(bits));
    return bits == 0;
}

void VkCloudShadows::set_density_override_for_test(
    float even_sigma, int32_t nan_slice, float odd_sigma,
    bool invalidate_history) {
    density_override_mode_ = 1;
    density_override_even_sigma_ = even_sigma;
    density_override_odd_sigma_ = odd_sigma;
    density_override_nan_slice_ = nan_slice;
    if (invalidate_history) force_history_invalidation_ = true;
}

bool VkCloudShadows::generate_for_test(
    uint32_t frame_slot, const matter::Float3& camera,
    const matter::Float3& sun_direction, float frame_time,
    std::string& error) {
    if (!prepare_frame(frame_slot, camera, sun_direction, 0.53f, error))
        return false;
    struct Request {
        VkCloudShadows* shadows;
        float time;
        bool ok = false;
        std::string error;
    } request{this, frame_time};
    const auto callback = [](VkCommandBuffer command_buffer, void* data) {
        auto& value = *static_cast<Request*>(data);
        value.ok = value.shadows->record(command_buffer, value.time,
                                         value.error);
    };
    std::vector<std::shared_ptr<void>> lifetimes;
    for (auto& level : active_levels_) {
        lifetimes.push_back(level.density.lifetime);
        for (auto& cumulative : level.cumulative)
            lifetimes.push_back(cumulative.lifetime);
        lifetimes.push_back(
            level.generation_constants[prepared_frame_slot_].lifetime);
    }
    lifetimes.push_back(cloud_layer_ssbo_[prepared_frame_slot_].lifetime);
    if (!matter::submit_immediate(
            *vulkan_, callback, &request, error,
            matter::ImmediateSubmitPhase::staging_upload,
            std::move(lifetimes))) return false;
    if (!request.ok) {
        error = request.error;
        return false;
    }
    return true;
}

bool VkCloudShadows::readback_voxel(
    matter::VkImageResource& image, uint32_t x, uint32_t y, uint32_t z,
    float& value, uint16_t& raw, std::string& error) {
    if (x >= image.extent.width || y >= image.extent.height ||
        z >= image.extent.depth) {
        error = "cloud-shadow readback voxel is out of bounds";
        return false;
    }
    matter::VkBufferResource readback;
    if (!matter::create_buffer(
            *vulkan_, sizeof(uint16_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, readback, error) ||
        !matter::map_buffer(readback, error)) return false;
    ReadTauRecord request{&image, readback.buffer, x, y, z};
    if (!matter::submit_immediate(
            *vulkan_, record_read_tau, &request, error,
            matter::ImmediateSubmitPhase::staging_readback,
            {image.lifetime, readback.lifetime}) ||
        !matter::invalidate_buffer(readback, 0, sizeof(raw), error))
        return false;
    std::memcpy(&raw, readback.mapped, sizeof(raw));
    value = half_to_float(raw);
    return true;
}

bool VkCloudShadows::readback_cumulative_voxel_for_test(
    uint32_t level_index, uint32_t ping, uint32_t x, uint32_t y, uint32_t z,
    float& value, uint16_t& raw, std::string& error) {
    error.clear();
    if (!active_ || level_index >= 2 || ping >= 2) {
        error = "cloud-shadow cumulative readback selection is invalid";
        return false;
    }
    return readback_voxel(active_levels_[level_index].cumulative[ping],
                          x, y, z, value, raw, error);
}

void VkCloudShadows::append_frame_lifetimes(
    uint32_t frame_slot,
    std::vector<std::shared_ptr<void>>& lifetimes) const {
    if (!active_ || frame_slot >= 2) return;
    for (const auto& level : active_levels_) {
        lifetimes.push_back(level.density.lifetime);
        lifetimes.push_back(level.cumulative[0].lifetime);
        lifetimes.push_back(level.cumulative[1].lifetime);
        lifetimes.push_back(level.generation_constants[frame_slot].lifetime);
    }
    lifetimes.push_back(cloud_layer_ssbo_[frame_slot].lifetime);
}

void VkCloudShadows::destroy() {
    if (!vulkan_ && device_ == VK_NULL_HANDLE) return;
    destroy_level_pair(active_levels_);
    for (auto& retired : retired_bundles_) destroy_level_pair(retired.levels);
    retired_bundles_.clear();
    for (auto& image : emergency_) image.reset();
    for (auto& buffer : cloud_layer_ssbo_) buffer.reset();
    if (reproject_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, reproject_pipeline_, nullptr);
    if (density_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, density_pipeline_, nullptr);
    if (integrate_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, integrate_pipeline_, nullptr);
    if (generation_pipeline_layout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, generation_pipeline_layout_, nullptr);
    if (generation_set_layout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, generation_set_layout_, nullptr);
    if (generation_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device_, generation_sampler_, nullptr);
    reproject_pipeline_ = density_pipeline_ = integrate_pipeline_ =
        VK_NULL_HANDLE;
    generation_pipeline_layout_ = VK_NULL_HANDLE;
    generation_set_layout_ = VK_NULL_HANDLE;
    generation_sampler_ = VK_NULL_HANDLE;
    failed_candidate_lifetimes_.clear();
    vulkan_ = nullptr;
    device_ = VK_NULL_HANDLE;
    active_ = false;
    initialized_ = false;
    request_failed_ = false;
    direct_sun_visible_ = false;
    allocation_error_.clear();
}

}  // namespace viewer
