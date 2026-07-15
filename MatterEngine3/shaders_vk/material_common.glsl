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

#endif
