#pragma once
// tileset_bake_ao.h — AO compute pass driver.
#include "tileset_spec.h"

#include <cstdint>
#include <string>
#include <vector>

typedef unsigned int GLuint;

class BLASManager;
class TLASManager;

namespace tileset {

// Runs the AO compute pass over the same TLAS as the primary pass. maxRayDist
// = cfg.edge_strip_width (seam-invariance guarantee). Returns AO as one byte
// per texel; 255 = fully unoccluded.
bool bake_ao(GLuint program,
             BLASManager& blas,
             TLASManager& tlas,
             const TileConfig& cfg,
             float ray_y,
             float height_min,
             float height_max,
             uint32_t seed,
             std::vector<uint8_t>& ao_r8_out,
             std::string& err);

} // namespace tileset
