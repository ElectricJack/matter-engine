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
    uint32_t capacities[4];
};

static_assert(sizeof(FrameConstants) == 208,
              "FrameConstants must match the std140 shader block");
static_assert(sizeof(VkCullStats) == 16,
              "VkCullStats must match the std430 stats block");

bool fail_vk(const char* operation, VkResult result, std::string& error) {
    error = std::string(operation) + " failed with VkResult " +
            std::to_string(static_cast<int>(result));
    return false;
}

bool checked_u32_add(uint32_t a, uint32_t b, uint32_t& result,
                     const char* label, std::string& error) {
    if (b > std::numeric_limits<uint32_t>::max() - a) {
        error = std::string(label) + " exceeds uint32_t capacity";
        return false;
    }
    result = a + b;
    return true;
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

namespace vk_scene_detail {

bool checked_mul_to_device_size(size_t count, size_t element_size,
                                VkDeviceSize& result, const char* label,
                                std::string& error) {
    error.clear();
    if (element_size != 0 &&
        count > std::numeric_limits<VkDeviceSize>::max() / element_size) {
        error = std::string(label) + " byte-size multiplication overflow";
        return false;
    }
    result = static_cast<VkDeviceSize>(count) *
             static_cast<VkDeviceSize>(element_size);
    return true;
}

bool checked_grown_capacity(VkDeviceSize current, VkDeviceSize required,
                            VkDeviceSize limit, VkDeviceSize& result,
                            const char* label, std::string& error) {
    error.clear();
    if (limit == 0 || required > limit || current > limit) {
        error = std::string(label) + " exceeds Vulkan device limit";
        return false;
    }
    VkDeviceSize capacity = current;
    if (capacity == 0) capacity = std::min<VkDeviceSize>(16, limit);
    while (capacity < required) {
        if (capacity > limit / 2) {
            capacity = limit;
            break;
        }
        capacity *= 2;
    }
    if (capacity < required) {
        error = std::string(label) + " capacity growth overflow";
        return false;
    }
    result = capacity;
    return true;
}

bool checked_dispatch_groups(uint32_t instance_count,
                             uint32_t max_clusters_per_instance,
                             uint32_t max_group_count_x, uint32_t& groups,
                             std::string& error) {
    error.clear();
    const uint64_t invocation_count =
        static_cast<uint64_t>(instance_count) * max_clusters_per_instance;
    if (invocation_count > std::numeric_limits<uint32_t>::max()) {
        error = "Vulkan cull dispatch exceeds uint32_t shader invocation capacity";
        return false;
    }
    const uint64_t group_count = (invocation_count + 63u) / 64u;
    if (group_count > std::numeric_limits<uint32_t>::max() ||
        group_count > max_group_count_x) {
        error = "Vulkan cull dispatch exceeds maxComputeWorkGroupCount[0]";
        return false;
    }
    groups = static_cast<uint32_t>(group_count);
    return true;
}

bool checked_size_to_int(size_t count, int& result, const char* label,
                         std::string& error) {
    error.clear();
    if (count > static_cast<size_t>(std::numeric_limits<int>::max())) {
        error = std::string(label) + " exceeds INT_MAX";
        return false;
    }
    result = static_cast<int>(count);
    return true;
}

}  // namespace vk_scene_detail

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
    const VkDeviceSize descriptor_limit =
        set_index == 0 ? limits_.max_uniform_buffer_range
                       : limits_.max_storage_buffer_range;
    const VkDeviceSize limit =
        std::min(descriptor_limit, limits_.max_buffer_size);
    required_size = std::max<VkDeviceSize>(required_size, 1);
    if (buffer.size >= required_size) return true;
    VkDeviceSize capacity = 0;
    if (!vk_scene_detail::checked_grown_capacity(
            buffer.size, required_size, limit, capacity,
            set_index == 0 ? "uniform buffer range" : "storage buffer range",
            error)) {
        return false;
    }
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

bool VkSceneRenderer::load_device_limits(std::string& error) {
    VkPhysicalDeviceMaintenance4Properties maintenance4{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties2.pNext = &maintenance4;
    vkGetPhysicalDeviceProperties2(vulkan_->physical_device(), &properties2);
    const VkPhysicalDeviceLimits& vk_limits = properties2.properties.limits;
    physical_limits_.max_storage_buffer_range = vk_limits.maxStorageBufferRange;
    physical_limits_.max_uniform_buffer_range = vk_limits.maxUniformBufferRange;
    physical_limits_.max_dispatch_group_count_x =
        vk_limits.maxComputeWorkGroupCount[0];
    physical_limits_.max_draw_indirect_count = vk_limits.maxDrawIndirectCount;
    physical_limits_.max_buffer_size = maintenance4.maxBufferSize;
    if (physical_limits_.max_buffer_size == 0)
        physical_limits_.max_buffer_size =
            std::numeric_limits<VkDeviceSize>::max();
    limits_ = physical_limits_;
    if (limits_.max_storage_buffer_range == 0 ||
        limits_.max_uniform_buffer_range == 0 ||
        limits_.max_dispatch_group_count_x == 0 ||
        limits_.max_draw_indirect_count == 0) {
        error = "Vulkan device reports unusable scene buffer or dispatch limits";
        return false;
    }
    if (vk_limits.maxBoundDescriptorSets < 2 ||
        vk_limits.maxPerStageDescriptorUniformBuffers < 1 ||
        vk_limits.maxDescriptorSetUniformBuffers < 1 ||
        vk_limits.maxPerStageDescriptorStorageBuffers < 5 ||
        vk_limits.maxDescriptorSetStorageBuffers < 5) {
        error = "Vulkan device descriptor limits cannot support scene culling";
        return false;
    }
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    if (use_test_limits_) {
        limits_.max_storage_buffer_range = std::min(
            limits_.max_storage_buffer_range,
            test_limits_.max_storage_buffer_range);
        limits_.max_uniform_buffer_range = std::min(
            limits_.max_uniform_buffer_range,
            test_limits_.max_uniform_buffer_range);
        limits_.max_buffer_size =
            std::min(limits_.max_buffer_size, test_limits_.max_buffer_size);
        limits_.max_dispatch_group_count_x = std::min(
            limits_.max_dispatch_group_count_x,
            test_limits_.max_dispatch_group_count_x);
        limits_.max_draw_indirect_count = std::min(
            limits_.max_draw_indirect_count,
            test_limits_.max_draw_indirect_count);
    }
#endif
    return true;
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
void VkSceneRenderer::set_test_device_limits(
    VkDeviceSize max_storage_buffer_range,
    VkDeviceSize max_uniform_buffer_range, VkDeviceSize max_buffer_size,
    uint32_t max_dispatch_group_count_x,
    uint32_t max_draw_indirect_count) {
    test_limits_.max_storage_buffer_range = max_storage_buffer_range;
    test_limits_.max_uniform_buffer_range = max_uniform_buffer_range;
    test_limits_.max_buffer_size = max_buffer_size;
    test_limits_.max_dispatch_group_count_x = max_dispatch_group_count_x;
    test_limits_.max_draw_indirect_count = max_draw_indirect_count;
    use_test_limits_ = true;
    if (initialized_) {
        limits_.max_storage_buffer_range = std::min(
            physical_limits_.max_storage_buffer_range,
            test_limits_.max_storage_buffer_range);
        limits_.max_uniform_buffer_range = std::min(
            physical_limits_.max_uniform_buffer_range,
            test_limits_.max_uniform_buffer_range);
        limits_.max_buffer_size = std::min(physical_limits_.max_buffer_size,
                                           test_limits_.max_buffer_size);
        limits_.max_dispatch_group_count_x = std::min(
            physical_limits_.max_dispatch_group_count_x,
            test_limits_.max_dispatch_group_count_x);
        limits_.max_draw_indirect_count = std::min(
            physical_limits_.max_draw_indirect_count,
            test_limits_.max_draw_indirect_count);
    }
}

void VkSceneRenderer::clear_test_device_limits(std::string& error) {
    error.clear();
    use_test_limits_ = false;
    limits_ = physical_limits_;
}

bool VkSceneRenderer::set_test_command_first_instance(
    uint32_t command_index, uint32_t first_instance, std::string& error) {
    error.clear();
    if (command_index >= command_template_.size()) {
        error = "test command index is outside the command table";
        return false;
    }
    std::vector<DrawCommand> candidate = command_template_;
    candidate[command_index].first_instance = first_instance;
    uint32_t previous = 0;
    for (size_t i = 0; i < candidate.size(); ++i) {
        const uint32_t offset = candidate[i].first_instance;
        if ((i != 0 && offset < previous) || offset > draw_transform_slots_) {
            error = "draw command transform regions must be monotonic and bounded";
            return false;
        }
        previous = offset;
    }
    command_template_ = std::move(candidate);
    return true;
}
#endif

bool VkSceneRenderer::init(std::string& error) {
    error.clear();
    if (initialized_) return true;
    if (pipeline_ != VK_NULL_HANDLE) destroy_pipeline();
    if (!load_device_limits(error)) return false;
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
    if (parts_.size() >= static_cast<size_t>(std::numeric_limits<int>::max()) ||
        part.clusters.size() > std::numeric_limits<uint32_t>::max() ||
        cluster_staging_.size() > std::numeric_limits<uint32_t>::max() -
                                      part.clusters.size()) {
        error = "VkScenePart exceeds uint32_t scene indexing capacity";
        return -1;
    }
    const size_t combined_clusters =
        cluster_staging_.size() + part.clusters.size();
    if (combined_clusters >
        std::numeric_limits<uint32_t>::max() / kVkMaxLod) {
        error = "VkScenePart exceeds uint32_t draw-command capacity";
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
    if (!rebuild_command_template(error)) {
        slot_of_.erase(part.part_hash);
        parts_.pop_back();
        cluster_staging_.resize(record.cluster_start);
        cluster_lods_.resize(record.cluster_start);
        std::string ignored_error;
        rebuild_command_template(ignored_error);
        return -1;
    }
    return slot;
}

void VkSceneRenderer::release_part(uint64_t part_hash) {
    const auto found = slot_of_.find(part_hash);
    if (found == slot_of_.end()) return;
    const uint32_t released_slot = static_cast<uint32_t>(found->second);
    std::vector<uint32_t> remap(parts_.size(),
                                std::numeric_limits<uint32_t>::max());
    std::vector<PartRecord> compact_parts;
    std::vector<GpuCluster> compact_clusters;
    std::vector<std::vector<VkSceneLod>> compact_lods;
    std::map<uint64_t, int> compact_slots;
    compact_parts.reserve(parts_.size() - 1);
    compact_clusters.reserve(cluster_staging_.size() -
                             parts_[released_slot].cluster_count);
    compact_lods.reserve(compact_clusters.capacity());
    for (uint32_t old_slot = 0; old_slot < parts_.size(); ++old_slot) {
        if (old_slot == released_slot) continue;
        const PartRecord& old_part = parts_[old_slot];
        const uint32_t new_slot = static_cast<uint32_t>(compact_parts.size());
        remap[old_slot] = new_slot;
        PartRecord new_part = old_part;
        new_part.cluster_start = static_cast<uint32_t>(compact_clusters.size());
        new_part.live = true;
        for (uint32_t i = 0; i < old_part.cluster_count; ++i) {
            GpuCluster cluster =
                cluster_staging_[old_part.cluster_start + i];
            cluster.part_slot = new_slot;
            compact_clusters.push_back(cluster);
            compact_lods.push_back(cluster_lods_[old_part.cluster_start + i]);
        }
        compact_slots[new_part.hash] = static_cast<int>(new_slot);
        compact_parts.push_back(new_part);
    }
    std::vector<GpuInstance> kept_instances;
    std::vector<uint32_t> kept_slots;
    kept_instances.reserve(instance_staging_.size());
    kept_slots.reserve(instance_part_slots_.size());
    for (size_t i = 0; i < instance_staging_.size(); ++i) {
        const uint32_t old_slot = instance_part_slots_[i];
        if (old_slot == released_slot) continue;
        const uint32_t new_slot = remap[old_slot];
        GpuInstance instance = instance_staging_[i];
        instance.part_slot = new_slot;
        instance.cluster_start = compact_parts[new_slot].cluster_start;
        instance.cluster_count = compact_parts[new_slot].cluster_count;
        kept_instances.push_back(instance);
        kept_slots.push_back(new_slot);
    }
    parts_ = std::move(compact_parts);
    slot_of_ = std::move(compact_slots);
    cluster_staging_ = std::move(compact_clusters);
    cluster_lods_ = std::move(compact_lods);
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
    std::string ignored_error;
    if (!rebuild_command_template(ignored_error)) {
        instance_staging_.clear();
        instance_part_slots_.clear();
        command_template_.clear();
        draw_transform_slots_ = 0;
    }
}

bool VkSceneRenderer::update_instances(
    const std::vector<VkSceneInstance>& instances, std::string& error) {
    error.clear();
    int public_count = 0;
    if (!vk_scene_detail::checked_size_to_int(
            instances.size(), public_count, "VkSceneInstance count", error)) {
        return false;
    }
    (void)public_count;
    auto old_instances = std::move(instance_staging_);
    auto old_slots = std::move(instance_part_slots_);
    auto old_rt = std::move(rt_instances_);
    auto old_commands = std::move(command_template_);
    const uint32_t old_max_clusters = max_clusters_per_instance_;
    const uint32_t old_transform_slots = draw_transform_slots_;
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
    if (!rebuild_command_template(error)) {
        instance_staging_ = std::move(old_instances);
        instance_part_slots_ = std::move(old_slots);
        rt_instances_ = std::move(old_rt);
        command_template_ = std::move(old_commands);
        max_clusters_per_instance_ = old_max_clusters;
        draw_transform_slots_ = old_transform_slots;
        return false;
    }
    return true;
}

bool VkSceneRenderer::rebuild_command_template(std::string& error) {
    VkDeviceSize command_count = 0;
    if (!vk_scene_detail::checked_mul_to_device_size(
            cluster_staging_.size(), kVkMaxLod, command_count,
            "draw-command count", error) ||
        command_count > std::numeric_limits<uint32_t>::max()) {
        if (error.empty()) error = "draw-command count exceeds uint32_t capacity";
        return false;
    }
    VkDeviceSize command_bytes = 0;
    if (!vk_scene_detail::checked_mul_to_device_size(
            static_cast<size_t>(command_count), sizeof(DrawCommand),
            command_bytes, "draw-command buffer", error)) {
        return false;
    }
    const VkDeviceSize storage_limit =
        std::min(limits_.max_storage_buffer_range, limits_.max_buffer_size);
    if (storage_limit != 0 && command_bytes > storage_limit) {
        error = "draw-command buffer exceeds Vulkan storage descriptor limit";
        return false;
    }
    if (limits_.max_draw_indirect_count != 0 &&
        command_count > limits_.max_draw_indirect_count) {
        error = "draw-command count exceeds Vulkan maxDrawIndirectCount";
        return false;
    }
    std::vector<uint32_t> per_part(parts_.size(), 0);
    for (uint32_t slot : instance_part_slots_) {
        if (slot >= per_part.size() || per_part[slot] ==
                                           std::numeric_limits<uint32_t>::max()) {
            error = "instance part bucket exceeds uint32_t capacity";
            return false;
        }
        ++per_part[slot];
    }
    uint32_t first_instance = 0;
    for (size_t cluster_index = 0; cluster_index < cluster_staging_.size();
         ++cluster_index) {
        const GpuCluster& cluster = cluster_staging_[cluster_index];
        const auto& lods = cluster_lods_[cluster_index];
        if (cluster.part_slot >= per_part.size()) {
            error = "cluster part bucket is outside the active part table";
            return false;
        }
        for (size_t lod = 0; lod < lods.size(); ++lod) {
            if (!checked_u32_add(first_instance, per_part[cluster.part_slot],
                                 first_instance, "draw transform slots",
                                 error)) {
                return false;
            }
        }
    }
    VkDeviceSize transform_bytes = 0;
    if (!vk_scene_detail::checked_mul_to_device_size(
            first_instance, sizeof(GpuMat4), transform_bytes,
            "draw-transform buffer", error)) {
        return false;
    }
    if (storage_limit != 0 && transform_bytes > storage_limit) {
        error = "draw-transform buffer exceeds Vulkan storage descriptor limit";
        return false;
    }

    command_template_.assign(static_cast<size_t>(command_count), {});
    uint32_t command_first_instance = 0;
    for (size_t cluster_index = 0; cluster_index < cluster_staging_.size();
         ++cluster_index) {
        const GpuCluster& cluster = cluster_staging_[cluster_index];
        const auto& lods = cluster_lods_[cluster_index];
        for (size_t lod = 0; lod < kVkMaxLod; ++lod) {
            DrawCommand& command =
                command_template_[cluster_index * kVkMaxLod + lod];
            command.first_instance = command_first_instance;
            if (lod < lods.size()) {
                command.vertex_count = lods[lod].vertex_count;
                command.first_vertex = lods[lod].first_vertex;
                if (!checked_u32_add(command_first_instance,
                                     per_part[cluster.part_slot],
                                     command_first_instance,
                                     "draw transform slots", error)) {
                    command_template_.clear();
                    return false;
                }
            }
        }
    }
    draw_transform_slots_ = first_instance;
    return true;
}

bool VkSceneRenderer::upload_scene_buffers(std::string& error) {
    VkDeviceSize cluster_bytes = 0;
    VkDeviceSize instance_bytes = 0;
    VkDeviceSize command_bytes = 0;
    VkDeviceSize transform_bytes = 0;
    if (!vk_scene_detail::checked_mul_to_device_size(
            cluster_staging_.size(), sizeof(GpuCluster), cluster_bytes,
            "cluster buffer", error) ||
        !vk_scene_detail::checked_mul_to_device_size(
            instance_staging_.size(), sizeof(GpuInstance), instance_bytes,
            "instance buffer", error) ||
        !vk_scene_detail::checked_mul_to_device_size(
            command_template_.size(), sizeof(DrawCommand), command_bytes,
            "draw-command buffer", error) ||
        !vk_scene_detail::checked_mul_to_device_size(
            draw_transform_slots_, sizeof(GpuMat4), transform_bytes,
            "draw-transform buffer", error)) {
        return false;
    }
    const auto storage_size_ok = [&](VkDeviceSize size, const char* label) {
        const VkDeviceSize required = std::max<VkDeviceSize>(size, 1);
        if (required > limits_.max_storage_buffer_range) {
            error = std::string(label) +
                    " exceeds Vulkan maxStorageBufferRange";
            return false;
        }
        if (required > limits_.max_buffer_size) {
            error = std::string(label) + " exceeds Vulkan maxBufferSize";
            return false;
        }
        return true;
    };
    if (!storage_size_ok(cluster_bytes, "cluster buffer") ||
        !storage_size_ok(instance_bytes, "instance buffer") ||
        !storage_size_ok(command_bytes, "draw-command buffer") ||
        !storage_size_ok(transform_bytes, "draw-transform buffer") ||
        !ensure_buffer(clusters_, cluster_bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, 0, error) ||
        !ensure_buffer(instances_, instance_bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, 1, error) ||
        !ensure_buffer(commands_, command_bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                       1, 2, error) ||
        !ensure_buffer(draw_transforms_, transform_bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, 3, error)) {
        return false;
    }
    const VkCullStats zero_stats{};
    const bool uploaded =
        (cluster_bytes == 0 ||
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
    if (uploaded) {
        uploaded_command_count_ =
            static_cast<uint32_t>(command_template_.size());
        uploaded_transform_slots_ = draw_transform_slots_;
    }
    return uploaded;
}

bool VkSceneRenderer::dispatch_culling(const FrameMatrices& frame,
                                       matter::Float3 camera_eye,
                                       float pixel_budget,
                                       std::string& error) {
    error.clear();
    if (!initialized_ && !init(error)) return false;
    if (command_template_.size() > limits_.max_draw_indirect_count) {
        error = "draw-command count exceeds Vulkan maxDrawIndirectCount";
        return false;
    }
    uint32_t previous_first = 0;
    for (size_t i = 0; i < command_template_.size(); ++i) {
        const uint32_t first = command_template_[i].first_instance;
        if ((i != 0 && first < previous_first) ||
            first > draw_transform_slots_) {
            error = "draw command transform regions must be monotonic and bounded";
            return false;
        }
        previous_first = first;
    }
    uint32_t group_count = 0;
    if (!vk_scene_detail::checked_dispatch_groups(
            static_cast<uint32_t>(instance_staging_.size()),
            max_clusters_per_instance_, limits_.max_dispatch_group_count_x,
            group_count, error)) {
        return false;
    }
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
    constants.capacities[0] = static_cast<uint32_t>(cluster_staging_.size());
    constants.capacities[1] = static_cast<uint32_t>(instance_staging_.size());
    constants.capacities[2] = static_cast<uint32_t>(command_template_.size());
    constants.capacities[3] = draw_transform_slots_;
    if (!matter::upload_buffer(*vulkan_, frame_constants_, &constants,
                               sizeof(constants), 0, error)) {
        return false;
    }
    if (instance_staging_.empty() || max_clusters_per_instance_ == 0)
        return true;
    CullDispatchRecord dispatch{pipeline_, pipeline_layout_,
                                {descriptor_sets_[0], descriptor_sets_[1]},
                                group_count};
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
    commands.resize(uploaded_command_count_);
    if (commands.empty()) return true;
    VkDeviceSize bytes = 0;
    if (!vk_scene_detail::checked_mul_to_device_size(
            commands.size(), sizeof(DrawCommand), bytes,
            "draw-command readback", error)) return false;
    return matter::readback_buffer(
        *vulkan_, commands_,
        commands.data(), bytes, 0, error);
}

bool VkSceneRenderer::readback_draw_transforms(
    std::vector<GpuMat4>& transforms, std::string& error) {
    transforms.resize(uploaded_transform_slots_);
    if (transforms.empty()) return true;
    VkDeviceSize bytes = 0;
    if (!vk_scene_detail::checked_mul_to_device_size(
            transforms.size(), sizeof(GpuMat4), bytes,
            "draw-transform readback", error)) return false;
    return matter::readback_buffer(
        *vulkan_, draw_transforms_, transforms.data(),
        bytes, 0, error);
}

int VkSceneRenderer::fill_rt_instances(
    std::vector<RtInstance>& output) const {
    int count = 0;
    std::string error;
    if (!vk_scene_detail::checked_size_to_int(rt_instances_.size(), count,
                                               "RT instance count", error)) {
        output.clear();
        return 0;
    }
    output = rt_instances_;
    return count;
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
    uploaded_command_count_ = 0;
    uploaded_transform_slots_ = 0;
}

}  // namespace viewer
