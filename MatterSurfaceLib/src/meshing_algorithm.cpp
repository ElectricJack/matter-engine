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
