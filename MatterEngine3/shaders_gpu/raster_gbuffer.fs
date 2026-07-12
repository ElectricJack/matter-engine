#version 460
// G-buffer MRT fragment shader: outputs surface properties for RT lighting.
// No lighting computation — RT pipeline handles all shading.
in vec3 fragNormal;
in vec4 fragTint;
in vec2 fragMatAO;
in vec3 fragWorldPos;

#define MAX_MATERIALS 32
#define MATERIAL_FLOATS_PER_DEF 12
uniform float materialTable[MAX_MATERIALS * MATERIAL_FLOATS_PER_DEF];
uniform int   materialCount;
uniform vec3  sunDir;   // unused here but kept for uniform-location parity with raster.fs

// Phase 4: Wang-tile ground sampling helpers.
#include "tileset_sampling.glsl"

layout(location = 0) out vec4 out_albedo;    // rgb = albedo, a = emission
layout(location = 1) out vec4 out_normal;    // xyz = world normal, w = translucency
layout(location = 2) out vec4 out_orm;       // r = roughness, g = metallic, b = bakedAO, a = materialId/255

void main() {
    int mid = int(max(fragMatAO.x, 0.0) + 0.5);
    if (mid >= materialCount) mid = 0;
    int b = mid * MATERIAL_FLOATS_PER_DEF;
    vec3  albedo       = vec3(materialTable[b], materialTable[b+1], materialTable[b+2]);
    float roughness    = materialTable[b+3];
    float metallic     = materialTable[b+4];
    float emission     = materialTable[b+5];
    float translucency = materialTable[b+7];

    albedo = mix(albedo, fragTint.rgb, fragTint.a);

    vec3 N = normalize(fragNormal);

    // Phase 4: Wang-tile ground sampling — override albedo/N/ORM from tileset atlas.
    int groundTilesetSlot = int(materialTable[b + 11]);
    if (groundTilesetSlot >= 0) {
        vec2 worldXZ = fragWorldPos.xz;
        vec2 dWorldXZ_dx = vec2(dFdx(fragWorldPos.x), dFdx(fragWorldPos.z));
        vec2 dWorldXZ_dy = vec2(dFdy(fragWorldPos.x), dFdy(fragWorldPos.z));
        vec3 baked_normal_ts, orm;
        vec3 ground_albedo = wang_sample_ground(groundTilesetSlot,
                                                worldXZ, dWorldXZ_dx, dWorldXZ_dy,
                                                baked_normal_ts, orm);
        albedo = ground_albedo;
        roughness = orm.x;
        metallic  = orm.y;
        vec3 upN = vec3(0.0, 1.0, 0.0);
        vec3 raw = cross(upN, N);
        vec3 T;
        if (dot(raw, raw) < 1e-6) {
            T = normalize(cross(vec3(1.0, 0.0, 0.0), N));
        } else {
            T = normalize(raw);
        }
        vec3 B = cross(N, T);
        N = normalize(T * baked_normal_ts.x + B * baked_normal_ts.y + N * baked_normal_ts.z);
    }

    float ao = clamp(fragMatAO.y, 0.0, 1.0);

    out_albedo = vec4(albedo, emission);
    out_normal = vec4(N * 0.5 + 0.5, translucency);
    out_orm    = vec4(roughness, metallic, ao, float(mid) / 255.0);
}
