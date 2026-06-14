#ifndef PARTICLE_H
#define PARTICLE_H

#include "raylib.h"
#include <stdbool.h>

// Particle structure representing a sphere with material ID
typedef struct {
    Vector3 position;
    float   radius;      // Per-particle radius used by the SDF union
    int     materialId;
} Particle;


#endif //PARTICLE_H