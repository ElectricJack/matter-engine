#pragma once

// MatterEngine3/src/render/vk_pipeline.h
//
// Minimal one-shot compute-pipeline support: build a compute pipeline from a
// named blob in the generated shaders_gen/embedded_spirv.h, point one
// descriptor set at some storage buffers, dispatch it, wait. Deliberately not
// the renderer's pipeline system — VkSceneRenderer builds its own graphics and
// compute pipelines; this is for standalone GPU work outside the frame loop
// (the transform probe below, and other immediate-submit utilities).
//
// LIFETIME. VkComputePipelineResource is move-only and owns its Vulkan objects
// through a detail::VkComputePipelineAllocation, which registers with the
// device's lifetime registry (see vk_device_internal.h). That means the
// objects are released either when the resource is destroyed or when the
// device tears down first — whichever comes first — and never against a device
// that was deliberately leaked.
//
// THREADING AND COST. dispatch_compute() SUBMITS AND BLOCKS on the graphics
// queue via submit_immediate(); it is not recorded into a frame's command
// buffer. Do not call it from inside begin_frame()/end_frame(), and do not
// treat it as cheap.

#include <vulkan/vulkan.h>

#include <string>
#include <string_view>
#include <memory>
#include <utility>
#include <vector>

#include "gpu_matrix_pack.h"
#include "matter/math_types.h"

namespace matter {

class VulkanDevice;
struct VkBufferResource;
namespace detail {
struct VkComputePipelineAllocation;
}

// Owning handle for one compute pipeline: descriptor set layout, pipeline
// layout, pipeline, and (only when the shader has bindings) a single-set
// descriptor pool and its set. Move-only; moved-from instances are left empty
// and safe to destroy.
//
// `lifetime` is what actually destroys the Vulkan objects — reset() drops it
// and lets the allocation release them, falling back to direct vkDestroy* only
// for a resource whose creation failed before the allocation existed.
// `referenced_buffers` keeps a keep-alive per bound binding, so a buffer whose
// owner drops it cannot be freed while this pipeline's descriptor set still
// points at it; rebinding a binding replaces its entry.
struct VkComputePipelineResource {
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    std::shared_ptr<detail::VkComputePipelineAllocation> lifetime;
    std::vector<std::pair<uint32_t, std::shared_ptr<void>>> referenced_buffers;

    VkComputePipelineResource() = default;
    ~VkComputePipelineResource();
    VkComputePipelineResource(const VkComputePipelineResource&) = delete;
    VkComputePipelineResource& operator=(const VkComputePipelineResource&) = delete;
    VkComputePipelineResource(VkComputePipelineResource&& other) noexcept;
    VkComputePipelineResource& operator=(VkComputePipelineResource&& other) noexcept;

    void reset();
};

// Builds a compute pipeline from the embedded SPIR-V blob named
// `embedded_spirv_name` (e.g. "transform_probe.comp.spv" — the key in
// shaders_gen/embedded_spirv.h, which the build regenerates from
// MatterEngine3/shaders_vk/). `bindings` describes the ONE descriptor set,
// set 0; pass an empty list for a shader with no descriptors and no set is
// allocated. `output` is only assigned on success — on failure it is left
// untouched and everything created so far is destroyed. Entry point is always
// "main"; there are no push constants and no specialization constants.
bool create_compute_pipeline(
    VulkanDevice& vulkan, std::string_view embedded_spirv_name,
    const std::vector<VkDescriptorSetLayoutBinding>& bindings,
    VkComputePipelineResource& output, std::string& error);

// Points `binding` in the pipeline's descriptor set at [offset, offset+range)
// of `buffer` (bytes; VK_WHOLE_SIZE is allowed for range) and takes a
// keep-alive on the buffer's allocation. Updates the descriptor set
// IMMEDIATELY via vkUpdateDescriptorSets, so it must not be called while a
// dispatch using that set is still in flight.
void write_storage_buffer_descriptor(VkComputePipelineResource& pipeline,
                                     uint32_t binding,
                                     VkBufferResource& buffer,
                                     VkDeviceSize offset,
                                     VkDeviceSize range);

// Records bind + vkCmdDispatch + a full memory barrier, submits it on the
// graphics queue and BLOCKS until it completes. Group counts are workgroups,
// not invocations, and all three must be nonzero. The pipeline and every
// buffer bound into it are retained across the submit, so they stay alive even
// if completion cannot be proven and the device keeps the command pool.
bool dispatch_compute(VulkanDevice& vulkan,
                      VkComputePipelineResource& pipeline,
                      uint32_t group_count_x, uint32_t group_count_y,
                      uint32_t group_count_z, std::string& error);

// Round-trips one vec4 through the GPU's own mat4 multiply using the packed
// matrix layout in gpu_matrix_pack.h, and writes the result to `output`. This
// is the check that the CPU's matrix packing and GLSL's column-major mat4
// agree — a self-test, not a rendering feature. Allocates a buffer, builds a
// pipeline and blocks on a dispatch, so it is expensive; reports failures on
// stderr and returns false (`output` is zeroed first).
bool run_transform_probe(VulkanDevice& vulkan,
                         const viewer::GpuMat4& packed_matrix,
                         matter::Float4 input, matter::Float4& output);

}  // namespace matter
