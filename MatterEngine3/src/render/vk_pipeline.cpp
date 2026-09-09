// MatterEngine3/src/render/vk_pipeline.cpp
//
// Implementation of the one-shot compute-pipeline helper declared in
// vk_pipeline.h. Three things worth knowing before editing:
//
//   OWNERSHIP. The Vulkan objects live in detail::VkComputePipelineAllocation,
//   a DeviceLifetimeControl (vk_device_internal.h). VkComputePipelineResource
//   holds the same handles for convenience but destroys them itself ONLY when
//   there is no allocation yet — the failure paths inside
//   create_compute_pipeline(), which return before the allocation is built.
//
//   SUBMISSION. dispatch_compute() goes through submit_immediate() with
//   ImmediateSubmitPhase::compute_dispatch: a private command buffer, a
//   submit, and a blocking wait, entirely outside the frame loop. The
//   keep-alive list it passes is what protects the pipeline and its buffers if
//   the device cannot prove that submission finished.
//
//   BARRIERS. record_dispatch() ends with a deliberately conservative pair of
//   memory barriers (shader-write -> all-commands, and shader-write -> host
//   read), so a caller can immediately either read the result back on the host
//   or use it in any later stage without adding its own synchronization.

#include "vk_pipeline.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <utility>

#include "matter/log.h"
#include "matter/vulkan_device.h"
#include "shaders_gen/embedded_spirv.h"
#include "vk_device_internal.h"
#include "vk_resources.h"

namespace matter {
namespace detail {

// The device-registered owner of a compute pipeline's Vulkan objects. Its
// release runs either from its own destructor (the resource was dropped) or
// from the device's teardown walk (the device went first), so
// release_device_objects() nulls every handle and is safe to run twice; and it
// destroys nothing at all when live_device() reports VK_NULL_HANDLE, which is
// how an intentionally leaked device avoids being touched. The descriptor SET
// is not destroyed here — it dies with its pool.
struct VkComputePipelineAllocation final : DeviceLifetimeControl {
    explicit VkComputePipelineAllocation(
        std::shared_ptr<DeviceAccessToken> device_access)
        : DeviceLifetimeControl(std::move(device_access)) {}

    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;

    ~VkComputePipelineAllocation() override { release_device_objects(); }

protected:
    void release_device_objects() noexcept override {
        const VkDevice device = live_device();
        if (device != VK_NULL_HANDLE && pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(device, pipeline, nullptr);
        if (device != VK_NULL_HANDLE && pipeline_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        if (device != VK_NULL_HANDLE && descriptor_pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        if (device != VK_NULL_HANDLE && descriptor_set_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
        descriptor_set_layout = VK_NULL_HANDLE;
        pipeline_layout = VK_NULL_HANDLE;
        pipeline = VK_NULL_HANDLE;
        descriptor_pool = VK_NULL_HANDLE;
    }
};

}  // namespace detail
namespace {

bool vk_error(const char* operation, VkResult result, std::string& error) {
    error = std::string(operation) + " failed with VkResult " +
            std::to_string(static_cast<int>(result));
    return false;
}

struct DispatchRecord {
    const VkComputePipelineResource* pipeline;
    uint32_t x;
    uint32_t y;
    uint32_t z;
};

// submit_immediate() recording callback: bind the pipeline (and its descriptor
// set, if the shader has one), dispatch, then insert two memory barriers that
// make every shader write visible to all later commands AND to a host read.
// That is heavier than most dispatches need — it exists so callers of
// dispatch_compute() never have to reason about synchronization at all.
// `user_data` is the DispatchRecord below, borrowed for the duration of the
// call only.
void record_dispatch(VkCommandBuffer command_buffer, void* user_data) {
    const auto& dispatch = *static_cast<const DispatchRecord*>(user_data);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      dispatch.pipeline->pipeline);
    if (dispatch.pipeline->descriptor_set != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                dispatch.pipeline->pipeline_layout, 0, 1,
                                &dispatch.pipeline->descriptor_set, 0, nullptr);
    }
    vkCmdDispatch(command_buffer, dispatch.x, dispatch.y, dispatch.z);

    VkMemoryBarrier2 barriers[2]{};
    barriers[0].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barriers[0].srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barriers[0].dstAccessMask =
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barriers[1].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barriers[1].srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 2;
    dependency.pMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

// The std430 storage-buffer layout the transform_probe.comp shader reads and
// writes: a packed mat4, the input vector, and the vector the shader produces.
// The static_asserts below pin every offset — if one of them fires, the CPU
// struct and the GLSL block have drifted and the probe would be comparing
// garbage rather than proving the packing.
struct alignas(16) TransformProbeData {
    viewer::GpuMat4 matrix;
    matter::Float4 input;
    matter::Float4 output;
};

static_assert(sizeof(viewer::GpuMat4) == 64, "GLSL mat4 must occupy 64 bytes");
static_assert(offsetof(TransformProbeData, input) == 64,
              "std430 probe input offset mismatch");
static_assert(offsetof(TransformProbeData, output) == 80,
              "std430 probe output offset mismatch");
static_assert(sizeof(TransformProbeData) == 96,
              "std430 transform probe size mismatch");

}  // namespace

VkComputePipelineResource::~VkComputePipelineResource() { reset(); }

VkComputePipelineResource::VkComputePipelineResource(
    VkComputePipelineResource&& other) noexcept {
    *this = std::move(other);
}

VkComputePipelineResource& VkComputePipelineResource::operator=(
    VkComputePipelineResource&& other) noexcept {
    if (this == &other) return *this;
    reset();
    device = std::exchange(other.device, VK_NULL_HANDLE);
    descriptor_set_layout =
        std::exchange(other.descriptor_set_layout, VK_NULL_HANDLE);
    pipeline_layout = std::exchange(other.pipeline_layout, VK_NULL_HANDLE);
    pipeline = std::exchange(other.pipeline, VK_NULL_HANDLE);
    descriptor_pool = std::exchange(other.descriptor_pool, VK_NULL_HANDLE);
    descriptor_set = std::exchange(other.descriptor_set, VK_NULL_HANDLE);
    lifetime = std::move(other.lifetime);
    referenced_buffers = std::move(other.referenced_buffers);
    return *this;
}

// Releases whatever this resource owns and returns it to the empty state.
// Prefers dropping the `lifetime` allocation (which does the destroying, and
// correctly does nothing when the device is already gone); the direct
// vkDestroy* branch is only reached by a half-built resource from a failed
// create_compute_pipeline(). Called by the destructor and by move-assignment,
// so it must stay safe on an already-empty resource.
void VkComputePipelineResource::reset() {
    if (lifetime) {
        lifetime.reset();
    } else {
        if (device != VK_NULL_HANDLE && pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(device, pipeline, nullptr);
        if (device != VK_NULL_HANDLE && pipeline_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        if (device != VK_NULL_HANDLE && descriptor_pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        if (device != VK_NULL_HANDLE && descriptor_set_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
    }
    device = VK_NULL_HANDLE;
    descriptor_set_layout = VK_NULL_HANDLE;
    pipeline_layout = VK_NULL_HANDLE;
    pipeline = VK_NULL_HANDLE;
    descriptor_pool = VK_NULL_HANDLE;
    descriptor_set = VK_NULL_HANDLE;
    referenced_buffers.clear();
}

// Builds everything into a local `candidate` and only moves it into `output`
// once every step has succeeded — so an early error return destroys the
// partial pipeline through candidate's destructor and leaves the caller's
// object untouched. The descriptor pool is sized by summing descriptorCount
// per descriptor type across `bindings`, with maxSets = 1: exactly one set is
// ever allocated from it. The shader module is transient and destroyed as soon
// as the pipeline is created.
bool create_compute_pipeline(
    VulkanDevice& vulkan, std::string_view embedded_spirv_name,
    const std::vector<VkDescriptorSetLayoutBinding>& bindings,
    VkComputePipelineResource& output, std::string& error) {
    const EmbeddedSpirvView spirv = find_spirv(embedded_spirv_name);
    if (!spirv.words || spirv.word_count == 0) {
        error = "embedded SPIR-V not found: " + std::string(embedded_spirv_name);
        return false;
    }
    VkComputePipelineResource candidate;
    candidate.device = vulkan.device();

    VkDescriptorSetLayoutCreateInfo set_layout{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout.bindingCount = static_cast<uint32_t>(bindings.size());
    set_layout.pBindings = bindings.data();
    VkResult result = vkCreateDescriptorSetLayout(
        candidate.device, &set_layout, nullptr, &candidate.descriptor_set_layout);
    if (result != VK_SUCCESS) {
        return vk_error("vkCreateDescriptorSetLayout", result, error);
    }
    VkPipelineLayoutCreateInfo pipeline_layout{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &candidate.descriptor_set_layout;
    result = vkCreatePipelineLayout(candidate.device, &pipeline_layout, nullptr,
                                    &candidate.pipeline_layout);
    if (result != VK_SUCCESS) {
        return vk_error("vkCreatePipelineLayout", result, error);
    }

    VkShaderModule shader = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo shader_info{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_info.codeSize = spirv.word_count * sizeof(uint32_t);
    shader_info.pCode = spirv.words;
    result = vkCreateShaderModule(candidate.device, &shader_info, nullptr,
                                  &shader);
    if (result != VK_SUCCESS) {
        return vk_error("vkCreateShaderModule", result, error);
    }
    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";
    VkComputePipelineCreateInfo create{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    create.stage = stage;
    create.layout = candidate.pipeline_layout;
    result = vkCreateComputePipelines(candidate.device, VK_NULL_HANDLE, 1,
                                      &create, nullptr, &candidate.pipeline);
    vkDestroyShaderModule(candidate.device, shader, nullptr);
    if (result != VK_SUCCESS) {
        return vk_error("vkCreateComputePipelines", result, error);
    }

    if (!bindings.empty()) {
        std::vector<VkDescriptorPoolSize> pool_sizes;
        for (const auto& binding : bindings) {
            auto existing = pool_sizes.end();
            for (auto it = pool_sizes.begin(); it != pool_sizes.end(); ++it) {
                if (it->type == binding.descriptorType) {
                    existing = it;
                    break;
                }
            }
            if (existing == pool_sizes.end()) {
                pool_sizes.push_back(
                    {binding.descriptorType, binding.descriptorCount});
            } else {
                existing->descriptorCount += binding.descriptorCount;
            }
        }
        VkDescriptorPoolCreateInfo pool{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool.pPoolSizes = pool_sizes.data();
        result = vkCreateDescriptorPool(candidate.device, &pool, nullptr,
                                        &candidate.descriptor_pool);
        if (result != VK_SUCCESS) {
            return vk_error("vkCreateDescriptorPool", result, error);
        }
        VkDescriptorSetAllocateInfo allocate{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = candidate.descriptor_pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &candidate.descriptor_set_layout;
        result = vkAllocateDescriptorSets(candidate.device, &allocate,
                                          &candidate.descriptor_set);
        if (result != VK_SUCCESS) {
            return vk_error("vkAllocateDescriptorSets", result, error);
        }
    }
    candidate.lifetime =
        std::make_shared<detail::VkComputePipelineAllocation>(
            detail::DeviceLifetimeAccess::token(vulkan));
    candidate.lifetime->descriptor_set_layout = candidate.descriptor_set_layout;
    candidate.lifetime->pipeline_layout = candidate.pipeline_layout;
    candidate.lifetime->pipeline = candidate.pipeline;
    candidate.lifetime->descriptor_pool = candidate.descriptor_pool;
    output = std::move(candidate);
    return true;
}

void write_storage_buffer_descriptor(VkComputePipelineResource& pipeline,
                                     uint32_t binding,
                                     VkBufferResource& buffer,
                                     VkDeviceSize offset,
                                     VkDeviceSize range) {
    VkDescriptorBufferInfo buffer_info{buffer.buffer, offset, range};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = pipeline.descriptor_set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(pipeline.device, 1, &write, 0, nullptr);
    const auto existing = std::find_if(
        pipeline.referenced_buffers.begin(), pipeline.referenced_buffers.end(),
        [binding](const auto& reference) { return reference.first == binding; });
    if (existing == pipeline.referenced_buffers.end()) {
        pipeline.referenced_buffers.emplace_back(binding, buffer.lifetime);
    } else {
        existing->second = buffer.lifetime;
    }
}

// Blocking one-shot dispatch. The dependency list handed to submit_immediate()
// is the pipeline's own allocation plus every distinct buffer allocation bound
// into its descriptor set, deduplicated — those are exactly the objects the
// GPU may still be reading if completion turns out to be unprovable, in which
// case the device retains them rather than freeing them underneath it.
bool dispatch_compute(VulkanDevice& vulkan,
                      VkComputePipelineResource& pipeline,
                      uint32_t group_count_x, uint32_t group_count_y,
                      uint32_t group_count_z, std::string& error) {
    if (group_count_x == 0 || group_count_y == 0 || group_count_z == 0) {
        error = "dispatch_compute requires nonzero group counts";
        return false;
    }
    DispatchRecord dispatch{&pipeline, group_count_x, group_count_y,
                            group_count_z};
    std::vector<std::shared_ptr<void>> dependencies{pipeline.lifetime};
    for (const auto& reference : pipeline.referenced_buffers) {
        if (std::find(dependencies.begin(), dependencies.end(),
                      reference.second) == dependencies.end()) {
            dependencies.push_back(reference.second);
        }
    }
    return submit_immediate(vulkan, record_dispatch, &dispatch, error,
                            ImmediateSubmitPhase::compute_dispatch,
                            std::move(dependencies));
}

bool run_transform_probe(VulkanDevice& vulkan,
                         const viewer::GpuMat4& packed_matrix,
                         matter::Float4 input, matter::Float4& output) {
    output = {};
    std::string error;
    TransformProbeData data{packed_matrix, input, {}};
    VkBufferResource buffer;
    if (!create_buffer(vulkan, sizeof(data),
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                       buffer, error) ||
        !upload_buffer(vulkan, buffer, &data, sizeof(data), 0, error)) {
        MATTER_LOGE("vk", "transform probe setup failed: %s\n", error.c_str());
        return false;
    }
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkComputePipelineResource pipeline;
    if (!create_compute_pipeline(vulkan, "transform_probe.comp.spv", {binding},
                                 pipeline, error)) {
        MATTER_LOGE("vk", "transform probe pipeline failed: %s\n",
                     error.c_str());
        return false;
    }
    write_storage_buffer_descriptor(pipeline, 0, buffer, 0, sizeof(data));
    if (!dispatch_compute(vulkan, pipeline, 1, 1, 1, error) ||
        !readback_buffer(vulkan, buffer, &data, sizeof(data), 0, error)) {
        MATTER_LOGE("vk", "transform probe dispatch failed: %s\n",
                     error.c_str());
        return false;
    }
    output = data.output;
    return true;
}

}  // namespace matter
