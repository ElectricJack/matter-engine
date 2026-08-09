// Keep windows.h (pulled in by vulkan_win32.h when VK_USE_PLATFORM_WIN32_KHR
// is defined, e.g. the vulkan_smoke_tests build) from declaring GDI/USER
// symbols (Rectangle, CloseWindow, ShowCursor) that collide with raylib.h,
// which reaches this TU via vk_emitter_gather.h -> part_asset_v2.h ->
// part_asset.h -> blas_manager.hpp. Same pattern as part_asset_v2.cpp.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#endif

#include "vk_volumetrics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "gpu_matrix_pack.h"
#include "frame_matrices.h"
#include "matter/vulkan_device.h"
#include "matter/world_definition.h"
#include "matter/world_session.h"
#include "shaders_gen/embedded_spirv.h"
#include "vk_emitter_gather.h"
#include "vk_resources.h"
#include "vk_scene_renderer.h"

namespace viewer {

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool vk_fail(const char* operation, VkResult result, std::string& error) {
    error = std::string(operation) + " failed with VkResult " +
            std::to_string(static_cast<int>(result));
    return false;
}

bool create_shader_module_from_spirv(VkDevice device, const char* name,
                                     VkShaderModule& shader,
                                     std::string& error) {
    const matter::EmbeddedSpirvView spirv = matter::find_spirv(name);
    if (!spirv.words || spirv.word_count == 0) {
        error = std::string("embedded SPIR-V not found: ") + name;
        return false;
    }
    VkShaderModuleCreateInfo create{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    create.codeSize = spirv.word_count * sizeof(uint32_t);
    create.pCode = spirv.words;
    const VkResult result =
        vkCreateShaderModule(device, &create, nullptr, &shader);
    if (result != VK_SUCCESS) return vk_fail("vkCreateShaderModule", result, error);
    return true;
}

VkDescriptorSetLayoutBinding make_binding(uint32_t binding,
                                          VkDescriptorType type,
                                          VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding result{};
    result.binding = binding;
    result.descriptorType = type;
    result.descriptorCount = 1;
    result.stageFlags = stages;
    return result;
}

void pack_mat4_column_major(float out[16], const matter::Mat4f& m) {
    // matter::Mat4f is row-major; GLSL mat4 is column-major.
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            out[col * 4 + row] = m.m[row * 4 + col];
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
    union { uint32_t bits; float value; } result{bits};
    return result.value;
}

// Simple 3D hash for procedural noise generation.
uint32_t hash3d(uint32_t x, uint32_t y, uint32_t z, uint32_t seed) {
    uint32_t h = x * 374761393u + y * 668265263u + z * 1274126177u + seed;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return h;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

VkVolumetrics::VkVolumetrics() = default;

VkVolumetrics::~VkVolumetrics() { destroy(); }

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

bool VkVolumetrics::init(matter::VulkanDevice& vulkan,
                         VkDescriptorSetLayout environment_layout,
                         std::string& error) {
    device_ = vulkan.device();
    vulkan_ = &vulkan;
    environment_set_layout_ = environment_layout;

    // Ray query availability follows the same ray-tracing capability check
    // that the engine uses for RT shadows.  If the device does not support
    // acceleration structures, shadow rays in the scatter shader cannot run
    // and the vol_scatter.comp SPIR-V (which requires GL_EXT_ray_query)
    // cannot be loaded.  In that case we skip all GPU resource creation and
    // record() will be a no-op.
    ray_query_available_ = vulkan.ray_tracing_available();
    if (!ray_query_available_) {
        initialized_ = true;
        return true;
    }

    if (!create_noise_texture(vulkan, error)) return false;
    if (!matter::create_image(vulkan, VK_IMAGE_TYPE_3D, VK_FORMAT_R16_SFLOAT,
                              {1, 1, 1}, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              cloud_density_dummy_, error)) return false;
    struct ClearDummy { matter::VkImageResource* image; } clear_dummy{&cloud_density_dummy_};
    const auto clear = [](VkCommandBuffer cmd, void* data) {
        auto& image = *static_cast<ClearDummy*>(data)->image;
        matter::record_image_transition(
            cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
        const VkClearColorValue zero{{0.0f, 0.0f, 0.0f, 0.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &zero, 1, &range);
        matter::record_image_transition(
            cmd, image, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    };
    if (!matter::submit_immediate(vulkan, clear, &clear_dummy, error,
                                  matter::ImmediateSubmitPhase::staging_upload,
                                  {cloud_density_dummy_.lifetime})) return false;
    if (!create_emitter_buffer(vulkan, error)) return false;
    if (!create_cloud_buffer(vulkan, error)) return false;
    if (!create_samplers(vulkan, error)) return false;
    if (!create_density_pipeline(vulkan, error)) return false;
    if (!create_scatter_pipeline(vulkan, error)) return false;
    if (!create_integrate_pipeline(vulkan, error)) return false;
    // A bundle owns every descriptor set that references its images. Pipelines
    // and layouts must therefore exist before the initial bundle is allocated.
    if (!create_froxel_bundle(vulkan, requested_dimensions_, active_bundle_, error))
        return false;

    resource_generation_ = 1;
    initialized_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// Noise texture (32^3 RGBA8 procedural curl noise)
// ---------------------------------------------------------------------------

bool VkVolumetrics::create_noise_texture(matter::VulkanDevice& vulkan,
                                          std::string& error) {
    const uint32_t N = kVolNoiseSize;
    const VkExtent3D extent{N, N, N};

    if (!matter::create_image(
            vulkan, VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, extent,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            noise_texture_, error)) {
        return false;
    }

    // Generate RGBA8 noise data on the CPU.
    const size_t texel_count = N * N * N;
    std::vector<uint8_t> pixels(texel_count * 4);
    for (uint32_t z = 0; z < N; ++z) {
        for (uint32_t y = 0; y < N; ++y) {
            for (uint32_t x = 0; x < N; ++x) {
                const size_t idx = (z * N * N + y * N + x) * 4;
                pixels[idx + 0] = static_cast<uint8_t>(hash3d(x, y, z, 0) & 0xFF);
                pixels[idx + 1] = static_cast<uint8_t>(hash3d(x, y, z, 7919) & 0xFF);
                pixels[idx + 2] = static_cast<uint8_t>(hash3d(x, y, z, 104729) & 0xFF);
                pixels[idx + 3] = 255;
            }
        }
    }

    // Upload via staging buffer.
    const VkDeviceSize byte_count = pixels.size();
    matter::VkBufferResource staging;
    if (!matter::create_buffer(
            vulkan, byte_count, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, error) ||
        !matter::map_buffer(staging, error)) {
        return false;
    }
    std::memcpy(staging.mapped, pixels.data(), byte_count);
    if (!matter::flush_buffer(staging, 0, byte_count, error)) return false;

    struct CopyInfo {
        VkBuffer src;
        VkImage dst;
        VkExtent3D extent;
    };
    CopyInfo info{staging.buffer, noise_texture_.image, extent};

    auto record_fn = [](VkCommandBuffer cmd, void* user_data) {
        const auto& ci = *static_cast<const CopyInfo*>(user_data);

        // Transition to TRANSFER_DST.
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.srcAccessMask = 0;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = ci.dst;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);

        // Copy.
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = ci.extent;
        vkCmdCopyBufferToImage(cmd, ci.src, ci.dst,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        // Transition to SHADER_READ_ONLY.
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dep);
    };

    std::vector<std::shared_ptr<void>> deps{staging.lifetime,
                                             noise_texture_.lifetime};
    if (!matter::submit_immediate(vulkan, record_fn, &info, error,
                                  matter::ImmediateSubmitPhase::staging_upload,
                                  std::move(deps))) {
        return false;
    }
    noise_texture_.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

// ---------------------------------------------------------------------------
// Volume images
// ---------------------------------------------------------------------------

bool VkVolumetrics::create_froxel_bundle(matter::VulkanDevice& vulkan,
                                         matter::FroxelGridDimensions dimensions,
                                         FroxelBundle& bundle, std::string& error) {
    if (fail_next_bundle_creation_for_test_) {
        fail_next_bundle_creation_for_test_ = false;
        error = "injected froxel bundle allocation failure";
        return false;
    }
    bundle.dimensions = dimensions;
    const auto fail = [&]() {
        destroy_froxel_bundle(bundle);
        return false;
    };
    const VkExtent3D vol_extent{dimensions.width, dimensions.height, dimensions.depth};
    const VkImageUsageFlags sampled_storage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    bundle.enhanced_clouds = enhanced_clouds_requested_;

    // vol_media_ (density pass output, scatter pass input).
    if (!matter::create_image(vulkan, VK_IMAGE_TYPE_3D, VK_FORMAT_R16G16B16A16_SFLOAT,
                              vol_extent, sampled_storage,
                              VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              bundle.media, error)) {
        return fail();
    }
    if (bundle.enhanced_clouds && !matter::create_image(
            vulkan, VK_IMAGE_TYPE_3D, VK_FORMAT_R16_SFLOAT, vol_extent,
            sampled_storage, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bundle.cloud_density, error)) return fail();

    // vol_scatter_[0..1] (ping-pong temporal).
    for (int i = 0; i < 2; ++i) {
        if (!matter::create_image(vulkan, VK_IMAGE_TYPE_3D,
                                  VK_FORMAT_R16G16B16A16_SFLOAT, vol_extent,
                                  sampled_storage, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                  bundle.scatter[i], error)) {
            return fail();
        }
    }

    // vol_integrated_ (integration output, composite shader input).
    if (!matter::create_image(vulkan, VK_IMAGE_TYPE_3D, VK_FORMAT_R16G16B16A16_SFLOAT,
                              vol_extent, sampled_storage,
                              VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              bundle.integrated, error)) {
        return fail();
    }
    if (!create_bundle_descriptors(bundle, error)) return fail();
    return true;
}

void VkVolumetrics::destroy_froxel_bundle(FroxelBundle& bundle) {
    if (bundle.descriptor_pool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(device_, bundle.descriptor_pool, nullptr);
    bundle.media.reset();
    bundle.scatter[0].reset();
    bundle.scatter[1].reset();
    bundle.integrated.reset();
    bundle.cloud_density.reset();
    bundle = {};
}

bool VkVolumetrics::create_bundle_descriptors(FroxelBundle& bundle,
                                              std::string& error) {
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 15},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 4},
    };
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 7;
    pool_info.poolSizeCount = static_cast<uint32_t>(std::size(sizes));
    pool_info.pPoolSizes = sizes;
    VkResult result = vkCreateDescriptorPool(device_, &pool_info, nullptr,
                                             &bundle.descriptor_pool);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateDescriptorPool(froxel bundle)", result, error);
    if (fail_next_bundle_descriptor_allocation_for_test_) {
        fail_next_bundle_descriptor_allocation_for_test_ = false;
        error = "injected froxel descriptor allocation failure";
        return false;
    }

    const VkDescriptorSetLayout layouts[] = {
        density_set_layout_, scatter_set_layout_, scatter_set_layout_,
        scatter_set_layout_, scatter_set_layout_,
        integrate_set_layout_, integrate_set_layout_};
    VkDescriptorSet sets[7]{};
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = bundle.descriptor_pool;
    alloc.descriptorSetCount = static_cast<uint32_t>(std::size(layouts));
    alloc.pSetLayouts = layouts;
    result = vkAllocateDescriptorSets(device_, &alloc, sets);
    if (result != VK_SUCCESS)
        return vk_fail("vkAllocateDescriptorSets(froxel bundle)", result, error);
    bundle.density_set = sets[0];
    bundle.scatter_sets[0][0] = sets[1];
    bundle.scatter_sets[0][1] = sets[2];
    bundle.scatter_sets[1][0] = sets[3];
    bundle.scatter_sets[1][1] = sets[4];
    bundle.integrate_sets[0] = sets[5];
    bundle.integrate_sets[1] = sets[6];

    VkDescriptorImageInfo media{};
    media.sampler = linear_clamp_sampler_;
    media.imageView = bundle.media.view;
    media.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo density{};
    density.imageView = bundle.media.view;
    density.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo noise{};
    noise.sampler = linear_repeat_sampler_;
    noise.imageView = noise_texture_.view;
    noise.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorBufferInfo emitters{emitter_ssbo_.buffer, 0, emitter_ssbo_.size};
    VkDescriptorBufferInfo clouds{cloud_ssbo_.buffer, 0, cloud_ssbo_.size};
    VkDescriptorImageInfo cloud_density{};
    cloud_density.imageView = (bundle.enhanced_clouds ? bundle.cloud_density : cloud_density_dummy_).view;
    cloud_density.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet density_writes[5]{};
    density_writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    density_writes[0].dstSet = bundle.density_set;
    density_writes[0].dstBinding = 0; density_writes[0].descriptorCount = 1;
    density_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    density_writes[0].pImageInfo = &density;
    density_writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    density_writes[1].dstSet = bundle.density_set;
    density_writes[1].dstBinding = 1; density_writes[1].descriptorCount = 1;
    density_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    density_writes[1].pImageInfo = &noise;
    density_writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    density_writes[2].dstSet = bundle.density_set;
    density_writes[2].dstBinding = 2; density_writes[2].descriptorCount = 1;
    density_writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    density_writes[2].pBufferInfo = &emitters;
    density_writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    density_writes[3].dstSet = bundle.density_set;
    density_writes[3].dstBinding = 3; density_writes[3].descriptorCount = 1;
    density_writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    density_writes[3].pBufferInfo = &clouds;
    density_writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    density_writes[4].dstSet = bundle.density_set;
    density_writes[4].dstBinding = 4; density_writes[4].descriptorCount = 1;
    density_writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    density_writes[4].pImageInfo = &cloud_density;
    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(std::size(density_writes)),
                           density_writes, 0, nullptr);
    for (int slot = 0; slot < 2; ++slot) for (int i = 0; i < 2; ++i) {
        VkDescriptorImageInfo write{};
        write.imageView = bundle.scatter[i].view;
        write.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo history{};
        history.sampler = linear_clamp_sampler_;
        history.imageView = bundle.scatter[1 - i].view;
        history.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet writes[3]{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[0].dstSet = bundle.scatter_sets[slot][i];
        writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &media;
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[1].dstSet = bundle.scatter_sets[slot][i];
        writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[1].pImageInfo = &write;
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[2].dstSet = bundle.scatter_sets[slot][i];
        writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[2].pImageInfo = &history;
        vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
    }
    for (int i = 0; i < 2; ++i) {
        VkDescriptorImageInfo scatter{};
        scatter.sampler = linear_clamp_sampler_;
        scatter.imageView = bundle.scatter[i].view;
        scatter.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo integrated{};
        integrated.imageView = bundle.integrated.view;
        integrated.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[2]{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[0].dstSet = bundle.integrate_sets[i];
        writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &scatter;
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[1].dstSet = bundle.integrate_sets[i];
        writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[1].pImageInfo = &integrated;
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    }
    return true;
}

bool VkVolumetrics::replace_froxel_bundle(uint32_t completed_frame_slot, std::string& error) {
    for (auto it = retired_bundles_.begin(); it != retired_bundles_.end();) {
        if (it->protected_slot == completed_frame_slot) {
            destroy_froxel_bundle(it->bundle);
            it = retired_bundles_.erase(it);
        } else ++it;
    }
    if (requested_dimensions_.width == active_bundle_.dimensions.width &&
        requested_dimensions_.height == active_bundle_.dimensions.height &&
        requested_dimensions_.depth == active_bundle_.dimensions.depth &&
        enhanced_clouds_requested_ == active_bundle_.enhanced_clouds) return true;
    FroxelBundle candidate{};
    if (!create_froxel_bundle(*vulkan_, requested_dimensions_, candidate, error)) {
        allocation_rejected_ = true;
        allocation_error_ = error;
        return false;
    }
    retired_bundles_.push_back({std::move(active_bundle_), completed_frame_slot ^ 1u});
    active_bundle_ = std::move(candidate);
    active_bundle_.ping_index = 0;
    has_prev_matrices_ = false;
    allocation_rejected_ = false;
    allocation_error_.clear();
    ++resource_generation_;
    return true;
}

bool VkVolumetrics::prepare_froxel_bundle(uint32_t frame_slot,
                                          std::string& error) {
    if (!initialized_ || !enabled_ || !ray_query_available_) return true;
    if (frame_slot >= 2) {
        error = "froxel descriptor frame slot is out of range";
        return false;
    }
    if (replace_froxel_bundle(frame_slot, error)) {
        prepared_frame_slot_ = frame_slot;
        return true;
    }
    // Allocation rejection is transactional: the old active bundle remains
    // valid for this frame. Preserve its diagnostic for UI/stats but do not
    // abandon an already-acquired renderer frame.
    if (active_bundle_.integrated.view != VK_NULL_HANDLE) {
        error.clear();
        prepared_frame_slot_ = frame_slot;
        return true;
    }
    return false;
}

matter::FroxelXyScale VkVolumetrics::effective_xy_scale() const {
    switch (active_bundle_.dimensions.width) {
        case 80: return matter::FroxelXyScale::X0_5;
        case 120: return matter::FroxelXyScale::X0_75;
        case 240: return matter::FroxelXyScale::X1_5;
        case 320: return matter::FroxelXyScale::X2_0;
        default: return matter::FroxelXyScale::X1_0;
    }
}

matter::FroxelDepthSlices VkVolumetrics::effective_depth_slices() const {
    switch (active_bundle_.dimensions.depth) {
        case 64: return matter::FroxelDepthSlices::D64;
        case 96: return matter::FroxelDepthSlices::D96;
        case 192: return matter::FroxelDepthSlices::D192;
        case 256: return matter::FroxelDepthSlices::D256;
        default: return matter::FroxelDepthSlices::D128;
    }
}

// ---------------------------------------------------------------------------
// Emitter SSBO
// ---------------------------------------------------------------------------

bool VkVolumetrics::create_emitter_buffer(matter::VulkanDevice& vulkan,
                                           std::string& error) {
    // Layout: uint32 count at offset 0, pad to 16, then GpuVolumeEmitter[256].
    const VkDeviceSize size = 16 + sizeof(GpuVolumeEmitter) * kVolMaxEmitters;
    if (!matter::create_buffer(
            vulkan, size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            emitter_ssbo_, error)) {
        return false;
    }
    if (!matter::map_buffer(emitter_ssbo_, error)) return false;

    // Zero-initialize (count = 0).
    std::memset(emitter_ssbo_.mapped, 0, static_cast<size_t>(size));
    return matter::flush_buffer(emitter_ssbo_, 0, size, error);
}

// ---------------------------------------------------------------------------
// Cloud-layer SSBO
//
// Always kMaxCloudLayers entries, always bound, even when no world uses a
// single one: a descriptor set layout cannot be specialized the way the loop
// bound can, and 256 bytes of unread host-visible memory is not worth a
// second layout. The CLOUD_LAYERS specialization is what makes the unused
// entries free, not the buffer's size.
// ---------------------------------------------------------------------------
bool VkVolumetrics::create_cloud_buffer(matter::VulkanDevice& vulkan,
                                        std::string& error) {
    const VkDeviceSize size =
        sizeof(matter::GpuCloudLayer) * matter::kMaxCloudLayers;
    if (!matter::create_buffer(
            vulkan, size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            cloud_ssbo_, error)) {
        return false;
    }
    if (!matter::map_buffer(cloud_ssbo_, error)) return false;
    std::memset(cloud_ssbo_.mapped, 0, static_cast<size_t>(size));
    return matter::flush_buffer(cloud_ssbo_, 0, size, error);
}

// ---------------------------------------------------------------------------
// Samplers
// ---------------------------------------------------------------------------

bool VkVolumetrics::create_samplers(matter::VulkanDevice& vulkan,
                                     std::string& error) {
    (void)vulkan;  // device handle comes from device_; kept for API symmetry
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.maxLod = 0.0f;

    // Clamp-to-edge for volume textures.
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkResult result = vkCreateSampler(device_, &info, nullptr,
                                      &linear_clamp_sampler_);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateSampler(clamp)", result, error);

    // Clamp-to-border (transparent black) for history texture so edge
    // samples return zero instead of smearing the edge texel.
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    result = vkCreateSampler(device_, &info, nullptr, &linear_border_sampler_);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateSampler(border)", result, error);

    // Repeat for noise texture.
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    result = vkCreateSampler(device_, &info, nullptr, &linear_repeat_sampler_);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateSampler(repeat)", result, error);

    return true;
}

// ---------------------------------------------------------------------------
// Density pipeline
// ---------------------------------------------------------------------------

bool VkVolumetrics::create_density_pipeline(matter::VulkanDevice& vulkan,
                                             std::string& error) {
    (void)vulkan;  // device handle comes from device_; kept for API symmetry
    // Bindings:
    //   0 = storage image (vol_media, writeonly)
    //   1 = combined image sampler (noise_tex)
    //   2 = storage buffer (emitter SSBO)
    //   3 = storage buffer (cloud-layer SSBO)
    const VkDescriptorSetLayoutBinding bindings[] = {
        make_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                     VK_SHADER_STAGE_COMPUTE_BIT),
        make_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     VK_SHADER_STAGE_COMPUTE_BIT),
        make_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     VK_SHADER_STAGE_COMPUTE_BIT),
        make_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     VK_SHADER_STAGE_COMPUTE_BIT),
        make_binding(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                     VK_SHADER_STAGE_COMPUTE_BIT),
    };

    VkDescriptorSetLayoutCreateInfo set_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 5;
    set_info.pBindings = bindings;
    VkResult result = vkCreateDescriptorSetLayout(device_, &set_info, nullptr,
                                                  &density_set_layout_);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateDescriptorSetLayout(density)", result, error);

    // Push constants: DensityConstants (128 bytes).
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.offset = 0;
    push.size = sizeof(DensityConstants);

    VkPipelineLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &density_set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    result = vkCreatePipelineLayout(device_, &layout_info, nullptr,
                                    &density_pipeline_layout_);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreatePipelineLayout(density)", result, error);

    // Shader module — ONE module, kMaxCloudLayers + 1 pipelines.
    //
    // vol_density.comp declares `layout(constant_id = 0) const int
    // CLOUD_LAYERS`, and every pipeline below bakes a different value of it.
    // At 0 the driver strips the whole cloud loop, its SSBO read and the fbm
    // it calls, which is what keeps a world with no clouds paying nothing for
    // the feature across 1.84M froxels a frame.
    //
    // All of them are built here rather than lazily on first use: five
    // compute pipelines from one already-loaded module is a few milliseconds
    // at startup, and building them on demand would put a driver compile in
    // the frame where the user ticks a layer on. It also means every
    // specialization is exercised by every run, so a permutation that fails
    // to compile fails at init with a name rather than the first time some
    // world happens to use four decks.
    VkShaderModule shader = VK_NULL_HANDLE;
    if (!create_shader_module_from_spirv(device_, "vol_density.comp.spv",
                                         shader, error)) {
        return false;
    }
    {
        VkSpecializationMapEntry entries[2]{};
        entries[0] = {0, 0, sizeof(int32_t)};
        entries[1] = {1, sizeof(int32_t), sizeof(int32_t)};

        bool ok = true;
        for (int count = 0; count <= matter::kMaxCloudLayers; ++count) for (int enhanced = 0; enhanced < 2; ++enhanced) {
            const int32_t values[2] = {count, enhanced};
            VkSpecializationInfo spec{};
            spec.mapEntryCount = 2;
            spec.pMapEntries = entries;
            spec.dataSize = sizeof(values);
            spec.pData = values;

            VkPipelineShaderStageCreateInfo stage{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = shader;
            stage.pName = "main";
            stage.pSpecializationInfo = &spec;

            VkComputePipelineCreateInfo create{
                VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            create.stage = stage;
            create.layout = density_pipeline_layout_;
            result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1,
                                              &create, nullptr,
                                              &density_pipelines_[count][enhanced]);
            if (result != VK_SUCCESS) {
                const std::string op =
                    "vkCreateComputePipelines(density, CLOUD_LAYERS=" +
                    std::to_string(count) + ")";
                vk_fail(op.c_str(), result, error);
                ok = false;
                break;
            }
        }
        vkDestroyShaderModule(device_, shader, nullptr);
        if (!ok) return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Scatter pipeline
// ---------------------------------------------------------------------------

bool VkVolumetrics::create_scatter_pipeline(matter::VulkanDevice& vulkan,
                                             std::string& error) {
    (void)vulkan;  // device handle comes from device_; kept for API symmetry
    // Bindings:
    //   0 = combined image sampler (vol_media, read)
    //   1 = storage image (vol_scatter[current], write)
    //   2 = combined image sampler (vol_scatter[history], read)
    //   3 = combined image sampler (depth texture)
    //   4 = acceleration structure (TLAS)
    const VkDescriptorSetLayoutBinding bindings[] = {
        make_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     VK_SHADER_STAGE_COMPUTE_BIT),
        make_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                     VK_SHADER_STAGE_COMPUTE_BIT),
        make_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     VK_SHADER_STAGE_COMPUTE_BIT),
        make_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     VK_SHADER_STAGE_COMPUTE_BIT),
        make_binding(4, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                     VK_SHADER_STAGE_COMPUTE_BIT),
    };

    VkDescriptorSetLayoutCreateInfo set_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 5;
    set_info.pBindings = bindings;
    VkResult result = vkCreateDescriptorSetLayout(device_, &set_info, nullptr,
                                                  &scatter_set_layout_);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateDescriptorSetLayout(scatter)", result, error);

    // Push constants: ScatterConstants (208 bytes).
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.offset = 0;
    push.size = sizeof(ScatterConstants);

    VkPipelineLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    const VkDescriptorSetLayout scatter_sets[] = {scatter_set_layout_,
                                                   environment_set_layout_};
    layout_info.setLayoutCount = 2;
    layout_info.pSetLayouts = scatter_sets;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    result = vkCreatePipelineLayout(device_, &layout_info, nullptr,
                                    &scatter_pipeline_layout_);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreatePipelineLayout(scatter)", result, error);

    // Shader module.
    VkShaderModule shader = VK_NULL_HANDLE;
    if (!create_shader_module_from_spirv(device_, "vol_scatter.comp.spv",
                                         shader, error)) {
        return false;
    }
    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";
    VkComputePipelineCreateInfo create{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    create.stage = stage;
    create.layout = scatter_pipeline_layout_;
    result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &create,
                                      nullptr, &scatter_pipeline_);
    vkDestroyShaderModule(device_, shader, nullptr);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateComputePipelines(scatter)", result, error);

    return true;
}

// ---------------------------------------------------------------------------
// Integrate pipeline
// ---------------------------------------------------------------------------

bool VkVolumetrics::create_integrate_pipeline(matter::VulkanDevice& vulkan,
                                               std::string& error) {
    (void)vulkan;  // device handle comes from device_; kept for API symmetry
    // Bindings:
    //   0 = combined image sampler (vol_scatter[current], read)
    //   1 = storage image (vol_integrated, write)
    const VkDescriptorSetLayoutBinding bindings[] = {
        make_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     VK_SHADER_STAGE_COMPUTE_BIT),
        make_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                     VK_SHADER_STAGE_COMPUTE_BIT),
    };

    VkDescriptorSetLayoutCreateInfo set_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 2;
    set_info.pBindings = bindings;
    VkResult result = vkCreateDescriptorSetLayout(device_, &set_info, nullptr,
                                                  &integrate_set_layout_);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateDescriptorSetLayout(integrate)", result, error);

    // No push constants for the integrate shader.
    VkPipelineLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &integrate_set_layout_;
    result = vkCreatePipelineLayout(device_, &layout_info, nullptr,
                                    &integrate_pipeline_layout_);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreatePipelineLayout(integrate)", result, error);

    // Shader module.
    VkShaderModule shader = VK_NULL_HANDLE;
    if (!create_shader_module_from_spirv(device_, "vol_integrate.comp.spv",
                                         shader, error)) {
        return false;
    }
    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";
    VkComputePipelineCreateInfo create{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    create.stage = stage;
    create.layout = integrate_pipeline_layout_;
    result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &create,
                                      nullptr, &integrate_pipeline_);
    vkDestroyShaderModule(device_, shader, nullptr);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateComputePipelines(integrate)", result, error);

    return true;
}

// ---------------------------------------------------------------------------
// update_settings
// ---------------------------------------------------------------------------

void VkVolumetrics::update_settings(
    const matter::VulkanVolumetricsSettings& vol,
    const matter::FogSettings& fog) {
    enabled_ = vol.enabled;
    temporal_blend_ = vol.temporal_blend;
    phase_g_ = vol.phase_g;
    requested_dimensions_ = matter::resolve_froxel_grid(vol);
    enhanced_clouds_requested_ = vol.local_sun_march_steps > 0 ||
        vol.multiple_scattering_orders > 1 || vol.multiple_scattering_strength > 0.0f ||
        vol.powder_strength > 0.0f;

    fog_density_ = fog.density;
    fog_floor_ = fog.floor;
    fog_falloff_ = fog.falloff;
    for (int i = 0; i < 3; ++i) {
        fog_color_[i] = fog.color[i];
        fog_wind_[i] = fog.wind[i];
    }

    // Cloud decks. The count is a PREFIX count (active_cloud_count stops at
    // the first gap), because it selects the specialization the next dispatch
    // binds — a shader compiled for 2 layers reads entries 0 and 1 and nothing
    // else, so a hole at index 0 would silently render layer 1's parameters
    // as layer 0's. Callers that can create a hole call compact_clouds first;
    // this is the belt to that braces.
    const int32_t requested =
        fog.cloud_count < 0 ? 0
                            : (fog.cloud_count > matter::kMaxCloudLayers
                                   ? matter::kMaxCloudLayers
                                   : fog.cloud_count);
    if (fog.cloud_count > matter::kMaxCloudLayers && !cloud_overflow_warned_) {
        cloud_overflow_warned_ = true;
        std::fprintf(stderr,
                     "[volumetrics] world asked for %d cloud layers; the "
                     "shader is specialized for at most %d, so the extra "
                     "layers are ignored\n",
                     static_cast<int>(fog.cloud_count), matter::kMaxCloudLayers);
    }
    (void)requested;
    cloud_count_ = matter::active_cloud_count(fog);
    for (int i = 0; i < matter::kMaxCloudLayers; ++i) cloud_layers_[i] = fog.clouds[i];
}

// ---------------------------------------------------------------------------
// set_lighting
// ---------------------------------------------------------------------------

void VkVolumetrics::set_lighting(const VkSceneLighting& lighting) {
    sun_direction_[0] = lighting.sun_direction.x;
    sun_direction_[1] = lighting.sun_direction.y;
    sun_direction_[2] = lighting.sun_direction.z;
    sun_intensity_ = lighting.sun_intensity;
    sun_color_[0] = lighting.sun_color.x;
    sun_color_[1] = lighting.sun_color.y;
    sun_color_[2] = lighting.sun_color.z;
    sky_color_[0] = lighting.sky_color.x;
    sky_color_[1] = lighting.sky_color.y;
    sky_color_[2] = lighting.sky_color.z;
}

// ---------------------------------------------------------------------------
// update_emitters
// ---------------------------------------------------------------------------

void VkVolumetrics::update_emitters(
    matter::VulkanDevice& vulkan,
    const std::vector<GpuVolumeEmitter>& emitters) {
    (void)vulkan;  // device handle comes from device_; kept for API symmetry
    if (!initialized_ || emitter_ssbo_.buffer == VK_NULL_HANDLE) return;

    const uint32_t count =
        std::min(static_cast<uint32_t>(emitters.size()), kVolMaxEmitters);

    // Write count at offset 0.
    auto* base = static_cast<uint8_t*>(emitter_ssbo_.mapped);
    std::memcpy(base, &count, sizeof(uint32_t));

    // Write emitter array at offset 16 (std430 alignment).
    if (count > 0) {
        std::memcpy(base + 16, emitters.data(),
                    count * sizeof(GpuVolumeEmitter));
    }

    std::string flush_error;
    matter::flush_buffer(emitter_ssbo_, 0, emitter_ssbo_.size, flush_error);
}

// ---------------------------------------------------------------------------
// record
// ---------------------------------------------------------------------------

bool VkVolumetrics::record(VkCommandBuffer cmd,
                           uint32_t frame_slot,
                           matter::VkImageResource& depth_image,
                           VkAccelerationStructureKHR tlas,
                           const FrameMatrices& matrices,
                           float frame_time,
                           std::string& error) {
    if (!initialized_) return true;
    if (!enabled_ || !ray_query_available_) return true;
    if (prepared_frame_slot_ != frame_slot &&
        !prepare_froxel_bundle(frame_slot, error)) return false;

    const uint32_t current = active_bundle_.ping_index;
    const uint32_t history = 1 - active_bundle_.ping_index;

    // --- Update per-frame scatter descriptors (depth + TLAS) ---
    {
        // Binding 3: depth texture.
        VkDescriptorImageInfo depth_info{};
        depth_info.sampler = linear_clamp_sampler_;
        depth_info.imageView = depth_image.view;
        depth_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Binding 4: TLAS.
        VkWriteDescriptorSetAccelerationStructureKHR as_write{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        as_write.accelerationStructureCount = 1;
        as_write.pAccelerationStructures = &tlas;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = active_bundle_.scatter_sets[frame_slot][current];
        writes[0].dstBinding = 3;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &depth_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].pNext = &as_write;
        writes[1].dstSet = active_bundle_.scatter_sets[frame_slot][current];
        writes[1].dstBinding = 4;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    }

    // ---------------------------------------------------------------
    // Pass 1: Density
    // ---------------------------------------------------------------

    // Transition vol_media_ to GENERAL for storage write.
    matter::record_image_transition(
        cmd, active_bundle_.media, VK_IMAGE_LAYOUT_GENERAL,
        active_bundle_.media.layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        active_bundle_.media.layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VkAccessFlags2(0)
            : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    if (active_bundle_.enhanced_clouds) {
        matter::record_image_transition(
            cmd, active_bundle_.cloud_density, VK_IMAGE_LAYOUT_GENERAL,
            active_bundle_.cloud_density.layout == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                : (active_bundle_.cloud_density.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                       ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                       : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT),
            active_bundle_.cloud_density.layout == VK_IMAGE_LAYOUT_UNDEFINED
                ? VkAccessFlags2(0)
                : (active_bundle_.cloud_density.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                       ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                       : VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }

    // Fill push constants.
    DensityConstants density_pc{};
    pack_mat4_column_major(density_pc.clip_to_world, matrices.clip_to_world);
    // Extract camera position by unprojecting the near-plane center.
    // Reversed-Z: near is NDC z = 1, so this is clip_to_world * (0,0,1,1).
    // (NDC (0,0,0) is now the FAR-plane center — using it put the "camera"
    // a kilometer out and flipped every view-dependent term.)
    {
        const float* m = matrices.clip_to_world.m;
        float w = m[14] + m[15];
        if (std::abs(w) > 1e-9f) {
            density_pc.camera_pos[0] = (m[2]  + m[3])  / w;
            density_pc.camera_pos[1] = (m[6]  + m[7])  / w;
            density_pc.camera_pos[2] = (m[10] + m[11]) / w;
        }
    }
    density_pc.frame_time = frame_time;
    density_pc.fog_density = fog_density_;
    // Always the ground-fog meaning now. The bounded-cloud mode that used to
    // REPURPOSE these two as a layer's [min, max] is gone: decks are the
    // cloud-layer SSBO, and the ground fog underneath them keeps its own
    // floor and falloff.
    density_pc.fog_floor = fog_floor_;
    density_pc.fog_falloff = fog_falloff_;
    // Reversed-ZO projection: m[10] = n/(f-n), m[11] = f*n/(f-n), so the
    // recovery identities are m[11]/m[10] = far and m[11]/(m[10]+1) = near
    // (the standard-ZO identities with roles swapped).
    density_pc.camera_near = matrices.view_to_clip.m[11] /
                             (matrices.view_to_clip.m[10] + 1.0f);
    for (int i = 0; i < 3; ++i) {
        density_pc.fog_color[i] = fog_color_[i];
        density_pc.fog_wind[i] = fog_wind_[i];
    }
    density_pc.camera_far = matrices.view_to_clip.m[11] /
                            matrices.view_to_clip.m[10];
    // pad2 is padding again. It used to smuggle the bounded layer's noise
    // scale through the 128-byte ABI (positive = layer on); cloud parameters
    // now live in their own SSBO where there is room to name them.
    density_pc.pad2 = 0.0f;

    // Upload the live decks and pick the matching specialization. Both are
    // driven by cloud_count_, so a shader compiled for N layers can only ever
    // read the N entries this loop just wrote.
    if (cloud_ssbo_.mapped != nullptr) {
        matter::GpuCloudLayer packed[matter::kMaxCloudLayers]{};
        for (int i = 0; i < cloud_count_; ++i)
            matter::pack_cloud_layer(cloud_layers_[i], i, packed[i]);
        std::memcpy(cloud_ssbo_.mapped, packed, sizeof(packed));
        std::string flush_error;
        matter::flush_buffer(cloud_ssbo_, 0, sizeof(packed), flush_error);
    }
    const int pipeline_index =
        cloud_count_ < 0 ? 0
                         : (cloud_count_ > matter::kMaxCloudLayers
                                ? matter::kMaxCloudLayers
                                : cloud_count_);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      density_pipelines_[pipeline_index][active_bundle_.enhanced_clouds ? 1 : 0]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            density_pipeline_layout_, 0, 1,
                            &active_bundle_.density_set,
                            0, nullptr);
    vkCmdPushConstants(cmd, density_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(DensityConstants), &density_pc);

    // Dispatch: ceil(160/4) x ceil(90/4) x 1 = 40 x 23 x 1.
    const uint32_t density_gx = (active_bundle_.dimensions.width + 3) / 4;
    const uint32_t density_gy = (active_bundle_.dimensions.height + 3) / 4;
    vkCmdDispatch(cmd, density_gx, density_gy, 1);

    // ---------------------------------------------------------------
    // Barrier: vol_media_ GENERAL -> SHADER_READ_ONLY
    // ---------------------------------------------------------------
    matter::record_image_transition(
        cmd, active_bundle_.media, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    // The enhanced density image is also sampled by the composite debug view.
    // Keep its independently produced extinction visible to that fragment
    // consumer before scatter/integration proceed with the packed media.
    if (active_bundle_.enhanced_clouds) {
        matter::record_image_transition(
            cmd, active_bundle_.cloud_density,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    // ---------------------------------------------------------------
    // Pass 2: Scatter
    // ---------------------------------------------------------------

    // Transition vol_scatter_[current] to GENERAL for storage write.
    matter::record_image_transition(
        cmd, active_bundle_.scatter[current], VK_IMAGE_LAYOUT_GENERAL,
        active_bundle_.scatter[current].layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        active_bundle_.scatter[current].layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VkAccessFlags2(0)
            : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    // Ensure history is readable (may still be UNDEFINED on first frame).
    if (active_bundle_.scatter[history].layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        matter::record_image_transition(
            cmd, active_bundle_.scatter[history],
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            active_bundle_.scatter[history].layout == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            active_bundle_.scatter[history].layout == VK_IMAGE_LAYOUT_UNDEFINED
                ? VkAccessFlags2(0)
                : VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }

    ScatterConstants scatter_pc{};
    pack_mat4_column_major(scatter_pc.clip_to_world, matrices.clip_to_world);
    pack_mat4_column_major(scatter_pc.prev_world_to_clip, prev_world_to_clip_);
    // Camera position = unprojected near-plane center (reversed-Z: NDC z = 1;
    // see the density_pc note above).
    {
        const float* m = matrices.clip_to_world.m;
        float w = m[14] + m[15];
        if (std::abs(w) > 1e-9f) {
            scatter_pc.camera_pos[0] = (m[2]  + m[3])  / w;
            scatter_pc.camera_pos[1] = (m[6]  + m[7])  / w;
            scatter_pc.camera_pos[2] = (m[10] + m[11]) / w;
        }
    }
    scatter_pc.frame_index = frame_index_;
    scatter_pc.sun_dir[0] = sun_direction_[0];
    scatter_pc.sun_dir[1] = sun_direction_[1];
    scatter_pc.sun_dir[2] = sun_direction_[2];
    scatter_pc.sun_intensity = sun_intensity_;
    scatter_pc.sun_color[0] = sun_color_[0];
    scatter_pc.sun_color[1] = sun_color_[1];
    scatter_pc.sun_color[2] = sun_color_[2];
    scatter_pc.phase_g = phase_g_;
    scatter_pc.sky_color[0] = sky_color_[0];
    scatter_pc.sky_color[1] = sky_color_[1];
    scatter_pc.sky_color[2] = sky_color_[2];
    scatter_pc.temporal_blend = temporal_blend_;
    scatter_pc.history_valid = has_prev_matrices_ ? 1u : 0u;
    last_scatter_history_was_valid_ = scatter_pc.history_valid != 0;
    // Reversed-ZO recovery identities — see the density_pc note above.
    scatter_pc.camera_near = matrices.view_to_clip.m[11] /
                             (matrices.view_to_clip.m[10] + 1.0f);
    scatter_pc.camera_far = matrices.view_to_clip.m[11] /
                            matrices.view_to_clip.m[10];
    scatter_pc.pad2 = 0.0f;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scatter_pipeline_);
    const VkDescriptorSet scatter_sets[] = {
                                             active_bundle_.scatter_sets[frame_slot][current],
                                             environment_descriptor_set_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            scatter_pipeline_layout_, 0, 2,
                            scatter_sets, 0, nullptr);
    vkCmdPushConstants(cmd, scatter_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(ScatterConstants), &scatter_pc);

    // Dispatch: same workgroup layout as density (4x4x1 threads, each iterates
    // all 128 depth slices).
    vkCmdDispatch(cmd, density_gx, density_gy, 1);

    // ---------------------------------------------------------------
    // Barrier: vol_scatter_[current] GENERAL -> SHADER_READ_ONLY
    // ---------------------------------------------------------------
    matter::record_image_transition(
        cmd, active_bundle_.scatter[current],
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    // ---------------------------------------------------------------
    // Pass 3: Integrate
    // ---------------------------------------------------------------

    // Transition vol_integrated_ to GENERAL for storage write.
    matter::record_image_transition(
        cmd, active_bundle_.integrated, VK_IMAGE_LAYOUT_GENERAL,
        active_bundle_.integrated.layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        active_bundle_.integrated.layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VkAccessFlags2(0)
            : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, integrate_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            integrate_pipeline_layout_, 0, 1,
                            &active_bundle_.integrate_sets[current], 0, nullptr);

    // Dispatch: ceil(160/8) x ceil(90/8) x 1 = 20 x 12 x 1.
    const uint32_t integrate_gx = (active_bundle_.dimensions.width + 7) / 8;
    const uint32_t integrate_gy = (active_bundle_.dimensions.height + 7) / 8;
    last_dispatch_grid_ = {density_gx, density_gy, integrate_gx, integrate_gy};
    vkCmdDispatch(cmd, integrate_gx, integrate_gy, 1);

    // ---------------------------------------------------------------
    // Barrier: vol_integrated_ GENERAL -> SHADER_READ_ONLY
    // (ready for composite fragment shader sampling)
    // ---------------------------------------------------------------
    matter::record_image_transition(
        cmd, active_bundle_.integrated,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    // Flip ping-pong, advance frame counter, store matrices for next frame.
    active_bundle_.ping_index ^= 1;
    ++frame_index_;
    prev_world_to_clip_ = matrices.world_to_clip;
    has_prev_matrices_ = true;
    prepared_frame_slot_ = UINT32_MAX;

    return true;
}

uint32_t VkVolumetrics::grid_rgba16f_volume_count_for_test() const {
    const auto is_grid_rgba16f = [&](const matter::VkImageResource& image) {
        const auto& d = active_bundle_.dimensions;
        return image.image != VK_NULL_HANDLE &&
               image.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
               image.extent.width == d.width && image.extent.height == d.height &&
               image.extent.depth == d.depth;
    };
    return static_cast<uint32_t>(is_grid_rgba16f(active_bundle_.media)) +
           static_cast<uint32_t>(is_grid_rgba16f(active_bundle_.scatter[0])) +
           static_cast<uint32_t>(is_grid_rgba16f(active_bundle_.scatter[1])) +
           static_cast<uint32_t>(is_grid_rgba16f(active_bundle_.integrated));
}

bool VkVolumetrics::cloud_density_allocated_for_test() const {
    return active_bundle_.enhanced_clouds &&
           active_bundle_.cloud_density.image != VK_NULL_HANDLE &&
           active_bundle_.cloud_density.format == VK_FORMAT_R16_SFLOAT;
}

matter::FroxelGridDimensions
VkVolumetrics::cloud_density_dimensions_for_test() const {
    const matter::VkImageResource& image = active_bundle_.enhanced_clouds
        ? active_bundle_.cloud_density : cloud_density_dummy_;
    return {image.extent.width, image.extent.height, image.extent.depth};
}

uint64_t VkVolumetrics::grid_bytes_for_test() const {
    return matter::estimate_froxel_bytes(active_bundle_.dimensions,
                                         active_bundle_.enhanced_clouds);
}

bool VkVolumetrics::readback_density_voxel_for_test(
    uint32_t x, uint32_t y, uint32_t z, matter::Float4& media,
    float& cloud_density, std::string& error) {
    if (!vulkan_ || !cloud_density_allocated_for_test()) {
        error = "enhanced cloud-density image is unavailable";
        return false;
    }
    const auto& d = active_bundle_.dimensions;
    if (x >= d.width || y >= d.height || z >= d.depth) {
        error = "cloud-density readback coordinate is outside the froxel grid";
        return false;
    }
    matter::VkBufferResource readback;
    if (!matter::create_buffer(*vulkan_, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               readback, error) || !matter::map_buffer(readback, error)) {
        return false;
    }
    struct ReadbackRequest {
        matter::VkImageResource* media;
        matter::VkImageResource* cloud_density;
        VkBuffer destination;
        uint32_t x, y, z;
    } request{&active_bundle_.media, &active_bundle_.cloud_density,
              readback.buffer, x, y, z};
    const auto copy = [](VkCommandBuffer cmd, void* data) {
        const auto& request = *static_cast<ReadbackRequest*>(data);
        const auto transition_to_copy = [&](matter::VkImageResource& image) {
            matter::record_image_transition(
                cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT);
        };
        transition_to_copy(*request.media);
        transition_to_copy(*request.cloud_density);
        VkBufferImageCopy media_region{};
        media_region.bufferOffset = 0;
        media_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        media_region.imageOffset = {static_cast<int32_t>(request.x),
                                    static_cast<int32_t>(request.y),
                                    static_cast<int32_t>(request.z)};
        media_region.imageExtent = {1, 1, 1};
        VkBufferImageCopy cloud_region = media_region;
        cloud_region.bufferOffset = 8;
        vkCmdCopyImageToBuffer(cmd, request.media->image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               request.destination, 1, &media_region);
        vkCmdCopyImageToBuffer(cmd, request.cloud_density->image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               request.destination, 1, &cloud_region);
        const auto restore_sampled = [&](matter::VkImageResource& image) {
            matter::record_image_transition(
                cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        };
        restore_sampled(*request.media);
        restore_sampled(*request.cloud_density);
    };
    if (!matter::submit_immediate(*vulkan_, copy, &request, error,
                                  matter::ImmediateSubmitPhase::staging_readback,
                                  {active_bundle_.media.lifetime,
                                   active_bundle_.cloud_density.lifetime,
                                   readback.lifetime}) ||
        !matter::invalidate_buffer(readback, 0, 16, error)) return false;
    const auto* values = static_cast<const uint16_t*>(readback.mapped);
    media = {half_to_float(values[0]), half_to_float(values[1]),
             half_to_float(values[2]), half_to_float(values[3])};
    cloud_density = half_to_float(values[4]);
    return true;
}

// ---------------------------------------------------------------------------
// destroy
// ---------------------------------------------------------------------------

void VkVolumetrics::destroy() {
    if (device_ == VK_NULL_HANDLE) return;

    // Pipelines. One density pipeline per cloud-layer specialization.
    for (auto& family : density_pipelines_) for (VkPipeline& p : family) {
        if (p != VK_NULL_HANDLE) vkDestroyPipeline(device_, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    cloud_density_dummy_.reset();
    if (scatter_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, scatter_pipeline_, nullptr);
    if (integrate_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, integrate_pipeline_, nullptr);

    // Pipeline layouts.
    if (density_pipeline_layout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, density_pipeline_layout_, nullptr);
    if (scatter_pipeline_layout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, scatter_pipeline_layout_, nullptr);
    if (integrate_pipeline_layout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, integrate_pipeline_layout_, nullptr);

    // Descriptor pools (implicitly free sets).

    // Descriptor set layouts.
    if (density_set_layout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, density_set_layout_, nullptr);
    if (scatter_set_layout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, scatter_set_layout_, nullptr);
    if (integrate_set_layout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, integrate_set_layout_, nullptr);

    // Samplers.
    if (linear_clamp_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device_, linear_clamp_sampler_, nullptr);
    if (linear_border_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device_, linear_border_sampler_, nullptr);
    if (linear_repeat_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device_, linear_repeat_sampler_, nullptr);

    // Resources (VkImageResource/VkBufferResource destructors handle cleanup
    // via their shared_ptr lifetime, but we reset them here for clarity).
    destroy_froxel_bundle(active_bundle_);
    for (RetiredBundle& retired : retired_bundles_) destroy_froxel_bundle(retired.bundle);
    retired_bundles_.clear();
    noise_texture_.reset();
    emitter_ssbo_.reset();
    cloud_ssbo_.reset();

    // Zero out all handles.
    scatter_pipeline_ = VK_NULL_HANDLE;
    integrate_pipeline_ = VK_NULL_HANDLE;
    density_pipeline_layout_ = VK_NULL_HANDLE;
    scatter_pipeline_layout_ = VK_NULL_HANDLE;
    integrate_pipeline_layout_ = VK_NULL_HANDLE;
    density_set_layout_ = VK_NULL_HANDLE;
    scatter_set_layout_ = VK_NULL_HANDLE;
    environment_set_layout_ = VK_NULL_HANDLE;
    environment_descriptor_set_ = VK_NULL_HANDLE;
    integrate_set_layout_ = VK_NULL_HANDLE;
    linear_clamp_sampler_ = VK_NULL_HANDLE;
    linear_border_sampler_ = VK_NULL_HANDLE;
    linear_repeat_sampler_ = VK_NULL_HANDLE;

    device_ = VK_NULL_HANDLE;
    vulkan_ = nullptr;
    initialized_ = false;
    ping_index_ = 0;
    frame_index_ = 0;
}

}  // namespace viewer
