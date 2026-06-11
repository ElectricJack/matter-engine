#ifndef PARTICLE_H
#define PARTICLE_H

#include "raylib.h"
#include "raymath.h"
#include <stdbool.h>

// Particle structure representing a sphere with material ID
typedef struct {
    Vector3 position;
    int     materialId;
} Particle;


#endif //PARTICLE_H