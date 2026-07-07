#pragma once
// tileset_bake_ao.h — AO compute pass driver.
#include "tileset_spec.h"
#include "material_registry.h"  // MaterialDef

#include <cstdint>
#include <string>
#include <vector>

// Use uint32_t in the public API instead of GLuint to avoid a GL header
// dependency here. GLuint is typedef unsigned int in all GL implementations;
// tileset_bake_ao.cpp asserts sizeof(GLuint)==sizeof(uint32_t) at compile
// time so a mismatch is a hard build error rather than a silent runtime bug.

class BLASManager;
class TLASManager;

namespace tileset {

// Runs the AO compute pass over the same TLAS as the primary pass. maxRayDist
// = cfg.edge_strip_width (seam-invariance guarantee). Returns AO as one byte
// per texel; 255 = fully unoccluded.
//
// Two overloads:
//   (1) No material table — forces materialCount=0 so getMaterialProperties()
//       returns the default. Suitable when the TLAS is base-only and the base
//       already carries per-vertex normals (build_base_blas fills N0/N1/N2
//       via central-difference sampling), which is the standard case now.
//   (2) Material table overload — uploads `mats` to both the MaterialBuf SSBO
//       and the materialTable uniform so getMaterialProperties() reads real
//       values (matches bake_primary's binding shape). Required whenever the
//       TLAS contains meshes whose triangles need real per-material properties
//       (flatShading, roughness, etc.) surfaced to the AO shader.
bool bake_ao(uint32_t program,
             BLASManager& blas,
             TLASManager& tlas,
             const TileConfig& cfg,
             float ray_y,
             float height_min,
             float height_max,
             uint32_t seed,
             std::vector<uint8_t>& ao_r8_out,
             std::string& err);

bool bake_ao(uint32_t program,
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
