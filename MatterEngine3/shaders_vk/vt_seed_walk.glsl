#ifndef MATTER_VT_SEED_WALK_GLSL
#define MATTER_VT_SEED_WALK_GLSL
// Included after vt_walk_locate. The immutable geometry allocation appends
// these 48-byte nodes after its original 112-byte triangle records.
struct VtSeedNode { vec4 lower, upper; uvec4 range; };
layout(buffer_reference, std430, buffer_reference_align = 16)
readonly buffer VtSeedNodes { VtSeedNode values[]; };
#ifndef VT_SEED_TRIANGLE_VISIT
#define VT_SEED_TRIANGLE_VISIT()
#endif
#ifndef VT_SEED_NODE_VISIT
#define VT_SEED_NODE_VISIT()
#endif
#ifndef VT_SEED_FORCE_LINEAR
#define VT_SEED_FORCE_LINEAR false
#endif

bool vt_seed_contains(uvec2 triangles, uint index, vec3 proxy, float texel_m) {
    VT_SEED_TRIANGLE_VISIT();
    GpuTriGeometry tri = VtWalkTriangles(triangles).values[index];
    vec3 bary, face_normal;
    if (!vt_walk_projection(tri, proxy, bary, face_normal) ||
        any(lessThan(bary, vec3(-2e-6))) ||
        abs(dot(proxy - tri.p0.xyz, face_normal)) > max(1e-5, texel_m * 0.01)) return false;
    return true;
}

bool vt_seed_search(inout VtWalkContext ctx, uint chart_id, GpuChart chart,
                    vec3 proxy, float texel_m, out VtWalkPoint point) {
    VtPageMetadata metadata=vt_walk_metadata(ctx);
    uint count = min(chart.tri_range.y, 2048u);
    uint node_count = metadata.geometry_counts.w;
    bool accelerated = !VT_SEED_FORCE_LINEAR && chart.tri_range.w > 0u &&
        chart.tri_range.w <= 511u && chart.tri_range.z <= node_count &&
        chart.tri_range.w <= node_count - chart.tri_range.z;
    if (accelerated) {
        // Full-width byte offset without requiring shaderInt64 arithmetic.
        uint high, low;
        umulExtended(metadata.geometry_counts.y, 112u, high, low);
        uvec2 pointer = metadata.triangles;
        uint address_low = pointer.x + low;
        pointer.y += high + uint(address_low < pointer.x);
        pointer.x = address_low;
        VtSeedNodes nodes = VtSeedNodes(pointer);
        uint cursor = chart.tri_range.z, end = cursor + chart.tri_range.w;
        float tolerance = max(1e-5, texel_m * 0.01);
        while (cursor < end) {
            VT_SEED_NODE_VISIT();
            VtSeedNode node = nodes.values[cursor];
            if (node.range.z <= cursor || node.range.z > end) return false;
            if (any(lessThan(proxy, node.lower.xyz - tolerance)) ||
                any(greaterThan(proxy, node.upper.xyz + tolerance))) {
                cursor = node.range.z;
                continue;
            }
            if (node.range.y == 0u) { ++cursor; continue; }
            if (node.range.y > 8u || node.range.x < chart.tri_range.x ||
                node.range.x - chart.tri_range.x > count ||
                node.range.y > count - (node.range.x - chart.tri_range.x)) return false;
            for (uint i = 0u; i < node.range.y; ++i) {
                uint index = node.range.x + i;
                if (!vt_seed_contains(metadata.triangles, index, proxy, texel_m)) continue;
                point.chart = chart_id; point.triangle = index;
                return vt_walk_locate(ctx, proxy, point);
            }
            cursor = node.range.z;
        }
        return false;
    }
    // Small charts and legacy producers retain the original search exactly.
    for (uint i = 0u; i < count; ++i) {
        uint index = chart.tri_range.x + i;
        if (!vt_seed_contains(metadata.triangles, index, proxy, texel_m)) continue;
        point.chart = chart_id; point.triangle = index;
        return vt_walk_locate(ctx, proxy, point);
    }
    return false;
}
#endif
