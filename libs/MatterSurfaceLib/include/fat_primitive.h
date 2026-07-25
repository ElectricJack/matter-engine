#ifndef FAT_PRIMITIVE_H
#define FAT_PRIMITIVE_H

// Typed "fat" iso-primitive for the voxel/SDF mesher (Phase 1: typed iso-primitives).
//
// Spheres remain the implicit hot path (StaticParticle/Particle + spatial hash +
// smooth-min union) and are NOT represented here. A FatPrim is a non-sphere brush
// (today only an oriented box) that the field eval linear-scans in addition to the
// sphere hash union. The struct is union-sized to the largest shape so adding a new
// kind costs one enum value + one primitive_sdf() case, with no per-shape array.
//
// This header is shared between C (surface.c field eval) and C++ (csg_lowering,
// cell.cpp), so it stays C-compatible (raylib Vector3/Matrix, no C++ features).

#include "raylib.h"   // Vector3, Matrix

#ifdef __cplusplus
extern "C" {
#endif

// Discriminator for primitive_sdf dispatch. Sphere is listed for completeness /
// future unification of the carve/clip scan, but additive spheres never become a
// FatPrim (they stay on the hash hot path). New shapes append here.
typedef enum {
    FAT_PRIM_SPHERE   = 0,
    FAT_PRIM_BOX      = 1,
    FAT_PRIM_CAPSULE  = 2,  // segment a->b skinned with radius r0 (sdSegment - r0)
    FAT_PRIM_CYLINDER = 3   // capped cone a->b, end radii r0/r1 (sdCappedCone; r0==r1 = straight cylinder, r1=0 = cone)
} FatPrimKind;

// One fat primitive. `center`/`boundRadius` is a bounding sphere used for mesh-bounds
// expansion and (future) culling. `invTransform` maps a WORLD point into the brush's
// local frame; the per-kind params (halfExtents/radius) are evaluated in that local
// frame. `stage` is the ordered-CSG stage index this primitive belongs to (see
// CsgStage in surface.c); primitives sharing a stage union together before the stage
// op folds into the running field.
typedef struct {
    int      kind;          // FatPrimKind
    Vector3  center;        // world-space bounding-sphere center
    float    boundRadius;   // world-space bounding-sphere radius (for bounds/cull)
    int      materialId;
    Vector4  tint;          // RGBA; a = blend strength. (1,1,1,0) = no tint.
    int      stage;         // ordered-CSG stage index (Layer-2 lowering assigns)

    // Param blob (union-sized to the largest shape). All evaluated in brush-local
    // space (post-invTransform), so the transform-stack scale is picked up exactly
    // like sphere/box (iso-primitives G2 rule).
    Matrix   invTransform;  // world -> brush-local (box, scaled sphere, capsule, cylinder)
    Vector3  halfExtents;   // box half-extents (FAT_PRIM_BOX)
    float    radius;        // sphere radius (FAT_PRIM_SPHERE)
    // Segment + tapered radii for the capsule (FAT_PRIM_CAPSULE) and capped cone
    // / cylinder (FAT_PRIM_CYLINDER). a,b are brush-local segment endpoints; r0 is
    // the radius at a, r1 the radius at b (capsule uses r0 only; cylinder r0==r1;
    // cone r1==0). Endpoints are stored pre-transformed (the brush emits its
    // segment in its own local frame and invTransform carries the world->local map).
    Vector3  segA;          // segment endpoint a (capsule/cylinder)
    Vector3  segB;          // segment endpoint b (capsule/cylinder)
    float    r0;            // radius at a (capsule radius; cylinder/cone base radius)
    float    r1;            // radius at b (cylinder/cone top radius; 0 = cone tip)
} FatPrim;

// Signed distance of WORLD point `p` to the primitive (negative inside). Maps the
// point into brush-local space via invTransform, then evaluates the per-kind SDF.
float primitive_sdf(const FatPrim* prim, Vector3 p);

#ifdef __cplusplus
}
#endif

#endif // FAT_PRIMITIVE_H
