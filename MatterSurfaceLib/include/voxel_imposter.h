#pragma once
#include "bvh.h"        // Tri, TriEx, float3
#include "tlas_manager.hpp"
#include "blas_manager.hpp"
#include <cstdint>
#include <string>
#include <vector>

// Dense voxel-volume imposter. See
// docs/superpowers/specs/2026-06-22-voxel-box-imposter-design.md
namespace voxel_imposter {

// A flattened world-space triangle plus its material/tint for albedo baking.
struct FlatTri { float3 v0, v1, v2; int materialId; float tint[4]; };

// Flatten all TLAS instances into world-space triangles, carrying per-triangle
// materialId and tint from the corresponding BLASEntry::tri_extra.
// Falls back to the instance material_id / neutral tint (1,1,1,0) when the
// BLAS entry has no tri_extra for a given triangle.
std::vector<FlatTri> flatten_part_triangles_mat(const BLASManager& blas,
                                                const TLASManager& tlas);

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

// Akenine-Moller triangle / axis-aligned-box overlap. boxCenter/boxHalf in the
// same space as the triangle verts. Returns true if the triangle intersects the box.
bool tri_box_overlap(const float boxCenter[3], const float boxHalf[3],
                     const float v0[3], const float v1[3], const float v2[3]);

// Octahedral-encode a unit normal into two bytes (RG8) and back. Must match the
// GLSL decode in bvh_tlas_common.glsl (Task 12).
void oct_encode(const float n[3], uint8_t out[2]);
void oct_decode(const uint8_t in[2], float n[3]);

// Surface-voxelize the flattened triangles into a dense grid: coverage=255 for
// any voxel a triangle overlaps (tri_box_overlap), with area-weighted albedo
// (MaterialRegistryGet(materialId)->albedo blended by tint) and area-weighted
// octahedral normal per covered voxel. Fills bounds/dims/coverage/albedo/normal.
// Returns false on empty/degenerate input. GL-free, unit-testable.
bool bake_voxels(const std::vector<FlatTri>& tris, const VoxGenParams& p,
                 uint64_t source_part_hash, VoxelImposter& out);

// Serialization -----------------------------------------------------------
// FNV-1a of p XOR kFormatVersion.
uint64_t compute_vox_hash(const VoxGenParams& p);
// "imposters/<16-hex-zero-padded>.vxi"
std::string cache_path(uint64_t hash);
// Serialize v to path (atomic temp+rename). Returns false on I/O failure.
bool save(const std::string& path, const VoxelImposter& v, uint64_t vox_hash);
// Deserialize from path. Returns false (leaves out untouched) on read failure,
// magic/version mismatch, vox_hash or source_part_hash mismatch, or content
// hash mismatch. On success fills out completely.
bool load(const std::string& path, uint64_t expected_vox_hash,
          uint64_t expected_source_hash, VoxelImposter& out);

// Amanatides-Woo 3D-DDA over a coverage grid in NORMALIZED box space [0,1]^3.
// origin/dir are in box space. On the first voxel with coverage>0 sets
// hitX/Y/Z and tHit (ray param in box space) and returns true; false on
// pass-through. dims = nx,ny,nz. coverage indexed via (z*ny+y)*nx+x.
bool dda_first_hit(const float origin[3], const float dir[3],
                   int nx,int ny,int nz, const std::vector<uint8_t>& coverage,
                   int& hitX,int& hitY,int& hitZ, float& tHit);

} // namespace voxel_imposter
