#ifndef MARCHING_CUBES_ALGORITHM_H
#define MARCHING_CUBES_ALGORITHM_H

// libs/MatterSurfaceLib/include/marching_cubes_algorithm.h
//
// The default mesher behind the MeshingAlgorithm interface
// (meshing_algorithm.h), selected per merge group by
// MaterialDef.meshingAlgorithm == 0 (MeshAlgorithm::MarchingCubes).
//
// Callers do not construct this: GetMeshingAlgorithm(MeshAlgorithm) returns
// the process-wide singleton, and Cell::build_group_mesh() invokes it. The
// class is stateless and generate() is const, which is what makes the shared
// singleton safe -- like every MeshingAlgorithm it must stay GL-free and
// reentrant on the caller's MeshContext::scratch so it can run on a mesh
// worker thread.
//
// Everything it needs arrives in MeshContext (particles, bounds, blend width,
// clip/carve sets, simplification ratio, and the optional ordered-CSG stages
// and fat primitives); it ignores nothing and is the only implementation that
// consumes the full context.

#include "meshing_algorithm.h"

// The existing isosurface mesher: smooth-min SDF union over the group's
// particles, marching cubes extraction, optional simplification, analytic
// gradient normals, then per-triangle nearest-particle material/tint tagging.
class MarchingCubesAlgorithm : public MeshingAlgorithm {
public:
    GroupMeshResult generate(const MeshContext& ctx) const override;
};

#endif // MARCHING_CUBES_ALGORITHM_H
