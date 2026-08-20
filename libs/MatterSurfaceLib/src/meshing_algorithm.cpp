// libs/MatterSurfaceLib/src/meshing_algorithm.cpp
//
// The mesher dispatch table. `MaterialDef.meshingAlgorithm` names a
// `MeshAlgorithm` per material; `Cell::build_group_mesh` resolves it per merge
// group and calls `generate()` on whatever this returns. See
// `include/meshing_algorithm.h` for the `MeshContext` payload.
//
// Adding a mesher means: a new `MeshAlgorithm` enum value, a subclass, and a
// case here. This file is the only place the enum is turned into an object.
//
// Threading and lifetime: the two implementations are stateless and const, and
// the function-local statics get thread-safe initialization on first use.
// Callers may hold the returned reference indefinitely — the singletons live
// until process exit and are never destroyed in a way a mesh build could
// observe. An unrecognized enum value falls through to marching cubes rather
// than failing.
#include "meshing_algorithm.h"
#include "marching_cubes_algorithm.h"
#include "oriented_cube_algorithm.h"

const MeshingAlgorithm& GetMeshingAlgorithm(MeshAlgorithm algo) {
    static const MarchingCubesAlgorithm marching_cubes;
    static const OrientedCubeAlgorithm  oriented_cubes;
    switch (algo) {
        case MeshAlgorithm::OrientedCubes: return oriented_cubes;
        case MeshAlgorithm::MarchingCubes:
        default:                           return marching_cubes;
    }
}
