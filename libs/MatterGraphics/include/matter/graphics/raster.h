#pragma once
#include <matter/graphics/composition.h>
#include <matter/graphics/scene_backend.h>

namespace matter::graphics {
// Resolved pixels only. No codecs, source resolvers or asset generators enter
// this boundary. Portable v1 inputs supply one tightly packed RGBA8 level.
struct RasterTexture {
  data::TextureDescription description;
  std::vector<uint8_t> rgba8;
};
enum class RasterAlpha { opaque, mask };
struct RasterMaterial {
  ShadingMaterial shading;
  // Base color, metallic/roughness, tangent normal, emissive. -1 means absent.
  std::array<int32_t, 4> textures{{-1, -1, -1, -1}};
  float normal_scale = 1, alpha_cutoff = 0.5f;
  RasterAlpha alpha = RasterAlpha::opaque;
  bool double_sided = false;
};
struct RasterVertex {
  data::Vec3 position{}, normal{}; // Zero normal selects geometric derivatives.
  data::Vec4 tangent{};            // Zero selects the UV-derivative tangent frame.
  data::Vec2 uv{}, base_uv{};
};
struct RasterGeometry {
  std::vector<RasterVertex> vertices;
  std::vector<uint32_t> indices;
  data::Winding winding = data::Winding::counterclockwise;
};
// Native prepared atlas: two contiguous RGBA8 layers, shade then tint, in the
// production 8x8 view grid. Shade=oct-normal/depth/coverage. Tint=RGB/blend.
// Graphics derives the existing coverage-preserving, per-view runtime mips.
struct RasterAtlas {
  uint32_t edge = 128;
  std::vector<uint8_t> rgba8;
};
struct RasterImpostor {
  uint32_t atlas = 0;
  data::Vec3 center{};
  float half_extent = 1;
  bool parallax = true;
};
struct RasterDraw {
  std::shared_ptr<const RasterGeometry> geometry;
  uint32_t first_index = 0, index_count = 0, material = 0, identity = 1;
  data::Mat4 transform{};
  std::optional<RasterImpostor> impostor; // Uses an internal four-corner card.
};
struct RasterIdentity {
  uint32_t token = 0;
  data::SemanticId instance;
  std::vector<data::SemanticId> path;
};
struct RasterInputs {
  uint32_t width = 1, height = 1;
  data::Mat4 world_to_clip{}; // Vulkan zero-to-one, reversed Z.
  data::Vec3 camera_eye{};
  std::vector<RasterMaterial> materials{RasterMaterial{}};
  std::vector<RasterTexture> textures;
  std::vector<RasterAtlas> atlases;
  std::vector<RasterDraw> draws;
  std::vector<RasterIdentity> identities;
  std::vector<data::Capability> required_capabilities;
};
struct RasterLimits {
  uint32_t max_dimension = 4096, max_texture_dimension = 4096;
  size_t max_draws = 100000, max_materials = 65536, max_textures = 4096;
  uint64_t max_input_bytes = 256ull * 1024 * 1024;
  uint64_t max_gpu_bytes = 512ull * 1024 * 1024;
};
struct GbufferOutput {
  FloatImage albedo, normal, orm, depth, emission;
  IdentityImage identity;
  std::vector<ShadingMaterial> materials;
  std::vector<RasterIdentity> identities;
};
std::vector<data::Capability> raster_capabilities();
data::ValidationResult validate_raster(const RasterInputs &, RasterLimits = {});
// Convert a retained scene snapshot. Shares packed prototypes among instances;
// retains no world/host context. Camera matrices/region follow SceneBackend.
// Output dimensions are explicit; all output is unchanged on failure.
data::ValidationResult raster_inputs(const SceneFrame &, uint32_t width, uint32_t height, RasterInputs &,
                                     RasterLimits = {});
// Transfer evaluated products into the typed deferred backend; preserves the
// caller's lighting/environment settings. Emission is a separate linear product.
void use_gbuffer(GbufferOutput, CompositionInputs &);
} // namespace matter::graphics
