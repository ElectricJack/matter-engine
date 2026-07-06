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

// PRECONDITION FOR TASK 6:
// The current implementation forces materialCount = 0 in the driver so
// getMaterialProperties() returns flatShading=true, avoiding NaN from
// normalize(vec3(0)) on the base-field mesh (which has zero vertex normals).
// When Task 6 scatters smooth-shaded parts (pebbles, rocks) into the TLAS,
// materialCount must be set to the actual registry count and the material
// table uploaded so per-triangle flatShading flags are honored — otherwise
// smooth normals are silently discarded and AO shading is wrong for scattered
// instances. See task-5-report.md and the Phase 3 final review discussion.

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
