#version 330
// Phase-1 lighting: flat sky ambient x baked vertex AO + unshadowed sun N.L,
// material albedo from the shared 64x12 material table, TriEx tint blend,
// emission add. Probe-volume lighting replaces ambient/sun terms in Phase 2.
in vec3 fragNormal;
in vec4 fragTint;
in vec2 fragMatAO;
in vec3 fragWorldPos;

uniform float materialTable[64 * 12];   // [0..2] albedo, [3] rough, [4] metal, [5] emission
uniform int   materialCount;
uniform vec3  sunDir;                   // normalized, points FROM the sun toward the scene
uniform vec3  sunColor;
uniform vec3  ambientColor;

uniform sampler3D probeAmbient;         // unit 4
uniform sampler3D probeDominant;        // unit 5
uniform vec3  probeOrigin;              // cell(0,0,0) CENTER
uniform float probeCell;
uniform vec3  probeDims;                // (nx,ny,nz) as floats
uniform int   useProbes;                // 0 => Phase-1 flat fallback

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

    vec3 lit;
    if (useProbes == 1) {
        vec3 uvw = ((fragWorldPos - probeOrigin) / probeCell + 0.5) / probeDims;
        vec4 A = texture(probeAmbient,  clamp(uvw, 0.0, 1.0));
        vec4 B = texture(probeDominant, clamp(uvw, 0.0, 1.0));
        vec3  pAmb   = A.rgb * 4.0;                    // kProbeAmbientScale
        float sunVis = A.a;
        vec3  dm     = B.xyz * 2.0 - 1.0;
        float dmLen  = max(length(dm), 1e-4);
        vec3  domDir = dm / dmLen;
        float domI   = B.a * 4.0;
        float ambLum = max(dot(pAmb, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
        vec3  ambChroma = pAmb / ambLum;
        vec3  direct = ambChroma * domI * 2.0 * max(dot(N, domDir), 0.0);
        lit = (pAmb + direct) * ao + sunColor * ndl * sunVis;
    } else {
        lit = ambientColor * ao + sunColor * ndl;      // Phase-1 fallback, never black
    }
    vec3 color = albedo * lit + albedo * emission;
    color = color / (color + vec3(1.0));          // Reinhard
    color = pow(color, vec3(1.0 / 2.2));          // gamma
    finalColor = vec4(color, 1.0);
}
