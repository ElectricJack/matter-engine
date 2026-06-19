#ifndef MARCHING_CUBES_ALGORITHM_H
#define MARCHING_CUBES_ALGORITHM_H

#include "meshing_algorithm.h"

// The existing isosurface mesher: smooth-min SDF union over the group's
// particles, marching cubes extraction, optional simplification, analytic
// gradient normals, then per-triangle nearest-particle material/tint tagging.
class MarchingCubesAlgorithm : public MeshingAlgorithm {
public:
    GroupMeshResult generate(const MeshContext& ctx) const override;
};

#endif // MARCHING_CUBES_ALGORITHM_H
