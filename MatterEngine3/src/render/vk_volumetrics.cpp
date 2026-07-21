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

bool VkVolumetrics::init(matter::VulkanDevice& vulkan, std::string& error) {
    device_ = vulkan.device();

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
    if (!create_volume_images(vulkan, error)) return false;
    if (!create_emitter_buffer(vulkan, error)) return false;
    if (!create_samplers(vulkan, error)) return false;
    if (!create_density_pipeline(vulkan, error)) return false;
    if (!create_scatter_pipeline(vulkan, error)) return false;
    if (!create_integrate_pipeline(vulkan, error)) return false;

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

bool VkVolumetrics::create_volume_images(matter::VulkanDevice& vulkan,
                                          std::string& error) {
    const VkExtent3D vol_extent{kVolGridW, kVolGridH, kVolGridD};
    const VkImageUsageFlags sampled_storage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

    // vol_media_ (density pass output, scatter pass input).
    if (!matter::create_image(vulkan, VK_IMAGE_TYPE_3D, VK_FORMAT_R16G16B16A16_SFLOAT,
                              vol_extent, sampled_storage,
                              VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              vol_media_, error)) {
        return false;
    }

    // vol_scatter_[0..1] (ping-pong temporal).
    for (int i = 0; i < 2; ++i) {
        if (!matter::create_image(vulkan, VK_IMAGE_TYPE_3D,
                                  VK_FORMAT_R16G16B16A16_SFLOAT, vol_extent,
                                  sampled_storage, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                  vol_scatter_[i], error)) {
            return false;
        }
    }

    // vol_integrated_ (integration output, composite shader input).
    if (!matter::create_image(vulkan, VK_IMAGE_TYPE_3D, VK_FORMAT_R16G16B16A16_SFLOAT,
                              vol_extent, sampled_storage,
                              VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              vol_integrated_, error)) {
        return false;
    }
    return true;
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
    const VkDescriptorSetLayoutBinding bindings[] = {
        make_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                     VK_SHADER_STAGE_COMPUTE_BIT),
        make_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     VK_SHADER_STAGE_COMPUTE_BIT),
        make_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     VK_SHADER_STAGE_COMPUTE_BIT),
    };

    VkDescriptorSetLayoutCreateInfo set_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 3;
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

    // Shader module.
    VkShaderModule shader = VK_NULL_HANDLE;
    if (!create_shader_module_from_spirv(device_, "vol_density.comp.spv",
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
    create.layout = density_pipeline_layout_;
    result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &create,
                                      nullptr, &density_pipeline_);
    vkDestroyShaderModule(device_, shader, nullptr);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateComputePipelines(density)", result, error);

    // Descriptor pool + set.
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
    };
    VkDescriptorPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = pool_sizes;
    result = vkCreateDescriptorPool(device_, &pool_info, nullptr, &density_pool_);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateDescriptorPool(density)", result, error);

    VkDescriptorSetAllocateInfo alloc{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = density_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &density_set_layout_;
    result = vkAllocateDescriptorSets(device_, &alloc, &density_set_);
    if (result != VK_SUCCESS)
        return vk_fail("vkAllocateDescriptorSets(density)", result, error);

    // Write descriptors for noise texture (binding 1) and emitter SSBO (binding 2).
    // vol_media (binding 0) is written each frame in record() since its image
    // view is stable but its layout transitions.
    //
    // Actually, all bindings are stable -- write them all now.  The storage image
    // descriptor works with any layout at update time; the layout in the descriptor
    // is what we promise to use at dispatch time (GENERAL for storage images).

    VkDescriptorImageInfo media_info{};
    media_info.imageView = vol_media_.view;
    media_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo noise_info{};
    noise_info.sampler = linear_repeat_sampler_;
    noise_info.imageView = noise_texture_.view;
    noise_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorBufferInfo emitter_info{};
    emitter_info.buffer = emitter_ssbo_.buffer;
    emitter_info.offset = 0;
    emitter_info.range = emitter_ssbo_.size;

    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = density_set_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &media_info;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = density_set_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &noise_info;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = density_set_;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &emitter_info;

    vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
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
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &scatter_set_layout_;
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

    // Descriptor pool: 2 sets (one per ping-pong state).
    // Each set has: 3 combined image samplers + 1 storage image + 1 AS.
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6},   // 3 per set x 2
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},             // 1 per set x 2
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2},// 1 per set x 2
    };
    VkDescriptorPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 2;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = pool_sizes;
    result = vkCreateDescriptorPool(device_, &pool_info, nullptr, &scatter_pool_);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateDescriptorPool(scatter)", result, error);

    // Allocate 2 descriptor sets.
    VkDescriptorSetLayout layouts[2] = {scatter_set_layout_, scatter_set_layout_};
    VkDescriptorSetAllocateInfo alloc{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = scatter_pool_;
    alloc.descriptorSetCount = 2;
    alloc.pSetLayouts = layouts;
    result = vkAllocateDescriptorSets(device_, &alloc, scatter_sets_);
    if (result != VK_SUCCESS)
        return vk_fail("vkAllocateDescriptorSets(scatter)", result, error);

    // Write the static portions of both scatter descriptor sets.
    // For set i:
    //   binding 0 = vol_media (combined image sampler, always the same)
    //   binding 1 = vol_scatter[i] as storage image (write target)
    //   binding 2 = vol_scatter[1-i] as combined image sampler (history)
    //   binding 3 = depth texture -- written per-frame in record()
    //   binding 4 = TLAS -- written per-frame in record()
    for (int i = 0; i < 2; ++i) {
        VkDescriptorImageInfo media_info{};
        media_info.sampler = linear_clamp_sampler_;
        media_info.imageView = vol_media_.view;
        media_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo write_info{};
        write_info.imageView = vol_scatter_[i].view;
        write_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo history_info{};
        history_info.sampler = linear_clamp_sampler_;
        history_info.imageView = vol_scatter_[1 - i].view;
        history_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[3]{};
        // Binding 0: vol_media as sampled.
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = scatter_sets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &media_info;

        // Binding 1: vol_scatter[i] as storage image (write).
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = scatter_sets_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &write_info;

        // Binding 2: vol_scatter[1-i] as sampled (history).
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = scatter_sets_[i];
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &history_info;

        vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
    }
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

    // Descriptor pool: 2 sets (one per ping-pong input).
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
    };
    VkDescriptorPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 2;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    result = vkCreateDescriptorPool(device_, &pool_info, nullptr,
                                    &integrate_pool_);
    if (result != VK_SUCCESS)
        return vk_fail("vkCreateDescriptorPool(integrate)", result, error);

    VkDescriptorSetLayout int_layouts[2] = {integrate_set_layout_,
                                             integrate_set_layout_};
    VkDescriptorSetAllocateInfo alloc{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = integrate_pool_;
    alloc.descriptorSetCount = 2;
    alloc.pSetLayouts = int_layouts;
    result = vkAllocateDescriptorSets(device_, &alloc, integrate_sets_);
    if (result != VK_SUCCESS)
        return vk_fail("vkAllocateDescriptorSets(integrate)", result, error);

    // Write descriptors for both integrate sets.
    // Set i: binding 0 = vol_scatter[i] as sampled, binding 1 = vol_integrated.
    for (int i = 0; i < 2; ++i) {
        VkDescriptorImageInfo scatter_info{};
        scatter_info.sampler = linear_clamp_sampler_;
        scatter_info.imageView = vol_scatter_[i].view;
        scatter_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo integrated_info{};
        integrated_info.imageView = vol_integrated_.view;
        integrated_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = integrate_sets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &scatter_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = integrate_sets_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &integrated_info;

        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    }
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
    fog_density_mul_ = vol.fog_density_mul;
    fog_floor_offset_ = vol.fog_floor_offset;
    fog_falloff_mul_ = vol.fog_falloff_mul;
    for (int i = 0; i < 3; ++i) {
        fog_color_mul_[i] = vol.fog_color_mul[i];
        fog_wind_mul_[i] = vol.fog_wind_mul[i];
    }

    fog_density_ = fog.density;
    fog_floor_ = fog.floor;
    fog_falloff_ = fog.falloff;
    for (int i = 0; i < 3; ++i) {
        fog_color_[i] = fog.color[i];
        fog_wind_[i] = fog.wind[i];
    }
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
    (void)frame_slot;
    (void)error;  // recording currently cannot fail after init succeeds
    if (!initialized_) return true;
    if (!enabled_ || !ray_query_available_) return true;

    const uint32_t current = ping_index_;
    const uint32_t history = 1 - ping_index_;

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
        writes[0].dstSet = scatter_sets_[current];
        writes[0].dstBinding = 3;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &depth_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].pNext = &as_write;
        writes[1].dstSet = scatter_sets_[current];
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
        cmd, vol_media_, VK_IMAGE_LAYOUT_GENERAL,
        vol_media_.layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        vol_media_.layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VkAccessFlags2(0)
            : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    // Fill push constants.
    DensityConstants density_pc{};
    pack_mat4_column_major(density_pc.clip_to_world, matrices.clip_to_world);
    // Extract camera position: clip_to_world * (0,0,0,1) in row-major storage.
    {
        float w = matrices.clip_to_world.m[15];
        if (std::abs(w) > 1e-9f) {
            density_pc.camera_pos[0] = matrices.clip_to_world.m[3] / w;
            density_pc.camera_pos[1] = matrices.clip_to_world.m[7] / w;
            density_pc.camera_pos[2] = matrices.clip_to_world.m[11] / w;
        }
    }
    density_pc.frame_time = frame_time;
    density_pc.fog_density = fog_density_ * fog_density_mul_;
    density_pc.fog_floor = fog_floor_ + fog_floor_offset_;
    density_pc.fog_falloff = fog_falloff_ * fog_falloff_mul_;
    // Reversed-ZO projection: m[10] = n/(f-n), m[11] = f*n/(f-n), so the
    // recovery identities are m[11]/m[10] = far and m[11]/(m[10]+1) = near
    // (the standard-ZO identities with roles swapped).
    density_pc.camera_near = matrices.view_to_clip.m[11] /
                             (matrices.view_to_clip.m[10] + 1.0f);
    for (int i = 0; i < 3; ++i) {
        density_pc.fog_color[i] = fog_color_[i] * fog_color_mul_[i];
        density_pc.fog_wind[i] = fog_wind_[i] * fog_wind_mul_[i];
    }
    density_pc.camera_far = matrices.view_to_clip.m[11] /
                            matrices.view_to_clip.m[10];
    density_pc.pad2 = 0.0f;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, density_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            density_pipeline_layout_, 0, 1, &density_set_,
                            0, nullptr);
    vkCmdPushConstants(cmd, density_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(DensityConstants), &density_pc);

    // Dispatch: ceil(160/4) x ceil(90/4) x 1 = 40 x 23 x 1.
    const uint32_t density_gx = (kVolGridW + 3) / 4;
    const uint32_t density_gy = (kVolGridH + 3) / 4;
    vkCmdDispatch(cmd, density_gx, density_gy, 1);

    // ---------------------------------------------------------------
    // Barrier: vol_media_ GENERAL -> SHADER_READ_ONLY
    // ---------------------------------------------------------------
    matter::record_image_transition(
        cmd, vol_media_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    // ---------------------------------------------------------------
    // Pass 2: Scatter
    // ---------------------------------------------------------------

    // Transition vol_scatter_[current] to GENERAL for storage write.
    matter::record_image_transition(
        cmd, vol_scatter_[current], VK_IMAGE_LAYOUT_GENERAL,
        vol_scatter_[current].layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        vol_scatter_[current].layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VkAccessFlags2(0)
            : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    // Ensure history is readable (may still be UNDEFINED on first frame).
    if (vol_scatter_[history].layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        matter::record_image_transition(
            cmd, vol_scatter_[history],
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            vol_scatter_[history].layout == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            vol_scatter_[history].layout == VK_IMAGE_LAYOUT_UNDEFINED
                ? VkAccessFlags2(0)
                : VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }

    ScatterConstants scatter_pc{};
    pack_mat4_column_major(scatter_pc.clip_to_world, matrices.clip_to_world);
    pack_mat4_column_major(scatter_pc.prev_world_to_clip, prev_world_to_clip_);
    // Camera position from the current clip_to_world.
    {
        float w = matrices.clip_to_world.m[15];
        if (std::abs(w) > 1e-9f) {
            scatter_pc.camera_pos[0] = matrices.clip_to_world.m[3] / w;
            scatter_pc.camera_pos[1] = matrices.clip_to_world.m[7] / w;
            scatter_pc.camera_pos[2] = matrices.clip_to_world.m[11] / w;
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
    // Reversed-ZO recovery identities — see the density_pc note above.
    scatter_pc.camera_near = matrices.view_to_clip.m[11] /
                             (matrices.view_to_clip.m[10] + 1.0f);
    scatter_pc.camera_far = matrices.view_to_clip.m[11] /
                            matrices.view_to_clip.m[10];
    scatter_pc.pad2 = 0.0f;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scatter_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            scatter_pipeline_layout_, 0, 1,
                            &scatter_sets_[current], 0, nullptr);
    vkCmdPushConstants(cmd, scatter_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(ScatterConstants), &scatter_pc);

    // Dispatch: same workgroup layout as density (4x4x1 threads, each iterates
    // all 128 depth slices).
    vkCmdDispatch(cmd, density_gx, density_gy, 1);

    // ---------------------------------------------------------------
    // Barrier: vol_scatter_[current] GENERAL -> SHADER_READ_ONLY
    // ---------------------------------------------------------------
    matter::record_image_transition(
        cmd, vol_scatter_[current],
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
        cmd, vol_integrated_, VK_IMAGE_LAYOUT_GENERAL,
        vol_integrated_.layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        vol_integrated_.layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VkAccessFlags2(0)
            : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, integrate_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            integrate_pipeline_layout_, 0, 1,
                            &integrate_sets_[current], 0, nullptr);

    // Dispatch: ceil(160/8) x ceil(90/8) x 1 = 20 x 12 x 1.
    const uint32_t integrate_gx = (kVolGridW + 7) / 8;
    const uint32_t integrate_gy = (kVolGridH + 7) / 8;
    vkCmdDispatch(cmd, integrate_gx, integrate_gy, 1);

    // ---------------------------------------------------------------
    // Barrier: vol_integrated_ GENERAL -> SHADER_READ_ONLY
    // (ready for composite fragment shader sampling)
    // ---------------------------------------------------------------
    matter::record_image_transition(
        cmd, vol_integrated_,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    // Flip ping-pong, advance frame counter, store matrices for next frame.
    ping_index_ ^= 1;
    ++frame_index_;
    prev_world_to_clip_ = matrices.world_to_clip;
    has_prev_matrices_ = true;

    return true;
}

// ---------------------------------------------------------------------------
// destroy
// ---------------------------------------------------------------------------

void VkVolumetrics::destroy() {
    if (device_ == VK_NULL_HANDLE) return;

    // Pipelines.
    if (density_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, density_pipeline_, nullptr);
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
    if (density_pool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(device_, density_pool_, nullptr);
    if (scatter_pool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(device_, scatter_pool_, nullptr);
    if (integrate_pool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(device_, integrate_pool_, nullptr);

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
    vol_media_.reset();
    vol_scatter_[0].reset();
    vol_scatter_[1].reset();
    vol_integrated_.reset();
    noise_texture_.reset();
    emitter_ssbo_.reset();

    // Zero out all handles.
    density_pipeline_ = VK_NULL_HANDLE;
    scatter_pipeline_ = VK_NULL_HANDLE;
    integrate_pipeline_ = VK_NULL_HANDLE;
    density_pipeline_layout_ = VK_NULL_HANDLE;
    scatter_pipeline_layout_ = VK_NULL_HANDLE;
    integrate_pipeline_layout_ = VK_NULL_HANDLE;
    density_pool_ = VK_NULL_HANDLE;
    scatter_pool_ = VK_NULL_HANDLE;
    integrate_pool_ = VK_NULL_HANDLE;
    density_set_ = VK_NULL_HANDLE;
    scatter_sets_[0] = VK_NULL_HANDLE;
    scatter_sets_[1] = VK_NULL_HANDLE;
    integrate_sets_[0] = VK_NULL_HANDLE;
    integrate_sets_[1] = VK_NULL_HANDLE;
    density_set_layout_ = VK_NULL_HANDLE;
    scatter_set_layout_ = VK_NULL_HANDLE;
    integrate_set_layout_ = VK_NULL_HANDLE;
    linear_clamp_sampler_ = VK_NULL_HANDLE;
    linear_border_sampler_ = VK_NULL_HANDLE;
    linear_repeat_sampler_ = VK_NULL_HANDLE;

    device_ = VK_NULL_HANDLE;
    initialized_ = false;
    ping_index_ = 0;
    frame_index_ = 0;
}

}  // namespace viewer
