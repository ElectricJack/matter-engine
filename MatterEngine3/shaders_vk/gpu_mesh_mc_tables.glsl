#ifndef MATTER_GPU_MESH_MC_TABLES_GLSL
#define MATTER_GPU_MESH_MC_TABLES_GLSL

// The 256x16 triangle table remains authored once in MatterSurfaceLib's
// mc_tables.h and is uploaded as a signed-int storage buffer. These helpers
// define the matching corner and edge ABI used by both extraction paths.
uvec3 gpuMeshCornerOffset(uint corner) {
    const uvec3 offsets[8] = uvec3[8](
        uvec3(0u, 0u, 0u), uvec3(1u, 0u, 0u),
        uvec3(1u, 1u, 0u), uvec3(0u, 1u, 0u),
        uvec3(0u, 0u, 1u), uvec3(1u, 0u, 1u),
        uvec3(1u, 1u, 1u), uvec3(0u, 1u, 1u));
    return offsets[corner];
}

uvec2 gpuMeshEdgeCorners(uint edge) {
    const uvec2 endpoints[12] = uvec2[12](
        uvec2(0u, 1u), uvec2(1u, 2u), uvec2(2u, 3u),
        uvec2(3u, 0u), uvec2(4u, 5u), uvec2(5u, 6u),
        uvec2(6u, 7u), uvec2(7u, 4u), uvec2(0u, 4u),
        uvec2(1u, 5u), uvec2(2u, 6u), uvec2(3u, 7u));
    return endpoints[edge];
}

#endif
