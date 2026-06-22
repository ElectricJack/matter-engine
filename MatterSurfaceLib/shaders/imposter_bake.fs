#version 330 core
in vec3 cageWorldPos;
in vec3 cageNormal;
out vec4 fragColor;

uniform float maxDisp;       // shell thickness; ray marches at most this far inward
uniform int   debugAlbedo;   // !=0: bake raw albedo (no lighting) to isolate ray/material
uniform int   debugAO;       // !=0: bake hit.ao as grayscale to isolate the AO term

// --- Free symbols required by lighting.glsl (mirror raytrace_tlas_blas.fs) ---
uniform float giStrength;
uniform float shadowStrength;
uniform int   aoEnabled;

vec3 lightPos   = vec3(3.0, 8.0, 2.0);
vec3 lightColor = vec3(4.0, 3.8, 3.5);
vec3 ambient    = vec3(0.34, 0.34, 0.33);

uint getGridHash(vec3 pos) {
    ivec3 gridPos = ivec3(floor(pos * 2.0));
    return uint(gridPos.x * 73 + gridPos.y * 137 + gridPos.z * 281);
}

#include "materials.glsl"
#include "bvh_tlas_common.glsl"
#include "lighting.glsl"

void main() {
    vec3 n = normalize(cageNormal);
    vec3 origin = cageWorldPos;
    vec3 dir = -n;                       // inward
    HitResult hit = intersectScene(origin, dir, maxDisp * 1.5);
    if (!hit.hit) { fragColor = vec4(0.0, 0.0, 0.0, 0.0); return; }

    vec3 hitPos = origin + dir * hit.t;
    vec3 hn = normalize(hit.normal);
    MaterialProperties matProps = getMaterialProperties(hit.material);
    vec3 albedo = mix(matProps.albedo, hit.tint, hit.tintAlpha);

    if (debugAlbedo != 0) { fragColor = vec4(albedo, 1.0); return; }
    if (debugAO != 0) { fragColor = vec4(vec3(hit.ao), 1.0); return; }

    // Fixed outward view (no live camera): bake radiance as seen from outside the cage.
    vec3 viewDir = dir;
    uint seed = uint(gl_FragCoord.x) * 1973u + uint(gl_FragCoord.y) * 9277u + 1u;
    vec3 radiance = calculatePBR(hitPos, hn, viewDir, albedo, matProps.roughness,
                                 matProps.metallic, hit.ao, true, seed);
    if (matProps.emission > 0.0) radiance += matProps.albedo * matProps.emission;
    fragColor = vec4(radiance, 1.0);
}
