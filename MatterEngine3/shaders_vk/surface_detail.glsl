#ifndef MATTER_SURFACE_DETAIL_GLSL
#define MATTER_SURFACE_DETAIL_GLSL
// Finished geometry-derived detail. Coordinates and normal convention match
// vt_composite.comp, independently of the legacy world-space ground sampler.
// Include tileset_common.glsl first. A shell represents the highest point;
// height is measured inward from height_max, never from the ground datum.
struct SurfaceDetailSample {
    vec3 position;
    vec3 normal;
    vec3 albedo;
    vec3 orm;
    float ray_t;
};

vec3 surface_detail_weights(vec3 n) {
    vec3 w = n * n; w *= w;
    return w / max(w.x + w.y + w.z, 1e-8);
}
vec2 surface_detail_uv(vec3 p, int axis) {
    return axis == 0 ? p.zy : (axis == 1 ? p.xz : p.xy);
}
vec4 surface_detail_tex(int slot, int channel, vec2 p, float lod) {
    int layer; vec2 uv;
    wang_resolve(slot, p, layer, uv);
    int tex = slot * TILESET_CHANNELS + channel;
    float last = float(textureQueryLevels(tilesetTex[nonuniformEXT(tex)]) - 1);
    return textureLod(tilesetTex[nonuniformEXT(tex)], vec3(uv, float(layer)),
                      clamp(lod, 0.0, last));
}
float surface_detail_depth(int slot, vec3 p, vec3 weights, float lod,
                           float range_m, float relief_m) {
    float h = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        if (weights[axis] <= 1e-5) continue;
        h += weights[axis] * surface_detail_tex(slot, TILESET_CH_HEIGHT,
                                               surface_detail_uv(p, axis), lod).r;
    }
    return clamp((1.0 - h) * range_m, 0.0, relief_m);
}

SurfaceDetailSample surface_detail_sample(int slot, vec3 p_local, vec3 n_local,
    vec3 ray_local_per_world_m, float footprint_local_m, int steps, int refine,
    float max_ray_m, float relief_cap_m) {
    SurfaceDetailSample s;
    s.position = p_local;
    s.ray_t = 0.0;
    vec3 n = normalize(n_local);
    vec3 weights = surface_detail_weights(n);
    float lod = log2(max(1.0, footprint_local_m *
                              TILESET_SLOT_SCALAR(texels_per_meter, slot)));
    float range_m = max(0.0, TILESET_SLOT_SCALAR(height_max, slot) -
                            TILESET_SLOT_SCALAR(height_min, slot));
    float relief_m = min(range_m, max(relief_cap_m, 0.0));
    float inward = -dot(n, ray_local_per_world_m);
    if (steps > 0 && inward > 1e-5 && relief_m > 1e-7 && max_ray_m > 0.0) {
        int count = clamp(steps, 1, 128);
        // Bounded even at grazing incidence. Failure to cross the heightfield
        // inside this interval retains the undisplaced surface, never NaNs.
        float end_t = min(relief_m / inward, max_ray_m);
        float lo = 0.0, hi = 0.0;
        bool crossed = surface_detail_depth(slot, p_local, weights, lod,
                                            range_m, relief_m) <= 1e-7;
        for (int i = 1; i <= count && !crossed; ++i) {
            hi = end_t * float(i) / float(count);
            float depth = surface_detail_depth(slot,
                p_local + ray_local_per_world_m * hi, weights, lod, range_m, relief_m);
            crossed = inward * hi + 1e-7 >= depth;
            if (!crossed) lo = hi;
        }
        if (crossed) {
            for (int i = 0; i < clamp(refine, 0, 12); ++i) {
                float mid = (lo + hi) * 0.5;
                float depth = surface_detail_depth(slot,
                    p_local + ray_local_per_world_m * mid, weights, lod, range_m, relief_m);
                if (inward * mid >= depth) hi = mid; else lo = mid;
            }
            s.ray_t = hi;
            s.position = p_local + ray_local_per_world_m * hi;
        }
    }
    const vec3 axis_t[3] = vec3[3](vec3(0,0,1), vec3(1,0,0), vec3(1,0,0));
    const vec3 axis_b[3] = vec3[3](vec3(0,1,0), vec3(0,0,1), vec3(0,1,0));
    s.albedo = vec3(0); s.orm = vec3(0);
    vec3 dn = vec3(0);
    for (int axis = 0; axis < 3; ++axis) {
        float w = weights[axis];
        if (w <= 1e-5) continue;
        vec2 uv = surface_detail_uv(s.position, axis);
        s.albedo += w * surface_detail_tex(slot, TILESET_CH_ALBEDO, uv, lod).rgb;
        s.orm += w * surface_detail_tex(slot, TILESET_CH_ORM, uv, lod).rgb;
        vec2 rg = surface_detail_tex(slot, TILESET_CH_NORMAL, uv, lod).rg * 2.0 - 1.0;
        dn += w * (axis_t[axis] * rg.x + axis_b[axis] * rg.y);
    }
    s.normal = normalize(n + dn);
    return s;
}
#endif
