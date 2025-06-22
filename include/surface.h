#ifndef SURFACE_H
#define SURFACE_H

#include "raylib.h"
#include "raymath.h"
#include "particle.h"
#include <stdbool.h>


// Bounds structure defining the volume for isosurface generation
typedef struct {
    Vector3 center;
    Vector3 size;
    int     divisionPow;  // Resolution = 2^divisionPow
} Bounds;


// Main API function for generating a mesh from particles
Mesh GenerateMesh(Particle* particles, float particleRadius, int particleCount, Bounds volume);

// Utility function to create color based on material ID
Color GetMaterialColor(int materialId);

// Utility function to generate unique edge key for marching cubes
unsigned long long GetEdgeKey(int x, int y, int z, int edgeIndex);


#endif // SURFACE_H