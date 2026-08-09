#include "vk_atmosphere.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "matter/vulkan_device.h"
#include "../../shaders_gen/embedded_spirv.h"
#include "vk_resources.h"

namespace viewer {
namespace {

constexpr float kPi = 3.14159265358979323846f;

bool same_settings(const matter::AtmosphereSettings& a,
                   const matter::AtmosphereSettings& b) {
    return a.sea_level_y == b.sea_level_y &&
           a.rayleigh_scale == b.rayleigh_scale && a.mie_scale == b.mie_scale &&
           a.mie_anisotropy == b.mie_anisotropy && a.ozone_scale == b.ozone_scale &&
           a.ground_albedo == b.ground_albedo;
}

float dot(const matter::Float3& a, const matter::Float3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

bool normalize(matter::Float3& value) {
    const float length2 = dot(value, value);
    if (!std::isfinite(length2) || length2 <= 0.0f) return false;
    const float inv_length = 1.0f / std::sqrt(length2);
    value.x *= inv_length;
    value.y *= inv_length;
    value.z *= inv_length;
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
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

bool vk_ok(VkResult result, const char* operation, std::string& error) {
    if (result == VK_SUCCESS) return true;
    error = std::string(operation) + " failed with VkResult " +
            std::to_string(static_cast<int>(result));
    return false;
}

bool create_shader_module(VkDevice device, const char* name,
                          VkShaderModule& output, std::string& error) {
    const matter::EmbeddedSpirvView spirv = matter::find_spirv(name);
    if (!spirv.words || spirv.word_count == 0) {
        error = std::string("embedded SPIR-V not found: ") + name;
        return false;
    }
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = spirv.word_count * sizeof(uint32_t);
    info.pCode = spirv.words;
    return vk_ok(vkCreateShaderModule(device, &info, nullptr, &output),
                 "vkCreateShaderModule(atmosphere)", error);
}

VkDescriptorSetLayoutBinding binding(uint32_t index, VkDescriptorType type) {
    VkDescriptorSetLayoutBinding result{};
    result.binding = index;
    result.descriptorType = type;
    result.descriptorCount = 1;
    result.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    return result;
}

}  // namespace

bool VkAtmosphere::init(matter::VulkanDevice& vulkan, std::string& error) {
    destroy();
    vulkan_ = &vulkan;
    if (!create_images(vulkan, error) || !initialize_emergency(vulkan, error) ||
        !create_pipelines(vulkan, error)) {
        destroy();
        return false;
    }
    initialized_ = true;
    return true;
}

void VkAtmosphere::request_settings(const matter::AtmosphereSettings& settings) {
    requested_settings_ = matter::sanitize_atmosphere(settings);
}

bool VkAtmosphere::create_images(matter::VulkanDevice& vulkan, std::string& error) {
    const auto create = [&](matter::VkImageResource& image, VkExtent3D extent) {
        return matter::create_image(vulkan, VK_IMAGE_TYPE_2D,
            VK_FORMAT_R16G16B16A16_SFLOAT, extent,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image, error);
    };
    const VkExtent3D trans{kTransmittanceWidth, kTransmittanceHeight, 1};
    const VkExtent3D multi{kMultiscatterSize, kMultiscatterSize, 1};
    const VkExtent3D sky{kSkyViewWidth, kSkyViewHeight, 1};
    const VkExtent3D irradiance{kIrradianceSize, kIrradianceSize, 1};
    return create(emergency_transmittance_, trans) && create(emergency_multiscatter_, multi) &&
           create(emergency_sky_view_, sky) && create(emergency_irradiance_sh_, irradiance) &&
           create(transmittance_, trans) && create(multiscatter_, multi) &&
           create(sky_view_, sky) && create(irradiance_sh_, irradiance);
}

bool VkAtmosphere::initialize_emergency(matter::VulkanDevice& vulkan,
                                        std::string& error) {
    struct ClearRequest { std::array<matter::VkImageResource*, 4> images; };
    ClearRequest request{{&emergency_transmittance_, &emergency_multiscatter_,
                          &emergency_sky_view_, &emergency_irradiance_sh_}};
    const auto clear = [](VkCommandBuffer command_buffer, void* data) {
        auto& request = *static_cast<ClearRequest*>(data);
        for (uint32_t index = 0; index < request.images.size(); ++index) {
            matter::VkImageResource& image = *request.images[index];
            matter::record_image_transition(command_buffer, image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT);
            VkClearColorValue color{};
            color.float32[0] = index == 0 ? 1.0f : 0.0f;
            color.float32[1] = index == 0 ? 1.0f : 0.0f;
            color.float32[2] = index == 0 ? 1.0f : 0.0f;
            color.float32[3] = 1.0f;
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(command_buffer, image.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
            matter::record_image_transition(command_buffer, image,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                    VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        }
    };
    return matter::submit_immediate(vulkan, clear, &request, error,
        matter::ImmediateSubmitPhase::staging_upload,
        {emergency_transmittance_.lifetime, emergency_multiscatter_.lifetime,
         emergency_sky_view_.lifetime, emergency_irradiance_sh_.lifetime});
}

bool VkAtmosphere::create_pipelines(matter::VulkanDevice& vulkan, std::string& error) {
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!vk_ok(vkCreateSampler(vulkan.device(), &sampler_info, nullptr, &linear_sampler_),
               "vkCreateSampler(atmosphere)", error)) return false;

    const auto create = [&](ComputePass& pass, const char* shader_name,
                            std::vector<VkDescriptorSetLayoutBinding> bindings) {
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = static_cast<uint32_t>(bindings.size());
        layout.pBindings = bindings.data();
        if (!vk_ok(vkCreateDescriptorSetLayout(vulkan.device(), &layout, nullptr,
                                               &pass.descriptor_layout),
                   "vkCreateDescriptorSetLayout(atmosphere)", error)) return false;
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 48};
        VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_layout.setLayoutCount = 1;
        pipeline_layout.pSetLayouts = &pass.descriptor_layout;
        pipeline_layout.pushConstantRangeCount = 1;
        pipeline_layout.pPushConstantRanges = &push;
        if (!vk_ok(vkCreatePipelineLayout(vulkan.device(), &pipeline_layout, nullptr,
                                          &pass.pipeline_layout),
                   "vkCreatePipelineLayout(atmosphere)", error)) return false;
        VkShaderModule shader = VK_NULL_HANDLE;
        if (!create_shader_module(vulkan.device(), shader_name, shader, error)) return false;
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.stage = stage;
        pipeline.layout = pass.pipeline_layout;
        const VkResult pipeline_result = vkCreateComputePipelines(
            vulkan.device(), VK_NULL_HANDLE, 1, &pipeline, nullptr, &pass.pipeline);
        vkDestroyShaderModule(vulkan.device(), shader, nullptr);
        if (!vk_ok(pipeline_result, "vkCreateComputePipelines(atmosphere)", error)) return false;
        std::array<VkDescriptorPoolSize, 2> sizes{{
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
        pool.pPoolSizes = sizes.data();
        if (!vk_ok(vkCreateDescriptorPool(vulkan.device(), &pool, nullptr, &pass.descriptor_pool),
                   "vkCreateDescriptorPool(atmosphere)", error)) return false;
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pass.descriptor_pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &pass.descriptor_layout;
        return vk_ok(vkAllocateDescriptorSets(vulkan.device(), &allocate, &pass.descriptor_set),
                     "vkAllocateDescriptorSets(atmosphere)", error);
    };
    if (!create(transmittance_pass_, "atmosphere_transmittance.comp.spv",
                {binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)}) ||
        !create(multiscatter_pass_, "atmosphere_multiscatter.comp.spv",
                {binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                 binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)}) ||
        !create(sky_view_pass_, "atmosphere_sky_view.comp.spv",
                {binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                 binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                 binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)}) ||
        !create(irradiance_pass_, "atmosphere_irradiance.comp.spv",
                {binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                 binding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)})) return false;

    const auto sampled = [&](matter::VkImageResource& image) {
        VkDescriptorImageInfo info{linear_sampler_, image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        return info;
    };
    const auto storage = [](matter::VkImageResource& image) {
        VkDescriptorImageInfo info{VK_NULL_HANDLE, image.view, VK_IMAGE_LAYOUT_GENERAL};
        return info;
    };
    const VkDescriptorImageInfo trans_store = storage(transmittance_);
    const VkDescriptorImageInfo trans_sample = sampled(transmittance_);
    const VkDescriptorImageInfo multi_store = storage(multiscatter_);
    const VkDescriptorImageInfo multi_sample = sampled(multiscatter_);
    const VkDescriptorImageInfo sky_store = storage(sky_view_);
    const VkDescriptorImageInfo sky_sample = sampled(sky_view_);
    const VkDescriptorImageInfo irradiance_store = storage(irradiance_sh_);
    std::array<VkWriteDescriptorSet, 8> writes{};
    const auto write = [&](uint32_t index, ComputePass& pass, uint32_t slot,
                           VkDescriptorType type, const VkDescriptorImageInfo* info) {
        writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[index].dstSet = pass.descriptor_set; writes[index].dstBinding = slot;
        writes[index].descriptorCount = 1; writes[index].descriptorType = type;
        writes[index].pImageInfo = info;
    };
    write(0, transmittance_pass_, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &trans_store);
    write(1, multiscatter_pass_, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &trans_sample);
    write(2, multiscatter_pass_, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &multi_store);
    write(3, sky_view_pass_, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &trans_sample);
    write(4, sky_view_pass_, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &multi_sample);
    write(5, sky_view_pass_, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &sky_store);
    write(6, irradiance_pass_, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sky_sample);
    write(7, irradiance_pass_, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &irradiance_store);
    vkUpdateDescriptorSets(vulkan.device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return true;
}

bool VkAtmosphere::coefficient_change_pending() const {
    return !has_committed_settings_ || !same_settings(requested_settings_, committed_settings_);
}

bool VkAtmosphere::view_change_pending(float camera_world_y,
                                       const matter::Float3& to_sun) const {
    if (!has_committed_settings_) return true;
    return std::fabs(camera_world_y - committed_camera_world_y_) > 10.0f ||
           dot(to_sun, committed_to_sun_) < 0.999999f;
}

bool VkAtmosphere::record_dispatches(VkCommandBuffer command_buffer,
                                     bool coefficients_dirty, float camera_world_y,
                                     const matter::Float3& to_sun,
                                     std::string& error) {
    if (command_buffer == VK_NULL_HANDLE) {
        error = "VkAtmosphere::record requires a command buffer";
        return false;
    }
    struct PushConstants {
        float settings0[4]; // rayleigh, mie, anisotropy, ground albedo
        float settings1[4]; // sea level, ozone, observer world y, pad
        float sun[4];
    } push{{requested_settings_.rayleigh_scale, requested_settings_.mie_scale,
            requested_settings_.mie_anisotropy, requested_settings_.ground_albedo},
           {requested_settings_.sea_level_y, requested_settings_.ozone_scale,
            camera_world_y, 0.0f}, {to_sun.x, to_sun.y, to_sun.z, 0.0f}};
    static_assert(sizeof(PushConstants) == 48, "atmosphere GLSL push ABI");
    const auto bind_dispatch = [&](ComputePass& pass, uint32_t x, uint32_t y) {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipeline);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pass.pipeline_layout, 0, 1, &pass.descriptor_set, 0, nullptr);
        vkCmdPushConstants(command_buffer, pass.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);
        vkCmdDispatch(command_buffer, x, y, 1);
    };
    if (coefficients_dirty) {
    matter::record_image_transition(command_buffer, transmittance_, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    bind_dispatch(transmittance_pass_, kTransmittanceWidth / 8, kTransmittanceHeight / 8);
    matter::record_image_transition(command_buffer, transmittance_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    matter::record_image_transition(command_buffer, multiscatter_, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    bind_dispatch(multiscatter_pass_, kMultiscatterSize / 8, kMultiscatterSize / 8);
    matter::record_image_transition(command_buffer, multiscatter_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    }
    matter::record_image_transition(command_buffer, sky_view_, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    bind_dispatch(sky_view_pass_, (kSkyViewWidth + 7) / 8, (kSkyViewHeight + 7) / 8);
    matter::record_image_transition(command_buffer, sky_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    matter::record_image_transition(command_buffer, irradiance_sh_, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    bind_dispatch(irradiance_pass_, 3, 3);
    matter::record_image_transition(command_buffer, irradiance_sh_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    return true;
}

bool VkAtmosphere::record(VkCommandBuffer command_buffer, float camera_world_y,
                          const matter::Float3& to_sun_input, std::string& error) {
    generated_this_frame_ = false;
    if (!initialized_) { error = "VkAtmosphere is not initialized"; return false; }
    if (!std::isfinite(camera_world_y)) { error = "atmosphere camera altitude is non-finite"; return false; }
    matter::Float3 to_sun = to_sun_input;
    if (!normalize(to_sun)) { error = "atmosphere to_sun is invalid"; return false; }
    const bool coefficients_dirty = coefficient_change_pending();
    const bool view_dirty = coefficients_dirty || view_change_pending(camera_world_y, to_sun);
    if (!view_dirty) return true;
    // The physical set is only selected after all four images' copies and
    // post-copy barriers have been recorded successfully.
    if (!record_dispatches(command_buffer, coefficients_dirty, camera_world_y, to_sun, error)) return false;
    committed_settings_ = requested_settings_;
    committed_camera_world_y_ = camera_world_y;
    committed_to_sun_ = to_sun;
    has_committed_settings_ = true;
    physical_selected_ = true;
    ++generation_serial_;
    generated_this_frame_ = true;
    return true;
}

const matter::VkImageResource& VkAtmosphere::sky_view() const {
    return physical_selected_ ? sky_view_ : emergency_sky_view_;
}

const matter::VkImageResource& VkAtmosphere::irradiance_sh() const {
    return physical_selected_ ? irradiance_sh_ : emergency_irradiance_sh_;
}

matter::Float3 VkAtmosphere::direct_sun_transmittance(
    float camera_world_y, const matter::Float3& to_sun_input) const {
    matter::Float3 to_sun = to_sun_input;
    if (!has_committed_settings_ || !std::isfinite(camera_world_y) || !normalize(to_sun)) return {};
    // Task 2's public helper accepts the engine's incoming sun direction.
    // VkAtmosphere instead exposes observer-to-sun consistently, so this is
    // the single, local conversion; the helper performs the complete spherical
    // planet-occlusion test for every observer altitude.
    return matter::atmosphere_direct_sun_transmittance(
        committed_settings_, camera_world_y, {-to_sun.x, -to_sun.y, -to_sun.z}, 40);
}

bool VkAtmosphere::readback_transmittance_for_test(
    matter::VulkanDevice& vulkan, uint32_t x, uint32_t y, matter::Float3& out,
    std::string& error) const {
    if (!physical_selected_ || x >= kTransmittanceWidth || y >= kTransmittanceHeight) {
        error = "atmosphere transmittance readback is unavailable";
        return false;
    }
    matter::VkBufferResource readback;
    if (!matter::create_buffer(vulkan, 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               readback, error) || !matter::map_buffer(readback, error)) return false;
    struct CopyRequest { matter::VkImageResource* image; VkBuffer buffer; uint32_t x; uint32_t y; };
    CopyRequest request{const_cast<matter::VkImageResource*>(&transmittance_), readback.buffer, x, y};
    const auto copy = [](VkCommandBuffer command_buffer, void* data) {
        auto& request = *static_cast<CopyRequest*>(data);
        matter::record_image_transition(command_buffer, *request.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {static_cast<int32_t>(request.x), static_cast<int32_t>(request.y), 0};
        region.imageExtent = {1, 1, 1};
        vkCmdCopyImageToBuffer(command_buffer, request.image->image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, request.buffer, 1, &region);
        matter::record_image_transition(command_buffer, *request.image,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    };
    if (!matter::submit_immediate(vulkan, copy, &request, error,
            matter::ImmediateSubmitPhase::staging_readback,
            {transmittance_.lifetime, readback.lifetime})) return false;
    if (!matter::invalidate_buffer(readback, 0, 8, error)) return false;
    const auto* values = static_cast<const uint16_t*>(readback.mapped);
    out = {half_to_float(values[0]), half_to_float(values[1]), half_to_float(values[2])};
    return true;
}

void VkAtmosphere::destroy() {
    const VkDevice device = vulkan_ ? vulkan_->device() : VK_NULL_HANDLE;
    const auto destroy_pass = [&](ComputePass& pass) {
        if (device != VK_NULL_HANDLE && pass.descriptor_pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device, pass.descriptor_pool, nullptr);
        if (device != VK_NULL_HANDLE && pass.pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(device, pass.pipeline, nullptr);
        if (device != VK_NULL_HANDLE && pass.pipeline_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, pass.pipeline_layout, nullptr);
        if (device != VK_NULL_HANDLE && pass.descriptor_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, pass.descriptor_layout, nullptr);
        pass = {};
    };
    destroy_pass(irradiance_pass_);
    destroy_pass(sky_view_pass_);
    destroy_pass(multiscatter_pass_);
    destroy_pass(transmittance_pass_);
    if (device != VK_NULL_HANDLE && linear_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device, linear_sampler_, nullptr);
    linear_sampler_ = VK_NULL_HANDLE;
    emergency_irradiance_sh_.reset(); emergency_sky_view_.reset(); emergency_multiscatter_.reset(); emergency_transmittance_.reset();
    irradiance_sh_.reset(); sky_view_.reset(); multiscatter_.reset(); transmittance_.reset();
    vulkan_ = nullptr;
    initialized_ = false;
    has_committed_settings_ = false;
    physical_selected_ = false;
    generated_this_frame_ = false;
    generation_serial_ = 0;
}

}  // namespace viewer
