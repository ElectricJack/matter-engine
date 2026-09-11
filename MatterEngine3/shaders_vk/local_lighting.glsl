// Shared local-light record, sparse world-grid lookup, attenuation and BRDF.
//
// Descriptor set 2 is deliberately independent of the G-buffer and RT scene
// sets. composite.frag consumes it now; the RT local-light stage binds the same
// ABI for arbitrary secondary-hit positions. Keep layouts byte-identical to
// world_lights.h and LocalLightGpuMeta in vk_scene_renderer.h.

const uint LOCAL_LIGHT_POINT = 0u;
const uint LOCAL_LIGHT_SPOT = 1u;
const uint LOCAL_DIRECT_RASTER = 0u;
const uint LOCAL_DIRECT_RAY_TRACED = 1u;
const float LOCAL_LIGHT_PI = 3.14159265358979323846;

struct LocalLightGpu {
    vec4 position_range;
    vec4 direction_cos_outer;
    vec4 color_source_radius;
    float cos_inner;
    uint kind;
    uint flags;
    uint reserved;
};

struct LocalLightCellGpu {
    ivec3 cell;
    uint offset;
    uint count;
    // Scalars are intentional: std430 gives a uvec3 16-byte base alignment,
    // which would move this lane to byte 32 and make the array stride 48.
    // The CPU LocalLightCell ABI is exactly 32 bytes with these fields at
    // offsets 20/24/28.
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

layout(set = 2, binding = 0, std430) readonly buffer LocalLightRecords {
    LocalLightGpu local_lights[];
};
layout(set = 2, binding = 1, std430) readonly buffer LocalLightCells {
    LocalLightCellGpu local_light_cells[];
};
layout(set = 2, binding = 2, std430) readonly buffer LocalLightIndices {
    uint local_light_indices[];
};
layout(set = 2, binding = 3, std430) readonly buffer LocalLightOversizedIndices {
    uint local_light_oversized_indices[];
};
layout(set = 2, binding = 4, std430) readonly buffer LocalLightMetadata {
    uvec4 local_light_counts;  // records, buckets, oversized, direct owner
    vec2 local_light_grid;     // cell size, inverse cell size
    uvec2 local_light_debug;   // max candidates per cell, reserved
};

uint local_light_cell_hash(ivec3 cell) {
    uint hash = uint(cell.x) * 0x8da6b343u;
    hash ^= uint(cell.y) * 0xd8163841u;
    hash ^= uint(cell.z) * 0xcb1ab31fu;
    hash ^= hash >> 16u;
    hash *= 0x7feb352du;
    hash ^= hash >> 15u;
    hash *= 0x846ca68bu;
    return hash ^ (hash >> 16u);
}

bool local_light_cell_span(vec3 world_position, out uint offset,
                           out uint count) {
    offset = 0u;
    count = 0u;
    uint bucket_count = local_light_counts.y;
    if (bucket_count == 0u || local_light_grid.y <= 0.0)
        return false;

    ivec3 key = ivec3(floor(world_position * local_light_grid.y));
    uint mask = bucket_count - 1u;
    uint bucket = local_light_cell_hash(key) & mask;
    for (uint probe = 0u; probe < bucket_count; ++probe) {
        LocalLightCellGpu cell = local_light_cells[bucket];
        if (cell.count == 0u)
            return false;
        if (all(equal(cell.cell, key))) {
            offset = cell.offset;
            count = cell.count;
            return true;
        }
        bucket = (bucket + 1u) & mask;
    }
    return false;
}

uint local_light_candidate_count(vec3 world_position) {
    uint offset = 0u;
    uint count = 0u;
    local_light_cell_span(world_position, offset, count);
    return count + local_light_counts.z;
}

float local_light_attenuation_gpu(LocalLightGpu light,
                                  vec3 receiver_position) {
    vec3 delta = receiver_position - light.position_range.xyz;
    float range = light.position_range.w;
    if (!(range > 0.0))
        return 0.0;

    // Match the CPU reference's range-normalized form so large finite ranges
    // do not overflow range*range and incorrectly extinguish nearby receivers.
    vec3 normalized_delta = delta / range;
    float normalized_distance2 = dot(normalized_delta, normalized_delta);
    if (isnan(normalized_distance2) || isinf(normalized_distance2) ||
        normalized_distance2 >= 1.0)
        return 0.0;

    float cone = 1.0;
    if (light.kind == LOCAL_LIGHT_SPOT && any(notEqual(delta, vec3(0.0)))) {
        float direction_scale = max(abs(delta.x), max(abs(delta.y), abs(delta.z)));
        vec3 unit_scaled = delta / direction_scale;
        float cos_theta = dot(light.direction_cos_outer.xyz,
                              unit_scaled * inversesqrt(dot(unit_scaled,
                                                            unit_scaled)));
        float width = light.cos_inner - light.direction_cos_outer.w;
        if (width <= 1.0e-7)
            cone = cos_theta >= light.cos_inner ? 1.0 : 0.0;
        else {
            float t = clamp((cos_theta - light.direction_cos_outer.w) /
                            width, 0.0, 1.0);
            cone = t * t * (3.0 - 2.0 * t);
        }
    }

    float cutoff = 1.0 - normalized_distance2;
    float radius = max(light.color_source_radius.w, 1.0e-4);
    float distance_scale = max(radius,
        max(abs(delta.x), max(abs(delta.y), abs(delta.z))));
    vec3 distance_scaled = delta / distance_scale;
    float radius_scaled = radius / distance_scale;
    float denominator = dot(distance_scaled, distance_scaled) +
                        radius_scaled * radius_scaled;
    float inverse_distance = (1.0 / distance_scale) /
                             distance_scale / denominator;
    return cone * cutoff * cutoff * inverse_distance;
}

struct LocalLightBrdf {
    vec3 diffuse;
    vec3 specular;
};

LocalLightBrdf evaluate_local_light_brdf(uint light_index,
                                         vec3 receiver_position,
                                         vec3 normal,
                                         vec3 view_direction,
                                         vec3 albedo,
                                         float roughness,
                                         float metallic) {
    LocalLightBrdf result;
    result.diffuse = vec3(0.0);
    result.specular = vec3(0.0);
    if (light_index >= local_light_counts.x)
        return result;

    LocalLightGpu light = local_lights[light_index];
    vec3 to_light = light.position_range.xyz - receiver_position;
    float distance2 = dot(to_light, to_light);
    if (distance2 <= 1.0e-20)
        return result;
    vec3 light_direction = to_light * inversesqrt(distance2);
    float n_dot_l = max(dot(normal, light_direction), 0.0);
    float n_dot_v = max(dot(normal, view_direction), 0.0);
    if (n_dot_l <= 0.0 || n_dot_v <= 0.0)
        return result;

    float attenuation = local_light_attenuation_gpu(light, receiver_position);
    if (attenuation <= 0.0)
        return result;
    vec3 irradiance = light.color_source_radius.rgb * attenuation;

    vec3 half_vector = view_direction + light_direction;
    float half_length2 = dot(half_vector, half_vector);
    if (half_length2 <= 1.0e-20)
        return result;
    half_vector *= inversesqrt(half_length2);
    float n_dot_h = max(dot(normal, half_vector), 0.0);
    float v_dot_h = max(dot(view_direction, half_vector), 0.0);

    vec3 f0 = mix(vec3(0.04), albedo, clamp(metallic, 0.0, 1.0));
    vec3 fresnel = f0 + (vec3(1.0) - f0) * pow(1.0 - v_dot_h, 5.0);
    float alpha = max(roughness * roughness, 0.0004);
    float alpha2 = alpha * alpha;
    float d_denom = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2 /
        max(LOCAL_LIGHT_PI * d_denom * d_denom, 1.0e-8);
    float g_view = 2.0 * n_dot_v /
        max(n_dot_v + sqrt(alpha2 + (1.0 - alpha2) * n_dot_v * n_dot_v),
            1.0e-6);
    float g_light = 2.0 * n_dot_l /
        max(n_dot_l + sqrt(alpha2 + (1.0 - alpha2) * n_dot_l * n_dot_l),
            1.0e-6);
    vec3 specular_brdf = fresnel * distribution * g_view * g_light /
                         max(4.0 * n_dot_v * n_dot_l, 1.0e-6);
    vec3 diffuse_brdf = (vec3(1.0) - fresnel) *
                        (1.0 - clamp(metallic, 0.0, 1.0)) * albedo /
                        LOCAL_LIGHT_PI;
    result.diffuse = diffuse_brdf * irradiance * n_dot_l;
    result.specular = specular_brdf * irradiance * n_dot_l;
    return result;
}
