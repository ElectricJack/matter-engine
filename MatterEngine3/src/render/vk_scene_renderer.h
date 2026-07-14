#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <map>
#include <limits>
#include <string>
#include <vector>

#include "frame_matrices.h"
#include "gpu_matrix_pack.h"
#include "matter/math_types.h"
#include "vk_draw_command.h"
#include "vk_resources.h"
#include "vk_temporal.h"

namespace matter {
class VulkanDevice;
struct VulkanFrame;
}

namespace viewer {

constexpr uint32_t kVkMaxLod = 9;

// Emission is stored as log2(1 + strength) in the alpha channel of the
// R16G16B16A16_SFLOAT normal attachment. 15.875 is exactly representable in
// binary16 and decodes to about 60096, leaving headroom below binary16's 65504
// maximum for the remaining composite terms. Larger finite authored values
// saturate monotonically at that finite HDR strength; invalid/non-positive
// values are defined as zero.
constexpr float kVkMaxEncodedEmission = 15.875f;
inline float vulkan_encode_emission(float emission) {
    if (!(emission > 0.0f) || !std::isfinite(emission)) return 0.0f;
    return std::fmin(std::log2(1.0f + emission), kVkMaxEncodedEmission);
}

inline bool vulkan_material_uses_unsupported_texture(float packed_slot) {
    return std::isfinite(packed_slot) && packed_slot >= 0.0f;
}

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
    // Stable identity within the owning world session. Zero selects the
    // renderer's deterministic input-order fallback for legacy callers.
    uint64_t instance_id = 0;
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
    VkRasterAttachment velocity{};
    VkRasterAttachment depth{};
    // R16G16B16A16_SFLOAT is the explicit linear HDR composite format.
    VkRasterAttachment hdr{};
    VkExtent2D extent{};
};

struct VkRasterPixel {
    matter::Float4 albedo{};
    matter::Float4 normal{};
    matter::Float4 orm{};
    matter::Float3 velocity{};
    matter::Float4 hdr{};
    float depth = 1.0f;
};

struct VkSceneLighting {
    // Direction from the sun toward the scene, matching WorldLights.
    matter::Float3 sun_direction{-0.45f, -0.80f, -0.35f};
    float sun_intensity = 1.0f;
    matter::Float3 sun_color{2.2f, 2.05f, 1.8f};
    float pad0 = 0.0f;
    matter::Float3 sky_color{0.38f, 0.43f, 0.52f};
    float pad1 = 0.0f;
};

struct VkSceneUploadCounters {
    uint64_t vertex_uploads = 0;
    uint64_t cluster_uploads = 0;
    uint64_t instance_uploads = 0;
    uint64_t command_uploads = 0;
    uint64_t command_layout_rebuilds = 0;
};

struct PartCommandRange {
    uint32_t first_command = 0;
    uint32_t command_count = 0;
    uint32_t part_slot = 0;
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
    void set_temporal_frame(const TemporalFrame& frame) { temporal_frame_ = frame; }
    bool prepare_frame(const matter::VulkanFrame& frame,
                       const FrameMatrices& matrices,
                       matter::Float3 camera_eye, float pixel_budget,
                       std::string& error);
    bool record_cull_and_render(const matter::VulkanFrame& frame,
                                const FrameMatrices& matrices,
                                matter::Float3 camera_eye,
                                float pixel_budget, std::string& error);
    const std::vector<PartCommandRange>& test_recorded_draw_ranges() const {
        return recorded_draw_ranges_;
    }
    VkSceneUploadCounters upload_counters() const noexcept {
        return upload_counters_;
    }
    VkCullStats cached_cull_stats() const noexcept { return cached_stats_; }
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    // Immediate submit/readback diagnostics are intentionally test-only. The
    // production path records through record_cull_and_render above.
    bool dispatch_culling(const FrameMatrices& frame, matter::Float3 camera_eye,
                          float pixel_budget, std::string& error);
    bool cull_stats(VkCullStats& stats, std::string& error);
    bool readback_commands(std::vector<DrawCommand>& commands,
                           std::string& error);
    bool readback_draw_transforms(std::vector<GpuMat4>& transforms,
                                  std::string& error);
    bool render_gbuffer_and_composite(uint32_t width, uint32_t height,
                                      std::string& error);
#endif
    void set_lighting(const VkSceneLighting& lighting) { lighting_ = lighting; }
    // Blit the real HDR world composite into the currently acquired swapchain
    // image, leaving it ready for UI dynamic rendering.
    bool record_composite_to_swapchain(const matter::VulkanFrame& frame,
                                       std::string& error);
    VkRasterAttachments raster_attachments() const;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    bool readback_raster_pixel(uint32_t x, uint32_t y,
                               VkRasterPixel& pixel, std::string& error);
#endif
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
    void set_test_frame_resource_failure(uint32_t fail_after_allocations);
#endif

    VkBuffer indirect_buffer() const {
        return poisoned() || frames_.empty()
                   ? VK_NULL_HANDLE
                   : frames_[active_frame_index_].commands.buffer;
    }
    VkBuffer draw_transform_buffer() const {
        return poisoned() || frames_.empty()
                   ? VK_NULL_HANDLE
                   : frames_[active_frame_index_].draw_transforms.buffer;
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
        return poisoned() || frames_.empty()
                   ? 0
                   : frames_[active_frame_index_].commands.size;
    }
    VkDeviceSize draw_transform_buffer_size() const {
        return poisoned() || frames_.empty()
                   ? 0
                   : frames_[active_frame_index_].draw_transforms.size;
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
        GpuMat4 previous_object_to_world;
        uint32_t part_slot;
        uint32_t base_lod;
        uint32_t cluster_start;
        uint32_t cluster_count;
        uint32_t history_valid;
        uint32_t pad[3];
    };
    struct GpuDrawTransform {
        GpuMat4 current;
        GpuMat4 previous;
        uint32_t history_valid;
        uint32_t pad[3];
    };
    static_assert(sizeof(GpuCluster) == 128);
    static_assert(sizeof(GpuInstance) == 160);
    static_assert(sizeof(GpuDrawTransform) == 144);

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

    struct FrameResources {
        matter::VkBufferResource frame_constants;
        matter::VkBufferResource instances;
        matter::VkBufferResource commands;
        matter::VkBufferResource draw_transforms;
        matter::VkBufferResource stats;
        VkDescriptorSet descriptor_sets[2]{};
        uint64_t static_generation = 0;
        uint64_t instance_generation = 0;
        uint64_t command_generation = 0;
        bool stats_valid = false;
    };

    bool create_pipeline(std::string& error);
    bool create_raster_pipelines(std::string& error);
    bool ensure_raster_targets(uint32_t width, uint32_t height,
                               std::string& error);
    bool ensure_vertex_buffer(VkDeviceSize required_size,
                              std::string& error, bool* replaced = nullptr);
    bool ensure_buffer(matter::VkBufferResource& buffer,
                       VkDeviceSize required_size, VkBufferUsageFlags usage,
                       std::string& error, bool* replaced = nullptr);
    void update_descriptor(VkDescriptorSet set, uint32_t binding,
                           VkDescriptorType type,
                           const matter::VkBufferResource& buffer);
    bool ensure_frame_resources(uint32_t frame_slot_count,
                                std::string& error);
    void update_frame_descriptors(FrameResources& frame);
    bool upload_scene_buffers(FrameResources& frame, bool reset_stats,
                              std::string& error);
    bool upload_frame_constants(FrameResources& frame,
                                const FrameMatrices& matrices,
                                matter::Float3 camera_eye,
                                float pixel_budget, std::string& error);
    bool validate_draw_command_regions(std::string& error) const;
    void note_command_layout_rebuild();
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
    bool initialized_ = false;

    matter::VkBufferResource clusters_;
    matter::VkBufferResource vertices_;
    std::vector<FrameResources> frames_;
    uint32_t active_frame_index_ = 0;
    uint32_t frame_resource_slot_capacity_ = 0;

    matter::VkImageResource albedo_;
    matter::VkImageResource normal_;
    matter::VkImageResource orm_;
    matter::VkImageResource velocity_;
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
    std::vector<uint32_t> part_instance_counts_;
    std::vector<DrawCommand> command_template_;
    std::vector<PartCommandRange> part_command_ranges_;
    std::vector<PartCommandRange> recorded_draw_ranges_;
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
    VkSceneLighting lighting_{};
    TemporalFrame temporal_frame_{};
    uint64_t instance_generation_ = 1;
    uint64_t static_generation_ = 1;
    uint64_t command_generation_ = 1;
    bool static_upload_dirty_ = true;
    VkSceneUploadCounters upload_counters_{};
    VkCullStats cached_stats_{};
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    DeviceLimits test_limits_{};
    bool use_test_limits_ = false;
    uint32_t test_fail_after_replacements_ =
        std::numeric_limits<uint32_t>::max();
    uint32_t test_fail_after_uploads_ =
        std::numeric_limits<uint32_t>::max();
    uint32_t test_fail_after_frame_resource_allocations_ =
        std::numeric_limits<uint32_t>::max();
#endif
};

}  // namespace viewer
