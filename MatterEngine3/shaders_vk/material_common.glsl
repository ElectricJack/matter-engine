#ifndef MATTER_VK_MATERIAL_COMMON_GLSL
#define MATTER_VK_MATERIAL_COMMON_GLSL

struct MaterialGpu {
    vec4 base_roughness;
    vec4 metal_opacity_spec_coat;
    vec4 specular_tint_coat_roughness;
    vec4 emission_strength;
    vec4 transmission;
    vec4 absorption_pad;
    vec4 scattering;
    vec4 scattering_shape;
    uvec4 flags_misc;
};

layout(set = 1, binding = 5, std430) readonly buffer Materials {
    MaterialGpu materials[];
};

vec3 resolveBaseColor(MaterialGpu material, vec4 tint) {
    return mix(material.base_roughness.rgb, tint.rgb,
               clamp(tint.a, 0.0, 1.0));
}

vec3 materialF0(MaterialGpu material, vec3 baseColor) {
    vec3 dielectric = vec3(0.04) *
        max(material.metal_opacity_spec_coat.z, 0.0) *
        max(material.specular_tint_coat_roughness.rgb, vec3(0.0));
    return mix(dielectric, baseColor,
               clamp(material.metal_opacity_spec_coat.x, 0.0, 1.0));
}

vec3 schlickFresnel(vec3 f0, float vDotH) {
    float f = pow(1.0 - clamp(vDotH, 0.0, 1.0), 5.0);
    return f0 + (vec3(1.0) - f0) * f;
}

float ggxDistribution(float nDotH, float roughness) {
    float alpha = max(roughness * roughness, 0.0004);
    float a2 = alpha * alpha;
    float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265359 * d * d, 1e-8);
}

float ggxReflectionPdf(float nDotH, float vDotH, float roughness) {
    return ggxDistribution(max(nDotH, 0.0), roughness) * max(nDotH, 0.0) /
           max(4.0 * abs(vDotH), 1e-6);
}

#endif
