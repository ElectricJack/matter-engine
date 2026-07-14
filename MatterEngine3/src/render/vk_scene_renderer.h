#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <limits>
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
bool checked_size_to_int(size_t count, int& result, const char* label,
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

struct VkRasterVertex {
    matter::Float3 position{};
    matter::Float3 normal{};
    matter::Float4 albedo{};
    matter::Float4 orm{};
};

struct VkScenePart {
    uint64_t part_hash = 0;
    std::vector<VkSceneCluster> clusters;
    // Cluster LOD first_vertex values are local to this array.  Parts without
    // raster vertices retain the Task 7 absolute first_vertex contract.
    std::vector<VkRasterVertex> vertices;
};

struct VkSceneInstance {
    uint64_t part_hash = 0;
    matter::Mat4f object_to_world{};
};

struct VkCullStats {
    uint32_t frustum_culled = 0;
    uint32_t hiz_culled = 0;
    uint32_t emitted = 0;
    uint32_t overflowed = 0;
};

struct VkRasterAttachment {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

struct VkRasterAttachments {
    VkRasterAttachment albedo{};
    VkRasterAttachment normal{};
    VkRasterAttachment orm{};
    VkRasterAttachment depth{};
    // R16G16B16A16_SFLOAT is the explicit linear HDR composite format.
    VkRasterAttachment hdr{};
    VkExtent2D extent{};
};

struct VkRasterPixel {
    matter::Float4 albedo{};
    matter::Float4 normal{};
    matter::Float4 orm{};
    matter::Float4 hdr{};
    float depth = 1.0f;
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
    bool render_gbuffer_and_composite(uint32_t width, uint32_t height,
                                      std::string& error);
    VkRasterAttachments raster_attachments() const;
    bool readback_raster_pixel(uint32_t x, uint32_t y,
                               VkRasterPixel& pixel, std::string& error);
    int fill_rt_instances(std::vector<RtInstance>& output) const;
    // A poisoned renderer fails closed. reset() then performs a full GPU
    // resource/pipeline teardown, clears the poison, and requires re-init
    // (explicitly or through the next dispatch_culling call) before reuse.
    void reset();
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    void set_test_device_limits(VkDeviceSize max_storage_buffer_range,
                                VkDeviceSize max_uniform_buffer_range,
                                VkDeviceSize max_buffer_size,
                                uint32_t max_dispatch_group_count_x,
                                uint32_t max_draw_indirect_count);
    void clear_test_device_limits(std::string& error);
    bool set_test_command_first_instance(uint32_t command_index,
                                         uint32_t first_instance,
                                         std::string& error);
    void set_test_scene_failure(uint32_t fail_after_replacements,
                                uint32_t fail_after_uploads);
#endif

    VkBuffer indirect_buffer() const {
        return poisoned() ? VK_NULL_HANDLE : commands_.buffer;
    }
    VkBuffer draw_transform_buffer() const {
        return poisoned() ? VK_NULL_HANDLE : draw_transforms_.buffer;
    }
    uint32_t draw_command_count() const {
        return poisoned() ? 0 : uploaded_command_count_;
    }
    uint32_t cluster_count() const {
        return poisoned() ? 0 : uploaded_cluster_count_;
    }
    VkDeviceSize cluster_buffer_size() const {
        return poisoned() ? 0 : clusters_.size;
    }
    VkDeviceSize command_buffer_size() const {
        return poisoned() ? 0 : commands_.size;
    }
    VkDeviceSize draw_transform_buffer_size() const {
        return poisoned() ? 0 : draw_transforms_.size;
    }
    uint32_t raster_vertex_count() const {
        return poisoned() ? 0
                          : static_cast<uint32_t>(vertex_staging_.size());
    }
    VkDeviceSize raster_vertex_buffer_size() const {
        return poisoned() ? 0 : vertices_.size;
    }
    uint32_t raster_draw_command_count() const {
        return poisoned() ? 0 : raster_draw_command_count_;
    }
    uint32_t uploaded_raster_draw_command_count() const {
        return poisoned() ? 0 : uploaded_raster_draw_command_count_;
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
        uint32_t vertex_start = 0;
        uint32_t vertex_count = 0;
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
    bool create_raster_pipelines(std::string& error);
    bool ensure_raster_targets(uint32_t width, uint32_t height,
                               std::string& error);
    bool ensure_vertex_buffer(VkDeviceSize required_size,
                              std::string& error, bool* replaced = nullptr);
    bool ensure_buffer(matter::VkBufferResource& buffer,
                       VkDeviceSize required_size, VkBufferUsageFlags usage,
                       uint32_t set_index, uint32_t binding,
                       std::string& error, bool* replaced = nullptr);
    void update_descriptor(uint32_t set_index, uint32_t binding,
                           const matter::VkBufferResource& buffer);
    bool upload_scene_buffers(std::string& error);
    bool rebuild_command_template(std::string& error);
    bool load_device_limits(std::string& error);
    bool fail_if_poisoned(std::string& error) const;
    bool poison(std::string& error);
    bool poisoned() const { return !poison_reason_.empty(); }
    void destroy_pipeline();

    matter::VulkanDevice* vulkan_ = nullptr;
    VkDescriptorSetLayout set_layouts_[2]{};
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline raster_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout composite_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout composite_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline composite_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool composite_descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet composite_descriptor_set_ = VK_NULL_HANDLE;
    VkSampler composite_sampler_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_sets_[2]{};
    bool initialized_ = false;

    matter::VkBufferResource frame_constants_;
    matter::VkBufferResource clusters_;
    matter::VkBufferResource instances_;
    matter::VkBufferResource commands_;
    matter::VkBufferResource draw_transforms_;
    matter::VkBufferResource stats_;
    matter::VkBufferResource vertices_;

    matter::VkImageResource albedo_;
    matter::VkImageResource normal_;
    matter::VkImageResource orm_;
    matter::VkImageResource depth_;
    matter::VkImageResource hdr_;
    VkExtent2D raster_extent_{};
    bool raster_attachments_ready_ = false;

    std::vector<PartRecord> parts_;
    std::map<uint64_t, int> slot_of_;
    std::vector<GpuCluster> cluster_staging_;
    std::vector<std::vector<VkSceneLod>> cluster_lods_;
    std::vector<GpuInstance> instance_staging_;
    std::vector<uint32_t> instance_part_slots_;
    std::vector<DrawCommand> command_template_;
    std::vector<uint8_t> raster_command_enabled_;
    std::vector<uint8_t> uploaded_raster_command_enabled_;
    std::vector<RtInstance> rt_instances_;
    std::vector<VkRasterVertex> vertex_staging_;
    uint32_t uploaded_vertex_count_ = 0;
    uint32_t raster_draw_command_count_ = 0;
    uint32_t uploaded_raster_draw_command_count_ = 0;
    uint32_t max_clusters_per_instance_ = 0;
    uint32_t draw_transform_slots_ = 0;
    uint32_t uploaded_command_count_ = 0;
    uint32_t uploaded_transform_slots_ = 0;
    uint32_t uploaded_cluster_count_ = 0;
    std::vector<RtInstance> uploaded_rt_instances_;
    std::string poison_reason_;
    DeviceLimits limits_{};
    DeviceLimits physical_limits_{};
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    DeviceLimits test_limits_{};
    bool use_test_limits_ = false;
    uint32_t test_fail_after_replacements_ =
        std::numeric_limits<uint32_t>::max();
    uint32_t test_fail_after_uploads_ =
        std::numeric_limits<uint32_t>::max();
#endif
};

}  // namespace viewer
