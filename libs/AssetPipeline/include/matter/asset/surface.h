#pragma once
#include <matter/contracts/generation.h>

namespace matter::asset {
namespace data = matter::contracts;

inline constexpr const char *native_surface_capability = "matter.material.surface.samples";
inline constexpr const char *surface_bake_capability = "matter.processing.surface.bake";
inline constexpr const char *surface_tape_capability = "matter.processing.surface.local-tape";

enum class SurfaceOutput { pbr_rgba8, native_float32 };
// All colors are linear. ORM = occlusion, perceptual roughness, metallic.
// Normals are unit tangent-space vectors (+Y bitangent). Height is in metres.
// Pixels are row-major, with the first row at v=0. No implicit vertical flip.
struct SurfaceTexel {
  data::Vec3 base_color{}, orm{1, 1, 0}, normal{0, 0, 1}, emissive{};
  float height_metres = 0;
  uint8_t coverage = 1;
};
struct SurfaceSettings {
  data::SemanticId asset_id, material_id;
  data::SourceIdentity provenance;
  uint32_t width = 0, height = 0;
  SurfaceOutput output = SurfaceOutput::pbr_rgba8;
  data::WrapMode wrap_u = data::WrapMode::clamp, wrap_v = data::WrapMode::clamp;
  data::FilterMode min_filter = data::FilterMode::linear, mag_filter = data::FilterMode::linear;
  std::vector<data::Capability> required_capabilities;
};
struct SurfaceImage {
  SurfaceSettings settings;
  std::vector<SurfaceTexel> texels;
};
// Consumer data only: no evaluator, shader, GPU handle or residency policy.
// These IDs name owned resources in SurfaceProduct::asset. Float resources
// use little-endian f32; coverage is u8 (0 or 1). Serialized in the required
// version-1 extension named by native_surface_capability.
struct NativeSurfaceDescription {
  uint32_t version = 1, width = 0, height = 0;
  data::SemanticId material, base_color, orm, normal, emissive, height_metres, coverage;
  float height_min_metres = 0, height_max_metres = 0;
  data::WrapMode wrap_u = data::WrapMode::clamp, wrap_v = data::WrapMode::clamp;
  data::FilterMode min_filter = data::FilterMode::linear, mag_filter = data::FilterMode::linear;
  std::optional<data::SemanticId> source_tape;
  uint32_t source_version = 0;
};
struct SurfaceProduct {
  services::AssetProduct asset;
  std::optional<NativeSurfaceDescription> native;
};
using SurfaceResult = services::JobResult<SurfaceProduct>;

// Explicit conversion of already evaluated samples. PBR conversion rejects
// relief, occlusion, incomplete coverage and out-of-gamut colors; it never
// treats them as optional metadata. Native output requires consumer opt-in in
// options.validation.supported_capabilities. No files or graphics are touched.
SurfaceResult bake_surface(const services::JobEnvelope &, const SurfaceImage &,
                           const services::EvaluationOptions & = {},
                           const services::JobControl & = {});
} // namespace matter::asset
