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

// counts.y is the temporal split; counts.zw identify active primary and
// secondary phases so inactive NaN sentinels never enter spatial bins.
bool gpuMeshBinParticleEnabled(uint particleId, GpuMeshBinParams params) {
    return particleId < params.counts.y
        ? params.counts.z != 0u
        : params.counts.w != 0u;
}

struct GpuMeshFieldParams {
    vec4 originAndIso;
    vec4 spacingAndBlend;
    vec4 binOriginAndSize;
    uvec4 sampleDimsAndCount;
    uvec4 binDimsAndCount;
    uvec4 counts;
    vec4 queryRadiusAndPadding;
    uvec4 source0PhaseSpans;
    uvec4 source1PhaseSpans;
    vec4 sourceBlendOriginAndEnabled;
    vec4 sourceBlendDirection;
    vec4 sourceBlendDistancesAndPadding;
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

bool gpuMeshParticleInSpan(uint particleId, uint begin, uint count) {
    return particleId >= begin && particleId - begin < count;
}

float gpuMeshSourceParticleWeight(uint source, uint particleId,
                                  GpuMeshFieldParams params) {
    const uvec4 spans = source == 0u
        ? params.source0PhaseSpans
        : params.source1PhaseSpans;
    if (gpuMeshParticleInSpan(particleId, spans.x, spans.y))
        return params.queryRadiusAndPadding.y;
    if (gpuMeshParticleInSpan(particleId, spans.z, spans.w))
        return params.queryRadiusAndPadding.z;
    return 0.0;
}

uint gpuMeshLinearBin(uvec3 coordinate, uvec3 dimensions) {
    return coordinate.x + dimensions.x *
        (coordinate.y + dimensions.y * coordinate.z);
}

#endif
