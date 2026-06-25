#include "../include/csg_lowering.h"
#include <cmath>

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

// Stamp a box as a pack of spheres at step = spacing/2 so the SDF surface (incl.
// sharp corners) is resolved to the resolution floor. Radius ~ step so spheres
// overlap and the union reads as a solid box to the mesher.
template <class Emit>
static void stamp_box(const BuildOp& o, Emit emit) {
    float step = (o.spacing > 0 ? o.spacing : 0.1f) * 0.5f;
    Vector3 h = o.halfExtents;
    for (float x=-h.x; x<=h.x+1e-4f; x+=step)
      for (float y=-h.y; y<=h.y+1e-4f; y+=step)
        for (float z=-h.z; z<=h.z+1e-4f; z+=step) {
            Vector3 local = { o.center.x+x, o.center.y+y, o.center.z+z };
            emit(xf(o.transform, local), step * 1.05f);
        }
    // Guarantee at least one sample for boxes smaller than a step (sub-min feature).
    if (h.x < step && h.y < step && h.z < step)
        emit(xf(o.transform, o.center), std::max(step, std::max(h.x,std::max(h.y,h.z)))*1.05f);
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

static float v3len(const Vector3& p){ return std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z); }
static float sdSphere(const Vector3& p, float r){ return v3len(p) - r; }
static float sdBox(const Vector3& p, const Vector3& h){
    Vector3 d={fabsf(p.x)-h.x, fabsf(p.y)-h.y, fabsf(p.z)-h.z};
    Vector3 m={fmaxf(d.x,0),fmaxf(d.y,0),fmaxf(d.z,0)};
    return v3len(m) + fminf(fmaxf(d.x,fmaxf(d.y,d.z)),0.0f);
}

bool field_is_solid(const BuildBuffer& buf, const Vector3& wp) {
    float field = 1e9f; // distance; <0 = inside. start empty (large positive)
    bool any=false;
    for (const BuildOp& o : buf.ops) {
        Matrix inv = mat_invert(o.transform);
        Vector3 lpw = xf(inv, wp);
        Vector3 lp = { lpw.x - o.center.x, lpw.y - o.center.y, lpw.z - o.center.z };
        float d = (o.kind==BrushKind::Sphere)? sdSphere(lp,o.radius) : sdBox(lp,o.halfExtents);
        if (!any) { field = d; any=true; continue; }
        switch (o.op) {
            case CsgOp::Union:        field = fminf(field, d); break;
            case CsgOp::Difference:   field = fmaxf(field, -d); break;
            case CsgOp::Intersection: field = fmaxf(field, d); break;
        }
    }
    return any && field < 0.0f;
}

LoweredField lower_build_buffer(const BuildBuffer& buf) {
    LoweredField out;
    for (const BuildOp& o : buf.ops) {
        out.smoothing = std::max(out.smoothing, o.smoothing);
        bool subtract = (o.op == CsgOp::Difference);
        if (o.kind == BrushKind::Sphere) {
            Vector3 c = xf(o.transform, o.center);
            if (subtract) { Particle p{ c, o.radius, (int)o.materialId }; out.carve.push_back(p); }
            else { out.additive.push_back(StaticParticle(c, o.radius, o.materialId,
                       {1,1,1,0}, o.spacing)); }
        } else { // Box
            if (subtract) stamp_box(o, [&](Vector3 c, float r){ out.carve.push_back(Particle{c,r,(int)o.materialId}); });
            else stamp_box(o, [&](Vector3 c, float r){ out.additive.push_back(StaticParticle(c,r,o.materialId,{1,1,1,0},o.spacing)); });
        }
    }
    return out;
}

} // namespace dsl
