#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require

// Ground tileset sampling (Task 9): RT set 0 mirrors the raster set 1
// bindings at 15 (tex array) / 16 (TilesetParams UBO) — 0-14 are occupied by
// this file's own bindings (3,4,5) and by rt_lighting.rgen's (0,1,6-14); see
// the binding sweep recorded in the Task 9 plan notes. Every shader that
// includes rt_surface_common.glsl (rt_surface.rchit, rt_visibility.rahit,
// rt_radiance.rmiss, rt_lighting.rgen, rt_surface_test.rgen) therefore
// declares these two bindings even if it never samples them; the renderer
// binds them uniformly across the RT pipeline's set 0, so the unused
// declarations are harmless.
#define TILESET_SET 0
#define TILESET_TEX_BINDING 15
#define TILESET_PARAMS_BINDING 16
#include "tileset_common.glsl"
#include "surface_detail.glsl"

// WP-G (chart VT in the RT path): RT set 0 mirrors the raster set 1 VT
// bindings 10/11/12 at 17/18/19 -- the same mirroring trick the tileset port
// used for 15/16 above, and for the same reason (0-16 are taken). 18 is the
// shared indirection STORAGE BUFFER (raster and RT read the SAME buffer; the
// old RT-side image mirror died with the buffer-indirection redesign).
// Binding 13 (the feedback storage image) is deliberately NOT mirrored: rays
// never request pages (spec Phase 5 leaves RT-side feedback optional and
// off), so VT_FEEDBACK_BINDING stays undefined here and vt_write_feedback()
// compiles to nothing. Every shader that includes this file declares 17-19
// even if it never samples VT; the renderer writes them uniformly across the
// RT set.
#define VT_SET 0
#define VT_POOL_BINDING 17
#define VT_INDIRECTION_BINDING 18
#define VT_VARIANTS_BINDING 19
#include "vt_common.glsl"
#include "vt_normal_frame.glsl"

struct RtSurface {
    vec3 position;
    float hit_t;
    vec3 normal;
    uint material_index;
    vec4 tint;
    vec2 uv;              // chart-atlas UV when vt_slot != 0 (WP-A/WP-G)
    float baked_ao;
    uint flags;
    // --- WP-G additions ----------------------------------------------------
    // Transported VT slot of the hit BLAS's rung (0 = chartless => legacy).
    uint vt_slot;
    // Atlas-UV units per world metre across the hit triangle, from the
    // triangle's UV area / world area (the standard ray-cone texture-LOD
    // estimator). Multiplying by the cone footprint gives the UV-space
    // derivative vt_desired_mip() wants. 0 for a degenerate/chartless triangle.
    float uv_density;
    // World-space cone footprint width at the hit point: the incoming cone
    // width plus spread * hit_t, written by the closest-hit shader. This is
    // the quantity that replaced RT_TILESET_CONE_SPREAD * hit_t.
    float cone_width;
    uint water_binding_slot;
    uint water_generation;
    // Nine additional payload scalars (36 bytes): transformed LOCAL frame.
    // Raygen has no instance-transform descriptor. Do not normalize columns:
    // inverse-transpose scale must survive until the final normal is formed.
    // When RT_SURFACE_DETAIL_APPLIED is set, columns instead hold precomputed
    // albedo, ORM and world shading normal. Closest-hit owns that tagged form.
    mat3 vt_normal_basis;
    vec3 visibility_position; // undisplaced proxy hit; ray origins stay outside it
};

const uint RT_SURFACE_DETAIL_APPLIED = 4u;
const uint RT_SURFACE_DETAIL_MATERIAL_FLAG = 32u;

// Ray cone (WP-G). `cone_width` is the footprint width at the RAY ORIGIN and
// `cone_spread` the cone's spread angle in radians (small-angle: width grows
// by spread * distance). Both are INPUTS written by the raygen before every
// traceRayEXT on this payload; the closest-hit shader consumes them and folds
// the result into RtSurface::cone_width. They occupy the two words that were
// pad0/pad1, so the payload size is unchanged.
struct RtSurfacePayload {
    RtSurface surface;
    uint part_slot;
    uint primitive;
    float cone_width;
    float cone_spread;
};

struct GpuRtPartRecord {
    uvec2 vertex_address;
    uvec2 index_address;
    uint vertex_stride;
    uint vertex_count;
    uint primitive_count;
    uint valid;
    uint vt_slot;   // WP-G, was pad0 (see vk_gi_contract.h)
    uint water_binding_slot;
    uint water_generation;
    uint pad3;
};

struct RtRasterVertex {
    vec3 position;
    vec3 normal;
    vec4 tint;
    vec4 surface;
    uint material_index;
    uint pad0;
    uint pad1;
    uint pad2;
};

layout(buffer_reference, buffer_reference_align = 4) readonly buffer
RtVertexBuffer {
    uint words[];
};

layout(buffer_reference, buffer_reference_align = 4) readonly buffer
RtIndexBuffer {
    uint indices[];
};

layout(set = 0, binding = 3, std430) readonly buffer RtPartTable {
    GpuRtPartRecord rt_parts[];
};

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

layout(set = 0, binding = 4, std430) readonly buffer RtMaterialTable {
    RtMaterialGpu rt_materials[];
};

layout(set = 0, binding = 5, std430) buffer RtErrorCounter {
    uint invalid_part_records;
    uint any_hit_invocations;
    uint any_hit_layers;
    uint capped_rays;
};

// RT hit-path ground tileset override (Task 9): flat (non-POM) Wang sampling
// at a traced hit, for GI bounces and reflection/refraction hits that land
// on ground geometry away from the primary GBuffer pixel (the primary pixel
// itself is already textured by gbuffer.frag/Task 7 via the albedo/normal/orm
// G-buffer textures rt_lighting.rgen reads back).
//
// WP-G: the fixed RT_TILESET_CONE_SPREAD = 0.01 stand-in is GONE. The
// footprint is now surface.cone_width -- a real ray cone spawned from the
// primary pixel's angular footprint and widened per bounce (see
// rt_lighting.rgen's cone model). The ground tileset is addressed by world
// XZ, so a world-space footprint width IS its UV derivative; no conversion.

struct RtTilesetSample {
    bool applied;
    vec3 albedo;     // valid only if applied
    vec3 normal;     // shading normal; = surface.normal when !applied
    float roughness; // valid only if applied
    float metallic;  // valid only if applied
    // Phase 2 (horizon-map lighting): mean of the slot's 8 baked horizon
    // occlusion samples (tileset_horizon_mean_occlusion), 0.0 when
    // !applied or the slot has no horizon data. Raw (not yet scaled by
    // horizon_strength) -- consumers (e.g. rt_lighting.rgen's hit_radiance)
    // apply tileset.pom_c.w themselves when scaling sky irradiance, so this
    // field never double-applies the strength knob.
    float mean_occlusion;
};

RtTilesetSample rt_tileset_sample(RtMaterialGpu material, RtSurface surface) {
    RtTilesetSample result;
    result.applied = false;
    result.albedo = vec3(0.0);
    result.normal = surface.normal;
    result.roughness = 0.0;
    result.metallic = 0.0;
    result.mean_occlusion = 0.0;
    if ((material.flags_misc.x & RT_SURFACE_DETAIL_MATERIAL_FLAG) != 0u) return result;
    int slot = tileset_detail_slot(material.flags_misc);
    if (slot < 0) return result;
    float footprint = max(surface.cone_width, 1e-4);
    vec2 dWdx = vec2(footprint, 0.0);
    vec2 dWdy = vec2(0.0, footprint);
    vec3 orm;
    // Triplanar, matching gbuffer.frag's ground branch. This has to track the
    // raster path or a reflection disagrees with the surface it reflects.
    // tileset_rotate_normal's comment already claimed that both pipelines
    // rotate identically; moving only the raster side to triplanar would have
    // quietly broken that claim, so the RT side moves with it.
    // No derivatives exist in an RT stage, so the cone footprint is handed
    // over as an isotropic square (use_iso_footprint = true); a world-space
    // width IS the tileset's UV derivative, since the tileset is addressed in
    // metres on every axis.
    vec3 normal_ws;
    float slope_w_y;
    vec3 albedo = tileset_sample_ground_triplanar(
        slot, surface.position, surface.normal, vec3(footprint),
        vec3(footprint), true, normal_ws, orm, slope_w_y);
    float tint_blend = clamp(surface.tint.a, 0.0, 1.0);
    result.applied = true;
    result.albedo = albedo * mix(vec3(1.0), surface.tint.rgb, tint_blend);
    result.normal = normal_ws;
    result.roughness = clamp(orm.g, 0.0, 1.0);
    result.metallic = clamp(orm.b, 0.0, 1.0);
    // Horizon data is baked elevation in a top-down frame, so it is weighted
    // out on steep ground exactly as the raster path retires it through the
    // POM slope fade -- otherwise a cliff would take ambient occlusion
    // computed for a floor.
    result.mean_occlusion = tileset_horizon_mean_occlusion(
                                slot, surface.position.xz, dWdx, dWdy) *
                            smoothstep(0.2, 0.7, slope_w_y);
    return result;
}

// --- WP-G: chart-space virtual texturing at a traced hit --------------------
//
// Structurally the same sample the raster G-buffer takes (vt_common.glsl,
// shared verbatim), so a secondary hit on a VT part shades from the SAME
// pages the primary pixel does -- consistency by construction rather than by
// re-deriving the appearance from the tileset.
//
// Differences from gbuffer.frag, both deliberate:
//   * Mip comes from the ray cone, not dFdx/dFdy (no derivatives in an RT
//     stage). duv = cone footprint (metres) * uv_density (atlas UV / metre).
//   * No near-band handoff. gbuffer.frag modulates the VT base by the live
//     Wang detail tileset inside the POM fade distance (the mean-preserving
//     ratio trick); secondary rays use PURE VT at every distance. The ratio
//     term is a mean-preserving high-frequency detail whose expectation is
//     the VT base itself, so dropping it is unbiased for GI/reflection
//     integrals -- and replicating it would need per-hit POM state that does
//     not exist off the primary pixel.
//   * No feedback write: rays never request pages (see the binding note at
//     the top of this file).
// Unmapped pages resolve to the pinned tail inside vt_resolve(), so this
// never faults and never waits.
struct RtVtSample {
    bool  applied;
    vec3  albedo;
    vec3  normal;      // world-space shading normal; = surface.normal if !applied
    float roughness;
    float metallic;
    float occlusion;
    float desired_mip; // cone-selected virtual mip (debug/test readback)
    float mapped_mip;  // what was actually resident (<= desired when coarser)
};

RtVtSample rt_vt_sample(RtSurface surface) {
    RtVtSample result;
    result.applied = false;
    result.albedo = vec3(0.0);
    result.normal = surface.normal;
    result.roughness = 0.0;
    result.metallic = 0.0;
    result.occlusion = 1.0;
    result.desired_mip = 0.0;
    result.mapped_mip = 0.0;
    if ((surface.flags & RT_SURFACE_DETAIL_APPLIED) != 0u) {
        result.applied = true;
        float tint_blend = clamp(surface.tint.a, 0.0, 1.0);
        result.albedo = surface.vt_normal_basis[0] * mix(vec3(1), surface.tint.rgb, tint_blend);
        vec3 orm = surface.vt_normal_basis[1];
        result.normal = surface.vt_normal_basis[2];
        result.occlusion = clamp(orm.r, 0.0, 1.0);
        result.roughness = clamp(orm.g, 0.0, 1.0);
        result.metallic = clamp(orm.b, 0.0, 1.0);
        return result;
    }
    if (surface.vt_slot == 0u) return result;
    // Cone footprint -> atlas-UV footprint. An isotropic square footprint is
    // the right model here: the cone has no anisotropy of its own, and
    // uv_density is already the isotropic UV-per-metre of the hit triangle.
    float duv = max(surface.cone_width, 0.0) * surface.uv_density;
    float lod = vt_desired_mip(surface.vt_slot, vec2(duv, 0.0), vec2(0.0, duv));
    VtAddress address = vt_resolve(surface.vt_slot, surface.uv, lod);
    if (!address.valid) return result;
    vec3 albedo = vt_sample_channel(address, VT_CHANNEL_ALBEDO).rgb;
    vec3 orm = vt_sample_channel(address, VT_CHANNEL_ORM).rgb;
    vec3 normal_ts =
        vt_decode_normal(vt_sample_channel(address, VT_CHANNEL_NORMAL));
    // Same tint application as gbuffer.frag's VT branch.
    float tint_blend = clamp(surface.tint.a, 0.0, 1.0);
    result.applied = true;
    result.albedo = albedo * mix(vec3(1.0), surface.tint.rgb, tint_blend);
    result.normal = normalize(surface.vt_normal_basis * normal_ts);
    result.occlusion = clamp(orm.r, 0.0, 1.0);
    result.roughness = clamp(orm.g, 0.0, 1.0);
    result.metallic = clamp(orm.b, 0.0, 1.0);
    result.desired_mip = float(address.desired_mip);
    result.mapped_mip = float(address.mapped_mip);
    return result;
}

const uint RT_SURFACE_VALID = 1u;
const uint RT_SURFACE_FRONT_FACE = 2u;

vec3 rt_load_vec3(RtVertexBuffer geometry, uint word) {
    return vec3(uintBitsToFloat(geometry.words[word]),
                uintBitsToFloat(geometry.words[word + 1u]),
                uintBitsToFloat(geometry.words[word + 2u]));
}

vec4 rt_load_vec4(RtVertexBuffer geometry, uint word) {
    return vec4(uintBitsToFloat(geometry.words[word]),
                uintBitsToFloat(geometry.words[word + 1u]),
                uintBitsToFloat(geometry.words[word + 2u]),
                uintBitsToFloat(geometry.words[word + 3u]));
}

RtRasterVertex rt_load_vertex(RtVertexBuffer geometry, uint vertex,
                              uint stride) {
    uint word = vertex * (stride / 4u);
    RtRasterVertex result;
    result.position = rt_load_vec3(geometry, word);
    result.normal = rt_load_vec3(geometry, word + 3u);
    result.tint = rt_load_vec4(geometry, word + 6u);
    result.surface = rt_load_vec4(geometry, word + 10u);
    result.material_index = geometry.words[word + 14u];
    result.pad0 = result.pad1 = result.pad2 = 0u;
    return result;
}

RtSurface invalid_rt_surface() {
    RtSurface surface;
    surface.position = vec3(1.0, 0.0, 1.0);
    surface.visibility_position = surface.position;
    surface.hit_t = -1.0;
    surface.normal = vec3(0.0, 1.0, 0.0);
    surface.material_index = 0xffffffffu;
    surface.tint = vec4(1.0, 0.0, 1.0, 1.0);
    surface.uv = vec2(0.0);
    surface.baked_ao = 1.0;
    surface.flags = 0u;
    surface.vt_slot = 0u;
    surface.uv_density = 0.0;
    surface.cone_width = 0.0;
    surface.water_binding_slot = 0xffffffffu;
    surface.water_generation = 0u;
    surface.vt_normal_basis = mat3(1.0);
    return surface;
}

#ifdef RT_SURFACE_HIT_SHADER
#ifdef RT_SURFACE_CLOSEST_HIT_SHADER
// Controlled secondary-hit experiment. Only rt_surface.rchit declares id 5;
// the shared any-hit includes retain their original interface and behavior.
// 0 = reference (default), 1 = footprint-adaptive, 2 = material channels only.
layout(constant_id = 5) const uint RT_SURFACE_DETAIL_MODE = 0u;

float rt_surface_relief_weight(float relief_m, float inward,
                              float local_ray_length, float max_ray_m,
                              float footprint_m) {
    if (inward <= 1e-5 || relief_m <= 1e-7 || max_ray_m <= 0.0) return 0.0;
    // Bound the complete local displacement along the ray, including its
    // tangential texture shift at grazing incidence. Keeping the depth
    // component makes this conservative for near-normal sharp reflections.
    // Both displacement and the incoming cone footprint are in local metres;
    // the ray parameter / max_ray_m remain world metres, as in the marcher.
    float displacement_m = min(relief_m / inward, max_ray_m) * local_ray_length;
    float footprint_ratio = displacement_m / max(footprint_m, 1e-4);
    // <= half a footprint: no relief; >= four footprints: exact reference
    // budget and relief. Between them, continuously fade relief and reduce
    // work. Distance alone never promotes a broad diffuse cone to full POM.
    return smoothstep(0.5, 4.0, footprint_ratio);
}
#endif
// Shading-only displacement: hardware intersection/any-hit remain the proxy.
// Keep ray origins at that proxy, while BRDF/texture positions use the relief.
void apply_rt_surface_detail(inout RtSurface surface) {
    if (surface.vt_slot == 0u || surface.material_index >= rt_materials.length()) return;
    RtMaterialGpu material = rt_materials[surface.material_index];
    if ((material.flags_misc.x & RT_SURFACE_DETAIL_MATERIAL_FLAG) == 0u) return;
    int slot = tileset_detail_slot(material.flags_misc);
    if (slot < 0 || slot >= TILESET_MAX_SLOTS) return;
    if (tileset.mean_albedo[slot].w <= 0.0 ||
        TILESET_SLOT_SCALAR(tile_size_m,slot) <= 0.0) return;
    mat3 world_to_local = mat3(gl_WorldToObjectEXT);
    vec3 local_position = gl_ObjectRayOriginEXT + gl_HitTEXT * gl_ObjectRayDirectionEXT;
    vec3 local_normal = normalize(transpose(mat3(gl_ObjectToWorldEXT)) * surface.normal);
    vec3 world_ray = normalize(gl_WorldRayDirectionEXT);
    vec3 local_ray = world_to_local * world_ray;
    // Frobenius norm bounds the largest singular value, including legacy scale.
    float cone_scale = sqrt(dot(world_to_local[0],world_to_local[0]) +
                            dot(world_to_local[1],world_to_local[1]) +
                            dot(world_to_local[2],world_to_local[2]));
    float band = max(tileset.pom_a.w, 1e-4);
    float fade = 1.0-clamp((surface.hit_t-(tileset.pom_a.z-band))/band,0.0,1.0);
    float footprint_m = max(surface.cone_width*cone_scale,1e-4);
    int detail_steps = int(tileset.pom_a.x);
    int detail_refine = int(tileset.pom_a.y);
    float max_ray_m = max(tileset.pom_b.w,0.0);
    float relief_cap_m = max(tileset.pom_b.z,0.0)*fade;
#ifdef RT_SURFACE_CLOSEST_HIT_SHADER
    if (RT_SURFACE_DETAIL_MODE == 2u) {
        // surface_detail_sample tests steps > 0 before its clamp(steps,1,128).
        // Zero skips all height reads; the final material/normal reads remain.
        detail_steps = 0;
    } else if (RT_SURFACE_DETAIL_MODE == 1u && detail_steps > 0) {
        float range_m = max(0.0, TILESET_SLOT_SCALAR(height_max,slot) -
                                 TILESET_SLOT_SCALAR(height_min,slot));
        float weight = rt_surface_relief_weight(min(range_m,relief_cap_m),
            -dot(local_normal,local_ray), length(local_ray), max_ray_m, footprint_m);
        if (weight < 1.0) {
            int full_steps = clamp(detail_steps,1,128);
            detail_steps = weight > 0.0
                ? min(full_steps,max(4,int(ceil(float(full_steps)*weight)))) : 0;
            detail_refine = int(ceil(float(clamp(detail_refine,0,12))*weight));
            // Fade the effective range, not just an oversized author cap.
            relief_cap_m = min(range_m,relief_cap_m)*weight;
        }
    }
#endif
    SurfaceDetailSample detail = surface_detail_sample(
        slot, local_position, local_normal, local_ray,
        footprint_m, detail_steps, detail_refine, max_ray_m, relief_cap_m);
    surface.position = (gl_ObjectToWorldEXT * vec4(detail.position,1)).xyz;
    surface.hit_t += detail.ray_t;
    surface.vt_normal_basis = mat3(detail.albedo, detail.orm,
                                  normalize(transpose(world_to_local)*detail.normal));
    surface.flags |= RT_SURFACE_DETAIL_APPLIED;
}
RtSurface load_rt_surface(vec2 hit_barycentrics) {
    uint part_slot = gl_InstanceCustomIndexEXT;
    GpuRtPartRecord part = rt_parts[part_slot];
    if (part.valid == 0u || all(equal(part.vertex_address, uvec2(0u))) ||
        all(equal(part.index_address, uvec2(0u))) ||
        part.vertex_stride != 88u || gl_PrimitiveID >= part.primitive_count) {
        atomicAdd(invalid_part_records, 1u);
        return invalid_rt_surface();
    }
    RtIndexBuffer index_buffer = RtIndexBuffer(part.index_address);
    uint tri = gl_PrimitiveID * 3u;
    uint i0 = index_buffer.indices[tri];
    uint i1 = index_buffer.indices[tri + 1u];
    uint i2 = index_buffer.indices[tri + 2u];
    if (max(i0, max(i1, i2)) >= part.vertex_count) {
        atomicAdd(invalid_part_records, 1u);
        return invalid_rt_surface();
    }
    RtVertexBuffer geometry = RtVertexBuffer(part.vertex_address);
    RtRasterVertex v0 = rt_load_vertex(geometry, i0, part.vertex_stride);
    RtRasterVertex v1 = rt_load_vertex(geometry, i1, part.vertex_stride);
    RtRasterVertex v2 = rt_load_vertex(geometry, i2, part.vertex_stride);
    vec3 weights = vec3(1.0 - hit_barycentrics.x - hit_barycentrics.y,
                        hit_barycentrics.x, hit_barycentrics.y);
    vec3 object_normal = normalize(v0.normal * weights.x +
                                   v1.normal * weights.y +
                                   v2.normal * weights.z);
    bool front_face = gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT;

    RtSurface surface;
    surface.position = gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT;
    surface.visibility_position = surface.position;
    surface.hit_t = gl_HitTEXT;
    surface.normal = normalize(transpose(mat3(gl_WorldToObjectEXT)) *
                               object_normal);
    if (!front_face) surface.normal = -surface.normal;
    surface.vt_normal_basis = transpose(mat3(gl_WorldToObjectEXT)) *
                              vt_normal_frame(object_normal);
    if (!front_face) surface.vt_normal_basis = -surface.vt_normal_basis;
    surface.material_index = v0.material_index;
    surface.tint = v0.tint * weights.x + v1.tint * weights.y +
                   v2.tint * weights.z;
    surface.uv = v0.surface.xy * weights.x + v1.surface.xy * weights.y +
                 v2.surface.xy * weights.z;
    surface.baked_ao = v0.surface.z * weights.x +
                       v1.surface.z * weights.y + v2.surface.z * weights.z;
    surface.flags = RT_SURFACE_VALID |
                    (front_face ? RT_SURFACE_FRONT_FACE : 0u);

    // WP-G: VT addressing state. surface.uv above is already the chart-atlas
    // UV (the same surface.xy the raster path forwards). The stride guard
    // above is 88 since VT Phase 2 appended the warp block to VkRasterVertex;
    // the word offsets this loader reads (position 0, normal 3, tint 6,
    // surface 10, material 14) are unchanged, and RT deliberately ignores
    // the warp words — RT has no march (spec §8), so it has no use for the
    // march's coordinate.
    surface.vt_slot = part.vt_slot;
    surface.water_binding_slot = part.water_binding_slot;
    surface.water_generation = part.water_generation;
    surface.uv_density = 0.0;
    surface.cone_width = 0.0;
    if (part.vt_slot != 0u) {
        // Isotropic atlas-UV-per-metre across this triangle: sqrt of the
        // ratio of its UV area to its WORLD area (Ray Tracing Gems ch. 20's
        // ray-cone LOD estimator). Object-space edges are pushed through the
        // instance transform so non-uniform instance scale is accounted for.
        vec2 duv1 = v1.surface.xy - v0.surface.xy;
        vec2 duv2 = v2.surface.xy - v0.surface.xy;
        float uv_area = abs(duv1.x * duv2.y - duv1.y * duv2.x);
        mat3 object_to_world = mat3(gl_ObjectToWorldEXT);
        vec3 e1 = object_to_world * (v1.position - v0.position);
        vec3 e2 = object_to_world * (v2.position - v0.position);
        float world_area = length(cross(e1, e2));
        // A degenerate triangle (or one with collapsed UVs) leaves density 0,
        // which pins the sample to mip 0 -- the safe direction: it can only
        // over-request sharpness, never sample outside the atlas.
        surface.uv_density =
            world_area > 1e-12 ? sqrt(uv_area / world_area) : 0.0;
    }
    return surface;
}
#endif
