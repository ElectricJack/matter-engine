#pragma once
#include <limits>
#include <matter/graphics/vulkan_raster.h>

namespace matter::graphics {
// Prepared mesh ladders and bounds. No geometry/animation evaluation occurs.
struct DrawLod {
  RasterDraw draw;
  float switch_distance = std::numeric_limits<float>::infinity();
  uint32_t vt_slot = 0;
};
struct DrawCluster {
  data::Bounds bounds;
  float radius = 1;
  uint32_t part = 0;
  std::vector<DrawLod> lods;
  uint32_t mesh_lods = 1;
  bool opaque_occluder = true;
};
struct SceneDrawInstance {
  data::Mat4 transform{}, previous_transform{};
  uint32_t first_cluster = 0, cluster_count = 1, part = 0, token = 1;
  uint32_t animation_slot = UINT32_MAX, animation_generation = 0;
  uint32_t water_slot = 0, water_generation = 0;
  bool history_valid = false;
};
struct AnimationDrawBound {
  data::Bounds bounds;
  uint32_t instance_slot = 0, generation = 0, cluster = 0, lod = 0;
  bool explicit_skin_draw = false; // Bridge owns its separate skin draw.
};
struct DrawOverride {
  float max_distance = 0, lod_bias = 1;
};
struct DrawHierarchyNode {
  data::Bounds bounds;
  float error = 0;
  uint32_t child0 = UINT32_MAX, child1 = UINT32_MAX, cluster = 0;
  bool ready = true;
};
struct DrawHierarchyJob {
  uint32_t instance = 0;
  std::vector<uint32_t> roots;
  uint32_t max_visits = 65536, max_selected = 65536;
  float transform_scale = 1, error_reach = 1;
};
struct SceneDrawInputs {
  RasterInputs raster; // Camera/material/texture/atlas products; draws empty.
  std::vector<DrawCluster> clusters;
  std::vector<SceneDrawInstance> instances;
  std::vector<AnimationDrawBound> animation_bounds;
  std::vector<DrawOverride> overrides;
  std::vector<DrawHierarchyNode> nodes;
  std::vector<DrawHierarchyJob> hierarchy;
  float pixel_budget = 1;
  bool occlusion = false;
  // Optional same-frame ID product. Empty means the producer renders the
  // unfiltered coarse mesh list first; alpha/impostors remain exempt.
  std::optional<IdentityImage> visibility;
  std::vector<data::Capability> required_capabilities;
};
struct SceneDrawCommand {
  uint32_t index_count = 0, instance_count = 0, first_index = 0;
  int32_t vertex_offset = 0;
  uint32_t first_instance = 0;
};
// One entry per emitted RasterDraw. Preserves the native products needed by
// VT, temporal and water adapters without evaluating those producers here.
struct SelectedDrawProduct {
  data::Mat4 previous_transform{};
  bool history_valid = false;
  uint32_t vt_slot = 0, lod = 0, water_slot = 0, water_generation = 0;
};
struct SceneDrawOutput {
  RasterInputs raster;
  std::vector<SelectedDrawProduct> selected;
  std::vector<SceneDrawCommand> commands;
  std::array<uint32_t, 6> stats{}; // frustum, occlusion, emitted, overflow, triangles, batches
  std::vector<std::array<uint32_t, 2>> missing_nodes; // job, node
  uint32_t hierarchy_status = 0;                      // native selected overflow/invalid, request/fallback
};
struct PickResult {
  uint32_t material = 0, token = 0;
  float depth = 0;
  std::optional<RasterIdentity> identity;
};
struct SelectionVertex {
  data::Vec3 position;
  Rgba color{1, 1, 0, 1};
};
data::ValidationResult pick(const GbufferOutput &, uint32_t x, uint32_t y, std::optional<PickResult> &);
std::vector<data::Capability> scene_draw_capabilities();
class VulkanSceneDraw {
public:
  explicit VulkanSceneDraw(VulkanDeviceBinding, RasterLimits = {});
  ~VulkanSceneDraw();
  data::ValidationResult select(const SceneDrawInputs &, SceneDrawOutput &);
  // Paired endpoints, native one-pixel selection lines. Transparent clear,
  // no depth test; caller composites this editor overlay after display.
  data::ValidationResult overlay(uint32_t width, uint32_t height, const data::Mat4 &world_to_clip,
                                 const std::vector<SelectionVertex> &, FloatImage &);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace matter::graphics
