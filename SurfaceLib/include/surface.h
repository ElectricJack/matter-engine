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

// Mesh generation configuration options
typedef struct {
    bool enableEdgeDeduplication;  // Enable/disable edge deduplication (saves memory but may have duplicate vertices)
    bool enableMemoryReuse;        // Enable memory pool reuse for better performance
} MeshGenerationConfig;


// Main API function for generating a mesh from particles
Mesh GenerateMesh(Particle* particles, float particleRadius, int particleCount, Bounds volume);

// Enhanced API function with configuration options
Mesh GenerateMeshWithConfig(Particle* particles, float particleRadius, int particleCount, Bounds volume, MeshGenerationConfig config);

// Create default configuration
MeshGenerationConfig GetDefaultMeshConfig(void);

// Cleanup function to release memory pool resources
void SurfaceLibCleanup(void);

// Utility function to create color based on material ID
Color GetMaterialColor(int materialId);

// Utility function to generate unique edge key for marching cubes
unsigned long long GetEdgeKey(int x, int y, int z, int edgeIndex);


#endif // SURFACE_H