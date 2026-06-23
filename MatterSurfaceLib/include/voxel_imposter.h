#pragma once
#include "bvh.h"        // Tri, float3
#include <cstdint>
#include <string>
#include <vector>

// Dense voxel-volume imposter. See
// docs/superpowers/specs/2026-06-22-voxel-box-imposter-design.md
namespace voxel_imposter {

constexpr uint32_t kMagic = 0x49584F56u;   // 'VOXI'
constexpr uint32_t kFormatVersion = 1u;

struct VoxGenParams {
    int   maxDim;       // resolution budget for the longest axis (e.g. 128)
    int   seed;         // reserved
    float coverThresh;  // surface-fill threshold in [0,1] (default 0.5)
};
static_assert(sizeof(VoxGenParams) == 12, "VoxGenParams padding-free for byte hashing");

struct VoxelImposter {
    float    bounds_min[3] = {0,0,0};
    float    bounds_max[3] = {0,0,0};
    int      nx = 0, ny = 0, nz = 0;
    uint64_t source_part_hash = 0;
    std::vector<uint8_t> coverage;  // nx*ny*nz, 0=empty 255=full
    std::vector<uint8_t> albedo;    // nx*ny*nz*3, RGB
    std::vector<uint8_t> normal;    // nx*ny*nz*2, octahedral RG8
    int voxel_index(int x,int y,int z) const { return (z*ny + y)*nx + x; }
};

// Choose per-axis grid dims so voxels stay ~isotropic in world space.
// v = maxExtent/maxDim; nx = clamp(ceil(extentX/v), 1, maxDim); etc.
// Returns false on degenerate (non-positive) extent on all axes.
bool choose_grid_dims(const float bounds_min[3], const float bounds_max[3],
                      int maxDim, int& nx, int& ny, int& nz);

} // namespace voxel_imposter
