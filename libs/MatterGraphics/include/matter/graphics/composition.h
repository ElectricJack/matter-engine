#pragma once
#include <matter/graphics/backend.h>

namespace matter::graphics {
using Rgba = std::array<float, 4>;
// Linear renderer products, row zero at v=0. Volume order is x, then y, then z.
// These are evaluated pixels, never procedural recipes or generation callbacks.
struct FloatImage {
  uint32_t width = 1, height = 1, depth = 1;
  std::vector<Rgba> pixels{{0, 0, 0, 0}};
};
struct IdentityImage {
  uint32_t width = 1, height = 1;
  std::vector<std::array<uint32_t, 2>> pixels{{0, 0}};
};
// Explicit native appearance input; ABI matches composite.frag's RtMaterialGpu.
// It is graphics-owned, not an extension of the portable material contract.
struct ShadingMaterial {
  Rgba base_roughness{1, 1, 1, 1};
  Rgba metal_opacity_spec_coat{0, 1, 1, 0};
  Rgba specular_tint_coat_roughness{1, 1, 1, 0};
  Rgba emission_strength{0, 0, 0, 0};
  Rgba transmission{0, 1.5f, 0, 0};
  Rgba absorption_pad{1, 1, 1, 0};
  Rgba scattering{0, 0, 0, 0}, scattering_shape{0, 0, 0, 0};
  std::array<uint32_t, 4> flags_misc{};
};
// No implicit loss of texture bindings: textured materials must be evaluated by
// a material/G-buffer pass before conversion to this uniform table entry.
data::ValidationResult shading_material(const data::MaterialDescription &, ShadingMaterial &);

struct LocalLight {
  data::Vec3 position{}, direction{0, 0, -1}, color{1, 1, 1};
  float range_metres = 100, source_radius_metres = 0.01f;
  float cos_inner = 1, cos_outer = 0;
  services::LightKind kind = services::LightKind::point;
};
enum class LocalDirectOwner { raster, ray_traced_product };
struct CompositionCamera {
  data::Vec3 position{}, forward{0, 0, -1};
  float tan_half_fov = 1, aspect_ratio = 1, near_metres = 0.1f, far_metres = 5000;
  std::array<float, 2> jitter_uv{};
};
struct CompositionLighting {
  data::Vec3 sun_direction{0, 0, -1}; // Incoming, sun to scene.
  float diffuse_rt_multiplier = 0, emission_multiplier = 1;
  float sun_disc_cos_edge = 0.99975f, sun_disc_cos_core = 0.99995f;
};
// Shader column-major matrices are private packing: public matrices retain the
// neutral row-major convention. Cloud maps carry evaluated optical depth.
struct CompositionEnvironment {
  FloatImage sky_view;
  FloatImage irradiance_sh{3, 3, 1, std::vector<Rgba>(9)};
  std::array<FloatImage, 4> cloud_optical_depth;
  std::array<data::Mat4, 2> cloud_world_to_uvw;
  Rgba cloud_state{}, cloud_filter{};
  data::Vec3 direct_sun{1, 1, 1}, sun_disc{}, sky_scale{1, 1, 1}, irradiance_scale{1, 1, 1};
};
enum class CompositionView { shaded, visibility, normal, depth, albedo, local_candidates };
struct CompositionInputs {
  // Albedo is linear; normal.xyz is world-space, .w=log2(1+emission).
  // ORM=(roughness, metallic, AO, horizon). Depth.r is finite reversed-Z [0,1].
  // Identity.x is material index plus optional native impostor bit 31.
  FloatImage albedo, normal, orm, depth;
  IdentityImage identity;
  FloatImage visibility{1, 1, 1, {{1, 1, 1, 1}}};
  FloatImage diffuse, specular, transmission, local_direct;
  FloatImage integrated_volume{1, 1, 1, {{0, 0, 0, 1}}};
  bool volume_enabled = false, volume_height_layer = false;
  float cloud_top_metres = 0;
  std::vector<ShadingMaterial> materials{ShadingMaterial{}};
  std::vector<LocalLight> local_lights;
  LocalDirectOwner local_direct_owner = LocalDirectOwner::raster;
  CompositionCamera camera;
  CompositionLighting lighting;
  CompositionEnvironment environment;
  CompositionView view = CompositionView::shaded;
  float exposure_ev = 0;
  std::vector<data::Capability> required_capabilities;
};
struct CompositionLimits {
  uint32_t max_dimension = 4096, max_volume_dimension = 256;
  size_t max_materials = 65536, max_lights = 4096;
  uint64_t max_input_bytes = 256ull * 1024 * 1024;
  // Per frame, includes driver image padding and buffer upload staging.
  uint64_t max_gpu_bytes = 512ull * 1024 * 1024;
};
struct CompositionOutput {
  uint32_t width = 0, height = 0;
  std::vector<Rgba> linear_hdr, display_srgb;
};
std::vector<data::Capability> composition_capabilities();
data::ValidationResult validate_composition(const CompositionInputs &, CompositionLimits = {});
} // namespace matter::graphics
