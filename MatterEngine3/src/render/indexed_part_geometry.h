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
};

IndexedPartGeometry build_indexed_part_geometry(const Tri* tris, const TriEx* triex,
                                                int tri_count, float default_mat_id = -1.0f);

uint64_t indexed_part_geometry_signature(const IndexedPartGeometry& geometry,
                                         uint32_t lod_ordinal);

} // namespace viewer
