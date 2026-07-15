#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require

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

struct GpuRtPartRecord {
    uvec2 vertex_address;
    uint vertex_stride;
    uint vertex_count;
    uint primitive_count;
    uint valid;
    uint pad0;
    uint pad1;
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
};

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
    uint first_vertex = gl_PrimitiveID * 3u;
    if (part.valid == 0u || all(equal(part.vertex_address, uvec2(0u))) ||
        part.vertex_stride != 72u || gl_PrimitiveID >= part.primitive_count ||
        first_vertex + 2u >= part.vertex_count) {
        atomicAdd(invalid_part_records, 1u);
        return invalid_rt_surface();
    }

    RtVertexBuffer geometry = RtVertexBuffer(part.vertex_address);
    RtRasterVertex v0 = rt_load_vertex(geometry, first_vertex,
                                       part.vertex_stride);
    RtRasterVertex v1 = rt_load_vertex(geometry, first_vertex + 1u,
                                       part.vertex_stride);
    RtRasterVertex v2 = rt_load_vertex(geometry, first_vertex + 2u,
                                       part.vertex_stride);
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
