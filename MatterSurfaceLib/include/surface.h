#ifndef SURFACE_H
#define SURFACE_H

#include "raylib.h"
#include "particle.h"
#include "fat_primitive.h"   // FatPrim (typed iso-primitives)
#include "csg_stages.h"      // FieldStages (ordered CSG)
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


#ifdef __cplusplus
extern "C" {
#endif

// Opaque per-thread scratch context owning all reusable mesh-build buffers (the
// scalar/mesh/edge memory pool and the particle spatial hash). One per thread.
typedef struct SurfaceScratch SurfaceScratch;
typedef struct SpatialHash SpatialHash;  // owned by the scratch; see SurfaceScratchHash
SurfaceScratch* CreateSurfaceScratch(void);
void            DestroySurfaceScratch(SurfaceScratch* scratch);

// Scratch-aware variants of GenerateMesh / ComputeSurfaceNormals: the caller
// supplies (and reuses) a SurfaceScratch so the spatial hash built during mesh
// generation can be shared with the normal pass and with downstream
// per-triangle nearest-particle lookups via SurfaceScratchHash. Geometry is
// byte-identical to the non-scratch APIs.
Mesh GenerateMeshWithScratch(SurfaceScratch* scratch, Particle* particles, float particleRadius,
                             int particleCount, Bounds volume, float blendWidth,
                             Particle* clipParticles, int clipCount,
                             Particle* carveParticles, int carveCount, float carveBlend);
void ComputeSurfaceNormalsWithScratch(SurfaceScratch* scratch, Mesh* mesh, Particle* particles,
                             float particleRadius, int particleCount, float blendWidth,
                             Particle* clipParticles, int clipCount,
                             Particle* carveParticles, int carveCount, float carveBlend);
// Returns the scratch's current spatial hash (the one the last GenerateMeshWithScratch
// built), or NULL if none yet. The scratch owns it; do not destroy it.
SpatialHash* SurfaceScratchHash(SurfaceScratch* scratch);

// Typed iso-primitives + ordered CSG (Phase 1). Like GenerateMeshWithScratch but
// also folds an ordered CSG stage list (`stages`, tagging the hashed spheres) and a
// borrowed fat-primitive array (`fat`,`fatCount`, e.g. oriented boxes) into the
// field. When `stages` is NULL/<=1 stage and `fatCount`==0, the field is computed
// by the legacy union-then-carve path (byte-identical to GenerateMeshWithScratch).
// The fat array is NOT inserted into the spatial hash; it is linear-scanned per
// sample. The caller must keep `fat`/`stages` arrays alive for the call.
Mesh GenerateMeshStaged(SurfaceScratch* scratch, Particle* particles, float particleRadius,
                        int particleCount, Bounds volume, float blendWidth,
                        const FieldStages* stages, const FatPrim* fat, int fatCount,
                        Particle* clipParticles, int clipCount,
                        Particle* carveParticles, int carveCount, float carveBlend);

// Test/probe seam: evaluate the production field scalar at a single WORLD point
// using the SAME staged + fat + carve/clip code path the mesher uses. `particles`
// is the additive sphere set (also tagged by `stages`); returns the signed field
// value (<0 => the meshed surface treats the point as inside). Builds a one-shot
// spatial hash internally on `scratch`. GL-free.
float ProbeFieldScalar(SurfaceScratch* scratch, Particle* particles, float particleRadius,
                       int particleCount, float blendWidth,
                       const FieldStages* stages, const FatPrim* fat, int fatCount,
                       Particle* carveParticles, int carveCount, float carveBlend,
                       Vector3 point);

// Main API function for generating a mesh from particles.
// particleRadius is a reference radius (max effective radius in the set) used to
// size the spatial-hash search; each particle's own .radius drives the SDF.
// blendWidth k sets the metaball smooth-min fillet size (0 = hard union, no blend).
// clipParticles/clipCount are FOREIGN particles (from other merge groups) used to
// clip this group's field: where a foreign surface is nearer than this group's own
// field, the group is forced outside so its isosurface terminates on the equidistant
// shared wall (material-aware surfacing). Pass NULL,0 for no clipping (byte-identical
// to the unclipped path).
// carveParticles/carveCount are SUBTRACTIVE particles smooth-CSG subtracted from
// the union (smooth-max against -(|p-c|-r)); carveBlend is the carve fillet width
// k_c (carveBlend<=0 => hard subtraction). Pass NULL,0,0 for no carving
// (byte-identical to the uncarved path).
Mesh GenerateMesh(Particle* particles, float particleRadius, int particleCount, Bounds volume, float blendWidth, Particle* clipParticles, int clipCount, Particle* carveParticles, int carveCount, float carveBlend);

// Enhanced API function with configuration options
Mesh GenerateMeshWithConfig(Particle* particles, float particleRadius, int particleCount, Bounds volume, float blendWidth, MeshGenerationConfig config, Particle* clipParticles, int clipCount, Particle* carveParticles, int carveCount, float carveBlend);

// Recompute per-vertex shading normals in place as the analytic SDF gradient of
// the (smooth-min) union-of-spheres field. With blendWidth 0 each normal is the
// unit vector from the nearest particle center to the vertex; with blendWidth k
// it is the softmax-weighted blend of those directions, matching the metaball
// field GenerateMesh produced. This depends only on world position, so it is
// continuous across independently-meshed cells (no shading seams), and it must
// be reapplied after any pass that moves vertices or rebuilds normals from face
// geometry (e.g. simplify_mesh, which reverts to per-cell face-normal averaging).
// Operates on mesh->vertices/mesh->normals; any existing normal is used as the
// fallback for degenerate vertices with no particle in range.
// clipParticles/clipCount mirror GenerateMesh's clip field so the recomputed
// normals match the carved surface; pass NULL,0 for no clipping.
void ComputeSurfaceNormals(Mesh* mesh, Particle* particles, float particleRadius, int particleCount, float blendWidth, Particle* clipParticles, int clipCount, Particle* carveParticles, int carveCount, float carveBlend);

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

#ifdef __cplusplus
}
#endif

#endif // SURFACE_H