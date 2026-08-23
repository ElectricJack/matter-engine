#ifndef MATTER_GPU_MESH_COMMON_GLSL
#define MATTER_GPU_MESH_COMMON_GLSL

struct GpuMeshParticle {
    vec3 positionM;
    float radiusM;
};

struct GpuMeshBinParams {
    vec4 binOriginAndSize;
    uvec4 binDimsAndCount;
    uvec4 counts;
};

uint gpuMeshLinearBin(uvec3 coordinate, uvec3 dimensions) {
    return coordinate.x + dimensions.x *
        (coordinate.y + dimensions.y * coordinate.z);
}

#endif
