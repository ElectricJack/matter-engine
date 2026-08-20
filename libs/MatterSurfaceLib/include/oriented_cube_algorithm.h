#ifndef ORIENTED_CUBE_ALGORITHM_H
#define ORIENTED_CUBE_ALGORITHM_H

// libs/MatterSurfaceLib/include/oriented_cube_algorithm.h
//
// The discrete-geometry mesher: one of the two implementations of the
// `MeshingAlgorithm` interface declared in `meshing_algorithm.h` (the other
// being marching cubes). Which one runs for a merge group is selected by the
// group's material (`MaterialDef.meshingAlgorithm` -> `MeshAlgorithm`) and
// resolved through `GetMeshingAlgorithm()`, which hands back a process-wide
// singleton -- so this class holds no state and `generate()` is `const`.
//
// How it fits:
//  - Input is a `MeshContext` built by `Cell::build_group_mesh` after the
//    group's particle subset has been resolved (cull + visibility clamp).
//  - Output is a `GroupMeshResult` (see `mesh_worker_pool.h`), the same type
//    the marching-cubes path produces, so downstream simplification / BLAS
//    packing does not care which mesher ran.
//  - Like every `MeshingAlgorithm` it must stay GL-free and reentrant: it runs
//    on the mesh worker pool's threads.
//
// Considerations:
//  - This algorithm ignores most of `MeshContext`: no SDF sampling, so
//    `blend_width`, `clip`, `carve`, `stages`, `fat` and the `SurfaceScratch`
//    are all unused. It reads the particle set, tints and bounds only.
//  - Because it emits one cube per particle, sub-slot refinement is pure cost
//    here -- that is what `CullParams::coarse_material_mask` (see
//    `particle_culling.h`) exists to suppress for cube materials.
//  - Determinism: orientation is derived from the particle's quantized
//    position, not from a call counter or RNG stream, so a re-mesh of the same
//    particle set reproduces the same geometry.

#include "meshing_algorithm.h"

// Renders each particle in the group as a deterministically oriented cube
// (edge = 2*radius*sizeScale). No SDF, no grid, no scratch: pure per-particle
// geometry. Orientation is seeded from the particle's quantized position so it
// is stable across re-meshes. sizeScale / rotation jitter are read from the
// MSL_CUBE_SIZE_SCALE / MSL_CUBE_ROT_JITTER env vars (defaults 1.0).
class OrientedCubeAlgorithm : public MeshingAlgorithm {
public:
    GroupMeshResult generate(const MeshContext& ctx) const override;
};

#endif // ORIENTED_CUBE_ALGORITHM_H
