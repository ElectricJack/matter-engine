#ifndef PARTICLE_H
#define PARTICLE_H

// libs/MatterSurfaceLib/include/particle.h
//
// `Particle` -- the atomic input to MatterSurfaceLib's surfacing pipeline. A
// particle is a sphere with a material; everything the mesher produces is the
// isosurface of a (smooth-min) union of these spheres.
//
// How it fits:
//  - Produced by the lattice/culling stage (`particle_culling.h` emits
//    `EmittedParticle`, which `Cluster::add_particle` turns into these).
//  - Consumed by the field evaluator and mesher in `src/surface.c` via the
//    `surface.h` C API, and by `cell.cpp` / `cluster.cpp` / `csg_lowering.cpp`
//    on the C++ side.
//  - The same struct is reused for the SUBTRACTIVE role: carve particles
//    (`generate_carve_particles`) and foreign clip particles are ordinary
//    `Particle`s; it is the parameter they are passed as that decides whether
//    they add to or subtract from the field.
//
// Considerations:
//  - This header must stay valid C. It is included from real C (`surface.c`)
//    as well as C++, which is why it uses `matter_math_c.h`'s POD `MtVec3`
//    rather than `mm::Vec3` -- see the note below and `matter_math_c.h`.
//  - `Particle` is a trivially copyable POD with no invariants. It is passed
//    around as bare `Particle*` + count arrays, never owned by this header.
//  - No coordinate space is implied. Positions and radii are in whatever space
//    the caller meshes in -- cluster-local for the cell mesher, world space for
//    `ProbeFieldScalar`. Whatever space is chosen must match the `Bounds`
//    handed to `GenerateMesh` and the blend/carve widths.

// Phase 4 (Step 3) of docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md:
// this header used to include raylib.h for Vector3. It is shared between real C
// (surface.c field eval) and C++ (cell.cpp, cluster.cpp, csg_lowering.cpp), so it
// uses matter_math_c.h's plain-C MtVec3 instead -- see that header's comment for
// why mm::Vec3 (matter_math.h) cannot be used here (namespace + default member
// initializers are both C++-only).
#include "matter_math_c.h"
#include <stdbool.h>

// Particle structure representing a sphere with material ID
// `position` is the sphere centre in the caller's meshing space; `radius` is
// this particle's own radius (distinct from the single reference radius passed
// to `GenerateMesh`, which only sizes the spatial-hash search); `materialId`
// indexes the material registry (`MaterialRegistryGet`) and is what drives
// merge-group splitting, albedo and the choice of meshing algorithm.
typedef struct {
    MtVec3 position;
    float   radius;      // Per-particle radius used by the SDF union
    int     materialId;
} Particle;


#endif //PARTICLE_H