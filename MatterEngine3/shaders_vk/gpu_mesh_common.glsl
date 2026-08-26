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

struct GpuMeshFieldParams {
    vec4 originAndIso;
    vec4 spacingAndBlend;
    vec4 binOriginAndSize;
    uvec4 sampleDimsAndCount;
    uvec4 binDimsAndCount;
    uvec4 counts;
    vec4 queryRadiusAndPadding;
};

// counts.z is the first secondary-phase particle index. The primary and
// secondary smooth-min weights are queryRadiusAndPadding.yz. counts.w is the
// emit-only active-cell count.
float gpuMeshParticlePhaseWeight(uint particleId,
                                 GpuMeshFieldParams params) {
    return particleId < params.counts.z
        ? params.queryRadiusAndPadding.y
        : params.queryRadiusAndPadding.z;
}

uint gpuMeshLinearBin(uvec3 coordinate, uvec3 dimensions) {
    return coordinate.x + dimensions.x *
        (coordinate.y + dimensions.y * coordinate.z);
}

#endif
