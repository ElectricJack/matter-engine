#ifndef VIEWER_RASTER_MESH_H
#define VIEWER_RASTER_MESH_H

// MatterEngine3/src/render/raster_mesh.h
//
// A naming layer over IndexedPartGeometry (indexed_part_geometry.h) plus two
// adapters. `RasterMeshData` IS `IndexedPartGeometry` — a type alias, not a
// wrapper — so the two names are interchangeable, and part_store.cpp and the
// Vulkan part builder use both.
//
// One value is the CPU-side vertex and index stream for ONE LOD rung of one
// part. It owns std::vectors only: no GPU handles, no device memory, no
// lifetime tied to a device — but copying one is a deep copy of what can be
// megabytes of geometry, so move it.
//
// Producer: build_raster_mesh_data, called by PartStore once per baked ladder
// rung; the result is stored in LoadedPart::lod_mesh_data and lives for the
// whole of that part's residency. Consumer: the Vulkan part builder, which
// uploads it. `default_mat_id` is the fallback material for triangles whose
// TriEx carries none — PartStore passes the part's dominant material id (see
// `dominant_mat` in stage_from_snapshot) so a re-baked coarse rung that lost
// its per-triangle material does not fall through to the instance material.
//
// STALE-COMMENT NOTE: the wording below predates the Vulkan-only renderer. The
// GL/raylib render path was deleted outright and no `RasterComposer` type
// remains in the tree (only comments still name it), so "upload happens in
// RasterComposer" no longer identifies a real call site. The channel MAPPING
// it describes is still accurate.
#include "indexed_part_geometry.h"   // IndexedPartGeometry, Tri, TriEx
#include <cstdint>
#include <vector>

namespace viewer {

// CPU-side vertex arrays for one LOD level, raylib-Mesh channel layout.
// TriEx maps onto standard channels: normals <- N0/N1/N2, colors <- tint RGBA,
// texcoords <- (materialId, per-vertex AO). GL-free: upload happens in RasterComposer.
using RasterMeshData = IndexedPartGeometry;

RasterMeshData build_raster_mesh_data(const Tri* tris, const TriEx* triex, int tri_count,
                                      float default_mat_id = -1.0f);
// Unweld an indexed mesh back to soup (3 corners per triangle, indices empty).
// Legacy shim for the GL path; new code should consume indices directly.
// No production caller remains: MatterEngine3/tests/viewer_logic_tests.cpp is
// the only user, for soup-layout assertions. It also does NOT carry the warp
// channels (`warp_uvs` / `warp_frames`) across, so a terrain-sector rung
// round-tripped through it silently loses its warp data.
RasterMeshData expand_indexed(const RasterMeshData& indexed);

} // namespace viewer
#endif
