#pragma once

// MatterEngine3/src/render/indexed_part_geometry.h
//
// The de-duplicated CPU vertex stream every part LOD passes through on its way
// to the GPU, and the two functions that produce and fingerprint it.
//
// Where it sits: the mesher/bake side produces loose triangles (`Tri` plus the
// optional per-corner `TriEx` attributes, both from SpatialQueryLib's tri.h);
// build_indexed_part_geometry welds them into indexed arrays; part_store /
// raster_mesh then upload those arrays, and the animation binding bake
// (animation/animation_binding_bake.cpp, anim_bundle.cpp) binds skin weights
// against the SAME welded stream. That shared consumption is the reason the
// weld must be deterministic: a binding baked against one welding of a mesh is
// meaningless against another.
//
// Layout: parallel arrays, all indexed by welded-vertex id, plus one `indices`
// array of triangle corners. vertex_count is the number of welded vertices, and
// every per-vertex array is that long times its own stride.
//
// This is a plain value type — no GPU resources, no ownership subtleties — but
// it is large; move it rather than copying it.

#include "tri.h"   // Tri, TriEx (SpatialQueryLib)

#include <cstdint>
#include <vector>

namespace viewer {

// Canonical CPU representation of a part LOD. Raster upload and animation
// binding share this exact vertex/index stream after every LOD remesh.
struct IndexedPartGeometry {
    std::vector<float> vertices;          // 3 per vertex, part-local space
    std::vector<float> normals;           // 3 per vertex, unit length
    std::vector<unsigned char> colors;    // 4 per vertex, RGBA tint (0-255)
    std::vector<float> texcoords;         // 2 per vertex; NOT a UV from the
                                          // builder below -- see its comment
    std::vector<float> surface_uvs;       // 2 per vertex, the real surface UV
    std::vector<uint32_t> material_ids;   // 1 per vertex; UINT32_MAX = none
    std::vector<float> baked_ao;          // 1 per vertex, 0-1 (1 = unoccluded)
    int vertex_count = 0;                 // welded vertices; every per-vertex
                                          // array is this long times its stride
    std::vector<uint32_t> indices;        // 3 per triangle, into the arrays above
    // Warp field (VT Phase 2): per-vertex warped ground coordinate, filled by
    // a post-pass over terrain-sector rung meshes (part_store stages evaluate
    // the solved field at each welded vertex; see warp_field.h). Empty for
    // everything else — build_vulkan_part then leaves the vertex's warp data
    // zeroed, which the shader reads as "no warp" (world-XZ addressing).
    std::vector<float> warp_uvs;       // 2 per vertex, world-anchored metres
    std::vector<uint32_t> warp_frames; // 2 per vertex: oct tangent, (su, sv) f16
};

// Weld loose triangles into an indexed stream. `triex` may be null, in which
// case normals are computed per face (flat shading), AO is 1, the tint is white
// with alpha 0, UVs are (0,0), and every vertex takes `default_mat_id` — or
// UINT32_MAX when that is negative, the "no material" sentinel.
//
// Two vertices merge only on an EXACT bit-for-bit match of every attribute, so
// the result is deterministic for identical input but does not merge
// near-duplicates. Returns an empty geometry for a null pointer or
// tri_count <= 0.
//
// Note what lands in `texcoords`: this builder writes (material id as a float,
// baked AO) there, not a texture coordinate. The surface UV goes to
// surface_uvs, and the material id is also available as an integer in
// material_ids.
IndexedPartGeometry build_indexed_part_geometry(const Tri* tris, const TriEx* triex,
                                                int tri_count, float default_mat_id = -1.0f);

// Content fingerprint of one LOD's welded stream, for change detection (an
// unchanged signature means the same bytes, so the upload or rebind can be
// skipped). FNV-1a over a format tag, the lod_ordinal and every array listed in
// the struct above; never returns 0, so 0 is free as an "unset" sentinel.
//
// It does NOT cover warp_uvs / warp_frames — those are filled by a later
// post-pass — so a change confined to the warp field leaves the signature
// unchanged.
uint64_t indexed_part_geometry_signature(const IndexedPartGeometry& geometry,
                                         uint32_t lod_ordinal);

} // namespace viewer
