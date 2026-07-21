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
    float camera_fwd_x;
    float camera_fwd_y;
    float camera_fwd_z;
    float tan_half_fov;
    float aspect_ratio;
    float jitter_offset_u;
    float jitter_offset_v;
    float vol_enabled;
    float vol_debug_view;
} lighting;

#include "sky_common.glsl"

vec3 compute_view_ray(vec2 uv) {
    vec3 fwd = vec3(lighting.camera_fwd_x, lighting.camera_fwd_y,
                    lighting.camera_fwd_z);
    vec3 world_up = abs(fwd.y) < 0.999 ? vec3(0, 1, 0) : vec3(0, 0, 1);
    vec3 right = normalize(cross(fwd, world_up));
    vec3 up = cross(right, fwd);
    vec2 compensated = uv - vec2(lighting.jitter_offset_u,
                                  lighting.jitter_offset_v);
    vec2 ndc = vec2(compensated.x * 2.0 - 1.0, 1.0 - compensated.y * 2.0);
    return normalize(fwd +
        right * ndc.x * lighting.aspect_ratio * lighting.tan_half_fov +
        up    * ndc.y * lighting.tan_half_fov);
}

void main() {
    vec4 albedo = texture(albedo_texture, in_uv);
    vec4 normal_payload = texture(normal_texture, in_uv);
    vec3 normal_sample = normal_payload.xyz;
    float normal_length_squared = dot(normal_sample, normal_sample);
    vec3 normal = normal_length_squared > 1e-20
                    ? normal_sample * inversesqrt(normal_length_squared)
                    : vec3(0.0);

    if (normal_length_squared <= 1e-20) {
        vec3 ray = compute_view_ray(in_uv);
        vec3 to_sun = normalize(-lighting.sun_direction);
        vec3 sky = sky_with_sun(ray, lighting.sky_color,
                                to_sun, lighting.sun_color,
                                lighting.sun_intensity);
        if (lighting.vol_enabled > 0.5) {
            vec3 far_uvw = vec3(in_uv, 1.0 - 0.5 / float(VOL_D));
            vec4 integrated = texture(vol_integrated_texture, far_uvw);
            sky = sky * integrated.a + integrated.rgb;
        }
        out_hdr = vec4(sky, 1.0);
        return;
    }

    vec4 orm = texture(orm_texture, in_uv);
    vec3 to_sun = normalize(-lighting.sun_direction);
    float direct = max(dot(normal, to_sun), 0.0);
    float roughness = orm.x;
    float metallic = orm.y;
    float ao = orm.z;
    vec3 diffuse = albedo.rgb * (1.0 - metallic);
    vec3 ambient = diffuse * sky_irradiance(normal, lighting.sky_color) * ao;
    vec3 visibility = texture(visibility_texture, in_uv).rgb;
    if (lighting.debug_view > 1.5) {
        out_hdr = vec4(normal * 0.5 + 0.5, 1.0);
        return;
    }
    if (lighting.debug_view > 0.5) {
        out_hdr = vec4(visibility, 1.0);
        return;
    }
    if (lighting.vol_debug_view > 2.5) {
        float depth_sample = texture(depth_texture, in_uv).r;
        float linear_depth = VOL_NEAR / max(depth_sample, 1e-6);
        float slice_n = depth_to_slice_n(linear_depth);
        vec3 uvw = vec3(in_uv, slice_n);
        if (lighting.vol_debug_view > 4.5) {
            vec4 integrated = texture(vol_integrated_texture, uvw);
            out_hdr = vec4(integrated.rgb, 1.0);
        } else if (lighting.vol_debug_view > 3.5) {
            vec4 integrated = texture(vol_integrated_texture, uvw);
            out_hdr = vec4(integrated.rgb * 5.0, 1.0);
        } else {
            vec4 integrated = texture(vol_integrated_texture, uvw);
            float density_vis = 1.0 - integrated.a;
            out_hdr = vec4(vec3(density_vis), 1.0);
        }
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
                float backlit = clamp(-dot(normal, to_sun), 0.0, 1.0);
                float aniso = clamp(material.scattering_shape.y, 0.0, 1.0);
                backlit = pow(backlit, mix(1.0, 4.0, aniso));
                float distance_falloff =
                    clamp(material.scattering_shape.x * 4.0, 0.0, 1.0);
                sun_response += material.scattering.rgb * subsurface *
                                backlit * distance_falloff;
            } else {
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
    vec3 glass_reflection = vec3(0.0);
    if (transmission_coverage < 0.01 && material_index < rt_materials.length()) {
        RtMaterialGpu mat = rt_materials[material_index];
        float mat_trans = clamp(mat.transmission.x, 0.0, 1.0);
        if (mat_trans > 0.0) {
            float ior = max(mat.transmission.y, 1.001);
            float r0 = pow((1.0 - ior) / (1.0 + ior), 2.0);
            vec3 view_ray = compute_view_ray(in_uv);
            float cos_i = clamp(abs(dot(normal, view_ray)), 0.0, 1.0);
            float fresnel = r0 + (1.0 - r0) * pow(1.0 - cos_i, 5.0);
            transmission_coverage = mat_trans * (1.0 - fresnel);
            vec3 refract_dir = refract(-view_ray, normal, 1.0 / ior);
            bool tir = dot(refract_dir, refract_dir) < 0.001;
            vec3 reflect_dir = reflect(-view_ray, normal);
            vec3 through_dir = tir ? reflect_dir : refract_dir;
            vec3 to_sun = normalize(-lighting.sun_direction);
            transmission.rgb = sky_with_sun(through_dir, lighting.sky_color,
                                            to_sun, lighting.sun_color,
                                            lighting.sun_intensity * 0.5)
                             * mat.absorption_pad.rgb;
            glass_reflection = sky_with_sun(reflect_dir, lighting.sky_color,
                                            to_sun, lighting.sun_color,
                                            lighting.sun_intensity)
                             * fresnel * mat_trans;
        }
    }
    vec3 linear_hdr = (ambient + sun * mix(1.0, 0.65, roughness) +
                       raw_diffuse) * (1.0 - transmission_coverage) +
                      emission + specular +
                      transmission.rgb * transmission_coverage +
                      glass_reflection;

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
