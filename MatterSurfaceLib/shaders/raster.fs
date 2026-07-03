#version 330
// Phase-1 lighting: flat sky ambient x baked vertex AO + unshadowed sun N.L,
// material albedo from the shared 64x12 material table, TriEx tint blend,
// emission add. Probe-volume lighting replaces ambient/sun terms in Phase 2.
in vec3 fragNormal;
in vec4 fragTint;
in vec2 fragMatAO;

uniform float materialTable[64 * 12];   // [0..2] albedo, [3] rough, [4] metal, [5] emission
uniform int   materialCount;
uniform vec3  sunDir;                   // normalized, points FROM the sun toward the scene
uniform vec3  sunColor;
uniform vec3  ambientColor;

out vec4 finalColor;

void main() {
    int mid = int(max(fragMatAO.x, 0.0) + 0.5);
    if (mid >= materialCount) mid = 0;
    int b = mid * 12;
    vec3  albedo   = vec3(materialTable[b], materialTable[b+1], materialTable[b+2]);
    float emission = materialTable[b+5];

    albedo = mix(albedo, fragTint.rgb, fragTint.a);

    vec3  N   = normalize(fragNormal);
    float ndl = max(dot(N, -sunDir), 0.0);
    float ao  = clamp(fragMatAO.y, 0.0, 1.0);

    vec3 color = albedo * (ambientColor * ao + sunColor * ndl) + albedo * emission;
    color = color / (color + vec3(1.0));          // Reinhard
    color = pow(color, vec3(1.0 / 2.2));          // gamma
    finalColor = vec4(color, 1.0);
}
