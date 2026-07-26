#include "dsl_state.h"
#include <cmath>

// NOTE: The TriangleBuildBuffer-touching members (ctor/dtor + beginShape/vertex/
// endShape/line) live in dsl_triangle.cpp, NOT here. triangle_emit.hpp pulls in
// MSL's precomp.h, whose `struct float3` collides with raymath.h's `float3`, so
// this TU stayed free of it even back when it needed raymath for the matrix
// stack. Phase 3 (mathlib-and-raylib-removal) moved the matrix stack onto
// mm::Mat4/mm::Vec3 (matter_math.h, pulled in via dsl_state.h), so this TU no
// longer includes raymath.h at all -- kept free of the float3 collision as a
// side effect rather than by design now, but the split from dsl_triangle.cpp
// stays since triangle_emit.hpp is still off-limits here.
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

// --- Transform stack -------------------------------------------------------
// Phase 3 (mathlib-and-raylib-removal): ported off raymath's MatrixMultiply/
// MatrixTranslate/MatrixRotate*/MatrixScale onto mm:: (matter_math.h).
//
// mm::translation/rotation_x/rotation_y/rotation_z/scale are drop-in
// replacements for raymath's MatrixTranslate/MatrixRotateX/Y/Z/MatrixScale --
// verified element-for-element against the vendored third_party/raylib/src/
// raymath.h source (both produce the identical row-major float[16], no sign
// or transpose difference for any of the five builders).
//
// MatrixMultiply is NOT a drop-in match: raymath's MatrixMultiply(a, b)
// applies `a` first, then `b`, which is the REVERSE of mm::multiply(a, b)'s
// convention (matter_math.h's documented policy: MatrixMultiply(a,b) ==
// mm::multiply(b,a)). Every call below was originally
// `MatrixMultiply(<new op>, stack_.back())` -- apply the new op first, then
// the existing stack top -- so the ported form swaps operands to
// `mm::multiply(stack_.back(), <new op>)`. Getting this backwards would
// silently reverse the entire DSL transform stack (see
// dsl_determinism_tests.cpp, which exists specifically to catch it).
void DslState::pushMatrix() { stack_.push_back(stack_.back()); }
void DslState::popMatrix() {
    if (stack_.size() <= 1) { set_error("popMatrix without matching pushMatrix"); return; }
    stack_.pop_back();
}
void DslState::translate(float x, float y, float z) {
    stack_.back() = mm::multiply(stack_.back(), mm::translation({x,y,z}));
}
void DslState::rotateX(float r){ stack_.back() = mm::multiply(stack_.back(), mm::rotation_x(r)); }
void DslState::rotateY(float r){ stack_.back() = mm::multiply(stack_.back(), mm::rotation_y(r)); }
void DslState::rotateZ(float r){ stack_.back() = mm::multiply(stack_.back(), mm::rotation_z(r)); }
void DslState::scale(float x,float y,float z){ stack_.back() = mm::multiply(stack_.back(), mm::scale({x,y,z})); }
void DslState::applyMatrix(const float m[16]) {
    mm::Mat4 applied;
    for (int i = 0; i < 16; ++i) applied.m[i] = m[i];
    stack_.back() = mm::multiply(stack_.back(), applied);
}
void DslState::lookAt(float tx, float ty, float tz,
                      float upx, float upy, float upz) {
    // Orient the current frame so its +Z (forward) points from the current frame
    // origin toward the target, composed onto the stack top. The frame origin is
    // the translation of the current matrix (position()). We build a rotation in
    // current-frame-local space, so it composes with the existing rotation/scale.
    mm::Vec3 origin = position();
    mm::Vec3 fwd = mm::sub({tx,ty,tz}, origin);
    if (mm::length(fwd) < 1e-9f) return;   // degenerate: target == origin, no-op
    fwd = mm::normalize(fwd);
    mm::Vec3 up = {upx,upy,upz};
    if (mm::length(up) < 1e-9f) up = mm::Vec3{0,1,0};
    up = mm::normalize(up);
    // Right = up x fwd; if up is parallel to fwd, pick a fallback up.
    mm::Vec3 right = mm::cross(up, fwd);
    if (mm::length(right) < 1e-6f) {
        up = (fabsf(fwd.y) < 0.9f) ? mm::Vec3{0,1,0} : mm::Vec3{1,0,0};
        right = mm::cross(up, fwd);
    }
    right = mm::normalize(right);
    mm::Vec3 trueUp = mm::cross(fwd, right);  // orthonormal
    // Column-basis rotation matrix mapping local +X->right, +Y->trueUp, +Z->fwd:
    // basis vectors go in COLUMNS of the row-major mm::Mat4 (column-vector
    // convention) so a local axis maps to its world basis vector.
    mm::Mat4 rot = mm::zero();
    rot.m[0]=right.x; rot.m[1]=trueUp.x; rot.m[2]=fwd.x; rot.m[3]=0;
    rot.m[4]=right.y; rot.m[5]=trueUp.y; rot.m[6]=fwd.y; rot.m[7]=0;
    rot.m[8]=right.z; rot.m[9]=trueUp.z; rot.m[10]=fwd.z; rot.m[11]=0;
    rot.m[12]=0;      rot.m[13]=0;       rot.m[14]=0;     rot.m[15]=1;
    // The basis above is expressed in the SAME space as the target/origin (i.e.
    // already-composed world). Replace the stack top's rotation while preserving
    // its translation (origin) so the oriented frame sits at the current position.
    mm::Mat4 m = mm::translation(origin);
    stack_.back() = mm::multiply(m, rot);
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

void DslState::emit_voxel_sphere(const mm::Vec3& c, float r, CsgOp op) {
    BuildOp o{}; o.kind=BrushKind::Sphere; o.op=op; o.transform=stack_.back();
    o.materialId=material_; o.center=c; o.radius=r; o.smoothing=smoothing_; o.spacing=spacing_;
    o.tint=tint_;
    buffer_.ops.push_back(o);
}
void DslState::emit_voxel_box(const mm::Vec3& c, const mm::Vec3& h, CsgOp op) {
    BuildOp o{}; o.kind=BrushKind::Box; o.op=op; o.transform=stack_.back();
    o.materialId=material_; o.center=c; o.halfExtents=h; o.smoothing=smoothing_; o.spacing=spacing_;
    o.tint=tint_;
    buffer_.ops.push_back(o);
}
// Capsule (sdSegment - r0) / cylinder|cone (sdCappedCone) brush. `center` holds
// segment endpoint a, `segB` endpoint b, `radius`=r0 and `r1`=r1 (capsule passes
// r1==r0; lowering uses only r0 for the capsule kind).
void DslState::emit_voxel_segment(BrushKind kind, const mm::Vec3& a, const mm::Vec3& b,
                                  float r0, float r1, CsgOp op) {
    BuildOp o{}; o.kind=kind; o.op=op; o.transform=stack_.back();
    o.materialId=material_; o.center=a; o.segB=b; o.radius=r0; o.r1=r1;
    o.smoothing=smoothing_; o.spacing=spacing_; o.tint=tint_;
    buffer_.ops.push_back(o);
}

bool DslState::raycast(const mm::Vec3& origin, const mm::Vec3& dir,
                       mm::Vec3& outPoint, mm::Vec3& outNormal) {
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
    mm::Vec3 d = { dir.x/len, dir.y/len, dir.z/len };

    const size_t b = session_start_, e = buffer_.ops.size();
    const float k = smoothing_;
    // field_distance's public signature stays raylib Vector3 (csg_lowering.h,
    // shared with test-oracle callers); convert at this one boundary.
    auto field = [&](const mm::Vec3& p) {
        return field_distance(buffer_, b, e, k, Vector3{p.x, p.y, p.z});
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
    mm::Vec3 p = origin;
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
            mm::Vec3 mp = { origin.x + d.x*mid, origin.y + d.y*mid, origin.z + d.z*mid };
            if (field(mp) > 0.0f) lo = mid; else hi = mid;
        }
        float tHit = 0.5f * (lo + hi);
        outPoint = { origin.x + d.x*tHit, origin.y + d.y*tHit, origin.z + d.z*tHit };
    }

    // Central-difference gradient, normalized outward.
    const float h = fmaxf(1e-3f, 0.25f * spacing_);
    mm::Vec3 g = {
        field({outPoint.x + h, outPoint.y, outPoint.z}) - field({outPoint.x - h, outPoint.y, outPoint.z}),
        field({outPoint.x, outPoint.y + h, outPoint.z}) - field({outPoint.x, outPoint.y - h, outPoint.z}),
        field({outPoint.x, outPoint.y, outPoint.z + h}) - field({outPoint.x, outPoint.y, outPoint.z - h}),
    };
    float gl = sqrtf(g.x*g.x + g.y*g.y + g.z*g.z);
    if (gl < 1e-9f) { outNormal = { -d.x, -d.y, -d.z }; }
    else            { outNormal = { g.x/gl, g.y/gl, g.z/gl }; }
    return true;
}

// mm::Mat4 is already row-major float[16] (matter_math.h), matching what
// world_flatten / ChildInstance expect -- a straight copy.
static void matrix_to_row16(const mm::Mat4& m, float out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = m.m[i];
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
    if (params_json && len > 0) {
        // params_json is expected to be CANONICAL (normalized via params_from_json ->
        // params_to_json by the caller) so this is an exact key match. The name2hash
        // table is keyed by params_to_json bytes (part_graph.cpp:149); callers in
        // dsl_bindings.cpp normalize raw JS_JSONStringify output before invoking us.
        // On miss: return false — no bare-module fallback for the with-params case
        // (that silent fallback was the bug causing all parametric placements of the
        // same module to resolve to one hash regardless of params).
        std::string key = module;
        key.push_back('\x1f');
        key.append(params_json, len);
        auto it = child_hashes_.find(key);
        if (it != child_hashes_.end()) { out = it->second; return true; }
        return false;
    }
    // No params: plain module-only lookup (unchanged behavior).
    auto it = child_hashes_.find(module);
    if (it == child_hashes_.end()) return false;
    out = it->second;
    return true;
}

void DslState::placeChild(const std::string& module,
                          const void* params, size_t params_len,
                          bool instanced, float inline_below_px) {
    uint64_t hash = 0;
    const char* params_ptr = static_cast<const char*>(params);
    if (!lookup_child_hash(module, params_ptr, params_len, hash)) {
        if (params_ptr && params_len > 0) {
            // With params: composite-key miss — no matching declared variant.
            // params_ptr is already normalized (canonical JSON) per the contract
            // set by dsl_bindings.cpp. Include it in the error for diagnostics.
            set_error("placeChild: no declared child of '" + module +
                      "' matches params " +
                      std::string(params_ptr, params_len) +
                      " (add it to static requires)");
        } else {
            set_error("placeChild: undeclared child '" + module +
                      "' (add it to static requires)");
        }
        return;
    }
    ChildPlacement p;
    p.hash = hash;
    matrix_to_row16(top(), p.transform);
    p.instanced = instanced;
    p.inline_below_px = inline_below_px;
    p.module = module;
    children_.push_back(p);
}

void DslState::emit_volume(const VolumeEmitter& e) {
    if (e.radius <= 0.0f) { set_error("emitVolume: radius must be > 0"); return; }
    if (e.length <= 0.0f) { set_error("emitVolume: length must be > 0"); return; }
    if (e.density < 0.0f) { set_error("emitVolume: density must be >= 0"); return; }
    // Normalize the direction vector; zero-length is an error.
    float dx = e.dir[0], dy = e.dir[1], dz = e.dir[2];
    float len = sqrtf(dx*dx + dy*dy + dz*dz);
    if (len < 1e-9f) { set_error("emitVolume: dir must be non-zero"); return; }
    VolumeEmitter normed = e;
    normed.dir[0] = dx / len;
    normed.dir[1] = dy / len;
    normed.dir[2] = dz / len;
    emitters_.push_back(normed);
}

} // namespace dsl
