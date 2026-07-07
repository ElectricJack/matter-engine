#ifndef MSL_MESH_INDEXED_HPP
#define MSL_MESH_INDEXED_HPP

#include "bvh.h"  // Tri, TriEx, float3

#include <cstdint>
#include <vector>

// Shared indexed mesh format for MatterSurfaceLib's mesh-transformation
// pipeline. Both mesh_simplifier and mesh_retopo consume and produce this.
// Non-indexed Tri is used only at the BLAS boundary.
struct MeshIndexed {
    std::vector<float3>   positions;
    std::vector<uint32_t> indices;      // 3 per triangle
    std::vector<TriEx>    triex;        // optional; parallel to triangles
                                         // (size == indices.size()/3 when present)
                                         // empty vector = TriEx not attached
};

// Weld tolerance for from_tri. Default matches mesh_simplifier's existing
// internal weld (1e-4 world units).
struct WeldOptions {
    float epsilon = 1e-4f;
};

// Weld the non-indexed Tri list into an indexed MeshIndexed. If `triex` is
// non-null it must be parallel to `tris` (size == tris.size()); triangles keep
// their per-triangle TriEx in the output.
MeshIndexed from_tri(const std::vector<Tri>& tris,
                     const std::vector<TriEx>* triex,
                     const WeldOptions& opts = {});

// Unweld back to non-indexed Tri, one triangle per 3 indices in emission order.
// `triex_out` is populated parallel to `tris_out` iff `in.triex` was populated.
void to_tri(const MeshIndexed& in,
            std::vector<Tri>& tris_out,
            std::vector<TriEx>& triex_out);

#endif // MSL_MESH_INDEXED_HPP
