#include "csg_lowering.h"
#include <cmath>
#include <cstring>
#include <vector>

// NOTE: raymath.h cannot be included in this TU: cluster.h transitively pulls in
// the prototype's precomp.h `struct float3` (x/y/z members) which collides with
// raymath's `struct float3 {float v[3];}`. matter_math.h (mm::, via dsl_state.h)
// has no float3 of its own so it does not collide; BuildOp's transform/center/
// halfExtents/segB (dsl_state.h) are mm::Vec3/mm::Mat4 as of Phase 3
// (mathlib-and-raylib-removal), and the matrix ops below operate on those.
//
// Particle/FatPrim (particle.h/fat_primitive.h) moved off raylib Vector3/
// Matrix/Vector4 onto matter_math_c.h's MtVec3/MtMat4/MtVec4 in Phase 4 Step 3
// (mathlib-and-raylib-removal) -- this file computes in mm:: and crosses that
// boundary via mm::to_c() (matter_math.h). StaticParticle (cluster.h) is a
// SEPARATE MatterSurfaceLib type that still takes a raylib Vector3; it stays
// that way until Phase 4 Step 4 migrates cluster.h, so to_raylib() below
// survives -- with exactly one remaining caller -- until then.

namespace dsl {

// mm::Vec3 -> raylib Vector3. Byte-identical fields; used only at the
// StaticParticle construction boundary (see note above) -- the one raylib
// type this file still has to produce. Everything that used to go through
// this for Particle/FatPrim now uses mm::to_c() (matter_math.h) instead,
// since those two structs moved to matter_math_c.h's MtVec3/MtMat4/MtVec4.
static Vector3 to_raylib(const mm::Vec3& v) { return Vector3{v.x, v.y, v.z}; }

// General 4x4 inverse, ported element-for-element from the original raylib-
// Matrix cofactor implementation (NOT swapped for mm::inverse's Gauss-Jordan
// elimination): the two algorithms are not guaranteed bit-identical on the
// same input (different operation order / accumulation), and this file's
// determinism contract is byte-identical bake output across the migration,
// not merely "some correct inverse". Keeping the original formula, only
// re-addressed from raylib's Matrix field names onto mm::Mat4's m[] (via the
// row<->col relabeling matter_math.h documents: field mN -> m[(N%4)*4+N/4]),
// keeps this bit-for-bit identical to the pre-Phase-3 output.
//
// Preserves the local zero-determinant guard (tech-debt.md #2: this guard is
// the entire reason a local copy exists rather than calling raymath's
// unguarded MatrixInvert) -- explicitly, via mm::zero(), since mm::Mat4{}
// default-constructs to IDENTITY (matter_math.h), not zero.
static mm::Mat4 mat_invert(const mm::Mat4& m) {
    float a00=m.m[0],  a01=m.m[4],  a02=m.m[8],  a03=m.m[12];
    float a10=m.m[1],  a11=m.m[5],  a12=m.m[9],  a13=m.m[13];
    float a20=m.m[2],  a21=m.m[6],  a22=m.m[10], a23=m.m[14];
    float a30=m.m[3],  a31=m.m[7],  a32=m.m[11], a33=m.m[15];

    float b00=a00*a11-a01*a10, b01=a00*a12-a02*a10, b02=a00*a13-a03*a10;
    float b03=a01*a12-a02*a11, b04=a01*a13-a03*a11, b05=a02*a13-a03*a12;
    float b06=a20*a31-a21*a30, b07=a20*a32-a22*a30, b08=a20*a33-a23*a30;
    float b09=a21*a32-a22*a31, b10=a21*a33-a23*a31, b11=a22*a33-a23*a32;

    float det = b00*b11 - b01*b10 + b02*b09 + b03*b08 - b04*b07 + b05*b06;
    if (det == 0.0f) return mm::zero();
    float invDet = 1.0f/det;

    mm::Mat4 r;
    r.m[0]  = ( a11*b11 - a12*b10 + a13*b09)*invDet;
    r.m[4]  = (-a01*b11 + a02*b10 - a03*b09)*invDet;
    r.m[8]  = ( a31*b05 - a32*b04 + a33*b03)*invDet;
    r.m[12] = (-a21*b05 + a22*b04 - a23*b03)*invDet;
    r.m[1]  = (-a10*b11 + a12*b08 - a13*b07)*invDet;
    r.m[5]  = ( a00*b11 - a02*b08 + a03*b07)*invDet;
    r.m[9]  = (-a30*b05 + a32*b02 - a33*b01)*invDet;
    r.m[13] = ( a20*b05 - a22*b02 + a23*b01)*invDet;
    r.m[2]  = ( a10*b10 - a11*b08 + a13*b06)*invDet;
    r.m[6]  = (-a00*b10 + a01*b08 - a03*b06)*invDet;
    r.m[10] = ( a30*b04 - a31*b02 + a33*b00)*invDet;
    r.m[14] = (-a20*b04 + a21*b02 - a23*b00)*invDet;
    r.m[3]  = (-a10*b09 + a11*b07 - a12*b06)*invDet;
    r.m[7]  = ( a00*b09 - a01*b07 + a02*b06)*invDet;
    r.m[11] = (-a30*b03 + a31*b01 - a32*b00)*invDet;
    r.m[15] = ( a20*b03 - a21*b01 + a22*b00)*invDet;
    return r;
}

static float v3len(const mm::Vec3& p){ return std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z); }
static float sdSphere(const mm::Vec3& p, float r){ return v3len(p) - r; }
// NOTE: sdBox now lives in MatterSurfaceLib's primitive_sdf (fat_primitive.c). The
// analytic oracle below keeps a local copy so the test oracle stays independent of
// the mesher TU (and so the oracle never needs raylib's Vector ops linked here).
static float sdBox(const mm::Vec3& p, const mm::Vec3& h){
    mm::Vec3 d={fabsf(p.x)-h.x, fabsf(p.y)-h.y, fabsf(p.z)-h.z};
    mm::Vec3 m={fmaxf(d.x,0),fmaxf(d.y,0),fmaxf(d.z,0)};
    return v3len(m) + fminf(fmaxf(d.x,fmaxf(d.y,d.z)),0.0f);
}
// Capsule / capped-cone oracle copies (mirror MSL primitive_sdf's fp_sdCapsule /
// fp_sdCappedCone). Kept local so the analytic oracle stays independent of the
// mesher TU, exactly like sdBox above.
static mm::Vec3 v3sub(const mm::Vec3& a, const mm::Vec3& b){ return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static float    v3dot(const mm::Vec3& a, const mm::Vec3& b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static float    fclampf(float x,float lo,float hi){ return x<lo?lo:(x>hi?hi:x); }
static float sdCapsule(const mm::Vec3& p, const mm::Vec3& a, const mm::Vec3& b, float r){
    mm::Vec3 pa=v3sub(p,a), ba=v3sub(b,a);
    float denom=v3dot(ba,ba);
    float h=(denom>0.0f)?fclampf(v3dot(pa,ba)/denom,0.0f,1.0f):0.0f;
    mm::Vec3 proj={pa.x-ba.x*h, pa.y-ba.y*h, pa.z-ba.z*h};
    return v3len(proj)-r;
}
static float sdCappedCone(const mm::Vec3& p, const mm::Vec3& a, const mm::Vec3& b, float ra, float rb){
    float rba=rb-ra; mm::Vec3 ba=v3sub(b,a); float baba=v3dot(ba,ba);
    if (baba<=0.0f){ float r=ra>rb?ra:rb; return v3len(v3sub(p,a))-r; }
    mm::Vec3 pa=v3sub(p,a); float papa=v3dot(pa,pa); float paba=v3dot(pa,ba)/baba;
    float x=sqrtf(papa-paba*paba*baba);
    float cax=fmaxf(0.0f, x-((paba<0.5f)?ra:rb));
    float cay=fabsf(paba-0.5f)-0.5f;
    float k=rba*rba+baba;
    float f=fclampf((rba*(x-ra)+paba*baba)/k,0.0f,1.0f);
    float cbx=x-ra-f*rba; float cby=paba-f;
    float s=(cbx<0.0f && cay<0.0f)?-1.0f:1.0f;
    float in0=cax*cax+cay*cay*baba; float in1=cbx*cbx+cby*cby*baba;
    return s*sqrtf(fminf(in0,in1));
}

// Column lengths of the transform's upper-left 3x3 = per-axis scale factors. A
// pure rotation+translation has all three == 1. We use these to decide whether a
// sphere brush can stay on the hashed hot path (uniform or no scale: just multiply
// the radius) or must become an ellipsoid FatPrim (non-uniform scale).
static void axis_scales(const mm::Mat4& m, float& sx, float& sy, float& sz) {
    sx = std::sqrt(m.m[0]*m.m[0] + m.m[4]*m.m[4] + m.m[8]*m.m[8]);
    sy = std::sqrt(m.m[1]*m.m[1] + m.m[5]*m.m[5] + m.m[9]*m.m[9]);
    sz = std::sqrt(m.m[2]*m.m[2] + m.m[6]*m.m[6] + m.m[10]*m.m[10]);
}

// Public signature stays raylib Vector3 (test-oracle callers, unchanged since
// Phase 1); converted once to mm::Vec3 here since BuildOp::transform is mm::Mat4.
bool field_is_solid(const BuildBuffer& buf, const Vector3& wp_raylib) {
    const mm::Vec3 wp{wp_raylib.x, wp_raylib.y, wp_raylib.z};
    float field = 1e9f; // distance; <0 = inside. start empty (large positive)
    bool any=false;
    for (const BuildOp& o : buf.ops) {
        mm::Mat4 inv = mat_invert(o.transform);
        mm::Vec3 lpw = mm::transform_point(inv, wp);
        // Sphere/box are center-relative; capsule/cylinder carry their own segment
        // endpoints in (center=a, segB=b) so they evaluate at the raw local point.
        mm::Vec3 lp = { lpw.x - o.center.x, lpw.y - o.center.y, lpw.z - o.center.z };
        float d;
        switch (o.kind) {
            case BrushKind::Sphere:   d = sdSphere(lp, o.radius); break;
            case BrushKind::Box:      d = sdBox(lp, o.halfExtents); break;
            case BrushKind::Capsule:  d = sdCapsule(lpw, o.center, o.segB, o.radius); break;
            case BrushKind::Cylinder: d = sdCappedCone(lpw, o.center, o.segB, o.radius, o.r1); break;
            default:                  d = sdSphere(lp, o.radius); break;
        }
        // Apply the op directly against the running field (starts at 1e9 = empty).
        // Opening Union: fminf(1e9,d)=d (correct seed). Opening Difference:
        // fmaxf(1e9,-d)=1e9 (nothing yet — correct). Opening Intersection: 1e9.
        switch (o.op) {
            case CsgOp::Union:        field = fminf(field, d); break;
            case CsgOp::Difference:   field = fmaxf(field, -d); break;
            case CsgOp::Intersection: field = fmaxf(field, d); break;
        }
        any = true;
    }
    return any && field < 0.0f;
}

// Signed-distance sibling of field_is_solid, mirroring the mesher's STAGED
// smooth-min evaluation (surface.c CalculateScalarStaged): consecutive same-op
// ops form a stage; within a stage distances combine via log-sum-exp smin with
// fillet k; stages fold in authored order starting from INFINITY=empty
// (Union=min, Difference=max(field,-d), Intersection=max(field,d)).
// With k<=1e-5 this degenerates to hard ops and matches field_is_solid's sign.
// Public signature stays raylib Vector3 (DslState::raycast and test-oracle
// callers, unchanged since Phase 1); converted once to mm::Vec3 here since
// BuildOp::transform is mm::Mat4.
float field_distance(const BuildBuffer& buf, size_t opBegin, size_t opEnd,
                     float k, const Vector3& worldPoint_raylib) {
    const mm::Vec3 worldPoint{worldPoint_raylib.x, worldPoint_raylib.y, worldPoint_raylib.z};
    float field = 1e9f;
    bool haveAny = false;

    // Accumulate one stage's per-brush distances, then fold and reset.
    std::vector<float> vals;
    vals.reserve(16);
    float stageMin = 1e9f;
    CsgOp stageOp = CsgOp::Union;
    bool stageOpen = false;

    auto foldStage = [&]() {
        if (!stageOpen || vals.empty()) { vals.clear(); stageOpen = false; return; }
        // smin_set (surface.c:1345): f = fmin - k*ln(sum exp(-(f_i-fmin)/k)).
        float d = stageMin;
        if (k > 1e-5f && vals.size() > 1) {
            float sum = 0.0f;
            for (float v : vals) sum += expf(-(v - stageMin) / k);
            d = stageMin - k * logf(sum);
        }
        // Apply each stage's op directly against the running field (starts at
        // 1e9 = empty). Opening Union: fminf(1e9,d)=d. Opening Difference:
        // fmaxf(1e9,-d)=1e9 (nothing yet). Opening Intersection: 1e9.
        switch (stageOp) {
            case CsgOp::Union:        field = fminf(field, d);  break;
            case CsgOp::Difference:   field = fmaxf(field, -d); break;
            case CsgOp::Intersection: field = fmaxf(field, d);  break;
        }
        haveAny = true;
        vals.clear(); stageMin = 1e9f; stageOpen = false;
    };

    if (opEnd > buf.ops.size()) opEnd = buf.ops.size();
    for (size_t i = opBegin; i < opEnd; ++i) {
        const BuildOp& o = buf.ops[i];
        if (stageOpen && o.op != stageOp) foldStage();
        if (!stageOpen) { stageOp = o.op; stageOpen = true; }

        // Per-brush distance: identical metric to field_is_solid.
        mm::Mat4 inv = mat_invert(o.transform);
        mm::Vec3 lpw = mm::transform_point(inv, worldPoint);
        mm::Vec3 lp = { lpw.x - o.center.x, lpw.y - o.center.y, lpw.z - o.center.z };
        float d;
        switch (o.kind) {
            case BrushKind::Sphere:   d = sdSphere(lp, o.radius); break;
            case BrushKind::Box:      d = sdBox(lp, o.halfExtents); break;
            case BrushKind::Capsule:  d = sdCapsule(lpw, o.center, o.segB, o.radius); break;
            case BrushKind::Cylinder: d = sdCappedCone(lpw, o.center, o.segB, o.radius, o.r1); break;
            default:                  d = sdSphere(lp, o.radius); break;
        }
        vals.push_back(d);
        if (d < stageMin) stageMin = d;
    }
    foldStage();
    return haveAny ? field : 1e9f;
}

// Map a dsl::CsgOp to the field-eval stage op enum (same ordering).
static CsgStageOp stage_op(CsgOp op) {
    switch (op) {
        case CsgOp::Union:        return CSG_STAGE_UNION;
        case CsgOp::Difference:   return CSG_STAGE_DIFFERENCE;
        case CsgOp::Intersection: return CSG_STAGE_INTERSECTION;
    }
    return CSG_STAGE_UNION;
}

LoweredField lower_build_buffer(const BuildBuffer& buf) {
    LoweredField out;
    // Predictable sizes now that a box is ONE fat primitive (no stamp_box): at
    // most one additive/carve/fat entry per op. reserve() up front so the additive
    // and fat arrays never reallocate during the single lowering pass.
    out.additive.reserve(buf.ops.size());
    out.additive_stage.reserve(buf.ops.size());
    out.fat.reserve(buf.ops.size());
    out.staged_spheres.reserve(buf.ops.size());
    out.staged_stage.reserve(buf.ops.size());

    // Build the ORDERED stage list. Each authored op opens or extends a stage;
    // consecutive same-op ops merge into the current stage (union is commutative
    // within a run). Stage 0 is implicitly the first op's op.
    int curStage = -1;
    CsgOp curOp = CsgOp::Union;

    for (const BuildOp& o : buf.ops) {
        out.smoothing = std::max(out.smoothing, o.smoothing);

        if (curStage < 0 || o.op != curOp) {
            curOp = o.op;
            curStage = (int)out.stages.size();
            out.stages.push_back(stage_op(o.op));
        }

        bool subtract = (o.op == CsgOp::Difference);

        if (o.kind == BrushKind::Sphere) {
            float sx, sy, sz;
            axis_scales(o.transform, sx, sy, sz);
            bool uniform = (fabsf(sx - sy) < 1e-4f && fabsf(sy - sz) < 1e-4f);
            mm::Vec3 c = mm::transform_point(o.transform, o.center);

            if (uniform) {
                // G2 fix (uniform/no scale): transform the center AND scale the
                // radius. With identity scale (sx==1) this is byte-identical to the
                // old `StaticParticle{xf(center), radius}` hot path.
                float r = o.radius * sx;
                if (subtract) {
                    out.carve.push_back(Particle{ mm::to_c(c), r, (int)o.materialId });
                } else {
                    out.additive.push_back(StaticParticle(to_raylib(c), r, o.materialId, {1,1,1,0}, o.spacing));
                    out.additive_stage.push_back(curStage);
                }
                // Every sphere brush (additive AND subtractive) joins the staged
                // hash stream so the ordered eval can fold a Difference as a real
                // Difference stage instead of a trailing carve.
                out.staged_spheres.push_back(Particle{ mm::to_c(c), r, (int)o.materialId });
                out.staged_stage.push_back(curStage);
            } else {
                // Non-uniform scale => the sphere is an ELLIPSOID, which the hashed
                // |p-c|-r union cannot represent. Emit it as a fat sphere primitive
                // evaluated via invTransform (matches the oracle by construction).
                FatPrim fp{};
                fp.kind = FAT_PRIM_SPHERE;
                fp.center = mm::to_c(c);
                fp.boundRadius = o.radius * std::max(sx, std::max(sy, sz));
                fp.materialId = (int)o.materialId;
                fp.tint = {1,1,1,0};
                fp.stage = curStage;
                fp.radius = o.radius;
                // invTransform maps a WORLD point into brush-local, center-relative
                // space so primitive_sdf evaluates |localP| - r directly:
                //   T' = translate(-center) * inv(transform).
                mm::Mat4 inv = mat_invert(o.transform);
                mm::Mat4 shift = mm::translation(mm::Vec3{-o.center.x, -o.center.y, -o.center.z});
                fp.invTransform = mm::to_c(mm::multiply(shift, inv));
                out.fat.push_back(fp);
            }
        } else if (o.kind == BrushKind::Box) { // Box -> ONE oriented fat primitive (stamp_box deleted)
            FatPrim fp{};
            fp.kind = FAT_PRIM_BOX;
            fp.center = mm::to_c(mm::transform_point(o.transform, o.center));
            float sx, sy, sz; axis_scales(o.transform, sx, sy, sz);
            // Bounding sphere of the (possibly scaled/rotated) box.
            mm::Vec3 he = o.halfExtents;
            fp.boundRadius = std::sqrt((he.x*sx)*(he.x*sx) + (he.y*sy)*(he.y*sy) + (he.z*sz)*(he.z*sz));
            fp.materialId = (int)o.materialId;
            fp.tint = {1,1,1,0};
            fp.stage = curStage;
            fp.halfExtents = mm::to_c(he);
            // invTransform maps WORLD point into box-local center-relative space:
            // T' = translate(-center) * inv(transform).
            mm::Mat4 inv = mat_invert(o.transform);
            mm::Mat4 shift = mm::translation(mm::Vec3{-o.center.x, -o.center.y, -o.center.z});
            fp.invTransform = mm::to_c(mm::multiply(shift, inv));
            out.fat.push_back(fp);
        } else { // Capsule / Cylinder (cone) -> ONE segment fat primitive.
            FatPrim fp{};
            fp.kind = (o.kind == BrushKind::Capsule) ? FAT_PRIM_CAPSULE : FAT_PRIM_CYLINDER;
            // Segment endpoints live in brush-local space (center=a, segB=b). Unlike
            // box/sphere these are NOT center-relative; invTransform is just
            // inv(transform) so primitive_sdf evaluates sdSegment/sdCappedCone on the
            // raw local point against the stored endpoints.
            fp.segA = mm::to_c(o.center);
            fp.segB = mm::to_c(o.segB);
            fp.r0   = o.radius;
            fp.r1   = (o.kind == BrushKind::Capsule) ? o.radius : o.r1;
            // Bounding sphere: world midpoint of the segment + half its world length
            // + the larger end radius (scaled). Uniform-ish scale assumed (the only
            // transform the stack produces here); rotation does not change lengths.
            mm::Vec3 wa = mm::transform_point(o.transform, o.center);
            mm::Vec3 wb = mm::transform_point(o.transform, o.segB);
            fp.center = MtVec3{ (wa.x+wb.x)*0.5f, (wa.y+wb.y)*0.5f, (wa.z+wb.z)*0.5f };
            float sx, sy, sz; axis_scales(o.transform, sx, sy, sz);
            float smax = std::max(sx, std::max(sy, sz));
            float halfLen = 0.5f * v3len(mm::Vec3{wb.x-wa.x, wb.y-wa.y, wb.z-wa.z});
            float rmax = std::max(fp.r0, fp.r1) * smax;
            fp.boundRadius = halfLen + rmax;
            fp.materialId = (int)o.materialId;
            fp.tint = {1,1,1,0};
            fp.stage = curStage;
            fp.invTransform = mm::to_c(mat_invert(o.transform));
            out.fat.push_back(fp);
        }
    }
    return out;
}

} // namespace dsl
