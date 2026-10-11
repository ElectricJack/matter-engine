#pragma once
#include <limits>
#include <matter/graphics/vulkan_raster.h>

namespace matter::graphics {
// Evaluated graphics products. Integrals, fitted distributions, mesh baking
// and hierarchy construction belong to external asset/compute services.
struct SparseBrickProduct {
  std::array<int32_t, 3> coordinate{};
  uint64_t mask = 0;
  uint32_t first_cell = 0;
};
struct SparseCellProduct {
  Rgba color_density{1, 1, 1, 1};
  data::Vec3 mean{};
  Rgba moment{1, 1, 1, 0};
  data::Vec2 cross_moment{};
  data::Vec3 support_min{}, support_max{1, 1, 1};
  Rgba plane{};
};
struct SurfaceVertexProduct {
  data::Vec3 position{}, normal{0, 0, 1}, color{1, 1, 1};
  data::Vec2 uv{};
};
struct SurfaceTriangleProduct {
  std::array<SurfaceVertexProduct, 3> vertices;
  int32_t texture = -1, projection_axis = -1;
};
struct SurfaceTexturePage {
  uint64_t mask = 0;
  std::vector<std::array<uint32_t, 2>> texels; // RGBA8 color, RGBA8 object normal.
};
struct SurfaceTextureMip {
  uint32_t width = 1, height = 1;
  float alpha_scale = 1;
  std::vector<std::array<uint32_t, 2>> texels;
  // Empty page_indices selects dense texels. Otherwise one index per 8x8
  // page: zero absent, 1+index into pages; texels contains no dense payload.
  std::vector<uint32_t> page_indices;
  std::vector<SurfaceTexturePage> pages;
};
struct SurfaceTextureProduct {
  std::vector<SurfaceTextureMip> mips;
};
enum class SparseRepresentation : uint32_t { voxel = 0, surface = 1, directional_surface = 2 };
struct SparseLevelProduct {
  uint32_t first = 0, count = 0;
  float switch_distance = std::numeric_limits<float>::infinity();
  data::Vec3 grid_offset{};
  float grid_scale = 1;
  SparseRepresentation representation = SparseRepresentation::voxel;
};
struct SparseChildProduct {
  uint32_t prototype = 0;
  data::Mat4 transform{};
};
struct SparsePrototypeProduct {
  data::Bounds bounds{}, lod_bounds{};
  uint32_t first_level = 0, level_count = 1;
  std::vector<SparseChildProduct> children;
  float refine_distance = std::numeric_limits<float>::infinity();
};
struct SparsePlacementProduct {
  uint32_t prototype = 0, material = 0, token = 1, coverage_seed = 0;
  float roughness = .5f;
  data::Mat4 transform{};
};
// The RT producer owns acceleration construction and provides an already
// completed forest plus all referenced inner AS objects. Their instance IDs
// index the corresponding local_instances range; AS custom indices are global
// level indices, and primitive IDs index triangles within that level.
struct SparseQueryObject {
  VkDeviceAddress acceleration = 0;
  uint32_t first_instance = 0, instance_count = 0;
};
struct SparseQueryPlacement {
  uint32_t object = 0, material = 0, token = 1;
  float roughness = .5f;
  bool use_part_materials = false;
  data::Mat4 transform{};
};
struct SparseQueryProduct {
  VkDevice device = VK_NULL_HANDLE;
  VkAccelerationStructureKHR forest = VK_NULL_HANDLE;
  std::shared_ptr<void> lifetime;
  std::vector<SparseQueryObject> objects;
  std::vector<SparsePlacementProduct> local_instances;
  std::vector<SparseQueryPlacement> placements;
};
struct SparseSurfaceInputs {
  RasterInputs raster; // Camera, material table, identities; no raster draws.
  std::vector<SparseBrickProduct> bricks;
  std::vector<SparseCellProduct> cells;
  std::vector<SurfaceTriangleProduct> surfaces;
  std::vector<SurfaceTextureProduct> textures;
  std::vector<SparseLevelProduct> levels;
  std::vector<SparsePrototypeProduct> prototypes;
  std::vector<SparsePlacementProduct> placements;
  // Hierarchy uses the native bounded selection algorithm. Flat placements
  // allow any affine transform; hierarchy edges/roots require similarities.
  bool hierarchy = false;
  uint32_t node_capacity = 4096, primitive_capacity = 65536;
  float pixel_budget = 1;
  data::Mat4 previous_world_to_clip{};
  bool history_valid = false, accumulate_samples = false;
  uint64_t presented_frame = 0;
  std::optional<SparseQueryProduct> query;
  std::vector<data::Capability> required_capabilities;
};
struct SparseSurfaceOutput {
  GbufferOutput gbuffer;
  FloatImage velocity, reactivity;
  std::array<uint32_t, 16> hierarchy_control{};
};
struct VulkanSparseFeatures {
  bool ray_query_enabled = false;
};
std::vector<data::Capability> sparse_surface_capabilities(VulkanSparseFeatures = {});
class VulkanSparseSurface {
public:
  explicit VulkanSparseSurface(VulkanDeviceBinding, RasterLimits = {}, VulkanSparseFeatures = {});
  ~VulkanSparseSurface();
  data::ValidationResult render(const SparseSurfaceInputs &, SparseSurfaceOutput &);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace matter::graphics
