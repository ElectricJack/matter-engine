#ifndef ORIENTED_CUBE_ALGORITHM_H
#define ORIENTED_CUBE_ALGORITHM_H

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
