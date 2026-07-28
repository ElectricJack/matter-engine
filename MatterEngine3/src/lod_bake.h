#pragma once
#include "tri.h"            // Tri, make_float3
#include "blas_manager.hpp" // BLASManager
#include "part_asset_v2.h"  // SP-1 LodLevel/LodLevels (authoritative shape)
#include "matter/bake_observer.h"  // optional per-rung observer (W3, Lab-only)
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

// NOTE (Task 8, Phase 5 autoremesher integration): `reproject_triex` moved to
// MatterSurfaceLib — see `mesh_transform.hpp` for the MeshIndexed-shaped
// replacement `void reproject_triex(const MeshIndexed& source, MeshIndexed&
// target)`. Callers historically built through Tri/TriEx vectors; those sites
// wrap via MSL `from_tri`/`to_tri` until lod_bake itself is refactored to
// MeshIndexed at its boundary (Task 11).

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
// tint, shading normals, AO). The undecimated level (keep == 1.0) takes it directly,
// since the triangle list is unchanged. Decimated levels reorder and merge triangles,
// so they get a REPROJECTED copy (MSL `reproject_triex`, SampleSource): each
// surviving triangle inherits materialId/tint/uv/AO from its nearest source
// triangle, and each corner samples the source's authored shading normals — hard
// edges stay hard, smooth surfaces stay smooth. Every rung therefore carries the
// authored material and shading character — a ladder whose rungs disagree is
// visible as a material pop, a box melting into a gradient at distance, or two
// differently-shaded surfaces at once wherever two rungs are on screen together.
//
// `observer` (optional, default null, W3 Lab seam): when non-null, on_rung_ready
// fires once per level after that level's geometry is decimated and registered in
// `blas`, in ladder order (finest first, index 0..N-1). Null observer costs one
// pointer check per level — see matter/bake_observer.h for the thread contract.
// `out_handles` (optional): when non-null, receives the BLASHandle registered
// for each level, in ladder order and one per level.
//
// Prefer it over LodLevel::blas_indices whenever `blas` is a LIVE manager that
// outlives the call. blas_indices holds an ABSOLUTE index into
// blas.get_entries(), and release_blas() erases from that vector and then
// rebuilds handle_to_index_ wholesale -- so every index above a released entry
// silently shifts. The value is only stable while nothing is released, which is
// true at bake time (script_host bakes into a per-part manager, where the
// absolute index is also the part-local one it serializes) and NOT true for the
// shared PartStore manager, where sectors stream in and out continuously.
// Handles survive that; indices do not.
LodLevels bake_lods(const std::vector<Tri>& tris, const BakeTargets& targets,
                    BLASManager& blas, const std::vector<TriEx>* triex = nullptr,
                    BakeObserver* observer = nullptr,
                    std::vector<BLASHandle>* out_handles = nullptr);

} // namespace lod_bake
