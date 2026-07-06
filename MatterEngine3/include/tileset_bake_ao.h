#pragma once
// tileset_bake_ao.h — AO compute pass driver.
#include "tileset_spec.h"
#include "material_registry.h"  // MaterialDef

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
//
// Two overloads:
//   (1) No material table — forces materialCount=0 so getMaterialProperties()
//       always returns the default (flatShading=true). Safe when the TLAS
//       contains only the base heightfield (which has zero vertex normals and
//       must use face normals to avoid normalize(vec3(0)) NaN).
//   (2) Material table overload — uploads `mats` to the SSBO and sets
//       materialCount to mats.size(). The caller is responsible for ensuring
//       all materials whose triangles carry zero vertex normals have
//       flatShading=true. The orchestrator (bake_tileset_gpu) uses this form
//       and forces the base material entry to flatShading=1 before calling.
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

bool bake_ao(GLuint program,
             BLASManager& blas,
             TLASManager& tlas,
             const std::vector<MaterialDef>& mats,
             const TileConfig& cfg,
             float ray_y,
             float height_min,
             float height_max,
             uint32_t seed,
             std::vector<uint8_t>& ao_r8_out,
             std::string& err);

} // namespace tileset
