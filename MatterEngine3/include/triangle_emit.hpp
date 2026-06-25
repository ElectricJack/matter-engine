#pragma once
#include "bvh.h"      // Tri, TriEx, mat4, float3, make_float3
#include "part_asset_v2.h"  // SP-1: part_asset::ChildInstance, part_asset::compute_resolved_hash
#include <vector>
#include <cstdint>

namespace tri_emit {

// Primitive types the direct-triangle session understands. Only TRIANGLES is
// needed for the SP-6 testing bullets; the others are reserved for the DSL
// surface form and fan/strip out to TRIANGLES at endShape().
enum class ShapeType { TRIANGLES, TRIANGLE_STRIP, TRIANGLE_FAN };

// Accumulates direct triangles as (Tri, TriEx) pairs. Triangles are literal
// thin surfaces: transformed by the supplied matrix, tagged with a per-triangle
// material id, carrying neutral tint (1,1,1,0) and a face-normal shading
// fallback. NO SDF/field interaction. JS-free so it is unit-testable directly.
class TriangleBuildBuffer {
public:
    void beginShape(ShapeType type, const mat4& transform, int material_id);
    void vertex(float3 position);   // local-space; transformed at endShape()
    void endShape();                // assembles pending vertices into Tri/TriEx

    // Append a radius-skinned segment as stepped-sphere solid triangles (Task 3).
    // r0 = radius at a, r1 = radius at b (lerped); step_count spheres along seg.
    void line(float3 a, float3 b, float r0, float r1, int material_id,
              const mat4& transform, int rings = 6, int segments = 8);

    const std::vector<Tri>&   triangles() const { return tris_; }
    const std::vector<TriEx>& tri_extra() const { return triex_; }

    // Append this buffer's contents onto a host's triangle/triex arrays (the
    // SP-2 build-buffer merge seam). Used so the voxel-lowered mesh and the
    // direct triangles register as ONE BLAS.
    void appendTo(std::vector<Tri>& out_tris, std::vector<TriEx>& out_triex) const;

    void clear();

private:
    void emitTriangle(float3 p0, float3 p1, float3 p2, int material_id,
                      const mat4& transform);

    std::vector<Tri>   tris_;
    std::vector<TriEx> triex_;
    // pending shape state between beginShape/endShape
    ShapeType          cur_type_  = ShapeType::TRIANGLES;
    mat4               cur_xf_;
    int                cur_mat_   = 0;
    bool               open_      = false;
    std::vector<float3> verts_;   // local-space pending vertices
};

} // namespace tri_emit
