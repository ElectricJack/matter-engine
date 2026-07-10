#include "dsl_state.h"
#define RAYMATH_IMPLEMENTATION
#include "raymath.h"
#include <cmath>

// NOTE: The TriangleBuildBuffer-touching members (ctor/dtor + beginShape/vertex/
// endShape/line) live in dsl_triangle.cpp, NOT here. triangle_emit.hpp pulls in
// MSL's precomp.h, whose `struct float3` collides with raymath.h's `float3`, so
// this TU (which needs raymath for the matrix stack) must stay free of it.
//
// csg_lowering.h carries the same transitive float3 collision (cluster.h ->
// vertex_ao.h -> bvh.h). Forward-declare the one function we need instead of
// including the full header.

// Forward-declaration: avoids the float3 collision from csg_lowering.h's
// transitive includes (bvh.h defines float3{x,y,z} which conflicts with
// raymath.h's float3{v[3]}). Implemented in csg_lowering.cpp.
namespace dsl {
    float field_distance(const BuildBuffer& buf, size_t opBegin, size_t opEnd,
                         float k, const Vector3& worldPoint);
}

namespace dsl {

void DslState::pushMatrix() { stack_.push_back(stack_.back()); }
void DslState::popMatrix() {
    if (stack_.size() <= 1) { set_error("popMatrix without matching pushMatrix"); return; }
    stack_.pop_back();
}
void DslState::translate(float x, float y, float z) {
    stack_.back() = MatrixMultiply(MatrixTranslate(x,y,z), stack_.back());
}
void DslState::rotateX(float r){ stack_.back()=MatrixMultiply(MatrixRotateX(r),stack_.back()); }
void DslState::rotateY(float r){ stack_.back()=MatrixMultiply(MatrixRotateY(r),stack_.back()); }
void DslState::rotateZ(float r){ stack_.back()=MatrixMultiply(MatrixRotateZ(r),stack_.back()); }
void DslState::scale(float x,float y,float z){ stack_.back()=MatrixMultiply(MatrixScale(x,y,z),stack_.back()); }
void DslState::applyMatrix(const float m[16]) {
    Matrix mm = { m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7],
                  m[8],m[9],m[10],m[11], m[12],m[13],m[14],m[15] };
    stack_.back() = MatrixMultiply(mm, stack_.back());
}
void DslState::lookAt(float tx, float ty, float tz,
                      float upx, float upy, float upz) {
    // Orient the current frame so its +Z (forward) points from the current frame
    // origin toward the target, composed onto the stack top. The frame origin is
    // the translation of the current matrix (position()). We build a rotation in
    // current-frame-local space, so it composes with the existing rotation/scale.
    Vector3 origin = position();
    Vector3 fwd = Vector3Subtract(Vector3{tx,ty,tz}, origin);
    if (Vector3Length(fwd) < 1e-9f) return;   // degenerate: target == origin, no-op
    fwd = Vector3Normalize(fwd);
    Vector3 up = Vector3{upx,upy,upz};
    if (Vector3Length(up) < 1e-9f) up = Vector3{0,1,0};
    up = Vector3Normalize(up);
    // Right = up x fwd; if up is parallel to fwd, pick a fallback up.
    Vector3 right = Vector3CrossProduct(up, fwd);
    if (Vector3Length(right) < 1e-6f) {
        up = (fabsf(fwd.y) < 0.9f) ? Vector3{0,1,0} : Vector3{1,0,0};
        right = Vector3CrossProduct(up, fwd);
    }
    right = Vector3Normalize(right);
    Vector3 trueUp = Vector3CrossProduct(fwd, right);  // orthonormal
    // Column-basis rotation matrix mapping local +X->right, +Y->trueUp, +Z->fwd.
    // raylib Matrix is column-major (m0,m4,m8 = first row); basis vectors go in
    // columns so a local axis maps to its world basis vector.
    Matrix rot = {
        right.x, trueUp.x, fwd.x, 0,
        right.y, trueUp.y, fwd.y, 0,
        right.z, trueUp.z, fwd.z, 0,
        0,       0,        0,     1
    };
    // The basis above is expressed in the SAME space as the target/origin (i.e.
    // already-composed world). Replace the stack top's rotation while preserving
    // its translation (origin) so the oriented frame sits at the current position.
    Matrix m = MatrixTranslate(origin.x, origin.y, origin.z);
    stack_.back() = MatrixMultiply(rot, m);
}

void DslState::beginVoxels(float spacing) {
    if (session_ != Session::None) { set_error("beginVoxels inside an open session"); return; }
    // A session change is a lazy-emission flush point: any unclaimed POLYGON
    // profile flat-fills here before the voxel session opens (P3).
    flush_retained_profile();
    session_ = Session::Voxels; spacing_ = (spacing > 0 ? spacing : 0.1f);
    session_start_ = buffer_.ops.size();
}
void DslState::endVoxels() {
    if (session_ != Session::Voxels) { set_error("endVoxels with no open voxel session"); return; }
    // Whole-expression smoothing: the spec applies smoothing(k) to the whole
    // session's union, not just the brush emitted while the cursor was set. Stamp
    // the final cursor onto every op emitted in this session so smoothing(k)
    // called anywhere in the build (incl. after the brushes) takes effect.
    for (size_t i = session_start_; i < buffer_.ops.size(); ++i)
        buffer_.ops[i].smoothing = smoothing_;
    session_ = Session::None;
}

void DslState::emit_voxel_sphere(const Vector3& c, float r, CsgOp op) {
    BuildOp o{}; o.kind=BrushKind::Sphere; o.op=op; o.transform=stack_.back();
    o.materialId=material_; o.center=c; o.radius=r; o.smoothing=smoothing_; o.spacing=spacing_;
    o.tint=tint_;
    buffer_.ops.push_back(o);
}
void DslState::emit_voxel_box(const Vector3& c, const Vector3& h, CsgOp op) {
    BuildOp o{}; o.kind=BrushKind::Box; o.op=op; o.transform=stack_.back();
    o.materialId=material_; o.center=c; o.halfExtents=h; o.smoothing=smoothing_; o.spacing=spacing_;
    o.tint=tint_;
    buffer_.ops.push_back(o);
}
// Capsule (sdSegment - r0) / cylinder|cone (sdCappedCone) brush. `center` holds
// segment endpoint a, `segB` endpoint b, `radius`=r0 and `r1`=r1 (capsule passes
// r1==r0; lowering uses only r0 for the capsule kind).
void DslState::emit_voxel_segment(BrushKind kind, const Vector3& a, const Vector3& b,
                                  float r0, float r1, CsgOp op) {
    BuildOp o{}; o.kind=kind; o.op=op; o.transform=stack_.back();
    o.materialId=material_; o.center=a; o.segB=b; o.radius=r0; o.r1=r1;
    o.smoothing=smoothing_; o.spacing=spacing_; o.tint=tint_;
    buffer_.ops.push_back(o);
}

bool DslState::raycast(const Vector3& origin, const Vector3& dir,
                       Vector3& outPoint, Vector3& outNormal) {
    if (session_ != Session::Voxels) {
        set_error("raycast outside an open voxel session");
        return false;
    }
    if (buffer_.ops.size() <= session_start_) {
        set_error("raycast with no brushes emitted in this session");
        return false;
    }
    float len = sqrtf(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    if (len < 1e-9f) { set_error("raycast with zero-length direction"); return false; }
    Vector3 d = { dir.x/len, dir.y/len, dir.z/len };

    const size_t b = session_start_, e = buffer_.ops.size();
    const float k = smoothing_;
    auto field = [&](const Vector3& p) {
        return field_distance(buffer_, b, e, k, p);
    };

    // Conservative sphere trace. The smin union under-reports distance (safe);
    // Difference/Intersection folds can over-report near carve boundaries, so
    // scale steps down and cap them. Bisect once a sign change brackets the hit.
    const float kStepScale = 0.7f;
    const float kMaxStep   = 0.5f;
    const float kMaxT      = 100.0f;
    const int   kMaxSteps  = 512;
    const float kEps       = 1e-4f;

    float t = 0.0f;
    Vector3 p = origin;
    float fd = field(p);
    if (fd <= kEps) {
        // Started on/inside the surface: report the origin itself.
        outPoint = origin;
    } else {
        float tPrev = t, fdPrev = fd;
        bool hit = false;
        for (int i = 0; i < kMaxSteps && t < kMaxT; ++i) {
            float step = fd * kStepScale;
            if (step > kMaxStep) step = kMaxStep;
            if (step < kEps)     step = kEps;
            tPrev = t; fdPrev = fd;
            t += step;
            p = { origin.x + d.x*t, origin.y + d.y*t, origin.z + d.z*t };
            fd = field(p);
            if (fd <= 0.0f) { hit = true; break; }
        }
        if (!hit) return false;   // miss: not an error
        // Bisection refine within [tPrev, t].
        float lo = tPrev, hi = t;
        (void)fdPrev;
        for (int i = 0; i < 32; ++i) {
            float mid = 0.5f * (lo + hi);
            Vector3 mp = { origin.x + d.x*mid, origin.y + d.y*mid, origin.z + d.z*mid };
            if (field(mp) > 0.0f) lo = mid; else hi = mid;
        }
        float tHit = 0.5f * (lo + hi);
        outPoint = { origin.x + d.x*tHit, origin.y + d.y*tHit, origin.z + d.z*tHit };
    }

    // Central-difference gradient, normalized outward.
    const float h = fmaxf(1e-3f, 0.25f * spacing_);
    Vector3 g = {
        field({outPoint.x + h, outPoint.y, outPoint.z}) - field({outPoint.x - h, outPoint.y, outPoint.z}),
        field({outPoint.x, outPoint.y + h, outPoint.z}) - field({outPoint.x, outPoint.y - h, outPoint.z}),
        field({outPoint.x, outPoint.y, outPoint.z + h}) - field({outPoint.x, outPoint.y, outPoint.z - h}),
    };
    float gl = sqrtf(g.x*g.x + g.y*g.y + g.z*g.z);
    if (gl < 1e-9f) { outNormal = { -d.x, -d.y, -d.z }; }
    else            { outNormal = { g.x/gl, g.y/gl, g.z/gl }; }
    return true;
}

// raylib Matrix stores its 16 floats column-major (m0,m4,m8,m12 = first row of
// the math matrix). world_flatten / ChildInstance consume row-major, so transpose
// the storage: translation (m12,m13,m14) lands in out[3],out[7],out[11].
static void matrix_to_row16(const Matrix& mm, float out[16]) {
    out[0]=mm.m0;  out[1]=mm.m4;  out[2]=mm.m8;  out[3]=mm.m12;
    out[4]=mm.m1;  out[5]=mm.m5;  out[6]=mm.m9;  out[7]=mm.m13;
    out[8]=mm.m2;  out[9]=mm.m6;  out[10]=mm.m10; out[11]=mm.m14;
    out[12]=mm.m3; out[13]=mm.m7; out[14]=mm.m11; out[15]=mm.m15;
}

bool DslState::has_composite_child_key(const std::string& module,
                                       const char* params_json, size_t len) {
    if (!params_json || len == 0) return false;
    std::string key = module;
    key.push_back('\x1f');
    key.append(params_json, len);
    return child_hashes_.find(key) != child_hashes_.end();
}

bool DslState::lookup_child_hash(const std::string& module,
                                 const char* params_json, size_t len,
                                 uint64_t& out) {
    const auto end = child_hashes_.end();
    auto it = end;
    if (params_json && len > 0) {
        std::string key = module;
        key.push_back('\x1f');
        key.append(params_json, len);
        it = child_hashes_.find(key);
    }
    if (it == end) it = child_hashes_.find(module);
    if (it == end) return false;
    out = it->second;
    return true;
}

void DslState::placeChild(const std::string& module,
                          const void* params, size_t params_len,
                          bool instanced, float inline_below_px) {
    uint64_t hash = 0;
    if (!lookup_child_hash(module,
                           static_cast<const char*>(params), params_len,
                           hash)) {
        set_error("placeChild: undeclared child '" + module +
                  "' (add it to static requires)");
        return;
    }
    ChildPlacement p;
    p.hash = hash;
    matrix_to_row16(top(), p.transform);
    p.instanced = instanced;
    p.inline_below_px = inline_below_px;
    children_.push_back(p);
}

} // namespace dsl
