#include "particle_flow.h"
#include <cmath>

namespace pf {

// ---------------------------------------------------------------------------
// Seeded value noise + curl. Divergence-free steering from the curl of a
// 3-component value-noise vector potential (finite differences).
// ---------------------------------------------------------------------------
static float hash01(uint32_t seed, int x, int y, int z) {
    uint32_t h = seed;
    h ^= (uint32_t)x * 0x8DA6B343u;
    h ^= (uint32_t)y * 0xD8163841u;
    h ^= (uint32_t)z * 0xCB1AB31Fu;
    h ^= h >> 13; h *= 0x5BD1E995u; h ^= h >> 15;
    return (float)(h & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

static float vnoise(uint32_t seed, V3 p) {
    int x0 = (int)std::floor(p.x), y0 = (int)std::floor(p.y), z0 = (int)std::floor(p.z);
    float fx = p.x - x0, fy = p.y - y0, fz = p.z - z0;
    // smoothstep fade
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy); fz = fz * fz * (3 - 2 * fz);
    float c[2][2][2];
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k)
                c[i][j][k] = hash01(seed, x0 + i, y0 + j, z0 + k);
    float x00 = c[0][0][0] + (c[1][0][0] - c[0][0][0]) * fx;
    float x10 = c[0][1][0] + (c[1][1][0] - c[0][1][0]) * fx;
    float x01 = c[0][0][1] + (c[1][0][1] - c[0][0][1]) * fx;
    float x11 = c[0][1][1] + (c[1][1][1] - c[0][1][1]) * fx;
    float y0v = x00 + (x10 - x00) * fy;
    float y1v = x01 + (x11 - x01) * fy;
    return y0v + (y1v - y0v) * fz;
}

static V3 potential(uint32_t seed, V3 p) {
    return { vnoise(seed ^ 0x9E3779B9u, p),
             vnoise(seed ^ 0x85EBCA6Bu, p),
             vnoise(seed ^ 0xC2B2AE35u, p) };
}

static V3 curl_noise(uint32_t seed, V3 p, float scale) {
    const float inv = scale > 1e-6f ? 1.0f / scale : 1.0f;
    p = p * inv;
    const float e = 0.05f;
    V3 dx1 = potential(seed, p + V3{e,0,0}), dx0 = potential(seed, p - V3{e,0,0});
    V3 dy1 = potential(seed, p + V3{0,e,0}), dy0 = potential(seed, p - V3{0,e,0});
    V3 dz1 = potential(seed, p + V3{0,0,e}), dz0 = potential(seed, p - V3{0,0,e});
    const float s = 1.0f / (2.0f * e);
    // curl F = (dFz/dy - dFy/dz, dFx/dz - dFz/dx, dFy/dx - dFx/dy)
    return normalize(V3{
        (dy1.z - dy0.z) * s - (dz1.y - dz0.y) * s,
        (dz1.x - dz0.x) * s - (dx1.z - dx0.z) * s,
        (dx1.y - dx0.y) * s - (dy1.x - dy0.x) * s });
}

// ---------------------------------------------------------------------------
// Neighborhood fields
// ---------------------------------------------------------------------------
static V3 slot_p(const Sim& s, uint32_t i) {
    const float* p = s.pos_data();
    return {p[3*i], p[3*i+1], p[3*i+2]};
}

static V3 adhere_dir(const Sim& s, const FieldConfig& f, V3 p) {
    V3 sum{0,0,0}; uint32_t n = 0;
    s.deposited_hash().query(p, f.radius, [&](uint32_t, V3 q, float) {
        sum = sum + q; ++n;
    });
    if (n == 0) return {0,0,0};
    V3 avg = sum * (1.0f / (float)n);
    V3 out = normalize(p - avg);                       // outward surface normal
    V3 target = avg + out * f.surface_offset;          // ride outside the wood
    return normalize(target - p);
}

// Average forward heading of nearby deposits: steer WITH the direction prior
// strands were traveling here, not just toward their wood (that's Adhere).
// Without this a follower can orbit a column while staying perfectly adhered.
static V3 align_dir(const Sim& s, const FieldConfig& f, V3 p) {
    const auto& dirs = s.deposited_dirs();
    V3 sum{0,0,0}; uint32_t n = 0;
    s.deposited_hash().query(p, f.radius, [&](uint32_t idx, V3, float) {
        sum = sum + dirs[idx]; ++n;
    });
    if (n == 0) return {0,0,0};
    return normalize(sum);
}

static V3 separate_dir(const Sim& s, const FieldConfig& f, uint32_t slot, V3 p) {
    V3 push{0,0,0}; uint32_t n = 0;
    s.live_hash().query(p, f.radius, [&](uint32_t idx, V3 q, float d2) {
        if (idx == slot) return;
        V3 d = p - q;
        float dist = std::sqrt(d2);
        if (dist > 1e-6f) { push = push + d * (1.0f / (dist * dist)); ++n; }
    });
    if (n == 0) return {0,0,0};
    return normalize(push);
}

V3 field_steer_dir(const Sim& s, const FieldConfig& f, uint32_t slot) {
    switch (f.type) {
        case FieldType::Bias:     return normalize(f.dir);
        case FieldType::Curl:     return curl_noise(f.seed, slot_p(s, slot), f.scale);
        case FieldType::Adhere:   return adhere_dir(s, f, slot_p(s, slot));
        case FieldType::Align:    return align_dir(s, f, slot_p(s, slot));
        case FieldType::Separate: return separate_dir(s, f, slot, slot_p(s, slot));
        default:                  return {0, 0, 0};   // Attract handled by Sim; Drag is force-only
    }
}

V3 field_force(const Sim& s, const FieldConfig& f, uint32_t slot) {
    switch (f.type) {
        case FieldType::Bias: return normalize(f.dir);
        case FieldType::Curl: return curl_noise(f.seed, slot_p(s, slot), f.scale);
        case FieldType::Drag: {
            const float* v = s.vel_data();
            return V3{v[3*slot], v[3*slot+1], v[3*slot+2]} * (-f.k);
        }
        default: return {0, 0, 0};
    }
}

// ---------------------------------------------------------------------------
// Sim members that need attractor mutation / deposited queries
// ---------------------------------------------------------------------------
V3 Sim::attract_dir(uint32_t slot, V3 p) {
    const FieldConfig* fc = nullptr;
    for (const auto& f : cfg_.fields)
        if (f.type == FieldType::Attract) { fc = &f; break; }
    if (!fc || attr_remaining_ == 0) return {0,0,0};
    int best = -1; float best_d2;
    if (claim_of_[slot] != UINT32_MAX && !attr_consumed_[claim_of_[slot]]) {
        // Claimed target: beeline regardless of influence radius.
        best = (int)claim_of_[slot];
        V3 d = attractors_[(size_t)best] - p;
        best_d2 = dot(d, d);
    } else {
        // Nearest unconsumed, unclaimed attractor within influence. Linear scan
        // is fine at the ~500-attractor scale; ascending index = deterministic
        // tie-break.
        best_d2 = fc->influence * fc->influence;
        for (size_t i = 0; i < attractors_.size(); ++i) {
            if (attr_consumed_[i] || attr_claimed_[i]) continue;
            V3 d = attractors_[i] - p;
            float d2 = dot(d, d);
            if (d2 < best_d2) { best_d2 = d2; best = (int)i; }
        }
        if (best < 0) return {0,0,0};
    }
    if (best_d2 <= fc->kill_radius * fc->kill_radius) {
        attr_consumed_[best] = 1;
        --attr_remaining_;
        if (fc->kill_on_consume) kill_slot(slot);
        else if (claim_of_[slot] == (uint32_t)best) claim_of_[slot] = UINT32_MAX;
        return {0,0,0};
    }
    return normalize(attractors_[best] - p);
}

V3 Sim::surface_normal(V3 p, float radius, bool* ok) const {
    V3 sum{0,0,0}; uint32_t n = 0;
    dep_hash_.query(p, radius, [&](uint32_t, V3 q, float) { sum = sum + q; ++n; });
    if (n == 0) { if (ok) *ok = false; return {0,0,0}; }
    if (ok) *ok = true;
    return normalize(p - sum * (1.0f / (float)n));
}

} // namespace pf
