#ifndef MATTER_VT_PARALLAX_GLSL
#define MATTER_VT_PARALLAX_GLSL
// Include vt_common.glsl first. All distances are local metres except ray_t,
// which is world metres. The proxy is the source's declared outer shell.

// Test-only work counters; production builds compile these to no operations.
#ifndef VT_POM_FOOTPRINT_VISIT
#define VT_POM_FOOTPRINT_VISIT()
#endif
#ifndef VT_POM_CONNECTED_VISIT
#define VT_POM_CONNECTED_VISIT()
#endif

// Two independent geometric/UV differences define the affine chart metric.
// Raster supplies quad derivatives; RT supplies triangle edges. Instance scale
// is retained in ray_local (local metres per world metre), never normalized.
bool vt_parallax_metric(vec3 dp0, vec3 dp1, vec2 duv0, vec2 duv1,
                        vec3 ray_local, vec2 atlas_size,
                        out vec2 uv_ray, out float texel_m) {
    uv_ray = vec2(0); texel_m = 0.0;
    float det = duv0.x * duv1.y - duv0.y * duv1.x;
    if (abs(det) < 1e-18) return false;
    vec3 pu = (dp0 * duv1.y - dp1 * duv0.y) / det;
    vec3 pv = (dp1 * duv0.x - dp0 * duv1.x) / det;
    vec3 n = cross(pu, pv);
    float n2 = dot(n, n);
    if (n2 < 1e-20) return false;
    uv_ray = vec2(dot(cross(pv, n), ray_local), dot(cross(n, pu), ray_local)) / n2;
    texel_m = max(length(pu) / atlas_size.x, length(pv) / atlas_size.y);
    return !any(isnan(uv_ray)) && !any(isinf(uv_ray)) &&
           !isnan(texel_m) && !isinf(texel_m) && texel_m > 0.0;
}

uint vt_aux_chart(uvec4 aux) { return aux.g | (aux.b << 8u); }

// Validate the entire bilinear height footprint against the original chart.
// Tag 2 is direct source inside geometry, tag 3 is its padded/dilated region,
// and tag 4 is an interior crease requiring connected-surface traversal.
// Legacy/tag-1 direct pages keep their flat shading route. Large chart IDs that
// cannot fit in 16 bits also use tag 1 and cannot accidentally alias a chart.
bool vt_height_footprint_valid(VtAddress address, uint chart, bool allow_crease) {
    VT_POM_FOOTPRINT_VISIT();
    if (!address.valid || address.height_version != 1u) return false;
    ivec2 size = textureSize(vt_pool[VT_CHANNEL_AUX], 0).xy;
    ivec2 lo = ivec2(floor(address.physical_uv * vec2(size) - 0.5));
    if (any(lessThan(lo, ivec2(0))) || any(greaterThanEqual(lo + 1, size))) return false;
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
        uvec4 aux = uvec4(texelFetch(vt_pool[VT_CHANNEL_AUX],
            ivec3(lo + ivec2(x,y), int(address.physical_layer)), 0) * 255.0 + 0.5);
        if ((aux.a != 2u && !(allow_crease && aux.a == 4u)) || vt_aux_chart(aux) != chart) return false;
    }
    return true;
}
bool vt_height_footprint_valid(VtAddress address, uint chart) {
    return vt_height_footprint_valid(address, chart, false);
}

// One ray can sample the same bilinear quad many times, especially during
// refinement. Pool contents are immutable for this draw: reuse only a proven
// valid quad, keyed by physical texels, array layer and expected chart. A new
// quad must still check all four tags. Height and page-input identity remain
// independently checked at every march step.
bool vt_height_footprint_valid_cached(VtAddress address, uint chart,
                                      inout ivec4 checked_footprint) {
    if (!address.valid || address.height_version != 1u) return false;
    ivec2 size = textureSize(vt_pool[VT_CHANNEL_AUX], 0).xy;
    ivec2 lo = ivec2(floor(address.physical_uv * vec2(size) - 0.5));
    ivec4 footprint = ivec4(lo, int(address.physical_layer), int(chart));
    if (all(equal(footprint, checked_footprint))) return true;
    if (!vt_height_footprint_valid(address, chart)) return false;
    checked_footprint = footprint;
    return true;
}

struct VtParallaxSample {
    VtAddress address;
    vec2 uv;
    float ray_t;
    uint status; // Diagnostic only; no buffer, descriptor or ray-payload ABI.
    bool connected;
    vec3 albedo, orm, normal_local; // reconstructed channels across chart cuts
};

const uint VT_POM_OFF = 0u;
const uint VT_POM_HIT = 1u;
const uint VT_POM_INITIAL_BOUNDARY = 2u;
const uint VT_POM_PATH_BOUNDARY = 3u;
const uint VT_POM_SNAPSHOT_MISMATCH = 4u;
const uint VT_POM_TRAVEL_LIMIT = 5u;
const uint VT_POM_UNRESOLVED = 6u;
const uint VT_POM_UNSUPPORTED = 7u;
const uint VT_POM_ZERO_DEPTH = 8u;

vec3 vt_parallax_status_color(uint status) {
    if (status == VT_POM_HIT) return vec3(0,1,0);
    if (status == VT_POM_INITIAL_BOUNDARY) return vec3(1,0.5,0);
    if (status == VT_POM_PATH_BOUNDARY) return vec3(1,0,0);
    if (status == VT_POM_SNAPSHOT_MISMATCH) return vec3(1,0,1);
    if (status == VT_POM_TRAVEL_LIMIT) return vec3(0,0,1);
    if (status == VT_POM_UNRESOLVED) return vec3(0,1,1);
    if (status == VT_POM_UNSUPPORTED) return vec3(1);
    if (status == VT_POM_ZERO_DEPTH) return vec3(0,0.25,0);
    return vec3(0.25);
}

bool vt_parallax_depth(uint slot, vec2 uv, float lod, uint chart,
                       vec2 decode, uint snapshot, uvec3 mapping, float relief, float fade,
                       inout ivec4 checked_footprint,
                       out float depth, out VtAddress address, out uint status) {
    depth = 0.0;
    status = VT_POM_PATH_BOUNDARY;
    address = vt_resolve(slot, uv, lod);
    // vt_resolve clamps ordinary texture lookups; marching must never wrap or
    // clamp its path into another part of the atlas.
    if (any(lessThan(uv, vec2(0))) || any(greaterThanEqual(uv, vec2(1))) ||
        !vt_height_footprint_valid_cached(address, chart, checked_footprint)) return false;
    // During a range edit, mixing two different outer shells is undefined.
    // Preserve flat coverage until this ray's participating pages agree.
    if (any(notEqual(vt_height_envelope(address), decode)) || address.input_snapshot != snapshot ||
        any(notEqual(vt_page_inputs[address.physical_slot].material.yzw,mapping))) {
        status = VT_POM_SNAPSHOT_MISMATCH;
        return false;
    }
    depth = clamp(decode.x + decode.y - vt_sample_height_m(address), 0.0, relief) * fade;
    return true;
}

VtParallaxSample vt_parallax_chart_sample(uint slot, vec2 uv, float lod,
    vec2 uv_ray, float inward, float texel_m, float footprint_m,
    int steps, int refine, float max_ray_m, float relief_cap_m, float distance_fade) {
    VtParallaxSample base_sample;
    base_sample.address = vt_resolve(slot, uv, lod);
    base_sample.uv = uv; base_sample.ray_t = 0.0;
    base_sample.status = VT_POM_OFF;
    base_sample.connected = false;
    base_sample.albedo = base_sample.orm = base_sample.normal_local = vec3(0);
    if (!base_sample.address.valid || steps <= 0 || inward <= 1e-5 ||
        max_ray_m <= 0.0 || texel_m <= 0.0) return base_sample;
    uvec4 aux = uvec4(vt_sample_aux(base_sample.address) * 255.0 + 0.5);
    uint chart = vt_aux_chart(aux);
    base_sample.status = VT_POM_UNSUPPORTED;
    if (aux.a == 1u || base_sample.address.height_version != 1u) return base_sample;
    vec2 decode = vt_height_envelope(base_sample.address);
    float relief = min(decode.y, max(relief_cap_m, 0.0));
    float material_texel_m = vt_material_texel_m(base_sample.address);
    float resolved_m = max(footprint_m, material_texel_m > 0.0 ?
        material_texel_m : texel_m * exp2(float(base_sample.address.mapped_mip)));
    float fade = clamp(distance_fade, 0.0, 1.0) *
                 smoothstep(0.25, 2.0, relief / max(resolved_m, 1e-6));
    base_sample.status = VT_POM_UNRESOLVED;
    // Flat fallback needs neither bilinear chart validation nor a connected
    // geometry seed. Keep the existing fade and near-surface march unchanged.
    if (relief * fade <= 1e-7) return base_sample;
    base_sample.status = VT_POM_INITIAL_BOUNDARY;
    ivec4 checked_footprint = ivec4(-1);
    if (aux.a != 2u || !vt_height_footprint_valid_cached(base_sample.address, chart, checked_footprint)) return base_sample;
    uvec3 mapping = vt_page_inputs[base_sample.address.physical_slot].material.yzw;
    float end_t = min(relief * fade / inward, max_ray_m);
    VtVariantRecord record = vt_variants[slot - 1u];
    vec2 requested_size = vec2(record.atlas_w, record.atlas_h) /
                          exp2(float(base_sample.address.desired_mip));
    // Half-texel stepping cannot leap across a one-texel invalid region or
    // chart discontinuity. Bound work to 128 samples, reducing travel if needed.
    float needed = ceil(2.0 * max(abs(uv_ray.x) * requested_size.x,
                                  abs(uv_ray.y) * requested_size.y) * end_t);
    vec4 gradient=vt_material_gradient(base_sample.address);
    needed=max(needed,ceil(2.0*max(abs(dot(gradient.xy,uv_ray)),abs(dot(gradient.zw,uv_ray)))*end_t));
    if (needed > 128.0) end_t *= 128.0 / needed;
    int count = clamp(max(steps, int(min(needed, 128.0))), 1, 128);
    float depth;
    VtAddress address;
    if (!vt_parallax_depth(slot, uv, lod, chart, decode, base_sample.address.input_snapshot, mapping,
                           relief, fade, checked_footprint, depth, address, base_sample.status)) return base_sample;
    base_sample.status = VT_POM_ZERO_DEPTH;
    if (depth <= 1e-7) return base_sample;
    float lo = 0.0, hi = 0.0;
    float lo_gap = -depth, hi_gap = -depth;
    bool crossed = false;
    for (int i = 1; i <= count; ++i) {
        hi = end_t * float(i) / float(count);
        if (!vt_parallax_depth(slot, uv + uv_ray * hi, lod, chart, decode, base_sample.address.input_snapshot, mapping,
                               relief, fade, checked_footprint, depth, address, base_sample.status)) return base_sample;
        hi_gap = inward * hi - depth;
        // At the bottom of the envelope, (depth / inward) * inward can
        // round just below depth. Bound tolerance by float precision at this
        // depth, rather than a fixed metre bias that grows at grazing angles.
        float roundoff = 4.0 * 1.1920929e-7 * max(abs(inward * hi), abs(depth));
        if (hi_gap >= -roundoff) { hi_gap = max(hi_gap, 0.0); crossed = true; break; }
        lo = hi; lo_gap = hi_gap;
    }
    base_sample.status = VT_POM_TRAVEL_LIMIT;
    if (!crossed) return base_sample;
    for (int i = 0; i < clamp(refine, 0, 12); ++i) {
        float mid = (lo + hi) * 0.5;
        if (!vt_parallax_depth(slot, uv + uv_ray * mid, lod, chart, decode, base_sample.address.input_snapshot, mapping,
                               relief, fade, checked_footprint, depth, address, base_sample.status)) return base_sample;
        float gap = inward * mid - depth;
        if (gap >= 0.0) { hi = mid; hi_gap = gap; }
        else { lo = mid; lo_gap = gap; }
    }
    VtParallaxSample result = base_sample;
    // Interpolate the final signed crossing. Constant/linear depth is exact
    // instead of retaining the upper-bound bias of a finite binary search.
    result.ray_t = mix(lo, hi, clamp(-lo_gap / max(hi_gap - lo_gap, 1e-12), 0.0, 1.0));
    result.uv = uv + uv_ray * result.ray_t;
    result.address = vt_resolve(slot, result.uv, lod);
    result.status = VT_POM_HIT;
    return result;
}

#include "vt_surface_walk.glsl"

bool vt_parallax_connected_gap(VtWalkContext ctx, vec3 position, float lod,
    float relief, float fade, inout VtWalkPoint point, out float gap, out uint status) {
    status = VT_POM_PATH_BOUNDARY;
    if (!vt_walk_locate(ctx, position, point)) return false;
    VtWalkValue value;
    VtAddress address;
    if (!vt_walk_sample(ctx, point, lod, false, value, address, status)) return false;
    vec2 decode = vt_height_envelope(address);
    float depth = clamp(decode.x + decode.y - value.height, 0.0, relief) * fade;
    gap = point.recess - depth;
    return true;
}

VtParallaxSample vt_parallax_sample(uint slot, vec2 uv, float lod,
    vec2 uv_ray, float inward, float texel_m, float footprint_m,
    int steps, int refine, float max_ray_m, float relief_cap_m, float distance_fade,
    vec3 proxy_local, vec3 ray_local) {
    VtParallaxSample base = vt_parallax_chart_sample(slot, uv, lod, uv_ray, inward,
        texel_m, footprint_m, steps, refine, max_ray_m, relief_cap_m, distance_fade);
    if (base.status != VT_POM_INITIAL_BOUNDARY && base.status != VT_POM_PATH_BOUNDARY) return base;
    float relief = min(vt_height_envelope(base.address).y, max(relief_cap_m, 0.0));
    float material_texel_m = vt_material_texel_m(base.address);
    float resolved_m = max(footprint_m, material_texel_m > 0.0 ?
        material_texel_m : texel_m * exp2(float(base.address.mapped_mip)));
    float fade = clamp(distance_fade, 0.0, 1.0) *
                 smoothstep(0.25, 2.0, relief / max(resolved_m, 1e-6));
    if (relief * fade <= 1e-7) { base.status = VT_POM_UNRESOLVED; return base; }
    VT_POM_CONNECTED_VISIT();
    VtWalkContext ctx;
    ctx.metadata = vt_page_inputs[base.address.physical_slot];
    ctx.slot = slot;
    VtVariantRecord record = vt_variants[slot - 1u];
    ctx.atlas_size = vec2(record.atlas_w, record.atlas_h);
    uvec4 aux = uvec4(vt_sample_aux(base.address) * 255.0 + 0.5);
    VtWalkPoint point;
    if (!vt_walk_seed(ctx, vt_aux_chart(aux), proxy_local, texel_m, point)) return base;
    float lo = 0.0, hi = 0.0, lo_gap, hi_gap;
    uint status;
    if (!vt_parallax_connected_gap(ctx, proxy_local, lod, relief, fade, point, lo_gap, status)) {
        base.status = status == VT_POM_SNAPSHOT_MISMATCH ? status : VT_POM_INITIAL_BOUNDARY;
        return base;
    }
    if (lo_gap >= -1e-7) { base.status = VT_POM_ZERO_DEPTH; return base; }
    VtWalkPoint low_point = point;
    float requested_scale = exp2(float(base.address.desired_mip));
    float nominal_step = min(relief * fade / inward, max_ray_m) / float(clamp(steps, 1, 128));
    bool crossed = false;
    for (int i = 0; i < 128; ++i) {
        GpuChart chart = VtWalkCharts(ctx.metadata.charts).values[point.chart];
        float half_texel_step = 0.5 * requested_scale / max(length(ray_local) * chart.origin_tpm.w, 1e-8);
        VtAddress mapped=vt_resolve_chart(slot,point.uv,lod,point.chart);
        vec2 local_uv_ray=vec2(dot(ray_local,chart.tangent_ou.xyz),dot(ray_local,chart.bitangent_ov.xyz))*
                          chart.origin_tpm.w/ctx.atlas_size;
        float module_rate=max(abs(dot(vt_material_gradient(mapped).xy,local_uv_ray)),
                              abs(dot(vt_material_gradient(mapped).zw,local_uv_ray)));
        if (module_rate>0.0) half_texel_step=min(half_texel_step,.5/module_rate);
        hi = min(max_ray_m, lo + min(nominal_step, half_texel_step));
        if (!(hi > lo)) break;
        if (!vt_parallax_connected_gap(ctx, proxy_local + ray_local * hi, lod, relief, fade,
                                      point, hi_gap, status)) { base.status = status; return base; }
        float roundoff = 4.0 * 1.1920929e-7 * max(abs(point.recess), relief);
        if (hi_gap >= -roundoff) { hi_gap = max(hi_gap, 0.0); crossed = true; break; }
        lo = hi; lo_gap = hi_gap; low_point = point;
    }
    if (!crossed) { base.status = VT_POM_TRAVEL_LIMIT; return base; }
    for (int i = 0; i < clamp(refine, 0, 12); ++i) {
        float mid = (lo + hi) * 0.5, gap;
        point = low_point;
        if (!vt_parallax_connected_gap(ctx, proxy_local + ray_local * mid, lod, relief, fade,
                                      point, gap, status)) { base.status = status; return base; }
        if (gap >= 0.0) { hi = mid; hi_gap = gap; }
        else { lo = mid; lo_gap = gap; low_point = point; }
    }
    VtParallaxSample result = base;
    result.ray_t = mix(lo, hi, clamp(-lo_gap / max(hi_gap - lo_gap, 1e-12), 0.0, 1.0));
    point = low_point;
    float gap;
    if (!vt_parallax_connected_gap(ctx, proxy_local + ray_local * result.ray_t, lod,
                                  relief, fade, point, gap, status)) { base.status = status; return base; }
    VtWalkValue value;
    if (!vt_walk_sample(ctx, point, lod, true, value, result.address, status)) { base.status = status; return base; }
    result.uv = point.uv;
    result.connected = true;
    result.albedo = value.albedo; result.orm = value.orm; result.normal_local = value.normal;
    result.status = VT_POM_HIT;
    return result;
}
#endif
