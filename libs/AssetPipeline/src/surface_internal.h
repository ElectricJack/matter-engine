#pragma once
#include "internal.h"
#include <matter/asset/surface.h>

namespace matter::asset::detail {
struct SurfaceOrigin {
  std::string tape;
  uint32_t source_version = 0;
  float height_min = 0, height_max = 0;
};
uint64_t surface_preflight(const SurfaceSettings &, size_t samples,
                           const services::EvaluationOptions &,
                           const std::vector<data::Capability> &available);
SurfaceResult finish_surface(const services::JobEnvelope &, const SurfaceImage &,
                             const services::EvaluationOptions &, services::JobMonitor &,
                             services::ResourceUsage, const SurfaceOrigin & = {});
} // namespace matter::asset::detail
