#ifndef MATTER_VT_SURFACE_WALK_GLSL
#define MATTER_VT_SURFACE_WALK_GLSL
// Read-only reconstruction of the immutable geometry retained by a VT page.
// Only used when the inexpensive chart-local POM path encounters a boundary.
#include "vt_chart_types.glsl"
#include "vt_normal_frame.glsl"

layout(buffer_reference, std430, buffer_reference_align = 16)
readonly buffer VtWalkCharts { GpuChart values[]; };
layout(buffer_reference, std430, buffer_reference_align = 16)
readonly buffer VtWalkTriangles { GpuTriGeometry values[]; };

struct VtWalkContext {
    VtPageMetadata metadata;
    uint slot;
    vec2 atlas_size;
};
struct VtWalkPoint {
    uint chart;
    uint triangle;
    vec3 bary;
    vec3 position;
    vec3 normal;
    vec2 uv;
    float recess; // signed distance inside the local triangle's proxy plane
};

bool vt_walk_available(VtWalkContext ctx) {
    return any(notEqual(ctx.metadata.charts, uvec2(0))) &&
           any(notEqual(ctx.metadata.triangles, uvec2(0))) &&
           ctx.metadata.geometry_counts.x > 0u && ctx.metadata.geometry_counts.x <= 65536u &&
           ctx.metadata.geometry_counts.y > 0u;
}

bool vt_walk_projection(GpuTriGeometry tri, vec3 p, out vec3 bary, out vec3 normal) {
    vec3 ab = tri.p1.xyz - tri.p0.xyz, ac = tri.p2.xyz - tri.p0.xyz;
    vec3 n = cross(ab, ac), q = p - tri.p0.xyz;
    float n2 = dot(n, n);
    if (!(n2 > 1e-20)) return false;
    float v = dot(cross(q, ac), n) / n2;
    float w = dot(cross(ab, q), n) / n2;
    bary = vec3(1.0 - v - w, v, w);
    normal = n * inversesqrt(n2);
    if (dot(normal, tri.n0.xyz + tri.n1.xyz + tri.n2.xyz) < 0.0) normal = -normal;
    return !any(isnan(bary)) && !any(isinf(bary));
}

bool vt_walk_chart_contains(GpuChart chart, uint triangle, uint count) {
    return chart.tri_range.x <= triangle && chart.tri_range.x <= count &&
           chart.tri_range.y <= count - chart.tri_range.x &&
           triangle - chart.tri_range.x < chart.tri_range.y;
}

bool vt_walk_chart(VtWalkContext ctx, uint triangle, inout uint chart) {
    if (chart < ctx.metadata.geometry_counts.x && vt_walk_chart_contains(
            VtWalkCharts(ctx.metadata.charts).values[chart], triangle,
            ctx.metadata.geometry_counts.y)) return true;
    // Emission is chart-grouped, including zero-sized ranges. Upper bound of
    // first-triangle selects the last range starting at/before this triangle.
    uint lo = 0u, hi = ctx.metadata.geometry_counts.x;
    for (int i = 0; i < 17 && lo < hi; ++i) {
        uint mid = lo + (hi - lo) / 2u;
        if (VtWalkCharts(ctx.metadata.charts).values[mid].tri_range.x <= triangle) lo = mid + 1u;
        else hi = mid;
    }
    if (lo == 0u || lo != hi) return false;
    chart = lo - 1u;
    return vt_walk_chart_contains(VtWalkCharts(ctx.metadata.charts).values[chart],
                                 triangle, ctx.metadata.geometry_counts.y);
}

bool vt_walk_locate(VtWalkContext ctx, vec3 p, inout VtWalkPoint point) {
    uint previous = 0xffffffffu;
    for (int hop = 0; hop < 32; ++hop) {
        if (point.triangle >= ctx.metadata.geometry_counts.y) return false;
        GpuTriGeometry tri = VtWalkTriangles(ctx.metadata.triangles).values[point.triangle];
        vec3 bary, face_normal;
        if (!vt_walk_projection(tri, p, bary, face_normal)) return false;
        // Adjacent offset faces meet at the edge's normal bisector. Testing
        // ordinary projected barycentrics would leave a gap between recessed
        // faces at a bend (and make the walk bounce between both triangles).
        // These three extruded edge planes partition that gap continuously.
        vec3 vertices[3] = vec3[3](tri.p0.xyz, tri.p1.xyz, tri.p2.xyz);
        int crossed_edge = -1;
        float furthest = 2e-6;
        for (int edge = 0; edge < 3; ++edge) {
            vec3 normal_sum = face_normal;
            uint neighbor = tri.mat[edge + 1];
            if (neighbor > ctx.metadata.geometry_counts.y) return false;
            if (neighbor != 0u) {
                GpuTriGeometry other = VtWalkTriangles(ctx.metadata.triangles).values[neighbor - 1u];
                vec3 unused, other_normal;
                if (!vt_walk_projection(other, p, unused, other_normal) ||
                    dot(face_normal, other_normal) <= -0.999) return false;
                normal_sum += other_normal;
            }
            vec3 side = cross(vertices[(edge + 1) % 3] - vertices[edge], normal_sum);
            if (dot(side, side) < 1e-20) return false;
            side = normalize(side);
            if (dot(side, vertices[(edge + 2) % 3] - vertices[edge]) > 0.0) side = -side;
            float outside = dot(side, p - vertices[edge]);
            if (outside > furthest) { furthest = outside; crossed_edge = edge; }
        }
        if (crossed_edge < 0) {
            if (!vt_walk_chart(ctx, point.triangle, point.chart)) return false;
            GpuChart chart = VtWalkCharts(ctx.metadata.charts).values[point.chart];
            if (!(chart.origin_tpm.w > 0.0)) return false;
            point.bary = max(bary, vec3(0));
            point.bary /= dot(point.bary, vec3(1));
            point.position = tri.p0.xyz * point.bary.x + tri.p1.xyz * point.bary.y + tri.p2.xyz * point.bary.z;
            vec3 normal = tri.n0.xyz * point.bary.x + tri.n1.xyz * point.bary.y + tri.n2.xyz * point.bary.z;
            point.normal = dot(normal, normal) > 1e-20 ? normalize(normal) : face_normal;
            point.recess = dot(tri.p0.xyz - p, face_normal);
            vec2 plane = vec2(dot(point.position, chart.tangent_ou.xyz),
                              dot(point.position, chart.bitangent_ov.xyz));
            point.uv = (vec2(chart.rect.xy) + float(VT_CHART_GUTTER) + chart.origin_tpm.w *
                (plane - vec2(chart.tangent_ou.w, chart.bitangent_ov.w))) / ctx.atlas_size;
            return !any(lessThan(point.uv, vec2(0))) && !any(greaterThanEqual(point.uv, vec2(1)));
        }
        uint neighbor = tri.mat[1 + crossed_edge];
        if (neighbor == 0u || neighbor > ctx.metadata.geometry_counts.y || neighbor - 1u == previous)
            return false;
        previous = point.triangle;
        point.triangle = neighbor - 1u;
    }
    return false;
}

bool vt_walk_seed(VtWalkContext ctx, uint chart_id, vec3 proxy, float texel_m,
                  out VtWalkPoint point) {
    if (!vt_walk_available(ctx) || chart_id >= ctx.metadata.geometry_counts.x) return false;
    GpuChart chart = VtWalkCharts(ctx.metadata.charts).values[chart_id];
    if (chart.tri_range.x > ctx.metadata.geometry_counts.y ||
        chart.tri_range.y > ctx.metadata.geometry_counts.y - chart.tri_range.x) return false;
    // This bounded search runs only for boundary rays. A spatial seed index is
    // a later optimization; failure preserves the already valid proxy sample.
    for (uint i = 0u; i < min(chart.tri_range.y, 2048u); ++i) {
        uint index = chart.tri_range.x + i;
        GpuTriGeometry tri = VtWalkTriangles(ctx.metadata.triangles).values[index];
        vec3 bary, face_normal;
        if (!vt_walk_projection(tri, proxy, bary, face_normal) ||
            any(lessThan(bary, vec3(-2e-6))) ||
            abs(dot(proxy - tri.p0.xyz, face_normal)) > max(1e-5, texel_m * 0.01)) continue;
        point.chart = chart_id;
        point.triangle = index;
        return vt_walk_locate(ctx, proxy, point);
    }
    return false;
}

bool vt_walk_same_page_inputs(VtWalkContext ctx, VtAddress address) {
    if (!address.valid || address.height_version != 1u) return false;
    VtPageMetadata other = vt_page_inputs[address.physical_slot];
    return all(equal(other.inputs, ctx.metadata.inputs)) &&
           all(equal(other.charts, ctx.metadata.charts)) &&
           all(equal(other.triangles, ctx.metadata.triangles)) &&
           // Per-page storage flags can differ across one continuous face.
           all(equal(other.geometry_counts.xy, ctx.metadata.geometry_counts.xy)) &&
           all(equal(other.material.yzw,ctx.metadata.material.yzw));
}

// Fold an extended filter tap across each connected edge. Orthogonal projection
// onto the next face collapses the entire footprint at a 90-degree corner;
// rotation about the common edge preserves its physical distance and scale.
bool vt_walk_filter_point(VtWalkContext ctx, vec3 p, inout VtWalkPoint point) {
    uint previous = 0xffffffffu;
    for (int hop = 0; hop < 32; ++hop) {
        if (point.triangle >= ctx.metadata.geometry_counts.y) return false;
        GpuTriGeometry tri = VtWalkTriangles(ctx.metadata.triangles).values[point.triangle];
        vec3 bary, normal;
        if (!vt_walk_projection(tri, p, bary, normal)) return false;
        if (all(greaterThanEqual(bary, vec3(-2e-6)))) return vt_walk_locate(ctx, p, point);
        int opposite = bary.x < bary.y ? 0 : 1;
        if (bary.z < bary[opposite]) opposite = 2;
        int edge = (opposite + 1) % 3;
        uint neighbor = tri.mat[edge + 1];
        if (neighbor == 0u || neighbor > ctx.metadata.geometry_counts.y || neighbor - 1u == previous)
            return false;
        GpuTriGeometry other = VtWalkTriangles(ctx.metadata.triangles).values[neighbor - 1u];
        vec3 unused, next_normal;
        if (!vt_walk_projection(other, p, unused, next_normal)) return false;
        float cosine = clamp(dot(normal, next_normal), -1.0, 1.0);
        if (cosine <= -0.999) return false;
        vec3 vertices[3] = vec3[3](tri.p0.xyz, tri.p1.xyz, tri.p2.xyz);
        vec3 axis = normalize(vertices[(edge + 1) % 3] - vertices[edge]);
        float sine = dot(axis, cross(normal, next_normal));
        vec3 q = p - vertices[edge];
        p = vertices[edge] + q * cosine + cross(axis, q) * sine +
            axis * dot(axis, q) * (1.0 - cosine);
        previous = point.triangle;
        point.triangle = neighbor - 1u;
    }
    return false;
}

// Extend one chart texel center into the current triangle's plane, then unfold
// the footprint through real surface connectivity. Atlas adjacency proves nothing.
bool vt_walk_texel_point(VtWalkContext ctx, VtWalkPoint center, vec2 uv,
                         out VtWalkPoint point) {
    GpuChart chart = VtWalkCharts(ctx.metadata.charts).values[center.chart];
    GpuTriGeometry tri = VtWalkTriangles(ctx.metadata.triangles).values[center.triangle];
    vec2 q = (uv * ctx.atlas_size - vec2(chart.rect.xy) - float(VT_CHART_GUTTER)) /
             chart.origin_tpm.w + vec2(chart.tangent_ou.w, chart.bitangent_ov.w);
    vec2 a = vec2(tri.p0.w, tri.n0.w), b = vec2(tri.p1.w, tri.n1.w), c = vec2(tri.p2.w, tri.n2.w);
    vec2 ab = b - a, ac = c - a, aq = q - a;
    float det = ab.x * ac.y - ab.y * ac.x;
    if (abs(det) < 1e-16) return false;
    float v = (aq.x * ac.y - aq.y * ac.x) / det;
    float w = (ab.x * aq.y - ab.y * aq.x) / det;
    point = center;
    return vt_walk_filter_point(ctx, tri.p0.xyz * (1.0 - v - w) + tri.p1.xyz * v + tri.p2.xyz * w, point);
}

struct VtWalkValue {
    float height;
    vec3 albedo;
    vec3 orm;
    vec3 normal;
};

bool vt_walk_sample(VtWalkContext ctx, VtWalkPoint center, float lod, bool material,
                    out VtWalkValue value, out VtAddress address, out uint status) {
    value.height = 0.0; value.albedo = value.orm = value.normal = vec3(0);
    status = VT_POM_PATH_BOUNDARY;
    address = vt_resolve_chart(ctx.slot, center.uv, lod, center.chart);
    if (!vt_walk_same_page_inputs(ctx, address)) { status = VT_POM_SNAPSHOT_MISMATCH; return false; }
    // The walk proves the finite surface point. An independently filtered
    // periodic material does not inherit the coarser receiver's texel grid.
    if (address.module_request.x!=0u || vt_height_footprint_valid(address, center.chart, true)) {
        value.height = vt_sample_height_m(address);
        if (material) {
            value.albedo = vt_sample_material(address, VT_CHANNEL_ALBEDO).rgb;
            value.orm = vt_sample_orm(address).rgb;
            value.normal = vt_frame_decode(vt_decode_normal(vt_sample_normal(address)), center.normal);
        }
        return true;
    }
    VtVariantRecord record = vt_variants[ctx.slot - 1u];
    vec2 mapped_size = vec2(max(record.atlas_w >> address.mapped_mip, 1u),
                            max(record.atlas_h >> address.mapped_mip, 1u));
    vec2 grid = center.uv * mapped_size - 0.5;
    vec2 first = floor(grid), fraction = fract(grid);
    ivec2 pool_size = textureSize(vt_pool[VT_CHANNEL_AUX], 0).xy;
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
        vec2 tap_uv = (first + vec2(x,y) + 0.5) / mapped_size;
        VtWalkPoint tap;
        if (!vt_walk_texel_point(ctx, center, tap_uv, tap)) return false;
        VtAddress sample_address = vt_resolve_chart(ctx.slot, tap.uv, lod, tap.chart);
        if (!vt_walk_same_page_inputs(ctx, sample_address)) { status = VT_POM_SNAPSHOT_MISMATCH; return false; }
        ivec2 texel = ivec2(floor(sample_address.physical_uv * vec2(pool_size)));
        if (any(lessThan(texel, ivec2(0))) || any(greaterThanEqual(texel, pool_size))) return false;
        ivec3 location = ivec3(texel, int(sample_address.physical_layer));
        uvec4 aux = uvec4(texelFetch(vt_pool[VT_CHANNEL_AUX], location, 0) * 255.0 + 0.5);
        // The geometry walk above proves that the requested tap lies on this
        // connected face. At a diagonal chart edge its nearest texel CENTER
        // can still be outside coverage; same-chart dilation then represents
        // the nearest valid edge sample. Accept it only after that geometric
        // proof, never to authorize travel over an actual boundary or gap.
        if ((aux.a != 2u && aux.a != 3u && aux.a != 4u) || vt_aux_chart(aux) != tap.chart) return false;
        // Coverage is receiver-specific even when these material pixels are
        // shared. Preserve the same within-page texel in the material pool.
        location = ivec3(ivec2(floor(sample_address.material_uv * vec2(pool_size))),
                         int(sample_address.material_layer));
        float weight = (x == 0 ? 1.0 - fraction.x : fraction.x) *
                       (y == 0 ? 1.0 - fraction.y : fraction.y);
        bool mapped=sample_address.module_request.x!=0u;
        value.height += weight * (mapped ? vt_sample_height_m(sample_address) :
            sample_address.height_decode.x + sample_address.height_decode.y *
            texelFetch(vt_pool[VT_CHANNEL_HEIGHT], location, 0).r);
        if (material) {
            value.albedo += weight * (mapped ? vt_sample_material(sample_address,VT_CHANNEL_ALBEDO) :
                texelFetch(vt_pool[VT_CHANNEL_ALBEDO], location, 0)).rgb;
            vec3 orm=mapped ? vt_sample_orm(sample_address).rgb :
                texelFetch(vt_pool[VT_CHANNEL_ORM], location, 0).rgb;
            if(!mapped)orm.r*=vt_sample_occlusion(sample_address);
            value.orm += weight * orm;
            value.normal += weight * vt_frame_decode(
                vt_decode_normal(mapped ? vt_sample_normal(sample_address) :
                    texelFetch(vt_pool[VT_CHANNEL_NORMAL], location, 0)), tap.normal);
        }
    }
    if (material) value.normal = normalize(value.normal);
    return true;
}
#endif
