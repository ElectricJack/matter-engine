#include "../include/fat_primitive.h"
#include <math.h>

// Transform a point by a raylib column-major Matrix as M * (p,1).
static Vector3 fp_xf(const Matrix* m, Vector3 p) {
    Vector3 o;
    o.x = m->m0*p.x + m->m4*p.y + m->m8 *p.z + m->m12;
    o.y = m->m1*p.x + m->m5*p.y + m->m9 *p.z + m->m13;
    o.z = m->m2*p.x + m->m6*p.y + m->m10*p.z + m->m14;
    return o;
}

static float fp_len(Vector3 p) { return sqrtf(p.x*p.x + p.y*p.y + p.z*p.z); }
static Vector3 fp_sub(Vector3 a, Vector3 b){ return (Vector3){a.x-b.x,a.y-b.y,a.z-b.z}; }
static float   fp_dot(Vector3 a, Vector3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static float   fp_clamp(float x, float lo, float hi){ return x<lo?lo:(x>hi?hi:x); }

// Distance from `p` to the segment a->b, minus radius r (a capsule).
// Standard iq sdCapsule: project p onto the segment, clamp to [0,1].
static float fp_sdCapsule(Vector3 p, Vector3 a, Vector3 b, float r) {
    Vector3 pa = fp_sub(p, a);
    Vector3 ba = fp_sub(b, a);
    float denom = fp_dot(ba, ba);
    float h = (denom > 0.0f) ? fp_clamp(fp_dot(pa, ba) / denom, 0.0f, 1.0f) : 0.0f;
    Vector3 proj = (Vector3){ pa.x - ba.x*h, pa.y - ba.y*h, pa.z - ba.z*h };
    return fp_len(proj) - r;
}

// Signed distance to a capped cone with axis a->b and end radii ra (at a) and rb
// (at b). Flat circular caps at both ends. ra==rb degenerates to a straight capped
// cylinder; rb==0 to a cone closing to a point at b. Adapted from iq's sdCappedCone
// (the general 3D segment form). Negative inside.
static float fp_sdCappedCone(Vector3 p, Vector3 a, Vector3 b, float ra, float rb) {
    float rba  = rb - ra;
    Vector3 ba = fp_sub(b, a);
    float baba = fp_dot(ba, ba);
    if (baba <= 0.0f) {
        // Degenerate axis: treat as a sphere of radius max(ra,rb) at a.
        float r = ra > rb ? ra : rb;
        return fp_len(fp_sub(p, a)) - r;
    }
    Vector3 pa = fp_sub(p, a);
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
static float fp_sdBox(Vector3 p, Vector3 h) {
    Vector3 d  = (Vector3){ fabsf(p.x) - h.x, fabsf(p.y) - h.y, fabsf(p.z) - h.z };
    Vector3 mx = (Vector3){ fmaxf(d.x, 0.0f), fmaxf(d.y, 0.0f), fmaxf(d.z, 0.0f) };
    return fp_len(mx) + fminf(fmaxf(d.x, fmaxf(d.y, d.z)), 0.0f);
}

float primitive_sdf(const FatPrim* prim, Vector3 p) {
    // invTransform maps the WORLD point into the brush's local, center-relative
    // frame; the per-kind SDF is then evaluated there.
    Vector3 local = fp_xf(&prim->invTransform, p);
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
