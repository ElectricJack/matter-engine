#pragma once

#include "tri.h"   // Tri, TriEx (SpatialQueryLib)

#include <cstdint>
#include <vector>

namespace viewer {

// Canonical CPU representation of a part LOD. Raster upload and animation
// binding share this exact vertex/index stream after every LOD remesh.
struct IndexedPartGeometry {
    std::vector<float> vertices;
    std::vector<float> normals;
    std::vector<unsigned char> colors;
    std::vector<float> texcoords;
    std::vector<float> surface_uvs;
    std::vector<uint32_t> material_ids;
    std::vector<float> baked_ao;
    int vertex_count = 0;
    std::vector<uint32_t> indices;
    // Warp field (VT Phase 2): per-vertex warped ground coordinate, filled by
    // a post-pass over terrain-sector rung meshes (part_store stages evaluate
    // the solved field at each welded vertex; see warp_field.h). Empty for
    // everything else — build_vulkan_part then leaves the vertex's warp data
    // zeroed, which the shader reads as "no warp" (world-XZ addressing).
    std::vector<float> warp_uvs;       // 2 per vertex, world-anchored metres
    std::vector<uint32_t> warp_frames; // 2 per vertex: oct tangent, (su, sv) f16
};

IndexedPartGeometry build_indexed_part_geometry(const Tri* tris, const TriEx* triex,
                                                int tri_count, float default_mat_id = -1.0f);

uint64_t indexed_part_geometry_signature(const IndexedPartGeometry& geometry,
                                         uint32_t lod_ordinal);

} // namespace viewer
