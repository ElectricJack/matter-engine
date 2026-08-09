#version 460
#extension GL_GOOGLE_include_directive : require

#include "vol_common.glsl"
// IMPOSTOR_IDENTITY_BIT + impostor_identity_material(). gbuffer.frag sets bit
// 31 of the identity attachment's .x on billboard fragments; the emission /
// subsurface lookup below uses .x as a material index and must mask it off.
#include "impostor_common.glsl"

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
    float camera_near;
    float camera_far;
    float camera_y;
    float vol_cloud_top;
    float vol_height_layer;
    // Sun angular size. sun_angular_diameter_deg is carried for completeness
    // (nothing here reads it); the two cosines are what the disc is drawn
    // from, computed on the CPU — see VkSceneLighting in vk_scene_renderer.h
    // and matter/sun_angles.h.
    float sun_angular_diameter_deg;
    float sun_disc_cos_edge;
    float sun_disc_cos_core;
} lighting;

#include "environment_common.glsl"

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

// ---- debug_view 3.0: linear depth ------------------------------------------
//
// Reversed-ZO inverse, same expression as the two inline copies below (the
// vol_debug_view branch and the main lighting path); kept as a function here
// so the depth view cannot drift from them.
float composite_linear_depth(float hw_depth) {
    return lighting.camera_near * lighting.camera_far /
        max(hw_depth * (lighting.camera_far - lighting.camera_near) +
            lighting.camera_near, 1e-6);
}

// Encode eye-space linear depth so it survives an 8-bit PNG with enough
// precision to do metric work (screen period -> world period, depth ratios
// between crops). All three channels are linear in LOG depth over
// [kDebugDepthNear, kDebugDepthFar]:
//
//   t = log(d / near) / log(far / near)      in [0,1]
//   R = t                        coarse; one code step is 1/255 of the log
//                                range, i.e. ~3.6% in depth
//   G = fract(t * CYCLES)        fine; one code step is ~0.11% in depth
//   B = fract(t * CYCLES + 0.5)  the same fine ramp in quadrature, so a
//                                decoder always has one fine channel at least
//                                a quarter cycle away from its wrap
//
// Decode: cycle index from R (its 3.6% resolution is far inside the 32.5%
// depth span of one cycle, so the index is unambiguous), fraction from
// whichever of G/B is furthest from a wrap.
//
// This only reads back correctly if the display pass is in passthrough — the
// ACES curve in display_transform.frag is not invertible at 8 bits. See
// `passthrough` there; vk_scene_renderer sets it for exactly this view.
const float kDebugDepthNear   = 1.0;
const float kDebugDepthFar    = 8192.0;
const float kDebugDepthCycles = 32.0;

vec3 encode_debug_depth(float linear_depth) {
    float t = log(clamp(linear_depth, kDebugDepthNear, kDebugDepthFar) /
                  kDebugDepthNear) / log(kDebugDepthFar / kDebugDepthNear);
    return vec3(t,
                fract(t * kDebugDepthCycles),
                fract(t * kDebugDepthCycles + 0.5));
}

float integrated_slice_at_depth(float depth) {
    // vol_integrate stores each texel after integrating through that froxel's
    // far edge. Shift the lookup back by half a texel so a terrain hit samples
    // the integral ending at the hit instead of blending in the next froxel
    // behind it. At kilometre scale that leaked froxel can be tens of metres
    // deep, enough to put valley fog over an otherwise clear summit.
    int depth_slices = textureSize(vol_integrated_texture, 0).z;
    return clamp(depth_to_slice_n(depth, float(depth_slices)) - 0.5 / float(depth_slices),
                 0.0, 1.0 - 0.5 / float(depth_slices));
}

void main() {
    // Ahead of the sky early-out below, so the depth view covers the whole
    // frame (sky reads hw depth 0 -> camera_far -> t = 1) rather than being
    // punched through by the physical sky wherever the gbuffer normal is empty.
    // debug_view 4.0: the GBuffer albedo, untouched by lighting. It exists for
    // the shaders that write a diagnostic FIELD into albedo rather than a
    // colour -- today gbuffer.frag's render.pom.horizon_debug, which draws the
    // baked horizon term and the reference march as greyscale over the
    // parallaxed ground. Anything lit is unreadable as a field: ambient scales
    // it by physical irradiance, which sweeps across a curved surface, so a
    // field that varied and a surface that curved would look the same.
    //
    // Ahead of the depth view for the same reason that one is ahead of the sky
    // early-out: these are ordered highest-mode-first, and `> 2.5` would
    // otherwise swallow mode 4.
    if (lighting.debug_view > 3.5) {
        out_hdr = vec4(texture(albedo_texture, in_uv).rgb, 1.0);
        return;
    }
    if (lighting.debug_view > 2.5) {
        out_hdr = vec4(encode_debug_depth(
            composite_linear_depth(texture(depth_texture, in_uv).r)), 1.0);
        return;
    }
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
        vec3 sky = sample_physical_sky(ray, to_sun, lighting.sky_color);
        float disc = smoothstep(lighting.sun_disc_cos_edge,
                                lighting.sun_disc_cos_core,
                                dot(normalize(ray), to_sun));
        sky += lighting.sun_color * lighting.sun_intensity * disc;
        if (lighting.vol_enabled > 0.5) {
            int depth_slices = textureSize(vol_integrated_texture, 0).z;
            vec3 far_uvw = vec3(in_uv, 1.0 - 0.5 / float(depth_slices));
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
    vec3 ambient = diffuse * sample_sky_irradiance(normal, lighting.sky_color) * ao;
    vec3 visibility = texture(visibility_texture, in_uv).rgb;
    if (lighting.debug_view > 1.5) {
        out_hdr = vec4(normal * 0.5 + 0.5, 1.0);
        return;
    }
    if (lighting.debug_view > 0.5) {
        out_hdr = vec4(visibility, 1.0);
        return;
    }
    if (lighting.vol_debug_view > 0.5) {
        float depth_sample = texture(depth_texture, in_uv).r;
        // Reversed-ZO inverse: hw=1 -> near, hw=0 -> far (see
        // vol_scatter.comp's prev_depth for the plug-in verification).
        float linear_depth = lighting.camera_near * lighting.camera_far /
            max(depth_sample * (lighting.camera_far - lighting.camera_near) +
                lighting.camera_near, 1e-6);
        float slice_n = integrated_slice_at_depth(linear_depth);
        vec3 uvw = vec3(in_uv, slice_n);
        if (lighting.vol_debug_view > 2.5) {
            vec4 integrated = texture(vol_integrated_texture, uvw);
            out_hdr = vec4(integrated.rgb, 1.0);
        } else if (lighting.vol_debug_view > 1.5) {
            vec4 integrated = texture(vol_integrated_texture, uvw);
            out_hdr = vec4(integrated.rgb * 5.0, 1.0);
        } else {
            vec4 integrated = texture(vol_integrated_texture, uvw);
            float density_vis = 1.0 - integrated.a;
            out_hdr = vec4(vec3(density_vis), 1.0);
        }
        return;
    }
    // Mask gbuffer.frag's impostor bit out of .x before using it as an index.
    // Without the mask an impostor pixel fails the bounds test below and
    // silently loses its subsurface term -- a wrong-but-plausible result, not
    // a visible failure, which is why the mask is spelled out rather than left
    // to the guard.
    uint material_index = impostor_identity_material(
        texelFetch(identity_texture, ivec2(gl_FragCoord.xy), 0).r);
    vec3 sun_response = diffuse * direct;
    // AN IMPOSTOR CARD GETS NO SUBSURFACE TERM.
    //
    // A card depicts a WHOLE OBJECT, and the atlas carries one voted material
    // for all of it (impostor_bake.cpp's per-cluster vote) -- a conifer is
    // MAT.bark plus MAT.foliageThin, and foliageThin wins. Applying that one
    // material's translucency to every pixel of the card therefore applies
    // FOLIAGE scattering to the BARK, and foliageThin is thin-walled with
    // scattering (0.25, 0.65, 0.12): bright green.
    //
    // That is not subtle. In issue eb9f2981 the camera faces the trees' shadow
    // side, so card normals point away from the sun, `backlit` fires hard, and
    // the green additive lands at roughly ten times the bark's ambient. It is
    // why impostor trunks render bright green while mesh trunks -- which carry
    // material 14, subsurface 0 -- render dim brown. Simulating this pixel
    // end to end gave (66,111,30) with the term and (17,12,6) without; the
    // measured impostor trunks are (90,148,45) and the mesh trunks (34,29,23).
    //
    // Cards lose genuine backlit foliage glow as a result. That is the right
    // trade: the glow is a subtle effect at impostor distances, and the
    // alternative on offer is green tree trunks. A per-texel material channel
    // would fix it properly and is the obvious v2 (impostor_bake.h says so);
    // this costs one bit test and no baked bytes.
    const bool impostor_card = impostor_identity_is_card(
        texelFetch(identity_texture, ivec2(gl_FragCoord.xy), 0).r);
    if (!impostor_card && material_index < rt_materials.length()) {
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
    // Per-texel metalness (G-buffer ORM.y, the VT surfaces() tape 'metallic'
    // lane): metals have no diffuse, so `sun_response` above is zero for
    // them, and the traced reflection lane only sees the sun through its
    // lobe-prefiltered miss disk -- which vanishes for rough lobes. Without
    // this term a rough metal in full sunlight reads as a dark pit. Add the
    // analytic GGX sun lobe for the METAL component only: F0 is the texel
    // albedo, the whole term is weighted by `metallic` so it is exactly zero
    // for dielectrics (the regression gate -- this block must not change
    // metalness-0 pixels), and the RT shadow visibility that already gates
    // the diffuse sun term gates this one too. Deterministic (no rays, no
    // denoiser lane). Known approximation: a mirror-smooth metal aimed dead
    // at the sun can also catch the traced lane's miss disk (prefilter -> 1
    // there), double-counting up to ~2x on a few glint pixels; accepted over
    // threading a lobe-selection term between two shaders.
    if (metallic > 0.0 && direct > 0.0) {
        vec3 view = -compute_view_ray(in_uv);
        vec3 sun_half = normalize(view + to_sun);
        float n_dot_v = max(dot(normal, view), 1e-4);
        float n_dot_h = max(dot(normal, sun_half), 0.0);
        float v_dot_h = max(dot(view, sun_half), 1e-4);
        float alpha = max(roughness * roughness, 0.0004);
        float a2 = alpha * alpha;
        float d_denom = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
        float ggx_d = a2 / max(3.14159265359 * d_denom * d_denom, 1e-8);
        float g1v = 2.0 * n_dot_v /
            max(n_dot_v + sqrt(a2 + (1.0 - a2) * n_dot_v * n_dot_v), 1e-6);
        float g1l = 2.0 * direct /
            max(direct + sqrt(a2 + (1.0 - a2) * direct * direct), 1e-6);
        vec3 metal_f = albedo.rgb +
            (vec3(1.0) - albedo.rgb) * pow(1.0 - v_dot_h, 5.0);
        sun += metallic * metal_f * ggx_d * g1v * g1l /
               max(4.0 * n_dot_v * direct, 1e-6) * direct *
               lighting.sun_color * lighting.sun_intensity * visibility;
    }
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
            // Legacy glass packs black absorption; treat as clear. Same guard
            // rt_lighting.rgen applies before Beer-Lambert -- without it a
            // zeroed absorptionColor multiplied this fallback to black (the
            // historical "black glass" failure, reachable whenever the RT
            // transmission lane did not cover this pixel).
            vec3 fallback_absorption = mat.absorption_pad.rgb;
            if (dot(fallback_absorption, fallback_absorption) < 1e-8)
                fallback_absorption = vec3(1.0);
            float through_disc = smoothstep(lighting.sun_disc_cos_edge,
                                             lighting.sun_disc_cos_core,
                                             dot(normalize(through_dir), to_sun));
            transmission.rgb = (sample_physical_sky(through_dir, to_sun,
                                                     lighting.sky_color) +
                                lighting.sun_color * lighting.sun_intensity *
                                    through_disc * 0.5) * fallback_absorption;
            float reflection_disc = smoothstep(lighting.sun_disc_cos_edge,
                                                lighting.sun_disc_cos_core,
                                                dot(normalize(reflect_dir), to_sun));
            glass_reflection = (sample_physical_sky(reflect_dir, to_sun,
                                                     lighting.sky_color) +
                               lighting.sun_color * lighting.sun_intensity *
                                   reflection_disc) * fresnel * mat_trans;
        }
    }
    vec3 linear_hdr = (ambient + sun * mix(1.0, 0.65, roughness) +
                       raw_diffuse) * (1.0 - transmission_coverage) +
                      emission + specular +
                      transmission.rgb * transmission_coverage +
                      glass_reflection;

    if (lighting.vol_enabled > 0.5) {
        float depth_sample = texture(depth_texture, in_uv).r;
        // Reversed-ZO inverse: hw=1 -> near, hw=0 -> far (see
        // vol_scatter.comp's prev_depth for the plug-in verification).
        float linear_depth = lighting.camera_near * lighting.camera_far /
            max(depth_sample * (lighting.camera_far - lighting.camera_near) +
                lighting.camera_near, 1e-6);
        vec3 view_ray = compute_view_ray(in_uv);
        vec3 camera_forward =
            normalize(vec3(lighting.camera_fwd_x, lighting.camera_fwd_y,
                           lighting.camera_fwd_z));
        float ray_distance =
            linear_depth / max(dot(view_ray, camera_forward), 1e-4);
        float surface_y = lighting.camera_y + view_ray.y * ray_distance;

        // A straight view ray whose camera and terrain endpoints are both
        // above a horizontal cloud layer cannot pass through that layer.
        // Enforce that invariant after froxel integration so coarse distant
        // slices cannot leak valley density over mountain summits.
        bool entirely_above_clouds =
            lighting.vol_height_layer > 0.5 &&
            lighting.camera_y >= lighting.vol_cloud_top &&
            surface_y >= lighting.vol_cloud_top;
        if (!entirely_above_clouds) {
            float slice_n = integrated_slice_at_depth(linear_depth);
            vec3 uvw = vec3(in_uv, slice_n);
            vec4 integrated = texture(vol_integrated_texture, uvw);
            linear_hdr = linear_hdr * integrated.a + integrated.rgb;
        }
    }

    out_hdr = vec4(linear_hdr, 1.0);
}
