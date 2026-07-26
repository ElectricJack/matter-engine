// Phase 3 (mathlib-and-raylib-removal plan) determinism harness.
//
// Purpose: catch any transform-convention regression introduced while porting
// dsl_state.cpp / csg_lowering.cpp from raylib Vector3/Matrix (raymath.h) onto
// MathLib's mm::Vec3/mm::Mat4. Two specific hazards this exists to catch:
//   1. dsl_state.cpp's DSL transform-stack builders (translate/rotateX/Y/Z/
//      scale/applyMatrix/lookAt) compose via raymath's MatrixMultiply, whose
//      operand order is REVERSED from mm::multiply (see matter_math.h's
//      documented policy). A mechanical find/replace without swapping
//      operands silently reverses the transform stack.
//   2. csg_lowering.cpp's local mat_invert has a zero-determinant guard that
//      must survive the port.
//
// This file deliberately calls DslState's public API using brace-init
// literals (`{x,y,z}`) rather than explicitly-typed local Vector3/mm::Vec3
// variables, so the SAME source compiles unchanged whether the underlying
// parameter type is raylib's Vector3/Matrix (pre-migration) or mm::Vec3/
// mm::Mat4 (post-migration) -- the point being to run this file, byte-for-
// byte unmodified, both before and after each migration chunk and diff the
// output.
//
// Output: for each authored scene, a stable FNV-1a 64-bit hash folding in
// every float/int the CSG lowering produced (StaticParticle positions/radii,
// carve particles, FatPrim invTransform + params, stage list), PLUS a grid
// sample of the analytic field_is_solid/field_distance oracle AND the real
// mesher's ProbeFieldScalar field (via mesh_field_is_solid, field_probe.cpp)
// -- covering both the CSG-lowering path and the runtime raycast/mesh path.
// A byte-identical stdout across a migration chunk is the pass criterion;
// this program does not itself judge pass/fail, it only prints.
//
// Review sweep (2026-07): an adversarial mutation pass found this harness
// blind to two things, both now closed --
//   - scene_apply_matrix_lookat called applyMatrix as the FIRST op on a
//     fresh (identity) stack, so reversing applyMatrix's operand order in
//     dsl_state.cpp was undetectable (multiply(I,A) == multiply(A,I)). Fixed
//     by composing a non-identity, non-commuting transform (translate +
//     rotateY) onto the stack before applyMatrix runs.
//   - no scene anywhere in the suite produced a singular (det==0) brush
//     transform, so csg_lowering.cpp's mat_invert zero-determinant guard
//     (`if (det==0.0f) return mm::zero();`) had zero coverage; swapping it
//     for the default-constructed `mm::Mat4{}` (IDENTITY, not zero --
//     matter_math.h's documented hazard) passed this file AND run-iso AND
//     run-script unchanged. Fixed by scene_degenerate_flatten, a single
//     brush under scale(1,0,1) (a legitimate flatten).
// Baseline stdout md5 after these two fixes: 42f0d97e72dd6cc4efba83db843f113e
// (previous baseline, before this sweep, was d4061d8156d5976f466b50fd1879cb22
// -- expected to change: this sweep adds/alters scenes). Verified: reversing
// applyMatrix's operands (dsl_state.cpp:62) changes apply_matrix_lookat's
// hashes; swapping mm::zero()->mm::Mat4{} (csg_lowering.cpp:63) changes
// degenerate_flatten's hashes; both revert to the md5 above.

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include "dsl_state.h"
#include "csg_lowering.h"

// Implemented in field_probe.cpp; links surface.c's real field evaluator.
bool mesh_field_is_solid(const dsl::LoweredField& f, Vector3 p);

namespace {

uint64_t fnv1a(uint64_t h, const void* data, size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}
uint64_t hf(uint64_t h, float v) { return fnv1a(h, &v, sizeof(v)); }
uint64_t hi(uint64_t h, int v)   { return fnv1a(h, &v, sizeof(v)); }
uint64_t hu(uint64_t h, unsigned v) { return fnv1a(h, &v, sizeof(v)); }

constexpr uint64_t kFnvSeed = 1469598103934665603ULL;

// Hash every float the CSG lowering produced, in vector index order (stable,
// single-threaded, no map/set iteration involved).
uint64_t hash_lowered_field(const dsl::LoweredField& f) {
    uint64_t h = kFnvSeed;

    for (const StaticParticle& sp : f.additive) {
        h = hf(h, sp.position.x); h = hf(h, sp.position.y); h = hf(h, sp.position.z);
        h = hf(h, sp.radius);
        h = hu(h, sp.materialId);
        h = hf(h, sp.tint.x); h = hf(h, sp.tint.y); h = hf(h, sp.tint.z); h = hf(h, sp.tint.w);
        h = hf(h, sp.detail_size);
    }
    for (int stage : f.additive_stage) h = hi(h, stage);

    for (const Particle& p : f.carve) {
        h = hf(h, p.position.x); h = hf(h, p.position.y); h = hf(h, p.position.z);
        h = hf(h, p.radius);
        h = hi(h, p.materialId);
    }

    h = hf(h, f.smoothing);

    for (const FatPrim& fp : f.fat) {
        h = hi(h, fp.kind);
        h = hf(h, fp.center.x); h = hf(h, fp.center.y); h = hf(h, fp.center.z);
        h = hf(h, fp.boundRadius);
        h = hi(h, fp.materialId);
        h = hf(h, fp.tint.x); h = hf(h, fp.tint.y); h = hf(h, fp.tint.z); h = hf(h, fp.tint.w);
        h = hi(h, fp.stage);
        // invTransform: raylib Matrix field-by-field (m0..m15), independent of
        // whatever internal type csg_lowering.cpp uses to compute it -- this
        // struct is MatterSurfaceLib-owned and does not migrate in Phase 3.
        h = hf(h, fp.invTransform.m0);  h = hf(h, fp.invTransform.m1);
        h = hf(h, fp.invTransform.m2);  h = hf(h, fp.invTransform.m3);
        h = hf(h, fp.invTransform.m4);  h = hf(h, fp.invTransform.m5);
        h = hf(h, fp.invTransform.m6);  h = hf(h, fp.invTransform.m7);
        h = hf(h, fp.invTransform.m8);  h = hf(h, fp.invTransform.m9);
        h = hf(h, fp.invTransform.m10); h = hf(h, fp.invTransform.m11);
        h = hf(h, fp.invTransform.m12); h = hf(h, fp.invTransform.m13);
        h = hf(h, fp.invTransform.m14); h = hf(h, fp.invTransform.m15);
        h = hf(h, fp.halfExtents.x); h = hf(h, fp.halfExtents.y); h = hf(h, fp.halfExtents.z);
        h = hf(h, fp.radius);
        h = hf(h, fp.segA.x); h = hf(h, fp.segA.y); h = hf(h, fp.segA.z);
        h = hf(h, fp.segB.x); h = hf(h, fp.segB.y); h = hf(h, fp.segB.z);
        h = hf(h, fp.r0); h = hf(h, fp.r1);
    }

    for (const Particle& p : f.staged_spheres) {
        h = hf(h, p.position.x); h = hf(h, p.position.y); h = hf(h, p.position.z);
        h = hf(h, p.radius);
        h = hi(h, p.materialId);
    }
    for (int stage : f.staged_stage) h = hi(h, stage);
    for (CsgStageOp op : f.stages) h = hi(h, (int)op);

    return h;
}

// Grid-sample the analytic oracle (field_is_solid/field_distance) and the
// real mesher field (mesh_field_is_solid) over [lo,hi]^3, folding sign +
// distance bits into the running hash. Exercises the raycast/runtime path
// (field_distance) and the mat_invert-per-op path (both oracles evaluate
// mat_invert(o.transform) for every grid point).
uint64_t hash_field_grid(const dsl::BuildBuffer& buf, const dsl::LoweredField& lowered,
                         float lo, float hi_, float step) {
    uint64_t h = kFnvSeed ^ 0xA5A5A5A5A5A5A5A5ULL;
    for (float x = lo; x <= hi_; x += step)
    for (float y = lo; y <= hi_; y += step)
    for (float z = lo; z <= hi_; z += step) {
        bool solid = dsl::field_is_solid(buf, {x, y, z});
        float d = dsl::field_distance(buf, 0, buf.ops.size(), 0.0f, {x, y, z});
        bool meshed = mesh_field_is_solid(lowered, Vector3{x, y, z});
        h = fnv1a(h, &solid, sizeof(solid));
        h = hf(h, d);
        h = fnv1a(h, &meshed, sizeof(meshed));
    }
    return h;
}

void run_scene(const char* name, dsl::DslState& s, float lo, float hi_, float step) {
    if (s.has_error()) {
        std::printf("scene=%-24s ERROR: %s\n", name, s.error().c_str());
        return;
    }
    const dsl::BuildBuffer& buf = s.buffer();
    dsl::LoweredField lowered = dsl::lower_build_buffer(buf);
    uint64_t lowered_hash = hash_lowered_field(lowered);
    uint64_t grid_hash = hash_field_grid(buf, lowered, lo, hi_, step);
    std::printf("scene=%-24s ops=%3zu additive=%3zu carve=%3zu fat=%3zu stages=%2zu "
                "lowered_hash=%016llx grid_hash=%016llx\n",
                name, buf.ops.size(), lowered.additive.size(), lowered.carve.size(),
                lowered.fat.size(), lowered.stages.size(),
                (unsigned long long)lowered_hash, (unsigned long long)grid_hash);
}

// --- Scenes ------------------------------------------------------------

void scene_flat(dsl::DslState& s) {
    s.beginVoxels(0.1f);
    s.fill(1);
    s.sphere({0, 0, 0}, 0.6f, dsl::CsgOp::Union);
    s.box({1.2f, 0, 0}, {0.3f, 0.4f, 0.5f}, dsl::CsgOp::Union);
    s.capsule({0, 1.0f, 0}, {0, 1.6f, 0}, 0.2f, dsl::CsgOp::Union);
    s.endVoxels();
}

// Triple-nested pushMatrix with translate + rotateX/Y/Z + scale composed at
// EACH level, and a Difference brush inside the innermost frame. Rotation
// composed with translation does not commute, so an operand-order bug in the
// MatrixMultiply->mm::multiply port changes this scene's geometry, unlike a
// translate-only scene.
void scene_nested_rotated_translated(dsl::DslState& s) {
    s.pushMatrix();
    s.translate(3, 0, 0);
    s.rotateY(0.6f);
    s.pushMatrix();
    s.translate(0, 1, 0);
    s.rotateX(0.35f);
    s.pushMatrix();
    s.scale(1.0f, 1.5f, 0.7f);
    s.translate(0.5f, 0, 0);
    s.rotateZ(0.2f);
    s.beginVoxels(0.1f);
    s.fill(2);
    s.sphere({0, 0, 0}, 0.6f, dsl::CsgOp::Union);
    s.box({0.3f, 0, 0}, {0.2f, 0.3f, 0.4f}, dsl::CsgOp::Union);
    s.capsule({-0.5f, 0, 0}, {0.5f, 0, 0}, 0.15f, dsl::CsgOp::Difference);
    s.endVoxels();
    s.popMatrix();
    s.rotateY(0.15f);
    s.beginVoxels(0.1f);
    s.fill(3);
    s.sphere({0, 0, 0.4f}, 0.3f, dsl::CsgOp::Union);
    s.endVoxels();
    s.popMatrix();
    s.translate(0, 0, 0.8f);
    s.beginVoxels(0.1f);
    s.fill(4);
    s.cylinder({0, -0.4f, 0}, {0, 0.4f, 0}, 0.25f, dsl::CsgOp::Union);
    s.endVoxels();
    s.popMatrix();
}

void scene_apply_matrix_lookat(dsl::DslState& s) {
    // Non-identity, non-commuting transform on the stack BEFORE applyMatrix
    // (gap fix, review sweep 2026-07): a fresh DslState's stack top is
    // identity, and multiply(I,A) == multiply(A,I), so calling applyMatrix
    // as the very first op made this scene blind to an operand-order bug in
    // DslState::applyMatrix (dsl_state.cpp) -- reversing
    // `mm::multiply(stack_.back(), applied)` to `mm::multiply(applied,
    // stack_.back())` produced byte-identical output. translate+rotateY
    // first means applyMatrix now composes against a non-identity stack top,
    // so operand order is observable.
    s.translate(1, 2, 3);
    s.rotateY(0.4f);
    float m[16] = {
        0.8f, -0.2f, 0.1f,  2.0f,
        0.1f,  0.9f, 0.05f,-1.0f,
       -0.05f, 0.1f, 0.95f, 0.5f,
        0.0f,  0.0f, 0.0f,  1.0f,
    };
    s.applyMatrix(m);
    s.pushMatrix();
    s.lookAt(5, 2, 0, 0, 1, 0);
    s.beginVoxels(0.1f);
    s.fill(5);
    s.sphere({0, 0, 0}, 0.4f, dsl::CsgOp::Union);
    s.cylinder({0, -0.5f, 0}, {0, 0.5f, 0}, 0.2f, dsl::CsgOp::Union);
    s.endVoxels();
    s.popMatrix();
    s.translate(0, 3, 0);
    s.rotateX(1.1f);
    s.beginVoxels(0.1f);
    s.fill(6);
    s.box({0, 0, 0}, {0.4f, 0.2f, 0.3f}, dsl::CsgOp::Union);
    s.endVoxels();
}

// Five levels deep, mixed rotate axes, non-uniform scale, ordered CSG
// (Union/Difference/Intersection) at multiple levels.
void scene_deep_stack(dsl::DslState& s) {
    s.beginVoxels(0.1f);
    s.fill(7);
    for (int level = 0; level < 5; ++level) {
        s.pushMatrix();
        s.translate(0.4f * level, 0.2f, -0.1f * level);
        if (level % 3 == 0) s.rotateX(0.25f + 0.1f * level);
        else if (level % 3 == 1) s.rotateY(0.2f - 0.05f * level);
        else s.rotateZ(0.15f * level);
        s.scale(1.0f - 0.05f * level, 1.0f + 0.05f * level, 1.0f);
        dsl::CsgOp op = (level == 2) ? dsl::CsgOp::Difference
                       : (level == 4) ? dsl::CsgOp::Intersection
                       : dsl::CsgOp::Union;
        s.sphere({0.1f, 0, 0}, 0.5f - 0.05f * level, op);
    }
    for (int level = 0; level < 5; ++level) s.popMatrix();
    s.endVoxels();
}

// Gap fix (review sweep 2026-07): a legitimate flatten -- scale(1,0,1) --
// collapses one axis of the brush transform to a singular (det==0) matrix.
// No other scene in this file ever produces a singular transform, so
// csg_lowering.cpp's mat_invert() zero-determinant guard (`if (det==0.0f)
// return mm::zero();`) had NO coverage anywhere in the suite: replacing that
// `mm::zero()` with the default-constructed `mm::Mat4{}` -- which is
// IDENTITY, not zero, per matter_math.h's documented hazard -- left this
// file byte-identical AND run-iso/run-script fully green. Under that
// mutation the flattened brush's invTransform silently becomes identity
// instead of zero, so the fat primitive would evaluate as if untransformed
// (full-size, at the world origin) instead of where the flatten put it --
// wrong geometry with nothing going red. This scene is a single brush with
// no other brush's field to mask the difference: mat_invert() runs on this
// op's transform both when csg_lowering builds the FatPrim's invTransform
// AND on every grid sample point in the analytic oracle, so both
// lowered_hash and grid_hash are sourced entirely from the guarded path.
void scene_degenerate_flatten(dsl::DslState& s) {
    s.pushMatrix();
    s.translate(2, 1, 0.5f);
    s.rotateY(0.3f);
    s.scale(1.0f, 0.0f, 1.0f);   // legitimate flatten -> singular transform (det==0)
    s.beginVoxels(0.1f);
    s.fill(8);
    s.sphere({0, 0, 0}, 0.5f, dsl::CsgOp::Union);
    s.endVoxels();
    s.popMatrix();
}

} // namespace

int main() {
    {
        dsl::DslState s;
        scene_flat(s);
        run_scene("flat", s, -1.5f, 2.0f, 0.5f);
    }
    {
        dsl::DslState s;
        scene_nested_rotated_translated(s);
        run_scene("nested_rotated_translated", s, 1.0f, 6.0f, 0.5f);
    }
    {
        dsl::DslState s;
        scene_apply_matrix_lookat(s);
        run_scene("apply_matrix_lookat", s, -2.0f, 6.0f, 0.5f);
    }
    {
        dsl::DslState s;
        scene_deep_stack(s);
        run_scene("deep_stack", s, -1.5f, 3.0f, 0.5f);
    }
    {
        dsl::DslState s;
        scene_degenerate_flatten(s);
        run_scene("degenerate_flatten", s, 0.0f, 3.0f, 0.5f);
    }
    std::printf("DETERMINISM HARNESS DONE\n");
    return 0;
}
