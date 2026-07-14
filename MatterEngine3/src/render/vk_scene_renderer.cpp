#include "vk_scene_renderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

#include "gpu_matrix_pack.h"
#include "matter/vulkan_device.h"
#include "shaders_gen/embedded_spirv.h"

namespace viewer {
namespace {

struct alignas(16) FrameConstants {
    GpuMat4 world_to_clip;
    float frustum_planes[6][4];
    float camera_eye_pixel_budget[4];
    uint32_t counts[4];
};

static_assert(sizeof(FrameConstants) == 192,
              "FrameConstants must match the std140 shader block");
static_assert(sizeof(VkCullStats) == 12,
              "VkCullStats must match the std430 stats block");

bool fail_vk(const char* operation, VkResult result, std::string& error) {
    error = std::string(operation) + " failed with VkResult " +
            std::to_string(static_cast<int>(result));
    return false;
}

VkDescriptorSetLayoutBinding descriptor_binding(
    uint32_t binding, VkDescriptorType type) {
    VkDescriptorSetLayoutBinding result{};
    result.binding = binding;
    result.descriptorType = type;
    result.descriptorCount = 1;
    result.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    return result;
}

struct CullDispatchRecord {
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSet sets[2];
    uint32_t group_count;
};

void record_cull_dispatch(VkCommandBuffer command_buffer, void* user_data) {
    const auto& dispatch = *static_cast<CullDispatchRecord*>(user_data);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      dispatch.pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            dispatch.layout, 0, 2, dispatch.sets, 0, nullptr);
    vkCmdDispatch(command_buffer, dispatch.group_count, 1, 1);

    VkMemoryBarrier2 barriers[2]{};
    barriers[0].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barriers[0].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                               VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                                VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    barriers[1].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barriers[1].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barriers[1].dstStageMask =
        VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barriers[1].dstAccessMask =
        VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 2;
    dependency.pMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

}  // namespace

VkSceneRenderer::VkSceneRenderer(matter::VulkanDevice& vulkan)
    : vulkan_(&vulkan) {}

VkSceneRenderer::~VkSceneRenderer() {
    if (vulkan_) vulkan_->wait_idle();
    destroy_pipeline();
}

void VkSceneRenderer::destroy_pipeline() {
    if (!vulkan_) return;
    const VkDevice device = vulkan_->device();
    if (pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device, pipeline_, nullptr);
    if (pipeline_layout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, pipeline_layout_, nullptr);
    if (descriptor_pool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
    for (VkDescriptorSetLayout& layout : set_layouts_) {
        if (layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, layout, nullptr);
        layout = VK_NULL_HANDLE;
    }
    pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_sets_[0] = descriptor_sets_[1] = VK_NULL_HANDLE;
    initialized_ = false;
}

bool VkSceneRenderer::create_pipeline(std::string& error) {
    const VkDevice device = vulkan_->device();
    const VkDescriptorSetLayoutBinding frame_binding =
        descriptor_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    VkDescriptorSetLayoutCreateInfo frame_layout{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    frame_layout.bindingCount = 1;
    frame_layout.pBindings = &frame_binding;
    VkResult result = vkCreateDescriptorSetLayout(
        device, &frame_layout, nullptr, &set_layouts_[0]);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorSetLayout(frame)", result, error);

    std::array<VkDescriptorSetLayoutBinding, 5> scene_bindings{};
    for (uint32_t i = 0; i < scene_bindings.size(); ++i)
        scene_bindings[i] =
            descriptor_binding(i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    VkDescriptorSetLayoutCreateInfo scene_layout{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    scene_layout.bindingCount =
        static_cast<uint32_t>(scene_bindings.size());
    scene_layout.pBindings = scene_bindings.data();
    result = vkCreateDescriptorSetLayout(device, &scene_layout, nullptr,
                                         &set_layouts_[1]);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorSetLayout(scene)", result, error);

    VkPipelineLayoutCreateInfo pipeline_layout{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout.setLayoutCount = 2;
    pipeline_layout.pSetLayouts = set_layouts_;
    result = vkCreatePipelineLayout(device, &pipeline_layout, nullptr,
                                    &pipeline_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreatePipelineLayout(cull)", result, error);

    const matter::EmbeddedSpirvView spirv =
        matter::find_spirv("cull.comp.spv");
    if (!spirv.words || spirv.word_count == 0) {
        error = "embedded SPIR-V not found: cull.comp.spv";
        return false;
    }
    VkShaderModule shader = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo shader_create{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_create.codeSize = spirv.word_count * sizeof(uint32_t);
    shader_create.pCode = spirv.words;
    result = vkCreateShaderModule(device, &shader_create, nullptr, &shader);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateShaderModule(cull)", result, error);
    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";
    VkComputePipelineCreateInfo pipeline_create{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_create.stage = stage;
    pipeline_create.layout = pipeline_layout_;
    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                      &pipeline_create, nullptr, &pipeline_);
    vkDestroyShaderModule(device, shader, nullptr);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateComputePipelines(cull)", result, error);

    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5}};
    VkDescriptorPoolCreateInfo pool{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 2;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = pool_sizes;
    result = vkCreateDescriptorPool(device, &pool, nullptr, &descriptor_pool_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorPool(cull)", result, error);
    VkDescriptorSetAllocateInfo allocate{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptor_pool_;
    allocate.descriptorSetCount = 2;
    allocate.pSetLayouts = set_layouts_;
    result = vkAllocateDescriptorSets(device, &allocate, descriptor_sets_);
    return result == VK_SUCCESS ||
           fail_vk("vkAllocateDescriptorSets(cull)", result, error);
}

void VkSceneRenderer::update_descriptor(
    uint32_t set_index, uint32_t binding,
    const matter::VkBufferResource& buffer) {
    VkDescriptorBufferInfo info{buffer.buffer, 0, buffer.size};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptor_sets_[set_index];
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = set_index == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                          : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(vulkan_->device(), 1, &write, 0, nullptr);
}

bool VkSceneRenderer::ensure_buffer(matter::VkBufferResource& buffer,
                                    VkDeviceSize required_size,
                                    VkBufferUsageFlags usage,
                                    uint32_t set_index, uint32_t binding,
                                    std::string& error) {
    required_size = std::max<VkDeviceSize>(required_size, 16);
    if (buffer.size >= required_size) return true;
    VkDeviceSize capacity = std::max<VkDeviceSize>(buffer.size, 16);
    while (capacity < required_size) capacity *= 2;
    matter::VkBufferResource replacement;
    if (!matter::create_buffer(
            *vulkan_, capacity,
            usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            replacement, error)) {
        return false;
    }
    buffer = std::move(replacement);
    update_descriptor(set_index, binding, buffer);
    return true;
}

bool VkSceneRenderer::init(std::string& error) {
    error.clear();
    if (initialized_) return true;
    if (pipeline_ != VK_NULL_HANDLE) destroy_pipeline();
    if (!create_pipeline(error)) {
        destroy_pipeline();
        return false;
    }
    initialized_ =
        ensure_buffer(frame_constants_, sizeof(FrameConstants),
                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0, 0, error) &&
        ensure_buffer(clusters_, sizeof(GpuCluster),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, 0, error) &&
        ensure_buffer(instances_, sizeof(GpuInstance),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, 1, error) &&
        ensure_buffer(commands_, sizeof(DrawCommand),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                      1, 2, error) &&
        ensure_buffer(draw_transforms_, sizeof(GpuMat4),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, 3, error) &&
        ensure_buffer(stats_, sizeof(VkCullStats),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, 4, error);
    if (!initialized_) {
        destroy_pipeline();
        frame_constants_.reset();
        clusters_.reset();
        instances_.reset();
        commands_.reset();
        draw_transforms_.reset();
        stats_.reset();
    }
    return initialized_;
}

int VkSceneRenderer::ensure_part(const VkScenePart& part,
                                 std::string& error) {
    error.clear();
    const auto existing = slot_of_.find(part.part_hash);
    if (existing != slot_of_.end()) return existing->second;
    if (part.clusters.empty()) {
        error = "VkScenePart requires at least one cluster";
        return -1;
    }
    for (const auto& cluster : part.clusters) {
        if (cluster.lods.empty() || cluster.lods.size() > kVkMaxLod) {
            error = "VkSceneCluster LOD count must be in [1, kVkMaxLod]";
            return -1;
        }
    }
    const int slot = static_cast<int>(parts_.size());
    PartRecord record{};
    record.hash = part.part_hash;
    record.cluster_start = static_cast<uint32_t>(cluster_staging_.size());
    record.cluster_count = static_cast<uint32_t>(part.clusters.size());
    record.live = true;
    for (size_t i = 0; i < part.clusters.size(); ++i) {
        const auto& source = part.clusters[i];
        GpuCluster cluster{};
        cluster.aabb_min[0] = source.aabb_min.x;
        cluster.aabb_min[1] = source.aabb_min.y;
        cluster.aabb_min[2] = source.aabb_min.z;
        cluster.aabb_max[0] = source.aabb_max.x;
        cluster.aabb_max[1] = source.aabb_max.y;
        cluster.aabb_max[2] = source.aabb_max.z;
        cluster.radius = source.radius;
        cluster.lod_count = static_cast<uint32_t>(source.lods.size());
        cluster.part_slot = static_cast<uint32_t>(slot);
        cluster.cluster_index = static_cast<uint32_t>(i);
        for (uint32_t lod = 0; lod < kVkMaxLod; ++lod) {
            cluster.thresholds[lod] =
                lod < source.lods.size()
                    ? source.lods[lod].threshold
                    : std::numeric_limits<float>::max();
            cluster.lod_mesh_idx[lod] = lod;
        }
        cluster_staging_.push_back(cluster);
        cluster_lods_.push_back(source.lods);
    }
    parts_.push_back(record);
    slot_of_[part.part_hash] = slot;
    rebuild_command_template();
    return slot;
}

void VkSceneRenderer::release_part(uint64_t part_hash) {
    const auto found = slot_of_.find(part_hash);
    if (found == slot_of_.end()) return;
    const uint32_t released_slot = static_cast<uint32_t>(found->second);
    PartRecord& part = parts_[released_slot];
    part.live = false;
    for (uint32_t i = 0; i < part.cluster_count; ++i) {
        cluster_staging_[part.cluster_start + i].lod_count = 0;
        cluster_lods_[part.cluster_start + i].clear();
    }
    slot_of_.erase(found);
    std::vector<GpuInstance> kept_instances;
    std::vector<uint32_t> kept_slots;
    kept_instances.reserve(instance_staging_.size());
    kept_slots.reserve(instance_part_slots_.size());
    for (size_t i = 0; i < instance_staging_.size(); ++i) {
        if (instance_part_slots_[i] == released_slot)
            continue;
        kept_instances.push_back(instance_staging_[i]);
        kept_slots.push_back(instance_part_slots_[i]);
    }
    instance_staging_ = std::move(kept_instances);
    instance_part_slots_ = std::move(kept_slots);
    rt_instances_.erase(
        std::remove_if(rt_instances_.begin(), rt_instances_.end(),
                       [part_hash](const RtInstance& instance) {
                           return instance.part_hash == part_hash;
                       }),
        rt_instances_.end());
    max_clusters_per_instance_ = 0;
    for (const auto& instance : instance_staging_)
        max_clusters_per_instance_ =
            std::max(max_clusters_per_instance_, instance.cluster_count);
    rebuild_command_template();
}

bool VkSceneRenderer::update_instances(
    const std::vector<VkSceneInstance>& instances, std::string& error) {
    error.clear();
    instance_staging_.clear();
    instance_part_slots_.clear();
    rt_instances_.clear();
    max_clusters_per_instance_ = 0;
    for (const VkSceneInstance& source : instances) {
        const auto found = slot_of_.find(source.part_hash);
        if (found == slot_of_.end()) continue;
        const PartRecord& part = parts_[found->second];
        GpuInstance instance{};
        instance.object_to_world = pack_glsl_mat4(source.object_to_world);
        instance.part_slot = static_cast<uint32_t>(found->second);
        instance.cluster_start = part.cluster_start;
        instance.cluster_count = part.cluster_count;
        instance_staging_.push_back(instance);
        instance_part_slots_.push_back(instance.part_slot);
        max_clusters_per_instance_ =
            std::max(max_clusters_per_instance_, part.cluster_count);
        RtInstance rt{};
        rt.part_hash = source.part_hash;
        std::memcpy(rt.transform, source.object_to_world.m, sizeof(rt.transform));
        rt_instances_.push_back(rt);
    }
    rebuild_command_template();
    return true;
}

void VkSceneRenderer::rebuild_command_template() {
    command_template_.assign(cluster_staging_.size() * kVkMaxLod, {});
    std::vector<uint32_t> per_part(parts_.size(), 0);
    for (uint32_t slot : instance_part_slots_) ++per_part[slot];
    uint32_t first_instance = 0;
    for (size_t cluster_index = 0; cluster_index < cluster_staging_.size();
         ++cluster_index) {
        const GpuCluster& cluster = cluster_staging_[cluster_index];
        const auto& lods = cluster_lods_[cluster_index];
        for (size_t lod = 0; lod < lods.size(); ++lod) {
            DrawCommand& command =
                command_template_[cluster_index * kVkMaxLod + lod];
            command.vertex_count = lods[lod].vertex_count;
            command.first_vertex = lods[lod].first_vertex;
            command.first_instance = first_instance;
            if (cluster.part_slot < per_part.size())
                first_instance += per_part[cluster.part_slot];
        }
    }
}

bool VkSceneRenderer::upload_scene_buffers(std::string& error) {
    const VkDeviceSize cluster_bytes =
        cluster_staging_.size() * sizeof(GpuCluster);
    const VkDeviceSize instance_bytes =
        instance_staging_.size() * sizeof(GpuInstance);
    const VkDeviceSize command_bytes =
        command_template_.size() * sizeof(DrawCommand);
    draw_transform_slots_ = 0;
    std::vector<uint32_t> per_part(parts_.size(), 0);
    for (uint32_t part_slot : instance_part_slots_) ++per_part[part_slot];
    for (size_t cluster_index = 0; cluster_index < cluster_lods_.size();
         ++cluster_index) {
        for (size_t lod = 0; lod < cluster_lods_[cluster_index].size(); ++lod) {
            const DrawCommand& command =
                command_template_[cluster_index * kVkMaxLod + lod];
            const uint32_t part_slot =
                cluster_staging_[cluster_index].part_slot;
            const uint32_t capacity = per_part[part_slot];
            draw_transform_slots_ = std::max(
                draw_transform_slots_, command.first_instance + capacity);
        }
    }
    if (!ensure_buffer(clusters_, cluster_bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, 0, error) ||
        !ensure_buffer(instances_, instance_bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, 1, error) ||
        !ensure_buffer(commands_, command_bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                       1, 2, error) ||
        !ensure_buffer(draw_transforms_,
                       draw_transform_slots_ * sizeof(GpuMat4),
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, 3, error)) {
        return false;
    }
    const VkCullStats zero_stats{};
    return (cluster_bytes == 0 ||
            matter::upload_buffer(*vulkan_, clusters_, cluster_staging_.data(),
                                  cluster_bytes, 0, error)) &&
           (instance_bytes == 0 ||
            matter::upload_buffer(*vulkan_, instances_, instance_staging_.data(),
                                  instance_bytes, 0, error)) &&
           (command_bytes == 0 ||
            matter::upload_buffer(*vulkan_, commands_, command_template_.data(),
                                  command_bytes, 0, error)) &&
           matter::upload_buffer(*vulkan_, stats_, &zero_stats,
                                 sizeof(zero_stats), 0, error);
}

bool VkSceneRenderer::dispatch_culling(const FrameMatrices& frame,
                                       matter::Float3 camera_eye,
                                       float pixel_budget,
                                       std::string& error) {
    error.clear();
    if (!initialized_ && !init(error)) return false;
    if (!upload_scene_buffers(error)) return false;
    FrameConstants constants{};
    constants.world_to_clip = pack_glsl_mat4(frame.world_to_clip);
    std::memcpy(constants.frustum_planes, frame.frustum_planes,
                sizeof(constants.frustum_planes));
    constants.camera_eye_pixel_budget[0] = camera_eye.x;
    constants.camera_eye_pixel_budget[1] = camera_eye.y;
    constants.camera_eye_pixel_budget[2] = camera_eye.z;
    constants.camera_eye_pixel_budget[3] = pixel_budget;
    constants.counts[0] = static_cast<uint32_t>(instance_staging_.size());
    constants.counts[1] = max_clusters_per_instance_;
    if (!matter::upload_buffer(*vulkan_, frame_constants_, &constants,
                               sizeof(constants), 0, error)) {
        return false;
    }
    if (instance_staging_.empty() || max_clusters_per_instance_ == 0)
        return true;
    const uint64_t invocation_count =
        static_cast<uint64_t>(instance_staging_.size()) *
        max_clusters_per_instance_;
    CullDispatchRecord dispatch{pipeline_, pipeline_layout_,
                                {descriptor_sets_[0], descriptor_sets_[1]},
                                static_cast<uint32_t>((invocation_count + 63) /
                                                      64)};
    std::vector<std::shared_ptr<void>> dependencies{
        frame_constants_.lifetime, clusters_.lifetime, instances_.lifetime,
        commands_.lifetime, draw_transforms_.lifetime, stats_.lifetime};
    return matter::submit_immediate(
        *vulkan_, record_cull_dispatch, &dispatch, error,
        matter::ImmediateSubmitPhase::compute_dispatch,
        std::move(dependencies));
}

bool VkSceneRenderer::cull_stats(VkCullStats& stats,
                                 std::string& error) {
    stats = {};
    return matter::readback_buffer(*vulkan_, stats_,
                                   &stats, sizeof(stats), 0, error);
}

bool VkSceneRenderer::readback_commands(
    std::vector<DrawCommand>& commands, std::string& error) {
    commands.resize(command_template_.size());
    if (commands.empty()) return true;
    return matter::readback_buffer(
        *vulkan_, commands_,
        commands.data(), commands.size() * sizeof(DrawCommand), 0, error);
}

bool VkSceneRenderer::readback_draw_transforms(
    std::vector<GpuMat4>& transforms, std::string& error) {
    transforms.resize(draw_transform_slots_);
    if (transforms.empty()) return true;
    return matter::readback_buffer(
        *vulkan_, draw_transforms_, transforms.data(),
        transforms.size() * sizeof(GpuMat4), 0, error);
}

int VkSceneRenderer::fill_rt_instances(
    std::vector<RtInstance>& output) const {
    output = rt_instances_;
    return static_cast<int>(output.size());
}

void VkSceneRenderer::reset() {
    parts_.clear();
    slot_of_.clear();
    cluster_staging_.clear();
    cluster_lods_.clear();
    instance_staging_.clear();
    instance_part_slots_.clear();
    command_template_.clear();
    rt_instances_.clear();
    max_clusters_per_instance_ = 0;
    draw_transform_slots_ = 0;
}

}  // namespace viewer
