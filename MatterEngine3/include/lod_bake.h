#pragma once
#include "bvh.h"            // Tri, make_float3
#include "blas_manager.hpp" // BLASManager
#include "part_asset_v2.h"  // SP-1 LodLevel/LodLevels (authoritative shape)
#include <cstdint>
#include <vector>

namespace lod_bake {

// SP-4 consumes SP-1's authoritative LOD shape directly so what is selected
// matches what is serialized. SP-1's LodLevel carries `screen_size_threshold`
// plus a `std::vector<uint32_t> blas_indices` (BLAS-table indices for the part
// at that detail). We alias rather than redefine to avoid a divergent mirror.
using part_asset::LodLevel;
using part_asset::LodLevels;

// Decimate a Tri set to approximately `keep_ratio` of its triangles via
// mesh_simplifier (QEM edge collapse). keep_ratio in (0,1]. Returns a NEW Tri
// vector; input is not mutated. Empty input -> empty output. If the simplifier
// degenerates (zeroed mesh), returns a copy of the input unchanged.
std::vector<Tri> decimate_tris(const std::vector<Tri>& tris, float keep_ratio);

// Decimate a Tri set until the minimum QEM collapse cost exceeds the given
// world-space error bound `epsilon` (quadric cost is a squared distance, so the
// simplifier is driven with epsilon^2). No triangle-count target: the mesh keeps
// collapsing while every remaining collapse moves the surface less than epsilon.
// Same fallback semantics as decimate_tris (empty in -> empty out; degenerate
// simplifier output -> copy of input).
//
// use_aabb_bounds: when true (default), the mesh's own AABB is passed as
// CellBounds so vertices on the mesh border (face planes) are frozen — correct
// for terrain tiles. When false, bounds=nullptr is passed so ONLY the
// topological boundary lock (lock_boundary=true, Task 8) freezes open edges;
// face-plane locking is suppressed, which is correct for cluster interiors that
// happen to touch the cluster AABB but are NOT terrain borders.
std::vector<Tri> decimate_to_error(const std::vector<Tri>& tris, float epsilon,
                                   bool use_aabb_bounds = true);

// Rebuild per-triangle TriEx for a decimated mesh by re-projection: for each
// output triangle, copy materialId/tint/AO from the nearest source triangle
// (by centroid, via a uniform spatial hash over `src_tris`), and set all three
// shading normals to the output triangle's geometric normal. `src_triex` must be
// parallel to `src_tris`. Returns a vector parallel to `out_tris` (empty if the
// source set is empty or non-parallel).
std::vector<TriEx> reproject_triex(const std::vector<Tri>& out_tris,
                                   const std::vector<Tri>& src_tris,
                                   const std::vector<TriEx>& src_triex);

// Per-level decimation targets (keep-ratios) and matching selection thresholds.
// Defaults: LOD0 = full (1.0), LOD1 ~ 1/10, LOD2 ~ 1/100. Thresholds are on the
// projected-size scale (bound_radius / distance) used by lod_select: a finer
// level demands a LARGER projected size to be chosen. Index 0 is the finest.
struct BakeTargets {
    std::vector<float> keep_ratio = {1.0f, 0.1f, 0.01f};
    std::vector<float> threshold  = {0.20f, 0.05f, 0.0125f};
};

// Decimate `tris` into N LOD levels (N = BakeTargets size), register each level's
// geometry as a BLAS in `blas`, and return the LodLevels (each level holds the
// registered BLAS index + its screen_size_threshold). LOD0 with keep_ratio 1.0 is
// the full input (no decimation). The returned blas_indices values index
// blas.get_entries() in registration order.
//
// `triex` (optional) is the per-triangle TriEx data parallel to `tris` (materialId,
// tint, shading normals, AO). It is attached only to the undecimated level (keep ==
// 1.0), where the triangle list is unchanged; decimated levels reorder/merge
// triangles so their per-triangle material falls back to the instance material.
LodLevels bake_lods(const std::vector<Tri>& tris, const BakeTargets& targets,
                    BLASManager& blas, const std::vector<TriEx>* triex = nullptr);

} // namespace lod_bake
