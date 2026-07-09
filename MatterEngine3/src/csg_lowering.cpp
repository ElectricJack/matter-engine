#include "csg_lowering.h"
#include <cmath>
#include <vector>

// NOTE: raymath.h cannot be included in this TU: cluster.h transitively pulls in
// the prototype's precomp.h `struct float3` (x/y/z members) which collides with
// raymath's `struct float3 {float v[3];}`. The few matrix ops needed here are
// implemented directly on raylib's column-major Matrix.

namespace dsl {

// raylib Matrix is column-major: m0..m3 col0, m4..m7 col1, etc. m12,m13,m14 =
// translation. Transform a point as M * (p,1).
static Vector3 xf(const Matrix& m, const Vector3& p) {
    Vector3 o;
    o.x = m.m0*p.x + m.m4*p.y + m.m8 *p.z + m.m12;
    o.y = m.m1*p.x + m.m5*p.y + m.m9 *p.z + m.m13;
    o.z = m.m2*p.x + m.m6*p.y + m.m10*p.z + m.m14;
    return o;
}

// General 4x4 inverse of a raylib column-major Matrix (cofactor method).
// Matches raymath's MatrixInvert semantics for the transforms we build.
static Matrix mat_invert(const Matrix& m) {
    float a00=m.m0,  a01=m.m1,  a02=m.m2,  a03=m.m3;
    float a10=m.m4,  a11=m.m5,  a12=m.m6,  a13=m.m7;
    float a20=m.m8,  a21=m.m9,  a22=m.m10, a23=m.m11;
    float a30=m.m12, a31=m.m13, a32=m.m14, a33=m.m15;

    float b00=a00*a11-a01*a10, b01=a00*a12-a02*a10, b02=a00*a13-a03*a10;
    float b03=a01*a12-a02*a11, b04=a01*a13-a03*a11, b05=a02*a13-a03*a12;
    float b06=a20*a31-a21*a30, b07=a20*a32-a22*a30, b08=a20*a33-a23*a30;
    float b09=a21*a32-a22*a31, b10=a21*a33-a23*a31, b11=a22*a33-a23*a32;

    float det = b00*b11 - b01*b10 + b02*b09 + b03*b08 - b04*b07 + b05*b06;
    Matrix r{};
    if (det == 0.0f) return r;
    float invDet = 1.0f/det;

    r.m0  = ( a11*b11 - a12*b10 + a13*b09)*invDet;
    r.m1  = (-a01*b11 + a02*b10 - a03*b09)*invDet;
    r.m2  = ( a31*b05 - a32*b04 + a33*b03)*invDet;
    r.m3  = (-a21*b05 + a22*b04 - a23*b03)*invDet;
    r.m4  = (-a10*b11 + a12*b08 - a13*b07)*invDet;
    r.m5  = ( a00*b11 - a02*b08 + a03*b07)*invDet;
    r.m6  = (-a30*b05 + a32*b02 - a33*b01)*invDet;
    r.m7  = ( a20*b05 - a22*b02 + a23*b01)*invDet;
    r.m8  = ( a10*b10 - a11*b08 + a13*b06)*invDet;
    r.m9  = (-a00*b10 + a01*b08 - a03*b06)*invDet;
    r.m10 = ( a30*b04 - a31*b02 + a33*b00)*invDet;
    r.m11 = (-a20*b04 + a21*b02 - a23*b00)*invDet;
    r.m12 = (-a10*b09 + a11*b07 - a12*b06)*invDet;
    r.m13 = ( a00*b09 - a01*b07 + a02*b06)*invDet;
    r.m14 = (-a30*b03 + a31*b01 - a32*b00)*invDet;
    r.m15 = ( a20*b03 - a21*b01 + a22*b00)*invDet;
    return r;
}

// Column-major 4x4 multiply: returns A*B (apply B first, then A).
static Matrix mat_mul(const Matrix& a, const Matrix& b) {
    Matrix r;
    r.m0  = a.m0*b.m0  + a.m4*b.m1  + a.m8 *b.m2  + a.m12*b.m3;
    r.m1  = a.m1*b.m0  + a.m5*b.m1  + a.m9 *b.m2  + a.m13*b.m3;
    r.m2  = a.m2*b.m0  + a.m6*b.m1  + a.m10*b.m2  + a.m14*b.m3;
    r.m3  = a.m3*b.m0  + a.m7*b.m1  + a.m11*b.m2  + a.m15*b.m3;
    r.m4  = a.m0*b.m4  + a.m4*b.m5  + a.m8 *b.m6  + a.m12*b.m7;
    r.m5  = a.m1*b.m4  + a.m5*b.m5  + a.m9 *b.m6  + a.m13*b.m7;
    r.m6  = a.m2*b.m4  + a.m6*b.m5  + a.m10*b.m6  + a.m14*b.m7;
    r.m7  = a.m3*b.m4  + a.m7*b.m5  + a.m11*b.m6  + a.m15*b.m7;
    r.m8  = a.m0*b.m8  + a.m4*b.m9  + a.m8 *b.m10 + a.m12*b.m11;
    r.m9  = a.m1*b.m8  + a.m5*b.m9  + a.m9 *b.m10 + a.m13*b.m11;
    r.m10 = a.m2*b.m8  + a.m6*b.m9  + a.m10*b.m10 + a.m14*b.m11;
    r.m11 = a.m3*b.m8  + a.m7*b.m9  + a.m11*b.m10 + a.m15*b.m11;
    r.m12 = a.m0*b.m12 + a.m4*b.m13 + a.m8 *b.m14 + a.m12*b.m15;
    r.m13 = a.m1*b.m12 + a.m5*b.m13 + a.m9 *b.m14 + a.m13*b.m15;
    r.m14 = a.m2*b.m12 + a.m6*b.m13 + a.m10*b.m14 + a.m14*b.m15;
    r.m15 = a.m3*b.m12 + a.m7*b.m13 + a.m11*b.m14 + a.m15*b.m15;
    return r;
}

static float v3len(const Vector3& p){ return std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z); }
static float sdSphere(const Vector3& p, float r){ return v3len(p) - r; }
// NOTE: sdBox now lives in MatterSurfaceLib's primitive_sdf (fat_primitive.c). The
// analytic oracle below keeps a local copy so the test oracle stays independent of
// the mesher TU (and so the oracle never needs raylib's Vector ops linked here).
static float sdBox(const Vector3& p, const Vector3& h){
    Vector3 d={fabsf(p.x)-h.x, fabsf(p.y)-h.y, fabsf(p.z)-h.z};
    Vector3 m={fmaxf(d.x,0),fmaxf(d.y,0),fmaxf(d.z,0)};
    return v3len(m) + fminf(fmaxf(d.x,fmaxf(d.y,d.z)),0.0f);
}
// Capsule / capped-cone oracle copies (mirror MSL primitive_sdf's fp_sdCapsule /
// fp_sdCappedCone). Kept local so the analytic oracle stays independent of the
// mesher TU, exactly like sdBox above.
static Vector3 v3sub(const Vector3& a, const Vector3& b){ return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static float   v3dot(const Vector3& a, const Vector3& b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static float   fclampf(float x,float lo,float hi){ return x<lo?lo:(x>hi?hi:x); }
static float sdCapsule(const Vector3& p, const Vector3& a, const Vector3& b, float r){
    Vector3 pa=v3sub(p,a), ba=v3sub(b,a);
    float denom=v3dot(ba,ba);
    float h=(denom>0.0f)?fclampf(v3dot(pa,ba)/denom,0.0f,1.0f):0.0f;
    Vector3 proj={pa.x-ba.x*h, pa.y-ba.y*h, pa.z-ba.z*h};
    return v3len(proj)-r;
}
static float sdCappedCone(const Vector3& p, const Vector3& a, const Vector3& b, float ra, float rb){
    float rba=rb-ra; Vector3 ba=v3sub(b,a); float baba=v3dot(ba,ba);
    if (baba<=0.0f){ float r=ra>rb?ra:rb; return v3len(v3sub(p,a))-r; }
    Vector3 pa=v3sub(p,a); float papa=v3dot(pa,pa); float paba=v3dot(pa,ba)/baba;
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
static void axis_scales(const Matrix& m, float& sx, float& sy, float& sz) {
    sx = std::sqrt(m.m0*m.m0 + m.m1*m.m1 + m.m2*m.m2);
    sy = std::sqrt(m.m4*m.m4 + m.m5*m.m5 + m.m6*m.m6);
    sz = std::sqrt(m.m8*m.m8 + m.m9*m.m9 + m.m10*m.m10);
}

bool field_is_solid(const BuildBuffer& buf, const Vector3& wp) {
    float field = 1e9f; // distance; <0 = inside. start empty (large positive)
    bool any=false;
    for (const BuildOp& o : buf.ops) {
        Matrix inv = mat_invert(o.transform);
        Vector3 lpw = xf(inv, wp);
        // Sphere/box are center-relative; capsule/cylinder carry their own segment
        // endpoints in (center=a, segB=b) so they evaluate at the raw local point.
        Vector3 lp = { lpw.x - o.center.x, lpw.y - o.center.y, lpw.z - o.center.z };
        float d;
        switch (o.kind) {
            case BrushKind::Sphere:   d = sdSphere(lp, o.radius); break;
            case BrushKind::Box:      d = sdBox(lp, o.halfExtents); break;
            case BrushKind::Capsule:  d = sdCapsule(lpw, o.center, o.segB, o.radius); break;
            case BrushKind::Cylinder: d = sdCappedCone(lpw, o.center, o.segB, o.radius, o.r1); break;
            default:                  d = sdSphere(lp, o.radius); break;
        }
        if (!any) { field = d; any=true; continue; }
        switch (o.op) {
            case CsgOp::Union:        field = fminf(field, d); break;
            case CsgOp::Difference:   field = fmaxf(field, -d); break;
            case CsgOp::Intersection: field = fmaxf(field, d); break;
        }
    }
    return any && field < 0.0f;
}

// Signed-distance sibling of field_is_solid, mirroring the mesher's STAGED
// smooth-min evaluation (surface.c CalculateScalarStaged): consecutive same-op
// ops form a stage; within a stage distances combine via log-sum-exp smin with
// fillet k; stages fold in authored order (first non-empty stage seeds the
// field; Union=min, Difference=max(field,-d), Intersection=max(field,d)).
// With k<=1e-5 this degenerates to hard ops and matches field_is_solid's sign.
float field_distance(const BuildBuffer& buf, size_t opBegin, size_t opEnd,
                     float k, const Vector3& worldPoint) {
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
        if (!haveAny) { field = d; haveAny = true; }
        else switch (stageOp) {
            case CsgOp::Union:        field = fminf(field, d);  break;
            case CsgOp::Difference:   field = fmaxf(field, -d); break;
            case CsgOp::Intersection: field = fmaxf(field, d);  break;
        }
        vals.clear(); stageMin = 1e9f; stageOpen = false;
    };

    if (opEnd > buf.ops.size()) opEnd = buf.ops.size();
    for (size_t i = opBegin; i < opEnd; ++i) {
        const BuildOp& o = buf.ops[i];
        if (stageOpen && o.op != stageOp) foldStage();
        if (!stageOpen) { stageOp = o.op; stageOpen = true; }

        // Per-brush distance: identical metric to field_is_solid.
        Matrix inv = mat_invert(o.transform);
        Vector3 lpw = xf(inv, worldPoint);
        Vector3 lp = { lpw.x - o.center.x, lpw.y - o.center.y, lpw.z - o.center.z };
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
            Vector3 c = xf(o.transform, o.center);

            if (uniform) {
                // G2 fix (uniform/no scale): transform the center AND scale the
                // radius. With identity scale (sx==1) this is byte-identical to the
                // old `StaticParticle{xf(center), radius}` hot path.
                float r = o.radius * sx;
                if (subtract) {
                    out.carve.push_back(Particle{ c, r, (int)o.materialId });
                } else {
                    out.additive.push_back(StaticParticle(c, r, o.materialId, {1,1,1,0}, o.spacing));
                    out.additive_stage.push_back(curStage);
                }
                // Every sphere brush (additive AND subtractive) joins the staged
                // hash stream so the ordered eval can fold a Difference as a real
                // Difference stage instead of a trailing carve.
                out.staged_spheres.push_back(Particle{ c, r, (int)o.materialId });
                out.staged_stage.push_back(curStage);
            } else {
                // Non-uniform scale => the sphere is an ELLIPSOID, which the hashed
                // |p-c|-r union cannot represent. Emit it as a fat sphere primitive
                // evaluated via invTransform (matches the oracle by construction).
                FatPrim fp{};
                fp.kind = FAT_PRIM_SPHERE;
                fp.center = c;
                fp.boundRadius = o.radius * std::max(sx, std::max(sy, sz));
                fp.materialId = (int)o.materialId;
                fp.tint = {1,1,1,0};
                fp.stage = curStage;
                fp.radius = o.radius;
                // invTransform maps a WORLD point into brush-local, center-relative
                // space so primitive_sdf evaluates |localP| - r directly:
                //   T' = translate(-center) * inv(transform).
                Matrix inv = mat_invert(o.transform);
                Matrix shift{}; shift.m0=shift.m5=shift.m10=shift.m15=1.0f;
                shift.m12 = -o.center.x; shift.m13 = -o.center.y; shift.m14 = -o.center.z;
                fp.invTransform = mat_mul(shift, inv);
                out.fat.push_back(fp);
            }
        } else if (o.kind == BrushKind::Box) { // Box -> ONE oriented fat primitive (stamp_box deleted)
            FatPrim fp{};
            fp.kind = FAT_PRIM_BOX;
            fp.center = xf(o.transform, o.center);
            float sx, sy, sz; axis_scales(o.transform, sx, sy, sz);
            // Bounding sphere of the (possibly scaled/rotated) box.
            Vector3 he = o.halfExtents;
            fp.boundRadius = std::sqrt((he.x*sx)*(he.x*sx) + (he.y*sy)*(he.y*sy) + (he.z*sz)*(he.z*sz));
            fp.materialId = (int)o.materialId;
            fp.tint = {1,1,1,0};
            fp.stage = curStage;
            fp.halfExtents = he;
            // invTransform maps WORLD point into box-local center-relative space:
            // T' = translate(-center) * inv(transform).
            Matrix inv = mat_invert(o.transform);
            Matrix shift{}; shift.m0=shift.m5=shift.m10=shift.m15=1.0f;
            shift.m12 = -o.center.x; shift.m13 = -o.center.y; shift.m14 = -o.center.z;
            fp.invTransform = mat_mul(shift, inv);
            out.fat.push_back(fp);
        } else { // Capsule / Cylinder (cone) -> ONE segment fat primitive.
            FatPrim fp{};
            fp.kind = (o.kind == BrushKind::Capsule) ? FAT_PRIM_CAPSULE : FAT_PRIM_CYLINDER;
            // Segment endpoints live in brush-local space (center=a, segB=b). Unlike
            // box/sphere these are NOT center-relative; invTransform is just
            // inv(transform) so primitive_sdf evaluates sdSegment/sdCappedCone on the
            // raw local point against the stored endpoints.
            fp.segA = o.center;
            fp.segB = o.segB;
            fp.r0   = o.radius;
            fp.r1   = (o.kind == BrushKind::Capsule) ? o.radius : o.r1;
            // Bounding sphere: world midpoint of the segment + half its world length
            // + the larger end radius (scaled). Uniform-ish scale assumed (the only
            // transform the stack produces here); rotation does not change lengths.
            Vector3 wa = xf(o.transform, o.center);
            Vector3 wb = xf(o.transform, o.segB);
            fp.center = { (wa.x+wb.x)*0.5f, (wa.y+wb.y)*0.5f, (wa.z+wb.z)*0.5f };
            float sx, sy, sz; axis_scales(o.transform, sx, sy, sz);
            float smax = std::max(sx, std::max(sy, sz));
            float halfLen = 0.5f * v3len({wb.x-wa.x, wb.y-wa.y, wb.z-wa.z});
            float rmax = std::max(fp.r0, fp.r1) * smax;
            fp.boundRadius = halfLen + rmax;
            fp.materialId = (int)o.materialId;
            fp.tint = {1,1,1,0};
            fp.stage = curStage;
            fp.invTransform = mat_invert(o.transform);
            out.fat.push_back(fp);
        }
    }
    return out;
}

} // namespace dsl
