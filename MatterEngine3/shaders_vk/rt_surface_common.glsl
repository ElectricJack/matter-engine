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

struct RtSurface {
    vec3 position;
    float hit_t;
    vec3 normal;
    uint material_index;
    vec4 tint;
    vec2 uv;
    float baked_ao;
    uint flags;
};

struct RtSurfacePayload {
    RtSurface surface;
    uint part_slot;
    uint primitive;
    uint pad0;
    uint pad1;
};

struct GpuRtPartRecord {
    uvec2 vertex_address;
    uvec2 index_address;
    uint vertex_stride;
    uint vertex_count;
    uint primitive_count;
    uint valid;
    uint pad0; uint pad1; uint pad2; uint pad3;
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
// Gradient proxy: no ray-cone/footprint term exists in RtSurfacePayload or
// RtSurface today (checked before adding one), so the mip/AA footprint is
// approximated as hit_distance * a fixed cone-spread constant — a constant
// angle stand-in for the true pixel-footprint cone (sufficient for diffuse
// GI/reflection ground LOD, which never needs to be pixel-sharp).
const float RT_TILESET_CONE_SPREAD = 0.01;

struct RtTilesetSample {
    bool applied;
    vec3 albedo;     // valid only if applied
    vec3 normal;     // shading normal; = surface.normal when !applied
    float roughness; // valid only if applied; no live consumer yet (see Task 9 notes)
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
    result.mean_occlusion = 0.0;
    int slot = tileset_detail_slot(material.flags_misc);
    if (slot < 0) return result;
    float footprint = max(max(surface.hit_t, 0.0) * RT_TILESET_CONE_SPREAD, 1e-4);
    vec2 dWdx = vec2(footprint, 0.0);
    vec2 dWdy = vec2(0.0, footprint);
    vec3 normal_ts, orm;
    vec3 albedo = tileset_sample_ground(slot, surface.position.xz, dWdx, dWdy,
                                        normal_ts, orm);
    float tint_blend = clamp(surface.tint.a, 0.0, 1.0);
    result.applied = true;
    result.albedo = albedo * mix(vec3(1.0), surface.tint.rgb, tint_blend);
    result.normal = tileset_rotate_normal(normal_ts, surface.normal);
    result.roughness = clamp(orm.g, 0.0, 1.0);
    result.mean_occlusion = tileset_horizon_mean_occlusion(
        slot, surface.position.xz, dWdx, dWdy);
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
    surface.hit_t = -1.0;
    surface.normal = vec3(0.0, 1.0, 0.0);
    surface.material_index = 0xffffffffu;
    surface.tint = vec4(1.0, 0.0, 1.0, 1.0);
    surface.uv = vec2(0.0);
    surface.baked_ao = 1.0;
    surface.flags = 0u;
    return surface;
}

#ifdef RT_SURFACE_HIT_SHADER
RtSurface load_rt_surface(vec2 hit_barycentrics) {
    uint part_slot = gl_InstanceCustomIndexEXT;
    GpuRtPartRecord part = rt_parts[part_slot];
    if (part.valid == 0u || all(equal(part.vertex_address, uvec2(0u))) ||
        all(equal(part.index_address, uvec2(0u))) ||
        part.vertex_stride != 72u || gl_PrimitiveID >= part.primitive_count) {
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
    surface.hit_t = gl_HitTEXT;
    surface.normal = normalize(transpose(mat3(gl_WorldToObjectEXT)) *
                               object_normal);
    if (!front_face) surface.normal = -surface.normal;
    surface.material_index = v0.material_index;
    surface.tint = v0.tint * weights.x + v1.tint * weights.y +
                   v2.tint * weights.z;
    surface.uv = v0.surface.xy * weights.x + v1.surface.xy * weights.y +
                 v2.surface.xy * weights.z;
    surface.baked_ao = v0.surface.z * weights.x +
                       v1.surface.z * weights.y + v2.surface.z * weights.z;
    surface.flags = RT_SURFACE_VALID |
                    (front_face ? RT_SURFACE_FRONT_FACE : 0u);
    return surface;
}
#endif
