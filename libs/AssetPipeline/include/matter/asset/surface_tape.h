#pragma once
#include <matter/asset/surface.h>

namespace matter::asset {
struct SurfaceBakePoint {
  data::Vec3 position{}, normal{0, 0, 1};
  data::Vec4 tangent{1, 0, 0, 1}; // xyz unit, w = bitangent handedness (+/-1)
  float footprint_metres = 0.001f;
  uint8_t coverage = 1;
};
struct SurfaceTapeRequest {
  SurfaceSettings settings;
  std::string tape;
  std::vector<SurfaceBakePoint> points;
};
// Optional native producer. Reuses the existing CPU SurfaceRuntime, with
// explicit local raster hits. Supports receiver-independent direct-source v1;
// world fields, classifier blending and v2 receiver derivatives fail explicitly.
// Finite-difference normals use max(footprint/2, 0.0001m), as the native face
// reference does. Native output retains the source tape and declared relief.
SurfaceResult bake_surface_tape(const services::JobEnvelope &, const SurfaceTapeRequest &,
                                const services::EvaluationOptions & = {},
                                const services::JobControl & = {});
} // namespace matter::asset
