#pragma once
#include <matter/terrain/field.h>

namespace matter::terrain {
// Contour profile v1: the output tile is [0,size]^3 relative to request.origin.
// size is a power of two in [8,64], spacing is 2/4/8 metres, with 2..32 cells
// per tile axis. The request sample domain MUST include exactly three ghost
// cells on each side: [-3*spacing,size+3*spacing], cell_counts = size/spacing+6.
// This explicitly authorizes the native mesher's gradient/material probes.
struct ChunkSettings {
  float size_metres = 32;
};
std::vector<data::Capability> mesh_capabilities();
services::TerrainResult generate_chunk(const services::TerrainRequest &, const EncodedField &,
                                      ChunkSettings = {}, const services::JobControl & = {});

// The supplied field is copied, never resolved from a filesystem or live world.
// A call validates its SHA-256 against request.field. No mutable service state.
class CpuTerrain final : public services::TerrainService {
public:
  explicit CpuTerrain(EncodedField field, ChunkSettings settings = {})
      : field_(std::move(field)), settings_(settings) {}
  std::vector<data::Capability> capabilities() const override { return mesh_capabilities(); }
  services::TerrainResult generate(const services::TerrainRequest &r,
                                  const services::JobControl &c) override {
    return generate_chunk(r, field_, settings_, c);
  }
private:
  EncodedField field_;
  ChunkSettings settings_;
};
} // namespace matter::terrain
