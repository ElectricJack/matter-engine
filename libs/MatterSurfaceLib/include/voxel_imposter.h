#pragma once

// libs/MatterSurfaceLib/include/voxel_imposter.h
//
// Dense voxel-volume impostors: a way to stand in for a fully baked part at
// distance by ray-marching a small 3D grid instead of intersecting its BVH.
//
// The pipeline, in the order the declarations below appear:
//   1. `flatten_part_triangles_mat(blas, tlas)` -- walk every TLAS instance
//      and emit world-space `FlatTri`s carrying material id and tint.
//   2. `choose_grid_dims()` -- pick per-axis dims so voxels stay roughly
//      isotropic under a `maxDim` budget for the longest axis.
//   3. `bake_voxels()` -- surface-voxelize via `tri_box_overlap`, filling
//      coverage plus area-weighted albedo and octahedrally encoded normal.
//   4. `save()` / `load()` -- persist to `imposters/<16-hex>.vxi`.
//   5. `dda_first_hit()` -- Amanatides-Woo traversal at render/trace time.
//
// Considerations:
//  - Cache identity uses TWO hashes and both are checked on load:
//    `compute_vox_hash(VoxGenParams)` pins the bake SETTINGS, and
//    `source_part_hash` pins the INPUT PART. Either mismatch makes `load()`
//    return false, which means "rebake", not "error". A content hash over the
//    body catches corruption on top of that.
//  - Encodings are shared with GLSL: the octahedral normal must match the
//    decode in `bvh_tlas_common.glsl`, and voxels are always indexed
//    `(z*ny + y)*nx + x`. Changing either without the other silently corrupts
//    shading rather than failing.
//  - Spaces differ per function, deliberately: `FlatTri` and `bake_voxels`
//    work in WORLD space, while `dda_first_hit` takes its origin and direction
//    in NORMALIZED box space [0,1]^3 and returns `tHit` in that same space.
//  - Everything here is GL-free and unit-testable; `save()` writes temp+rename
//    so a crash cannot leave a half-written artifact under its final name.
//  - The only in-tree consumers today are `src/voxel_imposter.cpp` and
//    `tests/voxel_imposter_tests.cpp`.
//
// Design doc: docs/superpowers/specs/2026-06-22-voxel-box-imposter-design.md

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

// The baked payload: three dense per-voxel channels over one axis-aligned box,
// plus the identity of the part it was baked from. Filled entirely by
// `bake_voxels()` or `load()`; a partially populated instance is not a valid
// state (`load()` leaves `out` untouched on failure rather than half-filling
// it).
//
// `bounds_min`/`bounds_max` are world-space and come from the flattened
// triangles. `nx`/`ny`/`nz` are per-axis voxel counts, chosen by
// `choose_grid_dims()` so voxels stay near-cubic, hence generally unequal.
// All three blobs are indexed through `voxel_index()`, which does NOT
// bounds-check.
//
// Memory is the reason `maxDim` is a budget: the three blobs cost 6 bytes per
// voxel together, so a full 128x128x128 grid is roughly 12 MB per part
// (2 MB coverage + 6 MB albedo + 4 MB normal) held in host memory.
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
