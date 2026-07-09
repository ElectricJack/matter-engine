// DslState members that touch the direct-triangle mesh buffer live here, split
// out of dsl_state.cpp. triangle_emit.hpp pulls in MSL's precomp.h, whose
// `struct float3` collides with raymath.h's `float3`; dsl_state.cpp needs raymath
// for the matrix stack, so the two must be compiled in separate translation
// units. This TU includes triangle_emit.hpp but NOT raymath.h. raylib.h (via
// dsl_state.h) supplies the Matrix type without defining float3, so there is no
// clash here.
#include "dsl_state.h"
#include "triangle_emit.hpp"
#include "polygon_triangulate.hpp"

namespace dsl {

// Row-major 16-float copy of a raylib (column-major) Matrix. Local to this TU to
// avoid raymath.h; only reads Matrix fields, no raymath functions needed.
static mat4 top_mat4(const Matrix& m) {
    mat4 r;
    r.cell[0]=m.m0;  r.cell[1]=m.m4;  r.cell[2]=m.m8;   r.cell[3]=m.m12;
    r.cell[4]=m.m1;  r.cell[5]=m.m5;  r.cell[6]=m.m9;   r.cell[7]=m.m13;
    r.cell[8]=m.m2;  r.cell[9]=m.m6;  r.cell[10]=m.m10; r.cell[11]=m.m14;
    r.cell[12]=m.m3; r.cell[13]=m.m7; r.cell[14]=m.m11; r.cell[15]=m.m15;
    return r;
}

// raylib Vector4 (the tint cursor) -> bvh.h float4 for the triangle emitters.
static float4 tint_f4(const Vector4& t) { return make_float4(t.x, t.y, t.z, t.w); }

DslState::DslState()
    : tris_buf_(std::make_unique<tri_emit::TriangleBuildBuffer>()) {
    // Identity Matrix without raymath's MatrixIdentity() (field order is
    // m0,m4,m8,m12, m1,m5,m9,m13, ...).
    stack_.push_back(Matrix{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1});
}
DslState::~DslState() = default;

void DslState::beginShape(int mode) {
    if (session_ == Session::Voxels) { set_error("beginShape inside an open voxel session"); return; }
    if (session_ == Session::Triangles) { set_error("nested beginShape (call endShape first)"); return; }
    if (polygon_open_) { set_error("nested beginShape (call endShape first)"); return; }
    if (mode < 0 || mode > 3) { set_error("beginShape: mode must be 0(triangles),1(strip),2(fan),3(polygon)"); return; }
    // Opening any shape is a lazy-emission flush point: an unclaimed POLYGON
    // profile from a prior beginShape flat-fills here before the new shape opens.
    flush_retained_profile();
    if (mode == 3) {
        // POLYGON: an outline to fill or sweep, retained (lazy) at endShape().
        // We accumulate contours in DSL state, not in the triangle buffer.
        polygon_open_ = true; contour_open_ = false;
        poly_outer_.clear(); poly_holes_.clear();
        return;
    }
    session_ = Session::Triangles;
    tris_buf_->beginShape((tri_emit::ShapeType)mode, top_mat4(top()), (int)material_,
                          tint_f4(tint_));
}
void DslState::vertex(float x, float y, float z) {
    if (polygon_open_) {
        // POLYGON: 2D (u,v) cross-section point; z ignored. Routes to the active
        // hole contour if one is open, else the outer contour.
        if (contour_open_) poly_holes_.back().push_back(ProfilePoint2{x, y});
        else               poly_outer_.push_back(ProfilePoint2{x, y});
        return;
    }
    if (session_ != Session::Triangles) { set_error("vertex() outside a beginShape/endShape pair"); return; }
    tris_buf_->vertex(make_float3(x, y, z));
}
void DslState::endShape() {
    if (polygon_open_) {
        if (contour_open_) { set_error("endShape with an open beginContour (call endContour first)"); return; }
        // Finalize + RETAIN the contour set as the current profile (lazy emit). A
        // consumer verb (extrude) may claim it before the next flush point; if it
        // does not, the next beginShape/session change / build end flat-fills it.
        retained_.outer      = poly_outer_;
        retained_.holes      = poly_holes_;
        retained_.transform  = top();
        retained_.materialId = material_;
        retained_.tint       = tint_;
        retained_.valid      = true;
        polygon_open_ = false;
        poly_outer_.clear(); poly_holes_.clear();
        return;
    }
    if (session_ != Session::Triangles) { set_error("endShape with no open shape"); return; }
    tris_buf_->endShape();
    session_ = Session::None;
}
void DslState::beginContour() {
    if (!polygon_open_) { set_error("beginContour outside a POLYGON beginShape"); return; }
    if (contour_open_)  { set_error("nested beginContour (call endContour first)"); return; }
    if (poly_outer_.size() < 3) { set_error("beginContour before the outer contour has >=3 vertices"); return; }
    contour_open_ = true;
    poly_holes_.emplace_back();
}
void DslState::endContour() {
    if (!contour_open_) { set_error("endContour with no open beginContour"); return; }
    contour_open_ = false;
}
void DslState::line(float ax, float ay, float az, float bx, float by, float bz,
                    float r0, float r1) {
    // G1: in a voxel session, line() IS a capsule (sdSegment - r) -- the same
    // primitive as voxel capsule(). Uses the segment radius r0 (a capsule has a
    // single radius; the swept-tube taper r0!=r1 is a mesh-only nicety). In
    // None/Triangles it keeps the landed swept-tube mesh behavior.
    if (session_ == Session::Voxels) {
        emit_voxel_segment(BrushKind::Capsule, {ax,ay,az}, {bx,by,bz}, r0, r0, CsgOp::Union);
        return;
    }
    if (session_ == Session::Triangles) { set_error("line() inside a beginShape (call endShape first)"); return; }
    tris_buf_->line(make_float3(ax, ay, az), make_float3(bx, by, bz),
                    r0, r1, (int)material_, top_mat4(top()), 4, 6, tint_f4(tint_));
}

// Round primitives (Phase 4 / P2). Voxel session -> analytic SDF brush. None ->
// clean error (mesh emitters land in Phase 5). Triangles (mid-beginShape) -> error.
void DslState::capsule(const Vector3& a, const Vector3& b, float r, CsgOp op) {
    if (session_ == Session::Voxels) { emit_voxel_segment(BrushKind::Capsule, a, b, r, r, op); return; }
    if (session_ == Session::Triangles) {
        set_error("capsule() inside a beginShape (a solid is its own primitive; call endShape first)");
        return;
    }
    // None / mesh mode: a hemisphere-capped swept tube baked under the transform.
    tris_buf_->capsule(make_float3(a.x, a.y, a.z), make_float3(b.x, b.y, b.z), r,
                       (int)material_, top_mat4(top()), 16, 6, tint_f4(tint_));
}
void DslState::cylinder(const Vector3& a, const Vector3& b, float r, CsgOp op) {
    if (session_ == Session::Voxels) { emit_voxel_segment(BrushKind::Cylinder, a, b, r, r, op); return; }
    if (session_ == Session::Triangles) {
        set_error("cylinder() inside a beginShape (a solid is its own primitive; call endShape first)");
        return;
    }
    // None / mesh mode: a flat-capped circular wall (cappedCone with equal radii).
    tris_buf_->cappedCone(make_float3(a.x, a.y, a.z), make_float3(b.x, b.y, b.z),
                          r, r, (int)material_, top_mat4(top()), 16, tint_f4(tint_));
}
void DslState::cone(const Vector3& a, const Vector3& b, float r0, float r1, CsgOp op) {
    // cone is sugar that lowers to the tapered Cylinder (sdCappedCone). r1<r0 (or
    // r1=0) gives the taper; equal radii is a straight cylinder.
    if (session_ == Session::Voxels) { emit_voxel_segment(BrushKind::Cylinder, a, b, r0, r1, op); return; }
    if (session_ == Session::Triangles) {
        set_error("cone() inside a beginShape (a solid is its own primitive; call endShape first)");
        return;
    }
    // None / mesh mode: a tapered flat-capped circular wall; r1==0 closes to a
    // point (cappedCone handles the degenerate apex without zero-area caps).
    tris_buf_->cappedCone(make_float3(a.x, a.y, a.z), make_float3(b.x, b.y, b.z),
                          r0, r1, (int)material_, top_mat4(top()), 16, tint_f4(tint_));
}

// G8: sphere()/box() are session-polymorphic. The dispatch lives here (not
// dsl_state.cpp) because the mesh branch needs the TriangleBuildBuffer, which
// pulls in MSL headers that collide with raymath.h — so dsl_state.cpp delegates
// its voxel-brush body via emit_voxel_sphere/emit_voxel_box.
void DslState::sphere(const Vector3& c, float r, CsgOp op) {
    if (session_ == Session::Voxels) { emit_voxel_sphere(c, r, op); return; }
    if (session_ == Session::Triangles) {
        set_error("sphere() inside a beginShape (a solid is its own primitive; call endShape first)");
        return;
    }
    // None / mesh mode: a triangulated solid baked under the current transform.
    tris_buf_->sphere(make_float3(c.x, c.y, c.z), r, (int)material_, top_mat4(top()),
                      12, tint_f4(tint_));
}
void DslState::box(const Vector3& c, const Vector3& h, CsgOp op) {
    if (session_ == Session::Voxels) { emit_voxel_box(c, h, op); return; }
    if (session_ == Session::Triangles) {
        set_error("box() inside a beginShape (a solid is its own primitive; call endShape first)");
        return;
    }
    tris_buf_->box(make_float3(c.x, c.y, c.z), make_float3(h.x, h.y, h.z),
                   (int)material_, top_mat4(top()), tint_f4(tint_));
}

// Build a tri_emit::Profile from a DSL RetainedProfile (same 2D point layout,
// different struct type). Shared by extrude() and flush_retained_profile().
static tri_emit::Profile to_emit_profile(const RetainedProfile& r) {
    tri_emit::Profile p;
    p.outer.reserve(r.outer.size());
    for (const auto& v : r.outer) p.outer.push_back(tri_emit::Pt2{v.x, v.y});
    p.holes.reserve(r.holes.size());
    for (const auto& h : r.holes) {
        std::vector<tri_emit::Pt2> hc; hc.reserve(h.size());
        for (const auto& v : h) hc.push_back(tri_emit::Pt2{v.x, v.y});
        p.holes.push_back(std::move(hc));
    }
    return p;
}

// Map the joinType cursor to the triangle-buffer enum.
static tri_emit::JoinType to_emit_join(JoinKind j) {
    switch (j) {
        case JoinKind::Bevel: return tri_emit::JoinType::BEVEL;
        case JoinKind::Round: return tri_emit::JoinType::ROUND;
        default:              return tri_emit::JoinType::MITER;
    }
}

void DslState::extrude(const float* path_xyz, int path_n) {
    // Dispatch rules (fail-closed). Voxel extrude is deferred this phase.
    if (session_ == Session::Voxels) {
        set_error("extrude not supported in voxel session yet");
        return;
    }
    if (polygon_open_) {
        set_error("extrude inside an open beginShape (call endShape first)");
        return;
    }
    if (session_ == Session::Triangles) {
        set_error("extrude inside an open beginShape (call endShape first)");
        return;
    }
    if (!retained_.valid || retained_.empty()) {
        set_error("extrude with no retained POLYGON profile (author a beginShape(POLYGON) first)");
        return;
    }
    if (!path_xyz || path_n < 2) {
        set_error("extrude path needs at least 2 points");
        return;
    }
    std::vector<float3> path;
    path.reserve(path_n);
    for (int i = 0; i < path_n; ++i)
        path.push_back(make_float3(path_xyz[3*i+0], path_xyz[3*i+1], path_xyz[3*i+2]));

    tri_emit::Profile prof = to_emit_profile(retained_);
    tris_buf_->extrude(prof, path.data(), (int)path.size(),
                       to_emit_join(join_), (int)retained_.materialId,
                       top_mat4(retained_.transform), tint_f4(retained_.tint));
    // Consumed: suppress the flat fill.
    retained_.valid = false;
    retained_.outer.clear();
    retained_.holes.clear();
}

void DslState::begin_modifier_region() {
    if (region_open_) { set_error("beginModifier: modifier regions do not nest"); return; }
    if (session_ != Session::None) {
        set_error("beginModifier inside an open session (call it before beginVoxels/beginShape)");
        return;
    }
    region_open_ = true;
    region_start_op_  = buffer_.ops.size();
    region_start_tri_ = tris_buf_->triangles().size();
}

void DslState::end_modifier_region(std::vector<ModifierSpec> stack) {
    if (!region_open_) { set_error("endModifier without beginModifier"); return; }
    if (session_ != Session::None) {
        set_error("endModifier inside an open session (close the session first)");
        return;
    }
    ModifierRegion r;
    r.op_begin  = region_start_op_;
    r.op_end    = buffer_.ops.size();
    r.tri_begin = region_start_tri_;
    r.tri_end   = tris_buf_->triangles().size();
    r.stack     = std::move(stack);
    regions_.push_back(std::move(r));
    region_open_ = false;
}

void DslState::flush_retained_profile() {
    if (!retained_.valid) return;
    if (!retained_.empty()) {
        // Lazy flat fill: triangulate the (outer + holes) profile and emit each
        // triangle baked under the retained transform/material/tint. Indices from
        // the triangulator reference the concatenated [outer, holes...] list.
        poly_tri::Profile pt;
        pt.outer.reserve(retained_.outer.size());
        for (const auto& v : retained_.outer) pt.outer.push_back(poly_tri::P2{v.x, v.y});
        for (const auto& h : retained_.holes) {
            poly_tri::Contour hc; hc.reserve(h.size());
            for (const auto& v : h) hc.push_back(poly_tri::P2{v.x, v.y});
            pt.holes.push_back(std::move(hc));
        }
        std::vector<poly_tri::P2> all = pt.outer;
        for (const auto& h : pt.holes) all.insert(all.end(), h.begin(), h.end());

        std::vector<int> idx = poly_tri::triangulate(pt);
        if (!idx.empty()) {
            mat4 xf = top_mat4(retained_.transform);
            float4 tn = tint_f4(retained_.tint);
            // The flat face lies in the profile's local (u,v) plane at z=0; the
            // retained transform lifts it into world like any other primitive.
            tris_buf_->beginShape(tri_emit::ShapeType::TRIANGLES, xf,
                                  (int)retained_.materialId, tn);
            for (int k = 0; k + 2 < (int)idx.size(); k += 3) {
                const poly_tri::P2& a = all[idx[k+0]];
                const poly_tri::P2& b = all[idx[k+1]];
                const poly_tri::P2& c = all[idx[k+2]];
                tris_buf_->vertex(make_float3(a.x, a.y, 0.0f));
                tris_buf_->vertex(make_float3(b.x, b.y, 0.0f));
                tris_buf_->vertex(make_float3(c.x, c.y, 0.0f));
            }
            tris_buf_->endShape();
        }
    }
    retained_.valid = false;
    retained_.outer.clear();
    retained_.holes.clear();
}

} // namespace dsl
