#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "frame_matrices.h"
#include "gpu_matrix_pack.h"
#include "matter/math_types.h"
#include "vk_draw_command.h"
#include "vk_resources.h"

namespace matter {
class VulkanDevice;
}

namespace viewer {

constexpr uint32_t kVkMaxLod = 9;

namespace vk_scene_detail {
bool checked_mul_to_device_size(size_t count, size_t element_size,
                                VkDeviceSize& result, const char* label,
                                std::string& error);
bool checked_grown_capacity(VkDeviceSize current, VkDeviceSize required,
                            VkDeviceSize limit, VkDeviceSize& result,
                            const char* label, std::string& error);
bool checked_dispatch_groups(uint32_t instance_count,
                             uint32_t max_clusters_per_instance,
                             uint32_t max_group_count_x, uint32_t& groups,
                             std::string& error);
}  // namespace vk_scene_detail

static_assert(sizeof(DrawCommand) == sizeof(VkDrawIndirectCommand),
              "DrawCommand must match VkDrawIndirectCommand");
static_assert(offsetof(DrawCommand, vertex_count) ==
              offsetof(VkDrawIndirectCommand, vertexCount));
static_assert(offsetof(DrawCommand, instance_count) ==
              offsetof(VkDrawIndirectCommand, instanceCount));
static_assert(offsetof(DrawCommand, first_vertex) ==
              offsetof(VkDrawIndirectCommand, firstVertex));
static_assert(offsetof(DrawCommand, first_instance) ==
              offsetof(VkDrawIndirectCommand, firstInstance));

struct VkSceneLod {
    uint32_t first_vertex = 0;
    uint32_t vertex_count = 0;
    float threshold = 0.0f;
};

struct VkSceneCluster {
    matter::Float3 aabb_min{};
    matter::Float3 aabb_max{};
    float radius = 0.0f;
    std::vector<VkSceneLod> lods;
};

struct VkScenePart {
    uint64_t part_hash = 0;
    std::vector<VkSceneCluster> clusters;
};

struct VkSceneInstance {
    uint64_t part_hash = 0;
    matter::Mat4f object_to_world{};
};

struct VkCullStats {
    uint32_t frustum_culled = 0;
    uint32_t hiz_culled = 0;
    uint32_t emitted = 0;
};

class VkSceneRenderer {
public:
    struct RtInstance {
        uint64_t part_hash = 0;
        float transform[16]{};
    };

    explicit VkSceneRenderer(matter::VulkanDevice& vulkan);
    ~VkSceneRenderer();
    VkSceneRenderer(const VkSceneRenderer&) = delete;
    VkSceneRenderer& operator=(const VkSceneRenderer&) = delete;

    bool init(std::string& error);
    int ensure_part(const VkScenePart& part, std::string& error);
    void release_part(uint64_t part_hash);
    bool update_instances(const std::vector<VkSceneInstance>& instances,
                          std::string& error);
    bool dispatch_culling(const FrameMatrices& frame, matter::Float3 camera_eye,
                          float pixel_budget, std::string& error);
    bool cull_stats(VkCullStats& stats, std::string& error);
    bool readback_commands(std::vector<DrawCommand>& commands,
                           std::string& error);
    bool readback_draw_transforms(std::vector<GpuMat4>& transforms,
                                  std::string& error);
    int fill_rt_instances(std::vector<RtInstance>& output) const;
    void reset();
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    void set_test_device_limits(VkDeviceSize max_storage_buffer_range,
                                VkDeviceSize max_uniform_buffer_range,
                                VkDeviceSize max_buffer_size,
                                uint32_t max_dispatch_group_count_x);
#endif

    VkBuffer indirect_buffer() const { return commands_.buffer; }
    VkBuffer draw_transform_buffer() const { return draw_transforms_.buffer; }
    uint32_t draw_command_count() const {
        return static_cast<uint32_t>(command_template_.size());
    }
    uint32_t cluster_count() const {
        return static_cast<uint32_t>(cluster_staging_.size());
    }
    VkDeviceSize cluster_buffer_size() const { return clusters_.size; }
    VkDeviceSize command_buffer_size() const { return commands_.size; }
    VkDeviceSize draw_transform_buffer_size() const {
        return draw_transforms_.size;
    }

private:
    struct GpuCluster {
        float aabb_min[3];
        float radius;
        float aabb_max[3];
        float pad0;
        float thresholds[kVkMaxLod];
        uint32_t lod_mesh_idx[kVkMaxLod];
        uint32_t lod_count;
        uint32_t part_slot;
        uint32_t cluster_index;
        uint32_t pad1[3];
    };
    struct GpuInstance {
        GpuMat4 object_to_world;
        uint32_t part_slot;
        uint32_t base_lod;
        uint32_t cluster_start;
        uint32_t cluster_count;
    };
    static_assert(sizeof(GpuCluster) == 128);
    static_assert(sizeof(GpuInstance) == 80);

    struct PartRecord {
        uint64_t hash = 0;
        uint32_t cluster_start = 0;
        uint32_t cluster_count = 0;
        bool live = false;
    };

    struct DeviceLimits {
        VkDeviceSize max_storage_buffer_range = 0;
        VkDeviceSize max_uniform_buffer_range = 0;
        VkDeviceSize max_buffer_size = 0;
        uint32_t max_dispatch_group_count_x = 0;
        uint32_t max_draw_indirect_count = 0;
    };

    bool create_pipeline(std::string& error);
    bool ensure_buffer(matter::VkBufferResource& buffer,
                       VkDeviceSize required_size, VkBufferUsageFlags usage,
                       uint32_t set_index, uint32_t binding,
                       std::string& error);
    void update_descriptor(uint32_t set_index, uint32_t binding,
                           const matter::VkBufferResource& buffer);
    bool upload_scene_buffers(std::string& error);
    bool rebuild_command_template(std::string& error);
    bool load_device_limits(std::string& error);
    void destroy_pipeline();

    matter::VulkanDevice* vulkan_ = nullptr;
    VkDescriptorSetLayout set_layouts_[2]{};
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_sets_[2]{};
    bool initialized_ = false;

    matter::VkBufferResource frame_constants_;
    matter::VkBufferResource clusters_;
    matter::VkBufferResource instances_;
    matter::VkBufferResource commands_;
    matter::VkBufferResource draw_transforms_;
    matter::VkBufferResource stats_;

    std::vector<PartRecord> parts_;
    std::map<uint64_t, int> slot_of_;
    std::vector<GpuCluster> cluster_staging_;
    std::vector<std::vector<VkSceneLod>> cluster_lods_;
    std::vector<GpuInstance> instance_staging_;
    std::vector<uint32_t> instance_part_slots_;
    std::vector<DrawCommand> command_template_;
    std::vector<RtInstance> rt_instances_;
    uint32_t max_clusters_per_instance_ = 0;
    uint32_t draw_transform_slots_ = 0;
    DeviceLimits limits_{};
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    DeviceLimits test_limits_{};
    bool use_test_limits_ = false;
#endif
};

}  // namespace viewer
