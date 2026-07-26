#ifndef PARTICLE_H
#define PARTICLE_H

// Phase 4 (Step 3) of docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md:
// this header used to include raylib.h for Vector3. It is shared between real C
// (surface.c field eval) and C++ (cell.cpp, cluster.cpp, csg_lowering.cpp), so it
// uses matter_math_c.h's plain-C MtVec3 instead -- see that header's comment for
// why mm::Vec3 (matter_math.h) cannot be used here (namespace + default member
// initializers are both C++-only).
#include "matter_math_c.h"
#include <stdbool.h>

// Particle structure representing a sphere with material ID
typedef struct {
    MtVec3 position;
    float   radius;      // Per-particle radius used by the SDF union
    int     materialId;
} Particle;


#endif //PARTICLE_H