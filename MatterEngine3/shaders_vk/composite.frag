#version 460
#extension GL_GOOGLE_include_directive : require

#include "vol_common.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_hdr;

layout(set = 0, binding = 0) uniform sampler2D albedo_texture;
layout(set = 0, binding = 1) uniform sampler2D normal_texture;
layout(set = 0, binding = 2) uniform sampler2D orm_texture;
layout(set = 0, binding = 3) uniform sampler2D visibility_texture;
layout(set = 0, binding = 4) uniform sampler2D raw_diffuse_texture;
layout(set = 0, binding = 5) uniform sampler2D specular_texture;
layout(set = 0, binding = 6) uniform usampler2D identity_texture;

// Mirrors MaterialGpuRecord / rt_surface_common.glsl RtMaterialGpu; declared
// locally because rt_surface_common.glsl claims set 0 bindings 3-5.
struct RtMaterialGpu {
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

layout(set = 0, binding = 7, std430) readonly buffer RtMaterialTable {
    RtMaterialGpu rt_materials[];
};

const uint MATERIAL_THIN_WALLED = 1u << 0u;

layout(set = 0, binding = 8) uniform sampler2D transmission_texture;
layout(set = 0, binding = 9) uniform sampler3D vol_integrated_texture;
layout(set = 0, binding = 10) uniform sampler2D depth_texture;

layout(push_constant) uniform SceneLighting {
    vec3 sun_direction;
    float sun_intensity;
    vec3 sun_color;
    float diffuse_rt_multiplier;
    vec3 sky_color;
    float emission_multiplier;
    float debug_view;
    float vol_enabled;
    float vol_debug_view;
    float pad2;
} lighting;

void main() {
    vec4 albedo = texture(albedo_texture, in_uv);
    vec4 normal_payload = texture(normal_texture, in_uv);
    vec3 normal_sample = normal_payload.xyz;
    float normal_length_squared = dot(normal_sample, normal_sample);
    vec3 normal = normal_length_squared > 1e-20
                    ? normal_sample * inversesqrt(normal_length_squared)
                    : vec3(0.0);
    vec4 orm = texture(orm_texture, in_uv);
    vec3 to_sun = normalize(-lighting.sun_direction);
    float direct = max(dot(normal, to_sun), 0.0);
    float roughness = orm.x;
    float metallic = orm.y;
    float ao = orm.z;
    vec3 diffuse = albedo.rgb * (1.0 - metallic);
    vec3 ambient = diffuse * lighting.sky_color * ao;
    vec3 visibility = texture(visibility_texture, in_uv).rgb;
    if (lighting.debug_view > 0.5 && lighting.debug_view < 2.5) {
        // Debug views 1-2 (normals, visibility): no fog applied.
        out_hdr = vec4(visibility, 1.0);
        return;
    }
    // Volumetric debug views (3/4/5).
    if (lighting.vol_debug_view > 2.5) {
        float depth_sample = texture(depth_texture, in_uv).r;
        float linear_depth = VOL_NEAR / max(depth_sample, 1e-6);
        float slice_n = depth_to_slice_n(linear_depth);
        vec3 uvw = vec3(in_uv, slice_n);
        if (lighting.vol_debug_view > 4.5) {
            // Debug 5: integrated result.
            vec4 integrated = texture(vol_integrated_texture, uvw);
            out_hdr = vec4(integrated.rgb, 1.0);
        } else if (lighting.vol_debug_view > 3.5) {
            // Debug 4: raw scatter (reuse vol_integrated for scatter vis).
            vec4 integrated = texture(vol_integrated_texture, uvw);
            out_hdr = vec4(integrated.rgb * 5.0, 1.0);
        } else {
            // Debug 3: raw density - sample the integrated transmittance.
            vec4 integrated = texture(vol_integrated_texture, uvw);
            float density_vis = 1.0 - integrated.a;
            out_hdr = vec4(vec3(density_vis), 1.0);
        }
        return;
    }
    // Sky early-return: pixels with no valid normal are sky. Sample the far
    // slice of the integrated volume for god rays visible against the sky.
    if (normal_length_squared <= 1e-20) {
        vec3 sky_result = vec3(0.0);
        if (lighting.vol_enabled > 0.5) {
            // Sample the far slice (slice_n ~ 1.0) for full-depth scattering.
            vec3 far_uvw = vec3(in_uv, 1.0 - 0.5 / float(VOL_D));
            vec4 integrated = texture(vol_integrated_texture, far_uvw);
            sky_result = integrated.rgb;
        }
        out_hdr = vec4(sky_result, 1.0);
        return;
    }
    uint material_index = texelFetch(identity_texture,
                                     ivec2(gl_FragCoord.xy), 0).r;
    vec3 sun_response = diffuse * direct;
    if (material_index < rt_materials.length()) {
        RtMaterialGpu material = rt_materials[material_index];
        float subsurface = clamp(material.scattering.w, 0.0, 1.0);
        if (subsurface > 0.0) {
            if ((material.flags_misc.x & MATERIAL_THIN_WALLED) != 0u) {
                // Thin-walled backlight: view-independent wrapped term
                // (composite has no camera position), sharpened by the
                // authored anisotropy.
                float backlit = clamp(-dot(normal, to_sun), 0.0, 1.0);
                float aniso = clamp(material.scattering_shape.y, 0.0, 1.0);
                backlit = pow(backlit, mix(1.0, 4.0, aniso));
                float distance_falloff =
                    clamp(material.scattering_shape.x * 4.0, 0.0, 1.0);
                sun_response += material.scattering.rgb * subsurface *
                                backlit * distance_falloff;
            } else {
                // Wax-style wrap: soften the terminator; the wrapped-in
                // region is tinted by the scattering color.
                float wrapped = clamp((dot(normal, to_sun) + subsurface) /
                                      (1.0 + subsurface), 0.0, 1.0);
                sun_response += diffuse * material.scattering.rgb *
                                max(wrapped - direct, 0.0);
            }
        }
    }
    vec3 sun = sun_response * lighting.sun_color * lighting.sun_intensity *
               visibility;
    float encoded_emission = normal_payload.w;
    float emission_strength =
        !isnan(encoded_emission) && !isinf(encoded_emission) &&
        encoded_emission > 0.0
            ? exp2(min(encoded_emission, 15.875)) - 1.0
            : 0.0;
    vec3 emission_color = material_index < rt_materials.length()
        ? rt_materials[material_index].emission_strength.rgb
        : albedo.rgb;
    vec3 emission = emission_color * emission_strength *
                    lighting.emission_multiplier;
    vec3 raw_diffuse = texture(raw_diffuse_texture, in_uv).rgb *
                       lighting.diffuse_rt_multiplier;
    vec3 specular = texture(specular_texture, in_uv).rgb;
    vec4 transmission = texture(transmission_texture, in_uv);
    float transmission_coverage = clamp(transmission.a, 0.0, 1.0);
    vec3 linear_hdr = (ambient + sun * mix(1.0, 0.65, roughness) +
                       raw_diffuse) * (1.0 - transmission_coverage) +
                      emission + specular + transmission.rgb;

    // Apply volumetric fog: color = color * T + inscatter.
    if (lighting.vol_enabled > 0.5) {
        float depth_sample = texture(depth_texture, in_uv).r;
        float linear_depth = VOL_NEAR / max(depth_sample, 1e-6);
        float slice_n = depth_to_slice_n(linear_depth);
        vec3 uvw = vec3(in_uv, slice_n);
        vec4 integrated = texture(vol_integrated_texture, uvw);
        linear_hdr = linear_hdr * integrated.a + integrated.rgb;
    }

    out_hdr = vec4(linear_hdr, 1.0);
}
