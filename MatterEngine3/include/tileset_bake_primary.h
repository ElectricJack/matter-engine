#pragma once
// tileset_bake_primary.h — GPU driver for the ortho-down bake pass.
#include "tileset_spec.h"  // TileConfig
#include "material_registry.h"  // MaterialDef

#include <cstdint>
#include <string>
#include <vector>

// Use uint32_t in the public API instead of GLuint to avoid a GL header
// dependency here. GLuint is typedef unsigned int in all GL implementations;
// tileset_bake_primary.cpp asserts sizeof(GLuint)==sizeof(uint32_t) at compile
// time so a mismatch is a hard build error rather than a silent runtime bug.

class BLASManager;
class TLASManager;

namespace tileset {

// Runs the primary compute pass:
//  - creates GL image textures (RGBA8 + RG8 + RGBA8 + R16) sized W*H,
//  - uploads mats[] to SSBO binding 10 (MATERIAL_FLOATS_PER_DEF = 12 floats each),
//  - binds BLAS + TLAS via BLASManager::bind_to_shader / TLASManager::bind_to_shader,
//  - dispatches ((W+7)/8, (H+7)/8, 1),
//  - reads back into the four output vectors.
//
// program: prebuilt GL compute program (from compile_compute_program).
// ray_y: ortho origin Y in world space (must be well above heightMax).
bool bake_primary(uint32_t program,
                  BLASManager& blas,
                  TLASManager& tlas,
                  const std::vector<MaterialDef>& mats,
                  const TileConfig& cfg,
                  float ray_y,
                  float height_min,
                  float height_max,
                  std::vector<uint8_t>&  albedo_rgb8_out,
                  std::vector<uint8_t>&  normal_rg8_out,
                  std::vector<uint8_t>&  orm_rgb8_out,
                  std::vector<uint16_t>& height_r16_out,
                  std::string& err);

} // namespace tileset
