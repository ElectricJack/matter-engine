// libs/MatterSurfaceLib/src/fat_primitive.c
//
// Signed-distance evaluation for the mesher's typed "fat" iso-primitives.
// `primitive_sdf` at the bottom is the only exported symbol; everything above
// it is a file-static helper.
//
// Fat primitives are the non-sphere brushes (oriented box, capsule,
// cylinder/capped cone). Additive spheres are NOT represented here -- they stay
// on the spatial-hash smooth-min hot path in surface.c. See
// include/fat_primitive.h for the FatPrim layout and the field conventions.
//
// Contract
// - Distances are SIGNED: negative inside the surface, zero on it. They are
//   Euclidean and (for the box) exact outside, so they are usable directly by
//   the marching-cubes field eval and by smooth-min blending.
// - `p` is a WORLD point. Each shape is evaluated in brush-local,
//   centre-relative space; `prim->invTransform` performs that mapping, which is
//   also how a brush's scale is picked up.
// - `invTransform` is treated as ROW-MAJOR and applied as M * (p,1) -- element
//   (row, col) at m[row*4 + col]. This matches matter_math_c.h's Mat4; feeding
//   it a column-major matrix silently transposes the brush.
// - The switch defaults to the sphere case, so an unrecognised `kind` yields a
//   sphere of `prim->radius` rather than an error.
//
// Pure and allocation-free: safe to call from any thread, and it is called per
// grid sample, so keep it cheap.
#include "../include/fat_primitive.h"
#include <math.h>

// Transform a point by a row-major MtMat4 as M * (p,1). See matter_math_c.h /
// matter_math.h's Mat4 comment for the layout: element (row,col) at
// m[row*4+col], so row 0 is m[0..3], row 1 is m[4..7], row 2 is m[8..11].
static MtVec3 fp_xf(const MtMat4* m, MtVec3 p) {
    MtVec3 o;
    o.x = m->m[0]*p.x + m->m[1]*p.y + m->m[2] *p.z + m->m[3];
    o.y = m->m[4]*p.x + m->m[5]*p.y + m->m[6] *p.z + m->m[7];
    o.z = m->m[8]*p.x + m->m[9]*p.y + m->m[10]*p.z + m->m[11];
    return o;
}

static float fp_len(MtVec3 p) { return sqrtf(p.x*p.x + p.y*p.y + p.z*p.z); }
static MtVec3 fp_sub(MtVec3 a, MtVec3 b){ return (MtVec3){a.x-b.x,a.y-b.y,a.z-b.z}; }
static float   fp_dot(MtVec3 a, MtVec3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static float   fp_clamp(float x, float lo, float hi){ return x<lo?lo:(x>hi?hi:x); }

// Distance from `p` to the segment a->b, minus radius r (a capsule).
// Standard iq sdCapsule: project p onto the segment, clamp to [0,1].
static float fp_sdCapsule(MtVec3 p, MtVec3 a, MtVec3 b, float r) {
    MtVec3 pa = fp_sub(p, a);
    MtVec3 ba = fp_sub(b, a);
    float denom = fp_dot(ba, ba);
    float h = (denom > 0.0f) ? fp_clamp(fp_dot(pa, ba) / denom, 0.0f, 1.0f) : 0.0f;
    MtVec3 proj = (MtVec3){ pa.x - ba.x*h, pa.y - ba.y*h, pa.z - ba.z*h };
    return fp_len(proj) - r;
}

// Signed distance to a capped cone with axis a->b and end radii ra (at a) and rb
// (at b). Flat circular caps at both ends. ra==rb degenerates to a straight capped
// cylinder; rb==0 to a cone closing to a point at b. Adapted from iq's sdCappedCone
// (the general 3D segment form). Negative inside.
static float fp_sdCappedCone(MtVec3 p, MtVec3 a, MtVec3 b, float ra, float rb) {
    float rba  = rb - ra;
    MtVec3 ba = fp_sub(b, a);
    float baba = fp_dot(ba, ba);
    if (baba <= 0.0f) {
        // Degenerate axis: treat as a sphere of radius max(ra,rb) at a.
        float r = ra > rb ? ra : rb;
        return fp_len(fp_sub(p, a)) - r;
    }
    MtVec3 pa = fp_sub(p, a);
    float papa = fp_dot(pa, pa);
    float paba = fp_dot(pa, ba) / baba;          // axial coord in [0,1] for the body
    // Perpendicular distance from the axis.
    float x = sqrtf(papa - paba*paba*baba);
    float cax = fmaxf(0.0f, x - ((paba < 0.5f) ? ra : rb));
    float cay = fabsf(paba - 0.5f) - 0.5f;
    float k   = rba*rba + baba;
    float f   = fp_clamp((rba*(x - ra) + paba*baba) / k, 0.0f, 1.0f);
    float cbx = x - ra - f*rba;
    float cby = paba - f;
    float s   = (cbx < 0.0f && cay < 0.0f) ? -1.0f : 1.0f;
    float in0 = cax*cax + cay*cay*baba;
    float in1 = cbx*cbx + cby*cby*baba;
    return s * sqrtf(fminf(in0, in1));
}

// Signed distance from `p` (already in box-local, center-relative space) to a box
// of half-extents h. Negative inside. Moved here from csg_lowering.cpp so the
// mesher owns the box SDF (the test oracle keeps its own copy).
static float fp_sdBox(MtVec3 p, MtVec3 h) {
    MtVec3 d  = (MtVec3){ fabsf(p.x) - h.x, fabsf(p.y) - h.y, fabsf(p.z) - h.z };
    MtVec3 mx = (MtVec3){ fmaxf(d.x, 0.0f), fmaxf(d.y, 0.0f), fmaxf(d.z, 0.0f) };
    return fp_len(mx) + fminf(fmaxf(d.x, fmaxf(d.y, d.z)), 0.0f);
}

float primitive_sdf(const FatPrim* prim, MtVec3 p) {
    // invTransform maps the WORLD point into the brush's local, center-relative
    // frame; the per-kind SDF is then evaluated there.
    MtVec3 local = fp_xf(&prim->invTransform, p);
    switch (prim->kind) {
        case FAT_PRIM_BOX:
            return fp_sdBox(local, prim->halfExtents);
        case FAT_PRIM_CAPSULE:
            return fp_sdCapsule(local, prim->segA, prim->segB, prim->r0);
        case FAT_PRIM_CYLINDER:
            return fp_sdCappedCone(local, prim->segA, prim->segB, prim->r0, prim->r1);
        case FAT_PRIM_SPHERE:
        default:
            return fp_len(local) - prim->radius;
    }
}
