#pragma once
// SP-6 direct-triangle session, JS-free core.
//
// The SP-2 ScriptHost binds these into the QuickJS DSL as the direct-triangle
// session (mutually exclusive with the voxel session at any instant, sequential
// within a part):
//   beginShape(type)      -> TriangleBuildBuffer::beginShape(type, host.currentTransform(), host.currentMaterial())
//   vertex(x,y,z)         -> TriangleBuildBuffer::vertex({x,y,z})
//   endShape()            -> TriangleBuildBuffer::endShape()
//   line(a,b)/lineThickness(r) -> TriangleBuildBuffer::line(a, b, r0, r1, host.currentMaterial(), host.currentTransform())
//   instance(child, variation) -> VariationRecorder::instance(childSource, paramsBytes, host.currentTransform())
//
// At bake the host MERGES this buffer into the SAME build buffer the voxel
// session fills (appendTo), then registers ONE BLAS via
// BLASManager::register_triangles(tris, count, triex). There is NO separate
// triangle BLAS and NO triangle render path. The VariationRecorder's children()
// feed save_v2's child-instance table (SP-1). Triangles never enter the SDF.
#include "bvh.h"      // Tri, TriEx, mat4, float3, make_float3
#include "part_asset_v2.h"  // SP-1: part_asset::ChildInstance, part_asset::compute_resolved_hash
#include <vector>
#include <cstdint>

namespace tri_emit {

// Primitive types the direct-triangle session understands. Only TRIANGLES is
// needed for the SP-6 testing bullets; the others are reserved for the DSL
// surface form and fan/strip out to TRIANGLES at endShape().
enum class ShapeType { TRIANGLES, TRIANGLE_STRIP, TRIANGLE_FAN };

// Phase 3 (extrude). A 2D cross-section profile: one outer contour (CCW) plus
// optional hole contours (CW). Authored via beginShape(POLYGON) + beginContour
// in the DSL, captured here for the extrude sweep and for flat fills. Points are
// in the profile (u,v) plane; the contour winding mirrors the triangulator's.
struct Pt2 { float x, y; };
struct Profile {
    std::vector<Pt2>              outer;   // CCW outer boundary
    std::vector<std::vector<Pt2>> holes;   // each CW (opposite the outer)
    bool empty() const { return outer.size() < 3; }
};

// Wall-stitch style at interior polyline vertices (the joinType cursor).
enum class JoinType { MITER, BEVEL, ROUND };

// Accumulates direct triangles as (Tri, TriEx) pairs. Triangles are literal
// thin surfaces: transformed by the supplied matrix, tagged with a per-triangle
// material id, carrying neutral tint (1,1,1,0) and a face-normal shading
// fallback. NO SDF/field interaction. JS-free so it is unit-testable directly.
class TriangleBuildBuffer {
public:
    // tint defaults to neutral (1,1,1,0 = alpha 0 = no tint) so existing callers
    // are byte-identical; the DSL passes the tint cursor through (G4).
    void beginShape(ShapeType type, const mat4& transform, int material_id,
                    float4 tint = make_float4(1,1,1,0));
    void vertex(float3 position);   // local-space; transformed at endShape()
    void endShape();                // assembles pending vertices into Tri/TriEx

    // Append a radius-skinned segment as a swept tube: a ring of `segments`
    // vertices at a (radius r0) and b (radius r1), a quad band for the wall and
    // fan caps at each end. Hollow, smooth-tapered. `rings` is ignored (kept for
    // call-site compatibility).
    void line(float3 a, float3 b, float r0, float r1, int material_id,
              const mat4& transform, int rings = 4, int segments = 6,
              float4 tint = make_float4(1,1,1,0));

    // G8 mesh-mode solids: a closed UV sphere and a 12-triangle box, baked under
    // `transform` with per-triangle material + tint, exactly like line(). Used by
    // the DSL when sphere()/box() run outside a voxel session (None/mesh mode).
    void sphere(float3 center, float r, int material_id, const mat4& transform,
                int segments = 12, float4 tint = make_float4(1,1,1,0));
    void box(float3 center, float3 halfExtents, int material_id,
             const mat4& transform, float4 tint = make_float4(1,1,1,0));

    // Phase 5 round-primitive mesh solids. A circular wall of `segments` sides
    // swept from a to b, baked under `transform` with per-triangle material+tint.
    //
    // cappedCone: radius tapers r0 at a -> r1 at b with FLAT end caps (a fan to
    // the axis endpoint). r0==r1 is a straight cylinder; a zero radius collapses
    // that end to a true apex (no degenerate zero-area cap there). Watertight.
    void cappedCone(float3 a, float3 b, float r0, float r1, int material_id,
                    const mat4& transform, int segments = 16,
                    float4 tint = make_float4(1,1,1,0));
    // capsule: constant-radius `r` cylindrical wall from a to b plus a HEMISPHERE
    // cap (radius r) at each end. Smooth, watertight. `rings` is the number of
    // latitude bands per hemisphere.
    void capsule(float3 a, float3 b, float r, int material_id,
                 const mat4& transform, int segments = 16, int rings = 6,
                 float4 tint = make_float4(1,1,1,0));

    // Phase 3: sweep a 2D profile (concave, with holes) along a path (one segment
    // = 2 points, or a polyline). Emits a closed solid: ring-to-ring quad walls
    // (outer wound outward, holes wound inward), triangulated end caps on an open
    // path (none on a closed loop), and join geometry (MITER/BEVEL/ROUND) at
    // interior vertices. A rotation-minimizing (parallel-transport) frame carries
    // the profile so it does not twist at bends. Baked under `transform` with the
    // per-triangle material + tint, exactly like line().
    void extrude(const Profile& profile, const float3* path, int path_n,
                 JoinType join, int material_id, const mat4& transform,
                 float4 tint = make_float4(1,1,1,0));

    const std::vector<Tri>&   triangles() const { return tris_; }
    const std::vector<TriEx>& tri_extra() const { return triex_; }

    // Append this buffer's contents onto a host's triangle/triex arrays (the
    // SP-2 build-buffer merge seam). Used so the voxel-lowered mesh and the
    // direct triangles register as ONE BLAS.
    void appendTo(std::vector<Tri>& out_tris, std::vector<TriEx>& out_triex) const;

    void clear();

private:
    void emitTriangle(float3 p0, float3 p1, float3 p2, int material_id,
                      const mat4& transform,
                      float4 tint = make_float4(1,1,1,0));

    std::vector<Tri>   tris_;
    std::vector<TriEx> triex_;
    // pending shape state between beginShape/endShape
    ShapeType          cur_type_  = ShapeType::TRIANGLES;
    mat4               cur_xf_;
    int                cur_mat_   = 0;
    float4             cur_tint_  = make_float4(1,1,1,0);
    bool               open_      = false;
    std::vector<float3> verts_;   // local-space pending vertices
};

// Variation binding. SP-1 (part_asset_v2.h) is implemented at execution time, so
// we consume its real ChildInstance + compute_resolved_hash rather than a local
// mirror; they are re-exported into tri_emit so callers/tests use one name. The
// struct layout matches the SP-1 spec byte-for-byte (8 + 64 = 72 bytes).
using ChildInstance = part_asset::ChildInstance;
using part_asset::compute_resolved_hash;

// Records instance(child, variation) calls. "variation" = the params bytes bound
// at instance time; they fold into the child's resolved hash so identical params
// dedup to one artifact (consistent with SP-3). Independent of LOD.
class VariationRecorder {
public:
    // Records a child instance at the current transform; returns the child's
    // resolved hash (the cache key / artifact identity).
    uint64_t instance(const void* child_source, size_t source_len,
                      const void* variation_params, size_t params_len,
                      const mat4& transform);

    const std::vector<ChildInstance>& children() const { return children_; }
    void clear() { children_.clear(); }

private:
    std::vector<ChildInstance> children_;
};

} // namespace tri_emit
