#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <map>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "frame_matrices.h"
#include "gpu_matrix_pack.h"
#include "matter/lod_contract.h"
#include "matter/math_types.h"
#include "material_registry.h"
#include "render/dynamic_instance_slots.h"
#include "vk_gi_contract.h"
#include "vk_draw_command.h"
#include "vk_resources.h"
#include "vk_temporal.h"

namespace matter {
class VulkanDevice;
class StreamlineBridge;
enum class DlssMode : uint8_t;
struct VulkanFrame;
struct VulkanVolumetricsSettings;
struct FogSettings;
}

namespace viewer {

class VkVolumetrics;

constexpr uint32_t kVkMaxLod =
    static_cast<uint32_t>(matter::kMaxSerializedLodLevels);
static_assert(kVkMaxLod == 9u,
              "update shaders_vk/cull.comp MAX_LOD with the shared capacity");

inline uint32_t vulkan_history_token(uint64_t instance_id) {
    const uint32_t folded = static_cast<uint32_t>(instance_id) ^
                            static_cast<uint32_t>(instance_id >> 32);
    return folded != 0 ? folded : 1u;
}

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
VkShaderStageFlags scene_binding_stage_flags(uint32_t binding) noexcept;
bool scene_storage_limits_supported(uint32_t max_per_stage,
                                    uint32_t max_per_set) noexcept;
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
size_t frame_constants_size_for_test() noexcept;
VkPipelineStageFlags2 ray_depth_destination_stages(
    bool native_ray_tracing_available) noexcept;
VkPipelineStageFlags2 gbuffer_sampled_stages_for_test(
    uint32_t attachment_index, bool native_ray_tracing_available) noexcept;
}  // namespace vk_scene_detail

static_assert(sizeof(DrawCommand) == sizeof(VkDrawIndexedIndirectCommand),
              "DrawCommand must match VkDrawIndexedIndirectCommand");
static_assert(offsetof(DrawCommand, index_count) ==
              offsetof(VkDrawIndexedIndirectCommand, indexCount));
static_assert(offsetof(DrawCommand, instance_count) ==
              offsetof(VkDrawIndexedIndirectCommand, instanceCount));
static_assert(offsetof(DrawCommand, first_index) ==
              offsetof(VkDrawIndexedIndirectCommand, firstIndex));
static_assert(offsetof(DrawCommand, vertex_offset) ==
              offsetof(VkDrawIndexedIndirectCommand, vertexOffset));
static_assert(offsetof(DrawCommand, first_instance) ==
              offsetof(VkDrawIndexedIndirectCommand, firstInstance));

struct VkSceneLod {
    // first_index/index_count are part-local (into VkScenePart::indices).
    // ensure_part rebases first_index to the global index_staging_ offset.
    uint32_t first_index = 0;   // into VkScenePart::indices (part-local until ensure_part rebases)
    uint32_t index_count = 0;   // 3 × triangle count
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
    matter::Float4 tint{};
    matter::Float4 surface{};
    uint32_t material_index = UINT32_MAX;
    uint32_t pad[3]{};
};

struct VkScenePart {
    uint64_t part_hash = 0;
    std::vector<VkSceneCluster> clusters;
    std::vector<VkRasterVertex> vertices;   // unique vertices, all LOD meshes concatenated
    // Index buffer: values are PART-LOCAL (already include each mesh's vertex
    // offset within the part).  Global vertex rebase is done by vertexOffset /
    // base addresses (Tasks 4-5); ensure_part never rewrites these values.
    std::vector<uint32_t> indices;
};

namespace vk_scene_detail {
struct RtGeometrySelection {
    uint32_t cluster_index = 0;
    uint32_t lod_index = 0;
    uint32_t first_index = 0;   // part-local index into VkScenePart::indices
    uint32_t index_count = 0;   // 3 × triangle count
};

uint32_t select_scene_cluster_lod(const VkSceneCluster& cluster,
                                  const matter::Mat4f& object_to_world,
                                  matter::Float3 camera_eye,
                                  float pixel_budget) noexcept;
uint32_t select_cluster_lod_view(const matter::Float3& aabb_min,
                                 const matter::Float3& aabb_max,
                                 float radius, const float* thresholds,
                                 uint32_t lod_count,
                                 const matter::Mat4f& object_to_world,
                                 matter::Float3 camera_eye,
                                 float pixel_budget) noexcept;
std::vector<uint32_t> dense_rt_lod_offsets(const VkScenePart& part);
bool dense_rt_lod_index(const std::vector<uint32_t>& offsets,
                        uint32_t cluster_index, uint32_t lod_index,
                        uint32_t& record_index) noexcept;
std::vector<RtGeometrySelection> select_rt_instance_geometry(
    const VkScenePart& part, const matter::Mat4f& object_to_world,
    matter::Float3 camera_eye, float pixel_budget);
}  // namespace vk_scene_detail

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
    uint32_t triangles = 0;
    uint32_t batches = 0;
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
    VkRasterAttachment material_instance{};
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
    uint32_t material_index = UINT32_MAX;
    uint32_t instance_token = UINT32_MAX;
    matter::Float4 hdr{};
    float depth = 1.0f;
    matter::Float3 visibility{1.0f, 1.0f, 1.0f};
    matter::Float4 raw_diffuse{};
    matter::Float4 accumulated_diffuse{};
    matter::Float4 raw_specular{};
    matter::Float4 accumulated_specular{};
};

#ifdef MATTER_VK_TEST_FAULT_INJECTION
    struct RtGeometryDebugRecord {
        uint64_t part_hash = 0;
        uint32_t cluster_index = 0;
        uint32_t lod_index = 0;
        uint32_t custom_index = 0;
        uint32_t first_index = 0;
        uint32_t index_count = 0;
        VkDeviceAddress vertex_address = 0;
        VkDeviceAddress blas_address = 0;
        bool opaque = false;
        bool built_this_frame = false;
    };
struct RtTraceCounters {
    uint32_t invalid_part_records = 0;
    uint32_t any_hit_invocations = 0;
    uint32_t any_hit_layers = 0;
    uint32_t capped_rays = 0;
};
static_assert(sizeof(RtTraceCounters) == 16);

constexpr uint32_t kRtSurfaceValid = 1u;
constexpr uint32_t kRtSurfaceFrontFace = 2u;
struct RtSurfaceHit {
    bool valid = false;
    uint32_t part_slot = UINT32_MAX;
    uint32_t primitive = UINT32_MAX;
    uint32_t material_index = UINT32_MAX;
    matter::Float3 position{};
    matter::Float3 normal{};
    matter::Float4 tint{};
    float uv[2]{};
    float baked_ao = 1.0f;
    float hit_t = 0.0f;
    uint32_t flags = 0;
};

struct GiTemporalGpuFixture {
    uint32_t signal_mode = 0;
    matter::Float4 raw{0.25f, 0.5f, 0.75f, 1.0f};
    matter::Float3 raw_aux{1.0f, 0.5f, 0.0f};
    matter::Float3 velocity{};
    float depth = 0.5f;
    matter::Float4 normal{0.0f, 0.0f, 1.0f, 0.0f};
    uint32_t material_index = 7;
    uint32_t instance_token = 41;
    matter::Float4 previous_radiance{0.25f, 0.5f, 0.75f, 1.0f};
    matter::Float3 previous_moments{};
    uint32_t previous_history_length = 3;
    float previous_depth = 0.5f;
    matter::Float4 previous_normal{0.0f, 0.0f, 1.0f, 0.0f};
    uint32_t previous_material_index = 7;
    uint32_t previous_instance_token = 41;
    matter::Float3 previous_aux{1.0f, 0.5f, 0.0f};
    GiPixelCoord output_pixel{3, 3};
    GiPixelCoord history_patch_pixel{3, 3};
    bool reset = false;
};

struct GiTemporalGpuResult {
    matter::Float4 radiance{};
    matter::Float3 moments{};
    uint32_t history_length = 0;
    uint32_t rejection_bits = 0;
    matter::Float3 aux{};
};

struct GiAtrousGpuFixture {
    VkExtent2D extent{9, 9};
    std::vector<matter::Float4> signal;
    std::vector<matter::Float3> moments;
    std::vector<float> depth;
    std::vector<matter::Float4> normal;
    std::vector<uint32_t> material_index;
    std::vector<uint32_t> history_length;
};

struct GiAtrousGpuResult {
    std::vector<matter::Float4> filtered;
    std::vector<matter::Float4> penultimate;
    std::array<uint32_t, 5> gpu_step_widths{};
};
#endif

struct VkSceneLighting {
    // Direction from the sun toward the scene, matching WorldLights.
    matter::Float3 sun_direction{-0.45f, -0.80f, -0.35f};
    float sun_intensity = 1.0f;
    matter::Float3 sun_color{2.2f, 2.05f, 1.8f};
    float diffuse_rt_multiplier = 0.0f;
    matter::Float3 sky_color{0.38f, 0.43f, 0.52f};
    float emission_multiplier = 1.0f;
    float debug_view = 0.0f;
    float camera_fwd_x = 0.0f;
    float camera_fwd_y = 0.0f;
    float camera_fwd_z = -1.0f;
    float tan_half_fov = 1.0f;
    float aspect_ratio = 1.0f;
    float jitter_offset_u = 0.0f;
    float jitter_offset_v = 0.0f;
    float vol_enabled = 0.0f;
    float vol_debug_view = 0.0f;
};
static_assert(sizeof(VkSceneLighting) == 88);

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
    bool update_materials(const std::vector<MaterialGpuRecord>& records,
                          uint64_t shading_revision,
                          uint64_t geometry_revision, std::string& error);
    bool consume_gi_history_reset();
    bool rt_geometry_classification_dirty(uint64_t part_hash) const;
    void release_part(uint64_t part_hash);
    bool update_instances(const std::vector<VkSceneInstance>& instances,
                          std::string& error);
    // Dynamic lane (Task 7): consumes CPU-side slot changes produced by
    // matter::render::DynamicInstanceSlots. submit_serial identifies the GPU
    // frame this batch of changes will be submitted with; finish_dynamic_frame
    // reports back the highest completed serial so callers can safely reuse
    // retired slots.
    bool update_dynamic_instances(const matter::render::DynamicSlotChange* changes,
                                  uint32_t count,
                                  uint64_t submit_serial,
                                  std::string& error);
    void finish_dynamic_frame(uint64_t completed_serial);
    void set_temporal_frame(const TemporalFrame& frame) { temporal_frame_ = frame; }
    void set_dlss_mode(matter::DlssMode mode);
    VkExtent2D dlss_internal_extent(VkExtent2D output_extent) const;
    matter::DlssMode selected_dlss_mode() const { return selected_dlss_mode_; }
    matter::DlssMode active_dlss_mode() const;
    const std::string& dlss_reason() const;
    uint64_t dlss_reset_count() const { return dlss_reset_count_; }
    bool rt_available_observed() const { return last_rt_available_; }
    bool rt_effective_observed() const { return last_rt_effective_; }
    uint32_t rt_trace_dispatches_observed() const {
        return last_rt_trace_dispatches_;
    }
    uint32_t rt_samples_observed() const { return last_rt_samples_; }
    bool rt_debug_view_observed() const { return last_rt_debug_view_; }
    const std::string& rt_fallback_reason_observed() const {
        return last_rt_fallback_reason_;
    }
    bool consume_dlss_history_reset();
    bool prepare_frame(const matter::VulkanFrame& frame,
                       const FrameMatrices& matrices,
                       matter::Float3 camera_eye, float pixel_budget,
                       std::string& error);
    bool record_cull_and_render(const matter::VulkanFrame& frame,
                                const FrameMatrices& matrices,
                                matter::Float3 camera_eye,
                                float pixel_budget, std::string& error);
    bool record_ray_traced_shadows(const matter::VulkanFrame& frame,
                                   const FrameMatrices& matrices,
                                   matter::Float3 camera_eye,
                                   float pixel_budget,
                                   VkExtent2D trace_extent,
                                   std::string& error);
    void finish_ray_tracing_frame(uint64_t frame_serial, bool succeeded);
    const std::vector<PartCommandRange>& test_recorded_draw_ranges() const {
        return recorded_draw_ranges_;
    }
    VkSceneUploadCounters upload_counters() const noexcept {
        return upload_counters_;
    }
    VkCullStats cached_cull_stats() const noexcept { return cached_stats_; }
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    const std::vector<RtGeometryDebugRecord>&
    test_last_rt_geometry_records() const {
        return test_last_rt_geometry_records_;
    }
    uint32_t test_last_rt_blas_build_count() const {
        return test_last_rt_blas_build_count_;
    }
    void set_test_dlss_bridge(matter::StreamlineBridge bridge);
    bool test_uses_device_streamline_bridge() const;
    VkImage test_dlss_output_image(uint32_t frame_slot) const {
        return frame_slot < frames_.size()
                   ? frames_[frame_slot].dlss_output.image
                   : VK_NULL_HANDLE;
    }
    std::weak_ptr<void> test_dlss_output_lifetime(uint32_t frame_slot) const;
    bool test_replace_dlss_output(uint32_t frame_slot, VkExtent2D extent,
                                  std::string& error);
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
    void set_lighting(const VkSceneLighting& lighting);
    void set_display_exposure(float exposure_ev);
    void set_composite_debug_view(float mode) { composite_debug_override_ = mode; }
    void set_ray_tracing_settings(
        const matter::VulkanRayTracingSettings& settings);
    void set_gi_settings(const matter::VulkanGiSettings& settings) {
        gi_settings_ = settings;
        gi_settings_.max_bounces = 1u;
        gi_settings_.samples_per_pixel = 1u;
        gi_settings_.trace_scale = std::max(0.125f, std::min(settings.trace_scale, 1.0f));
    }
    void set_volumetrics_settings(const matter::VulkanVolumetricsSettings& s,
                                  const matter::FogSettings& fog);
    // Blit the real HDR world composite into the currently acquired swapchain
    // image, leaving it ready for UI dynamic rendering.
    bool record_composite_to_swapchain(const matter::VulkanFrame& frame,
                                       std::string& error);
    VkRasterAttachments raster_attachments() const;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    void test_force_rt_unavailable(bool unavailable) {
        test_force_rt_unavailable_ = unavailable;
    }
    bool readback_raster_pixel(uint32_t x, uint32_t y,
                               VkRasterPixel& pixel, std::string& error);
    bool readback_materials(std::vector<MaterialGpuRecord>& records,
                            std::string& error);
    uint64_t test_material_upload_record_count(uint32_t frame_slot) const {
        return frame_slot < frames_.size()
                   ? frames_[frame_slot].material_upload_record_count
                   : 0;
    }
    VkMemoryPropertyFlags test_material_buffer_memory(
        uint32_t frame_slot) const {
        return frame_slot < frames_.size()
                   ? frames_[frame_slot].materials.memory_properties
                   : 0;
    }
    VkMemoryPropertyFlags test_material_staging_memory(
        uint32_t frame_slot) const {
        return frame_slot < frames_.size()
                   ? frames_[frame_slot].material_upload.memory_properties
                   : 0;
    }
    VkDeviceAddress test_rt_geometry_address(uint64_t part_hash) const;
    bool record_test_surface_ray(const matter::VulkanFrame& frame,
                                 matter::Float3 origin,
                                 matter::Float3 direction,
                                 uint32_t invalid_part_slot,
                                 std::string& error);
    bool readback_test_surface_hit(uint32_t frame_slot, RtSurfaceHit& hit,
                                   uint32_t& invalid_count,
                                   std::string& error);
    bool readback_rt_trace_counters(uint32_t frame_slot,
                                    RtTraceCounters& counters,
                                    std::string& error);
    bool test_readback_reflection_sample_counts(uint32_t& base_samples,
                                                uint32_t& coat_samples,
                                                std::string& error);
    bool test_dispatch_gi_temporal_fixture(
        const GiTemporalGpuFixture& fixture, GiTemporalGpuResult& result,
        std::string& error);
    bool test_dispatch_gi_atrous_fixture(
        const GiAtrousGpuFixture& fixture, GiAtrousGpuResult& result,
        std::string& error);
    bool test_record_hdr_constant(const matter::VulkanFrame& frame,
                                  matter::Float3 color,
                                  std::string& error);
    bool test_rt_blas_built(uint64_t part_hash) const;
    uint64_t test_rt_blas_candidate_serial(uint64_t part_hash) const;
    std::weak_ptr<void> test_rt_blas_lifetime(uint64_t part_hash) const;
    VkDeviceAddress test_rt_sbt_address() const { return rt_sbt_address_; }
    VkDeviceAddress test_rt_test_raygen_address() const {
        return rt_sbt_test_raygen_address_;
    }
    VkDeviceSize test_rt_sbt_stride() const { return rt_sbt_stride_; }
    VkDeviceAddress test_rt_miss_address() const { return rt_sbt_miss_address_; }
    VkDeviceAddress test_rt_hit_address() const { return rt_sbt_hit_address_; }
    VkDeviceSize test_rt_miss_region_size() const { return rt_sbt_miss_size_; }
    VkDeviceSize test_rt_hit_region_size() const { return rt_sbt_hit_size_; }
    uint32_t test_surface_trace_dispatches() const {
        return test_surface_trace_dispatches_;
    }
    VkDeviceAddress test_rt_scratch_address(uint32_t frame_slot) const;
    uint32_t test_last_rt_samples() const { return last_rt_samples_; }
    bool test_last_rt_debug_view() const { return last_rt_debug_view_; }
    VkImageUsageFlags test_visibility_usage() const {
        return visibility_usage_;
    }
    VkFormat test_visibility_format() const { return visibility_.format; }
    VkFormat test_raw_diffuse_format() const { return raw_diffuse_.format; }
    VkExtent2D test_raw_diffuse_extent() const { return raw_diffuse_extent_; }
    uint32_t test_gi_presented_history_index() const {
        return gi_presented_history_index_;
    }
    uint32_t test_gi_candidate_history_index() const {
        return gi_candidate_history_index_;
    }
    bool test_composite_uses_gi_temporal() const {
        return last_composite_used_gi_temporal_;
    }
    uint64_t test_gi_history_reset_count() const {
        return gi_history_reset_count_;
    }
    uint32_t test_gi_samples_per_pixel() const {
        return gi_settings_.samples_per_pixel;
    }
    float test_shadow_visibility_for_ray(bool occluded) const {
        return ray_tracing_settings_.enabled && occluded ? 0.0f : 1.0f;
    }
    // Returns the first_index of rt_lods[rt_lod_index] for the given part hash,
    // or UINT32_MAX if the part is not found or the index is out of range.
    uint32_t test_rt_lod_first_index(uint64_t part_hash,
                                     uint32_t rt_lod_index) const {
        const auto found = slot_of_.find(part_hash);
        if (found == slot_of_.end()) return UINT32_MAX;
        const PartRecord& part = parts_[static_cast<size_t>(found->second)];
        if (rt_lod_index >= part.rt_lods.size()) return UINT32_MAX;
        return part.rt_lods[rt_lod_index].first_index;
    }
#endif
    int fill_rt_instances(std::vector<RtInstance>& output) const;

    // GPU timer results (ms, EMA-smoothed). Zones are non-overlapping;
    // each begin is recorded after the previous zone's end.
    // 0=total 1=cull 2=gbuffer 3=blas 4=tlas 5=rt 6=denoise 7=dlss 8=composite
    static constexpr uint32_t kGpuZoneTotal        = 0;
    static constexpr uint32_t kGpuZoneCull         = 1;
    static constexpr uint32_t kGpuZoneGBuffer      = 2;
    static constexpr uint32_t kGpuZoneBlas         = 3;
    static constexpr uint32_t kGpuZoneTlas         = 4;
    static constexpr uint32_t kGpuZoneRt           = 5;
    static constexpr uint32_t kGpuZoneDenoise      = 6;
    static constexpr uint32_t kGpuZoneDlss         = 7;
    static constexpr uint32_t kGpuZoneComposite    = 8;
    static constexpr uint32_t kGpuZoneVolumetrics  = 9;
    static constexpr uint32_t kGpuZoneCount        = 10;
    bool gpu_timers_supported() const { return gpu_timers_supported_; }
    float gpu_zone_ms(uint32_t zone) const {
        return zone < kGpuZoneCount ? gpu_smoothed_ms_[zone] : 0.0f;
    }
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
        uint32_t instance_token;
        uint32_t pad[2];
    };
    struct GpuDrawTransform {
        GpuMat4 current;
        GpuMat4 previous;
        uint32_t history_valid;
        uint32_t instance_token;
        uint32_t pad[2];
    };
    static_assert(sizeof(GpuCluster) == 128);
    static_assert(sizeof(GpuInstance) == 160);
    static_assert(sizeof(GpuDrawTransform) == 144);

    struct RtLodRecord {
        uint32_t cluster_index = 0;
        uint32_t lod_index = 0;
        // first_index is part-local (NOT rebased; stored this way so compaction
        // in release_part does not invalidate surviving parts' rt_lods).
        // Consumers address the per-part rt_index buffer directly via this offset.
        uint32_t first_index = 0;    // part-local index into rt_index buffer
        uint32_t index_count = 0;    // 3 × triangle count
        uint32_t primitive_count = 0;
        std::shared_ptr<matter::VkAccelerationStructureResource> blas;
        std::shared_ptr<matter::VkAccelerationStructureResource> candidate;
        bool built = false;
        bool geometry_opaque = false;
        bool candidate_opaque = false;
        uint64_t candidate_serial = 0;
        std::vector<uint32_t> material_ids;
    };

    struct PartRecord {
        uint64_t hash = 0;
        uint32_t cluster_start = 0;
        uint32_t cluster_count = 0;
        uint32_t vertex_start = 0;   // kept for Task 4 vertexOffset; NOT folded into lod offsets
        uint32_t vertex_count = 0;
        uint32_t index_start = 0;    // global offset into index_staging_
        uint32_t index_count = 0;
        bool live = false;
        std::shared_ptr<matter::VkBufferResource> rt_geometry;
        std::shared_ptr<matter::VkBufferResource> rt_index;
        std::vector<RtLodRecord> rt_lods;
        std::vector<uint32_t> rt_cluster_lod_offsets;
        std::vector<uint32_t> material_ids;
        bool rt_geometry_classification_dirty = false;
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
        matter::VkBufferResource material_upload;
        matter::VkBufferResource materials;
        matter::VkImageResource dlss_output;
        matter::VkBufferResource rt_instances;
        matter::VkBufferResource rt_scratch;
        matter::VkBufferResource rt_tlas_scratch;
        matter::VkAccelerationStructureResource rt_tlas;
        matter::VkBufferResource rt_parts;
        matter::VkBufferResource rt_error_counter;
        matter::VkBufferResource rt_test_output;
        matter::VkBufferResource gi_atrous_markers;
        VkExtent2D dlss_output_extent{};
        VkDescriptorSet descriptor_sets[2]{};
        VkDescriptorSet composite_descriptor_set = VK_NULL_HANDLE;
        VkDescriptorSet display_descriptor_set = VK_NULL_HANDLE;
        VkDescriptorSet gi_temporal_descriptor_sets[2]{};
        VkDescriptorSet gi_atrous_descriptor_sets[6]{};
        uint64_t static_generation = 0;
        uint64_t instance_generation = 0;
        uint64_t command_generation = 0;
        uint64_t material_generation = 0;
        uint64_t material_upload_record_count = 0;
        VkDeviceSize pending_material_bytes = 0;
        bool stats_valid = false;
        // GPU timestamp query pool: 18 slots (9 zones × begin/end).
        VkQueryPool ts_pool = VK_NULL_HANDLE;
        // Per zone: bit 0 set when begin was written, bit 1 when end was.
        uint8_t ts_written[kGpuZoneCount]{};
        // True when the previous recording wrote at least the total zone.
        bool ts_valid = false;
    };

    // Intermediate state shared between the record_ray_traced_shadows phases.
    struct RtBuildSel {
        PartRecord* part = nullptr;
        RtLodRecord* lod = nullptr;
        const RtInstance* source = nullptr;
        bool opaque = false;
    };
    struct RtBlasPending {
        PartRecord* part = nullptr;
        RtLodRecord* lod = nullptr;
        VkAccelerationStructureGeometryKHR geometry{};
        VkAccelerationStructureBuildGeometryInfoKHR build{};
        VkAccelerationStructureBuildRangeInfoKHR range{};
        std::shared_ptr<matter::VkAccelerationStructureResource> target;
        VkDeviceSize scratch_size = 0;
        VkDeviceSize scratch_offset = 0;
    };

    bool build_ray_geometry(
        const matter::VulkanFrame& frame,
        matter::Float3 camera_eye, float pixel_budget,
        PFN_vkGetAccelerationStructureBuildSizesKHR get_sizes,
        PFN_vkCmdBuildAccelerationStructuresKHR cmd_build,
        std::vector<RtBuildSel>& selected_geometry,
        std::vector<RtBlasPending>& pending,
        std::string& error);
    bool emit_ray_instances(
        const matter::VulkanFrame& frame,
        PFN_vkGetAccelerationStructureBuildSizesKHR get_sizes,
        PFN_vkCmdBuildAccelerationStructuresKHR cmd_build,
        VkDeviceSize scratch_alignment,
        const std::vector<RtBuildSel>& selected_geometry,
        const std::vector<RtBlasPending>& pending,
        bool& instances_empty,
        std::string& error);
    bool record_ray_trace_dispatch(
        const matter::VulkanFrame& frame,
        const FrameMatrices& matrices,
        VkExtent2D trace_extent,
        PFN_vkCmdTraceRaysKHR cmd_trace,
        std::string& error);

    bool create_pipeline(std::string& error);
    bool create_raster_pipelines(std::string& error);
    bool create_display_pipeline(std::string& error);
    bool create_ray_tracing_pipeline(std::string& error);
    bool create_gi_temporal_pipeline(std::string& error);
    bool create_gi_atrous_pipeline(std::string& error);
    bool ensure_raster_targets(uint32_t width, uint32_t height,
                               std::string& error);
    bool ensure_dlss_output(FrameResources& frame, VkExtent2D output_extent,
                            std::string& error);
    bool record_gi_temporal(const matter::VulkanFrame& frame,
                            std::string& error, bool retain = true);
    bool record_gi_temporal_signal(const matter::VulkanFrame& frame,
                                   uint32_t signal_mode,
                                   std::string& error, bool retain);
    bool record_gi_atrous(const matter::VulkanFrame& frame,
                          std::string& error, bool retain = true);
    bool record_gi_atrous_signal(const matter::VulkanFrame& frame,
                                 uint32_t signal_mode,
                                 std::string& error, bool retain);
    bool ensure_vertex_buffer(VkDeviceSize required_size,
                              std::string& error, bool* replaced = nullptr);
    bool ensure_index_buffer(VkDeviceSize required_size,
                             std::string& error, bool* replaced = nullptr);
    bool ensure_buffer(matter::VkBufferResource& buffer,
                       VkDeviceSize required_size, VkBufferUsageFlags usage,
                       std::string& error, bool* replaced = nullptr);
    bool ensure_build_buffer(matter::VkBufferResource& buffer,
                             VkDeviceSize required_size,
                             VkBufferUsageFlags usage, std::string& error);
    void update_descriptor(VkDescriptorSet set, uint32_t binding,
                           VkDescriptorType type,
                           const matter::VkBufferResource& buffer);
    bool ensure_frame_resources(uint32_t frame_slot_count,
                                std::string& error);
    void update_frame_descriptors(FrameResources& frame);
    void update_composite_descriptor(FrameResources& frame);
    void update_display_descriptor(VkDescriptorSet set, VkImageView view);
    bool upload_scene_buffers(FrameResources& frame,
                              VkCommandBuffer material_command_buffer,
                              bool reset_stats, std::string& error);
    void record_material_upload(VkCommandBuffer command_buffer,
                                FrameResources& frame);
    bool upload_frame_constants(FrameResources& frame,
                                const FrameMatrices& matrices,
                                matter::Float3 camera_eye,
                                float pixel_budget, std::string& error);
    bool validate_draw_command_regions(std::string& error) const;
    void note_command_layout_rebuild();
    bool rebuild_command_template(std::string& error);
    bool apply_dynamic_command_layout(std::string& error);
    bool load_device_limits(std::string& error);
    bool fail_if_poisoned(std::string& error) const;
    bool poison(std::string& error);
    bool poisoned() const { return !poison_reason_.empty(); }
    void destroy_pipeline();

    matter::VulkanDevice* vulkan_ = nullptr;
    matter::StreamlineBridge* dlss_bridge_ = nullptr;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    std::unique_ptr<matter::StreamlineBridge> test_dlss_bridge_override_;
    bool test_force_rt_unavailable_ = false;
    std::vector<RtGeometryDebugRecord> test_last_rt_geometry_records_;
    uint32_t test_last_rt_blas_build_count_ = 0;
#endif
    matter::DlssMode selected_dlss_mode_ = static_cast<matter::DlssMode>(0);
    bool dlss_history_reset_pending_ = false;
    uint64_t dlss_reset_count_ = 0;
    VkDescriptorSetLayout set_layouts_[2]{};
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline raster_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout composite_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout composite_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline composite_pipeline_ = VK_NULL_HANDLE;
    VkSampler composite_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout display_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout display_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline display_pipeline_ = VK_NULL_HANDLE;
    VkFormat display_pipeline_format_ = VK_FORMAT_UNDEFINED;
    VkDescriptorSetLayout gi_temporal_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout gi_temporal_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline gi_temporal_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout gi_atrous_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout gi_atrous_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline gi_atrous_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout rt_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout rt_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline rt_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool rt_descriptor_pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> rt_descriptor_sets_;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    bool initialized_ = false;

    matter::VkBufferResource clusters_;
    matter::VkBufferResource vertices_;
    matter::VkBufferResource indices_;
    std::vector<FrameResources> frames_;
    uint32_t active_frame_index_ = 0;
    uint32_t frame_resource_slot_capacity_ = 0;

    matter::VkImageResource albedo_;
    matter::VkImageResource normal_;
    matter::VkImageResource orm_;
    matter::VkImageResource velocity_;
    matter::VkImageResource material_instance_;
    matter::VkImageResource depth_;
    matter::VkImageResource hdr_;
    matter::VkImageResource visibility_;
    matter::VkImageResource raw_diffuse_;
    matter::VkImageResource raw_specular_;
    matter::VkImageResource raw_specular_aux_;
    matter::VkImageResource raw_transmission_;
    matter::VkImageResource vol_dummy_3d_;
    VkExtent2D raw_diffuse_extent_{};
    struct GiHistorySet {
        matter::VkImageResource radiance;
        matter::VkImageResource moments;
        matter::VkImageResource history_length;
        matter::VkImageResource depth;
        matter::VkImageResource normal;
        matter::VkImageResource identity;
        matter::VkImageResource rejection;
        matter::VkImageResource aux;
    } gi_history_[2];
    GiHistorySet gi_spec_history_[2];
    matter::VkImageResource gi_atrous_[2];
    matter::VkImageResource gi_spec_atrous_[2];
    uint32_t gi_filtered_index_ = 0;
    bool gi_filtered_valid_ = false;
    uint32_t gi_presented_history_index_ = 0;
    uint32_t gi_candidate_history_index_ = 1;
    uint32_t gi_composite_history_index_ = 1;
    uint64_t gi_candidate_frame_serial_ = 0;
    uint64_t gi_candidate_attempt_token_ = 0;
    uint64_t gi_presented_attempt_token_ = 0;
    uint64_t gi_history_reset_count_ = 0;
    bool gi_candidate_was_reset_ = false;
    bool last_composite_used_gi_temporal_ = false;
    VkImageUsageFlags visibility_usage_ = 0;
    matter::VkBufferResource rt_sbt_;
    VkDeviceAddress rt_sbt_address_ = 0;
    VkDeviceAddress rt_sbt_test_raygen_address_ = 0;
    VkDeviceAddress rt_sbt_lighting_raygen_address_ = 0;
    VkDeviceAddress rt_sbt_miss_address_ = 0;
    VkDeviceAddress rt_sbt_hit_address_ = 0;
    VkDeviceSize rt_sbt_stride_ = 0;
    VkDeviceSize rt_sbt_miss_size_ = 0;
    VkDeviceSize rt_sbt_hit_size_ = 0;
    VkExtent2D raster_extent_{};
    bool raster_attachments_ready_ = false;

    std::vector<PartRecord> parts_;
    std::map<uint64_t, int> slot_of_;
    std::vector<GpuCluster> cluster_staging_;
    std::vector<std::vector<VkSceneLod>> cluster_lods_;
    std::vector<GpuInstance> instance_staging_;
    std::vector<uint32_t> instance_part_slots_;
    size_t static_instance_count_ = 0;

    // Dynamic lane state (Task 7)
    std::vector<GpuInstance> dynamic_instance_staging_;
    std::vector<uint32_t> dynamic_instance_part_slots_;
    uint32_t dynamic_instance_count_ = 0;
    uint64_t dynamic_submit_serial_ = 0;
    uint64_t dynamic_completed_serial_ = 0;
    bool dynamic_dirty_ = false;
    // True while command_template_/part_command_ranges_/draw_transform_slots_
    // carry per-frame offsets that include dynamic instances (see
    // apply_dynamic_command_layout). Cleared once the static baseline is
    // restored after the last dynamic instance disappears.
    bool dynamic_command_layout_applied_ = false;

    std::vector<uint32_t> part_instance_counts_;
    std::vector<DrawCommand> command_template_;
    std::vector<PartCommandRange> part_command_ranges_;
    std::vector<PartCommandRange> recorded_draw_ranges_;
    std::vector<uint8_t> raster_command_enabled_;
    std::vector<uint8_t> uploaded_raster_command_enabled_;
    std::vector<RtInstance> rt_instances_;
    size_t static_rt_instance_count_ = 0;
    std::vector<VkRasterVertex> vertex_staging_;
    std::vector<uint32_t> index_staging_;   // CPU-side mirror; Task 4 uploads to GPU
    std::vector<MaterialGpuRecord> material_staging_;
    uint64_t material_shading_revision_ = 0;
    uint64_t material_geometry_revision_ = 0;
    uint64_t material_generation_ = 1;
    bool gi_history_reset_pending_ = false;
    uint32_t uploaded_vertex_count_ = 0;
    uint32_t uploaded_index_count_ = 0;
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
    bool lighting_initialized_ = false;
    float display_exposure_ev_ = -2.0f;
    float composite_debug_override_ = 0.0f;
    matter::VulkanRayTracingSettings ray_tracing_settings_{};
    matter::VulkanGiSettings gi_settings_{};
    std::unique_ptr<VkVolumetrics> volumetrics_;
    bool volumetrics_enabled_ = false;
    float volumetrics_debug_view_ = 0.0f;
    uint32_t last_rt_samples_ = 1;
    bool last_rt_debug_view_ = false;
    bool last_rt_available_ = false;
    bool last_rt_effective_ = false;
    uint32_t last_rt_trace_dispatches_ = 0;
    std::string last_rt_fallback_reason_;
    TemporalFrame temporal_frame_{};
    uint64_t instance_generation_ = 1;
    uint64_t static_generation_ = 1;
    uint64_t command_generation_ = 1;
    bool static_upload_dirty_ = true;
    VkSceneUploadCounters upload_counters_{};
    VkCullStats cached_stats_{};
    // GPU timestamp support. Cached at init time from device properties.
    bool gpu_timers_supported_ = false;
    float timestamp_period_ns_ = 0.0f;
    // EMA-smoothed per-zone GPU timings (ms). Updated each frame on readback.
    float gpu_smoothed_ms_[kGpuZoneCount]{};
    // Helper recorded per-frame to stamp the command buffer.
    void write_gpu_timestamp(VkCommandBuffer cmd, uint32_t zone_id,
                             bool is_end, FrameResources& frame);
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    uint32_t test_surface_trace_dispatches_ = 0;
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
