#version 330
// Phase-1 lighting: flat sky ambient x baked vertex AO + unshadowed sun N.L,
// material albedo from the shared 64x12 material table, TriEx tint blend,
// emission add.
in vec3 fragNormal;
in vec4 fragTint;
in vec2 fragMatAO;
in vec3 fragWorldPos;

uniform float materialTable[64 * 12];   // [0..2] albedo, [3] rough, [4] metal, [5] emission
uniform int   materialCount;
uniform vec3  sunDir;                   // normalized, points FROM the sun toward the scene
uniform vec3  sunColor;
uniform vec3  ambientColor;

// Phase 4: Wang-tile ground sampling helpers.
#include "tileset_sampling.glsl"

out vec4 finalColor;

void main() {
    int mid = int(max(fragMatAO.x, 0.0) + 0.5);
    if (mid >= materialCount) mid = 0;
    int b = mid * 12;
    vec3  albedo   = vec3(materialTable[b], materialTable[b+1], materialTable[b+2]);
    float emission = materialTable[b+5];

    albedo = mix(albedo, fragTint.rgb, fragTint.a);

    vec3  N   = normalize(fragNormal);

    // Phase 4: Wang-tile ground sampling — branch on groundTilesetSlot field at [11].
    // Layout dependency: slot [11] = groundTilesetSlot per MaterialRegistryPackForGPU.
    // Kept in sync with materials.glsl:53 which reads the same offset.
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
        // Rebase the surface normal onto the baked tangent-space normal.
        vec3 upN = vec3(0.0, 1.0, 0.0);
        vec3 raw = cross(upN, N);
        vec3 T;
        if (dot(raw, raw) < 1e-6) {
            // normal is nearly parallel to +Y — pick +X as the tangent instead
            T = normalize(cross(vec3(1.0, 0.0, 0.0), N));
        } else {
            T = normalize(raw);
        }
        vec3 B = cross(N, T);
        N = normalize(T * baked_normal_ts.x + B * baked_normal_ts.y + N * baked_normal_ts.z);
    }
    float ndl = max(dot(N, -sunDir), 0.0);
    float ao  = clamp(fragMatAO.y, 0.0, 1.0);

    vec3 lit = ambientColor * ao + sunColor * ndl;
    vec3 color = albedo * lit + albedo * emission;
    color = color / (color + vec3(1.0));          // Reinhard
    color = pow(color, vec3(1.0 / 2.2));          // gamma
    finalColor = vec4(color, 1.0);
}
