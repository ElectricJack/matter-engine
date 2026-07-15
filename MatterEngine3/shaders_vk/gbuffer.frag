#version 460
#extension GL_GOOGLE_include_directive : require

#include "material_common.glsl"

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec4 in_tint;
layout(location = 2) in vec4 in_surface;
layout(location = 3) in vec3 in_velocity_valid;
layout(location = 4) flat in uint in_material_index;
layout(location = 5) flat in uint in_instance_token;
layout(location = 6) flat in uint in_material_valid;

layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_orm;
layout(location = 3) out vec2 out_velocity;
layout(location = 4) out uvec2 out_material_instance;

void main() {
    MaterialGpu material;
    if (in_material_valid != 0u) {
        material = materials[in_material_index];
    } else {
        material.base_roughness = vec4(0.5, 0.5, 0.5, 1.0);
        material.metal_opacity_spec_coat = vec4(0.0, 1.0, 1.0, 0.0);
        material.specular_tint_coat_roughness = vec4(0.0);
        material.emission_strength = vec4(0.0);
        material.transmission = vec4(0.0);
        material.absorption_pad = vec4(0.0);
        material.scattering = vec4(0.0);
        material.scattering_shape = vec4(0.0);
        material.flags_misc = uvec4(0u);
    }
    vec3 base_color = resolveBaseColor(material, in_tint);
    float roughness = clamp(material.base_roughness.w, 0.0, 1.0);
    float metallic = clamp(material.metal_opacity_spec_coat.x, 0.0, 1.0);
    float opacity = clamp(material.metal_opacity_spec_coat.y, 0.0, 1.0);
    float emission = max(material.emission_strength.w, 0.0);
    float encoded_emission = min(log2(1.0 + emission), 15.875);
    float ao = in_surface.w > 0.5 ? clamp(in_surface.z, 0.0, 1.0) : 1.0;
    out_albedo = vec4(base_color, opacity);
    out_normal = vec4(normalize(in_normal), encoded_emission);
    // ORM alpha is reserved/opaque (1.0); emission lives in normal alpha.
    out_orm = vec4(roughness, metallic, ao, 1.0);
    out_velocity = in_velocity_valid.z > 0.5
                       ? in_velocity_valid.xy
                       : vec2(0.0);
    out_material_instance =
        uvec2(in_material_index, in_instance_token);
}
