#ifndef SURFACE_H
#define SURFACE_H

#include "raylib.h"
#include "raymath.h"
#include "particle.h"
#include <stdbool.h>

// Forward declaration for BVH Triangle
typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    Vec3 v0, v1, v2;      // Triangle vertices
    Vec3 n0, n1, n2;      // Per-vertex normals
    Vec3 centroid;        // Pre-computed centroid for faster BVH building
    Vec3 normal;          // Face normal (computed from vertices)
    int  material_id;     // Material identifier
} BVHTriangle;


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

// Recompute per-vertex shading normals in place as the analytic SDF gradient of
// the union-of-spheres field: each normal is the unit vector from the nearest
// particle center to the vertex. This depends only on world position, so it is
// continuous across independently-meshed cells (no shading seams), and it must
// be reapplied after any pass that moves vertices or rebuilds normals from face
// geometry (e.g. simplify_mesh, which reverts to per-cell face-normal averaging).
// Operates on mesh->vertices/mesh->normals; any existing normal is used as the
// fallback for degenerate vertices with no particle in range.
void ComputeSurfaceNormals(Mesh* mesh, Particle* particles, float particleRadius, int particleCount);

// Create default configuration
MeshGenerationConfig GetDefaultMeshConfig(void);

// Cleanup function to release memory pool resources
void SurfaceLibCleanup(void);

// Utility function to create color based on material ID
Color GetMaterialColor(int materialId);

// Utility function to generate unique edge key for marching cubes
unsigned long long GetEdgeKey(int x, int y, int z, int edgeIndex);

// Convert raylib Mesh to BVH Triangle array with per-vertex normals
BVHTriangle* ConvertMeshToBVHTriangles(Mesh mesh, int* triangleCount);

// Free BVH triangle array
void FreeBVHTriangles(BVHTriangle* triangles);


#endif // SURFACE_H