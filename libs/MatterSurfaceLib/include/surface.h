#ifndef SURFACE_H
#define SURFACE_H

// libs/MatterSurfaceLib/include/surface.h
//
// The public C API of MatterSurfaceLib's isosurface mesher. Given an array of
// `Particle` spheres it samples a smooth-min (metaball) union-of-spheres
// signed field over the grid described by `Bounds` and marching-cubes it into
// a mesh, optionally folding in ordered CSG stages, non-sphere "fat"
// primitives, subtractive carve particles and foreign clip particles.
//
// How it fits:
//  - Implemented in `src/surface.c` -- real C, hence the `extern "C"` block
//    and the plain-C `MtVec3` / `Particle` types.
//  - The marching-cubes branch of `meshing_algorithm.h` calls in here;
//    `cell.cpp` / `cluster.cpp` drive it per merge group; MatterEngine3's bake
//    pipeline sits above that.
//  - It does NOT touch the GPU. `Mesh` and `Color` are raylib POD types only
//    (the GL/raylib render path is deleted); a `Mesh` here is a CPU vertex /
//    index buffer that the BLAS packer later consumes.
//
// Threading and scratch:
//  - There is no global state. Concurrency is expressed through
//    `SurfaceScratch`: create ONE per worker thread with
//    `CreateSurfaceScratch()`, reuse it across calls, and destroy it with
//    `DestroySurfaceScratch()`. It owns the reusable memory pool and the
//    particle spatial hash.
//  - `GenerateMeshWithScratch` / `GenerateMeshStaged` / `ProbeFieldScalar` and
//    the `...WithScratch` normal pass all take that scratch; the plain
//    `GenerateMesh` / `ComputeSurfaceNormals` entry points are the same
//    algorithms without the reuse. Geometry is byte-identical either way.
//  - `SurfaceScratchHash()` exposes the hash the last scratch-based mesh build
//    produced so downstream per-triangle nearest-particle lookups can reuse it
//    instead of rebuilding.
//
// Conventions and gotchas:
//  - `particleRadius` is a REFERENCE radius (the maximum effective radius in
//    the set) used only to size the spatial-hash search; each particle's own
//    `.radius` is what the field actually integrates. Passing something
//    smaller than the true maximum silently loses geometry.
//  - `blendWidth` (and `carveBlend`) are fillet widths in the same length
//    units as the radii; 0 means a hard union / hard subtraction.
//  - Every optional feature has a documented "pass NULL, 0" form that is
//    byte-identical to the path without it. That is deliberate -- it is what
//    lets new features ship without invalidating existing bakes -- so preserve
//    it when extending these signatures.
//  - Shading normals from `ComputeSurfaceNormals` are the analytic field
//    gradient, which depends only on world position and is therefore
//    continuous across independently meshed cells. Any pass that moves
//    vertices or recomputes normals from face geometry (e.g. mesh
//    simplification) must be followed by re-running it, or shading seams
//    appear at cell boundaries.

// Phase 4 (Step 3) of docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md:
// Bounds and ProbeFieldScalar's `point` param moved off raylib's Vector3 onto
// matter_math_c.h's MtVec3. raylib.h stays included -- Mesh/Color (GenerateMesh's
// return type, GetMaterialColor, ConvertMeshToBVHTriangles) are out of scope for
// this phase (Mesh migration is deferred; see the Phase 4 brief).
#include "raylib.h"
#include "matter_math_c.h"   // MtVec3
#include "particle.h"
#include "fat_primitive.h"   // FatPrim (typed iso-primitives)
#include "csg_stages.h"      // FieldStages (ordered CSG)
#include <stdbool.h>

// Plain-C vector and triangle records for handing geometry to a BVH builder.
// Despite the comment below these are definitions, not forward declarations,
// and neither type is referenced anywhere else in the tree today -- the
// engine's BVH stores SpatialQueryLib's `Tri` / `TriEx` (`tri.h`) instead.
// Treat them as a legacy interchange format, not as the current one.
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


// The sampling volume for one mesh build. `center` and `size` are in the same
// space as the `Particle` positions handed to the same call (cluster-local for
// the cell mesher, world space for a probe), and `divisionPow` sets the grid
// resolution to 2^divisionPow per axis -- so it, together with `size`, fixes
// the sampled cell size and hence the smallest feature the mesher can resolve
// (`MeshContext::voxel` in `meshing_algorithm.h` is the derived figure).
// Raising `divisionPow` by one multiplies field-evaluation cost by roughly
// eight.
// Bounds structure defining the volume for isosurface generation
typedef struct {
    MtVec3 center;
    MtVec3 size;
    int     divisionPow;  // Resolution = 2^divisionPow
} Bounds;

// Legacy tuning flags. Note that no function declared in this header accepts a
// `MeshGenerationConfig` -- `GetDefaultMeshConfig()` is its only producer, and
// the behaviour it describes is fixed inside `src/surface.c`. Kept for source
// compatibility; do not expect setting these to change anything.
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
                       MtVec3 point);

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


// Utility function to create color based on material ID
Color GetMaterialColor(int materialId);

// Utility function to generate unique edge key for marching cubes
unsigned long long GetEdgeKey(int x, int y, int z, int edgeIndex);



#ifdef __cplusplus
}
#endif

#endif // SURFACE_H