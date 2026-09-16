// gi_bake.cpp — see gi_bake.h for the contract and docs/bake-gi.md for the
// user-facing description.

#include "gi_bake.h"

#include "../../libs/MeshChartingLib/include/mesh_charting.h"   // the renderer's chart pipeline
#include "material_registry.h"   // MaterialRegistryGet (albedo per materialId)
#include "world_tracer.h"        // WorldTracer (CPU scene tracer)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace gi_bake {

namespace {

constexpr float kPi = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Deterministic per-texel random stream: splitmix64 seeding + PCG32 output.
// ---------------------------------------------------------------------------
struct Rng {
    uint64_t state = 0;
    uint64_t inc = 0;

    static uint64_t splitmix(uint64_t& x) {
        uint64_t z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    explicit Rng(uint64_t seed) {
        uint64_t x = seed;
        state = splitmix(x);
        inc = splitmix(x) | 1ull;
        next_u32();
    }
    uint32_t next_u32() {
        const uint64_t old = state;
        state = old * 6364136223846793005ull + inc;
        const uint32_t xorshifted = uint32_t(((old >> 18u) ^ old) >> 27u);
        const uint32_t rot = uint32_t(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }
    // Uniform in [0, 1).
    float next_f() { return float(next_u32() >> 8) * (1.0f / 16777216.0f); }
};

// ---------------------------------------------------------------------------
// Row-major 4x4 (ChildInstance / TLAS DrawInstance layout) helpers.
// ---------------------------------------------------------------------------
inline float3 xform_point(const float* m, const float3& p) {
    return make_float3(m[0] * p.x + m[1] * p.y + m[2]  * p.z + m[3],
                       m[4] * p.x + m[5] * p.y + m[6]  * p.z + m[7],
                       m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]);
}

struct Mat3 { float c[9]; };

// Inverse-transpose of the upper 3x3 (the normal matrix). Falls back to the
// plain 3x3 (uniform-scale-correct) when the block is singular.
Mat3 normal_matrix(const float* m) {
    const float a = m[0], b = m[1], c = m[2];
    const float d = m[4], e = m[5], f = m[6];
    const float g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    Mat3 out;
    if (std::fabs(det) < 1e-20f) {
        out.c[0] = a; out.c[1] = b; out.c[2] = c;
        out.c[3] = d; out.c[4] = e; out.c[5] = f;
        out.c[6] = g; out.c[7] = h; out.c[8] = i;
        return out;
    }
    const float inv = 1.0f / det;
    // inverse (row-major), then transpose: out = (M^-1)^T
    const float i00 = (e * i - f * h) * inv, i01 = (c * h - b * i) * inv, i02 = (b * f - c * e) * inv;
    const float i10 = (f * g - d * i) * inv, i11 = (a * i - c * g) * inv, i12 = (c * d - a * f) * inv;
    const float i20 = (d * h - e * g) * inv, i21 = (b * g - a * h) * inv, i22 = (a * e - b * d) * inv;
    out.c[0] = i00; out.c[1] = i10; out.c[2] = i20;
    out.c[3] = i01; out.c[4] = i11; out.c[5] = i21;
    out.c[6] = i02; out.c[7] = i12; out.c[8] = i22;
    return out;
}

inline float3 apply3(const Mat3& n, const float3& v) {
    return make_float3(n.c[0] * v.x + n.c[1] * v.y + n.c[2] * v.z,
                       n.c[3] * v.x + n.c[4] * v.y + n.c[5] * v.z,
                       n.c[6] * v.x + n.c[7] * v.y + n.c[8] * v.z);
}

inline float3 safe_normalize(const float3& v, const float3& fallback) {
    const float l2 = dot(v, v);
    if (!(l2 > 1e-24f) || !std::isfinite(l2)) return fallback;
    return v * (1.0f / std::sqrt(l2));
}

// Branchless orthonormal basis (Duff et al.).
inline void onb(const float3& n, float3& t, float3& b) {
    const float sign = n.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (sign + n.z);
    const float bb = n.x * n.y * a;
    t = make_float3(1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x);
    b = make_float3(bb, sign + n.y * n.y * a, -n.y);
}

inline float luminance(const float* rgb) {
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

// material_common.glsl resolveBaseColor: mix(albedo, tint.rgb, clamp(tint.a)).
inline float3 resolve_base_color(const float* albedo, const float* tint) {
    const float a = std::min(1.0f, std::max(0.0f, tint[3]));
    return make_float3(albedo[0] + (tint[0] - albedo[0]) * a,
                       albedo[1] + (tint[1] - albedo[1]) * a,
                       albedo[2] + (tint[2] - albedo[2]) * a);
}

// ---------------------------------------------------------------------------
// Per-instance texel records (only covered texels are materialized).
// ---------------------------------------------------------------------------
struct TexelRec {
    float3   pos;      // world
    float3   nrm;      // world shading normal (unit)
    float3   gnrm;     // world geometric normal (unit)
    float3   albedo;   // resolved base colour
    uint32_t chart;
};

struct Raster {
    uint32_t w = 0, h = 0;
    std::vector<int32_t>  index;        // atlas texel -> rec index, -1 = empty
    std::vector<TexelRec> recs;
    std::vector<uint32_t> rect_chart;   // atlas texel -> chart whose outer rect holds it
};

// Closest point on the 2D triangle (a,b,c) to p; returns barycentrics of that
// point. Used to claim texels whose centre falls just outside the mesh in
// chart space (cracks between charts' gutters and the content edge).
void closest_bary(const float a[2], const float b[2], const float c[2], const float p[2],
                  float out[3], float& dist2) {
    auto seg = [](const float s0[2], const float s1[2], const float q[2], float& t) {
        const float dx = s1[0] - s0[0], dy = s1[1] - s0[1];
        const float l2 = dx * dx + dy * dy;
        t = l2 > 0.0f ? ((q[0] - s0[0]) * dx + (q[1] - s0[1]) * dy) / l2 : 0.0f;
        t = std::min(1.0f, std::max(0.0f, t));
        const float px = s0[0] + dx * t - q[0], py = s0[1] + dy * t - q[1];
        return px * px + py * py;
    };
    float t0, t1, t2;
    const float d0 = seg(a, b, p, t0);
    const float d1 = seg(b, c, p, t1);
    const float d2 = seg(c, a, p, t2);
    if (d0 <= d1 && d0 <= d2) { out[0] = 1.0f - t0; out[1] = t0; out[2] = 0.0f; dist2 = d0; }
    else if (d1 <= d2)        { out[0] = 0.0f; out[1] = 1.0f - t1; out[2] = t1; dist2 = d1; }
    else                      { out[0] = t2; out[1] = 0.0f; out[2] = 1.0f - t2; dist2 = d2; }
}

void rasterize_instance(const Part& part, const PartAtlas& atlas, const Instance& inst,
                        Raster& r) {
    r.w = atlas.width; r.h = atlas.height;
    const size_t n = size_t(r.w) * r.h;
    r.index.assign(n, -1);
    r.recs.clear();
    r.rect_chart.assign(n, ~0u);
    for (uint32_t c = 0; c < atlas.rung.charts.size(); ++c) {
        const chart_atlas::ChartEntry& e = atlas.rung.charts[c];
        for (uint32_t y = e.rect_y; y < std::min(r.h, e.rect_y + e.rect_h); ++y)
            for (uint32_t x = e.rect_x; x < std::min(r.w, e.rect_x + e.rect_w); ++x)
                r.rect_chart[size_t(y) * r.w + x] = c;
    }

    const Mat3 nm = normal_matrix(inst.transform);
    const float fw = float(r.w), fh = float(r.h);

    auto emit = [&](uint32_t t, uint32_t x, uint32_t y, const float bary[3]) {
        const Tri& tri = part.tris[t];
        const TriEx& ex = part.triex[t];
        const float3 lp = tri.vertex0 * bary[0] + tri.vertex1 * bary[1] + tri.vertex2 * bary[2];
        const float3 e1 = tri.vertex1 - tri.vertex0, e2 = tri.vertex2 - tri.vertex0;
        const float3 lgn = cross(e1, e2);
        float3 lsn = ex.N0 * bary[0] + ex.N1 * bary[1] + ex.N2 * bary[2];
        if (!(dot(lsn, lsn) > 1e-12f)) lsn = lgn;
        TexelRec rec;
        rec.pos = xform_point(inst.transform, lp);
        rec.gnrm = safe_normalize(apply3(nm, lgn), make_float3(0.0f, 1.0f, 0.0f));
        rec.nrm = safe_normalize(apply3(nm, lsn), rec.gnrm);
        if (dot(rec.nrm, rec.gnrm) < 0.0f) rec.nrm = rec.nrm * -1.0f;
        const MaterialDef* mat = MaterialRegistryGet(ex.materialId % 1000000);
        const float grey[3] = {0.5f, 0.5f, 0.5f};
        const float tint[4] = {ex.tint.x, ex.tint.y, ex.tint.z, ex.tint.w};
        rec.albedo = resolve_base_color(mat ? mat->albedo : grey, tint);
        rec.chart = atlas.tri_chart[t];
        r.index[size_t(y) * r.w + x] = int32_t(r.recs.size());
        r.recs.push_back(rec);
    };

    // Pass 1: texel centres strictly inside a triangle. Pass 2: still-empty
    // texels within half a texel of a triangle edge take the closest surface
    // point, so the mesh boundary is covered without a seam of empty texels.
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t t = 0; t < part.tris.size(); ++t) {
            const TriEx& ex = part.triex[t];
            const float a[2] = {ex.uv0.x * fw, ex.uv0.y * fh};
            const float b[2] = {ex.uv1.x * fw, ex.uv1.y * fh};
            const float c[2] = {ex.uv2.x * fw, ex.uv2.y * fh};
            const float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            if (!(std::fabs(area) > 1e-12f)) continue;
            const float inv_area = 1.0f / area;
            const float pad = pass == 0 ? 0.0f : 1.0f;
            const int x0 = std::max(0, int(std::floor(std::min(a[0], std::min(b[0], c[0])) - pad)));
            const int x1 = std::min(int(r.w) - 1, int(std::ceil(std::max(a[0], std::max(b[0], c[0])) + pad)));
            const int y0 = std::max(0, int(std::floor(std::min(a[1], std::min(b[1], c[1])) - pad)));
            const int y1 = std::min(int(r.h) - 1, int(std::ceil(std::max(a[1], std::max(b[1], c[1])) + pad)));
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const size_t ti = size_t(y) * r.w + x;
                    if (r.index[ti] >= 0) continue;
                    const float p[2] = {x + 0.5f, y + 0.5f};
                    float w0 = ((b[0] - p[0]) * (c[1] - p[1]) - (b[1] - p[1]) * (c[0] - p[0])) * inv_area;
                    float w1 = ((c[0] - p[0]) * (a[1] - p[1]) - (c[1] - p[1]) * (a[0] - p[0])) * inv_area;
                    float w2 = 1.0f - w0 - w1;
                    if (pass == 0) {
                        const float eps = -1e-5f;
                        if (w0 < eps || w1 < eps || w2 < eps) continue;
                        const float bary[3] = {w0, w1, w2};
                        emit(t, uint32_t(x), uint32_t(y), bary);
                    } else {
                        if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                            const float bary[3] = {w0, w1, w2};
                            emit(t, uint32_t(x), uint32_t(y), bary);
                            continue;
                        }
                        float bary[3]; float d2 = 0.0f;
                        closest_bary(a, b, c, p, bary, d2);
                        if (d2 <= 0.5f * 0.5f + 1e-6f)
                            emit(t, uint32_t(x), uint32_t(y), bary);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The estimator.
// ---------------------------------------------------------------------------
struct SunModel {
    float3 to_sun;
    float3 t, b;
    float  tan_half;   // tan of the angular radius
    float3 color;
    float3 sky;
};

SunModel make_sun(const Lighting& l) {
    SunModel s;
    const float3 dir = make_float3(l.sun_direction[0], l.sun_direction[1], l.sun_direction[2]);
    s.to_sun = safe_normalize(dir * -1.0f, make_float3(0.0f, 1.0f, 0.0f));
    onb(s.to_sun, s.t, s.b);
    const float half = std::max(0.0f, l.sun_angular_diameter_deg) * 0.5f * kPi / 180.0f;
    s.tan_half = std::tan(std::min(half, 0.5f));
    s.color = make_float3(l.sun_color[0], l.sun_color[1], l.sun_color[2]);
    s.sky = make_float3(l.sky_color[0], l.sky_color[1], l.sky_color[2]);
    return s;
}

inline float3 sample_sun_dir(const SunModel& s, Rng& rng) {
    const float r = std::sqrt(rng.next_f()) * s.tan_half;
    const float phi = rng.next_f() * 2.0f * kPi;
    return safe_normalize(s.to_sun + s.t * (r * std::cos(phi)) + s.b * (r * std::sin(phi)), s.to_sun);
}

inline float3 cosine_sample(const float3& n, Rng& rng) {
    const float u1 = rng.next_f(), u2 = rng.next_f();
    const float r = std::sqrt(u1);
    const float phi = 2.0f * kPi * u2;
    const float z = std::sqrt(std::max(0.0f, 1.0f - u1));
    float3 t, b; onb(n, t, b);
    return safe_normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * z, n);
}

struct TraceCtx {
    const world_tracer::WorldTracer* tracer = nullptr;
    SunModel sun;
    uint32_t samples = 1;
    uint32_t bounces = 0;
    float    eps = 1e-4f;
    uint64_t seed = 0;
};

// Direct sun at (p, n) with the geometric normal gn deciding the ray offset side.
inline float3 sun_term(const TraceCtx& c, const float3& p, const float3& n, const float3& gn,
                       Rng& rng, uint64_t& rays) {
    const float3 l = sample_sun_dir(c.sun, rng);
    const float ndl = dot(n, l);
    if (ndl <= 0.0f || dot(gn, l) <= 0.0f) return make_float3(0.0f);
    const float3 o = p + gn * c.eps;
    const float origin[3] = {o.x, o.y, o.z};
    const float dir[3] = {l.x, l.y, l.z};
    ++rays;
    if (c.tracer->occluded(origin, dir, 1e30f)) return make_float3(0.0f);
    return c.sun.color * ndl;
}

// Mean of N samples plus the variance OF THE MEAN of the per-sample luminance,
// which is what the denoiser needs to tell noise from a real gradient.
float3 trace_texel(const TraceCtx& c, const TexelRec& rec, Rng& rng, uint64_t& rays,
                   float& var_of_mean) {
    float3 sum = make_float3(0.0f);
    double sum_l = 0.0, sum_l2 = 0.0;
    for (uint32_t s = 0; s < c.samples; ++s) {
        float3 v = sun_term(c, rec.pos, rec.nrm, rec.gnrm, rng, rays);

        float3 T = make_float3(1.0f);
        float3 x = rec.pos, n = rec.nrm, gn = rec.gnrm;
        for (uint32_t d = 0; d <= c.bounces; ++d) {
            const float3 w = cosine_sample(n, rng);
            if (dot(w, gn) <= 0.0f) break;              // below the geometric horizon
            const float3 o = x + gn * c.eps;
            const float origin[3] = {o.x, o.y, o.z};
            const float dir[3] = {w.x, w.y, w.z};
            world_tracer::Hit hit;
            ++rays;
            if (!c.tracer->trace(origin, dir, 1e30f, hit)) {
                v += T * c.sun.sky;                       // escaped: uniform sky
                break;
            }
            if (d == c.bounces) break;                    // surface hit past the last bounce
            const float3 hp = o + w * hit.t;
            const float3 hn = safe_normalize(make_float3(hit.normal[0], hit.normal[1], hit.normal[2]),
                                             w * -1.0f);
            T = T * resolve_base_color(hit.albedo, hit.tint);
            const float3 e = make_float3(hit.emission_color[0], hit.emission_color[1],
                                         hit.emission_color[2]) * hit.emission;
            v += T * (e + sun_term(c, hp, hn, hn, rng, rays));
            x = hp; n = hn; gn = hn;
        }
        sum += v;
        const float l = 0.2126f * v.x + 0.7152f * v.y + 0.0722f * v.z;
        sum_l += l; sum_l2 += double(l) * l;
    }
    const double n = double(c.samples);
    const double mean_l = sum_l / n;
    const double var = std::max(0.0, sum_l2 / n - mean_l * mean_l);   // per-sample variance
    var_of_mean = float(var / n);
    return sum * (1.0f / float(c.samples));
}

// ---------------------------------------------------------------------------
// Post passes.
// ---------------------------------------------------------------------------

// gi-firefly-filtering policy (docs/gi-firefly-filtering-2026-09-12.md) in
// texel space: centre-excluded 3x3 luminance median/MAD over same-chart
// covered neighbours (5x5 ring when fewer than three), ceiling
// max(0.25, 4*median, median + 6*MAD), applied while preserving RGB ratios.
void firefly_pass(const Raster& r, std::vector<float>& rgb) {
    const int w = int(r.w), h = int(r.h);
    std::vector<float> out = rgb;
    float peers[24];
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t ti = size_t(y) * w + x;
            const int32_t ri = r.index[ti];
            if (ri < 0) continue;
            const uint32_t chart = r.recs[ri].chart;
            int n = 0;
            auto gather = [&](int radius) {
                n = 0;
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        if (radius == 2 && std::abs(dx) < 2 && std::abs(dy) < 2) continue;
                        const int qx = x + dx, qy = y + dy;
                        if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
                        const size_t qi = size_t(qy) * w + qx;
                        const int32_t qr = r.index[qi];
                        if (qr < 0 || r.recs[qr].chart != chart) continue;
                        peers[n++] = luminance(&rgb[qi * 3]);
                    }
                }
            };
            gather(1);
            if (n < 3) {
                float inner[8]; int inner_n = n;
                for (int i = 0; i < n; ++i) inner[i] = peers[i];
                gather(2);
                for (int i = 0; i < inner_n && n < 24; ++i) peers[n++] = inner[i];
            }
            if (n < 3) continue;
            std::sort(peers, peers + n);
            const float median = (n & 1) ? peers[n / 2] : 0.5f * (peers[n / 2 - 1] + peers[n / 2]);
            float dev[24];
            for (int i = 0; i < n; ++i) dev[i] = std::fabs(peers[i] - median);
            std::sort(dev, dev + n);
            const float mad = (n & 1) ? dev[n / 2] : 0.5f * (dev[n / 2 - 1] + dev[n / 2]);
            const float ceiling = std::max(0.25f, std::max(4.0f * median, median + 6.0f * mad));
            const float lum = luminance(&rgb[ti * 3]);
            if (lum > ceiling && lum > 0.0f) {
                const float k = ceiling / lum;
                out[ti * 3] = rgb[ti * 3] * k;
                out[ti * 3 + 1] = rgb[ti * 3 + 1] * k;
                out[ti * 3 + 2] = rgb[ti * 3 + 2] * k;
            }
        }
    }
    rgb.swap(out);
}

// Variance-guided a-trous wavelet denoise (3 iterations, steps 1/2/4), guided
// by normal agreement, world distance and chart identity. The luminance term
// is measured against the two texels' own noise (the variance of their means,
// propagated through each iteration): a difference within ~2 sigma blends, a
// real gradient or shadow edge does not. That is what keeps the filter from
// biasing values at chart borders, where the kernel support is one-sided —
// a magnitude-relative term there drags the border toward the chart interior
// and shows up as a step across the seam (gi_bake_tests' vault fixture).
void denoise_pass(const Raster& r, float texel_m, std::vector<float>& rgb, std::vector<float>& var) {
    const int w = int(r.w), h = int(r.h);
    static const float kKernel[5] = {1.0f / 16.0f, 1.0f / 4.0f, 3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 16.0f};
    std::vector<float> ping = rgb, pong(rgb.size());
    std::vector<float> vping = var, vpong(var.size());
    for (int iter = 0; iter < 3; ++iter) {
        const int step = 1 << iter;
        const float sigma_p = 2.0f * float(step) * std::max(texel_m, 1e-4f);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t ti = size_t(y) * w + x;
                const int32_t ri = r.index[ti];
                if (ri < 0) {
                    pong[ti * 3] = ping[ti * 3]; pong[ti * 3 + 1] = ping[ti * 3 + 1]; pong[ti * 3 + 2] = ping[ti * 3 + 2];
                    vpong[ti] = vping[ti];
                    continue;
                }
                const TexelRec& c = r.recs[ri];
                const float lc = luminance(&ping[ti * 3]);
                const float floor2 = (0.005f * lc) * (0.005f * lc);   // 0.5% is never a visible step
                float acc[3] = {0.0f, 0.0f, 0.0f};
                float wsum = 0.0f, w2var = 0.0f;
                // A tap resolves to a same-chart covered texel, or is dropped.
                auto resolve = [&](int qx, int qy, size_t& qi) -> const TexelRec* {
                    if (qx < 0 || qy < 0 || qx >= w || qy >= h) return nullptr;
                    qi = size_t(qy) * w + qx;
                    const int32_t qr = r.index[qi];
                    if (qr < 0) return nullptr;
                    const TexelRec& q = r.recs[qr];
                    return q.chart == c.chart ? &q : nullptr;
                };
                for (int ky = 0; ky < 5; ++ky) {
                    for (int kx = 0; kx < 5; ++kx) {
                        const int ox = (kx - 2) * step, oy = (ky - 2) * step;
                        if (!ox && !oy) {
                            const float wgt = kKernel[kx] * kKernel[ky];
                            acc[0] += ping[ti * 3] * wgt; acc[1] += ping[ti * 3 + 1] * wgt; acc[2] += ping[ti * 3 + 2] * wgt;
                            wsum += wgt; w2var += wgt * wgt * vping[ti];
                            continue;
                        }
                        size_t qi = 0;
                        const TexelRec* q = resolve(x + ox, y + oy, qi);
                        float val[3];
                        if (q) {
                            val[0] = ping[qi * 3]; val[1] = ping[qi * 3 + 1]; val[2] = ping[qi * 3 + 2];
                        } else {
                            // Outside the chart (or an empty texel): odd-reflect the
                            // opposite tap, 2*centre - mirror, so the support stays
                            // symmetric and a linear gradient filters to itself
                            // instead of being dragged toward the chart interior.
                            q = resolve(x - ox, y - oy, qi);
                            if (!q) continue;
                            for (int ch = 0; ch < 3; ++ch)
                                val[ch] = std::max(0.0f, 2.0f * ping[ti * 3 + ch] - ping[qi * 3 + ch]);
                        }
                        const float ndot = std::max(0.0f, dot(c.nrm, q->nrm));
                        float wn = ndot * ndot; wn *= wn; wn *= wn; wn *= wn;   // ^16
                        const float3 dp = q->pos - c.pos;
                        const float wp = std::exp(-dot(dp, dp) / (sigma_p * sigma_p));
                        const float lq = luminance(val);
                        const float dl = lq - lc;
                        const float sigma2 = vping[ti] + vping[qi] + floor2;
                        const float wl = std::exp(-(dl * dl) / (8.0f * sigma2 + 1e-12f));
                        const float wgt = kKernel[kx] * kKernel[ky] * wn * wp * wl;
                        acc[0] += val[0] * wgt;
                        acc[1] += val[1] * wgt;
                        acc[2] += val[2] * wgt;
                        wsum += wgt;
                        w2var += wgt * wgt * vping[qi];
                    }
                }
                if (wsum > 0.0f) {
                    pong[ti * 3] = acc[0] / wsum; pong[ti * 3 + 1] = acc[1] / wsum; pong[ti * 3 + 2] = acc[2] / wsum;
                    vpong[ti] = w2var / (wsum * wsum);
                } else {
                    pong[ti * 3] = ping[ti * 3]; pong[ti * 3 + 1] = ping[ti * 3 + 1]; pong[ti * 3 + 2] = ping[ti * 3 + 2];
                    vpong[ti] = vping[ti];
                }
            }
        }
        ping.swap(pong);
        vping.swap(vpong);
    }
    rgb.swap(ping);
    var.swap(vping);
}

// Chart-bounded dilation: an empty texel inside a chart's outer rect that
// touches a filled texel of that chart takes the mean of those neighbours.
// Runs `rounds` times so content reaches `rounds` texels into the gutter.
void dilate_pass(const Raster& r, uint32_t rounds, std::vector<float>& rgb,
                 std::vector<uint8_t>& coverage, std::vector<float>* albedo) {
    const int w = int(r.w), h = int(r.h);
    std::vector<uint32_t> chart_of(size_t(w) * h, ~0u);
    for (size_t i = 0; i < chart_of.size(); ++i)
        if (r.index[i] >= 0) chart_of[i] = r.recs[r.index[i]].chart;
    for (uint32_t round = 0; round < rounds; ++round) {
        std::vector<uint8_t> next_cov = coverage;
        std::vector<float> next_rgb = rgb;
        std::vector<float> next_alb = albedo ? *albedo : std::vector<float>();
        std::vector<uint32_t> next_chart = chart_of;
        bool any = false;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t ti = size_t(y) * w + x;
                if (coverage[ti] != kTexelEmpty) continue;
                const uint32_t rc = r.rect_chart[ti];
                if (rc == ~0u) continue;
                float acc[3] = {0, 0, 0}, alb[3] = {0, 0, 0};
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy) continue;
                        const int qx = x + dx, qy = y + dy;
                        if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
                        const size_t qi = size_t(qy) * w + qx;
                        if (coverage[qi] == kTexelEmpty || chart_of[qi] != rc) continue;
                        acc[0] += rgb[qi * 3]; acc[1] += rgb[qi * 3 + 1]; acc[2] += rgb[qi * 3 + 2];
                        if (albedo) { alb[0] += (*albedo)[qi * 3]; alb[1] += (*albedo)[qi * 3 + 1]; alb[2] += (*albedo)[qi * 3 + 2]; }
                        ++n;
                    }
                }
                if (!n) continue;
                const float inv = 1.0f / float(n);
                next_rgb[ti * 3] = acc[0] * inv; next_rgb[ti * 3 + 1] = acc[1] * inv; next_rgb[ti * 3 + 2] = acc[2] * inv;
                if (albedo) { next_alb[ti * 3] = alb[0] * inv; next_alb[ti * 3 + 1] = alb[1] * inv; next_alb[ti * 3 + 2] = alb[2] * inv; }
                next_cov[ti] = kTexelDilated;
                next_chart[ti] = rc;
                any = true;
            }
        }
        coverage.swap(next_cov);
        rgb.swap(next_rgb);
        if (albedo) albedo->swap(next_alb);
        chart_of.swap(next_chart);
        if (!any) break;
    }
}

// ---------------------------------------------------------------------------
// Hash helpers.
// ---------------------------------------------------------------------------
inline uint64_t mix_f(uint64_t h, float f) {
    uint32_t bits; std::memcpy(&bits, &f, 4);
    if (bits == 0x80000000u) bits = 0;   // -0 == +0
    return fnv1a64(&bits, 4, h);
}
inline uint64_t mix_u(uint64_t h, uint64_t v) { return fnv1a64(&v, 8, h); }

std::string hex16(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// Public hashing.
// ---------------------------------------------------------------------------
uint64_t fnv1a64(const void* data, size_t len, uint64_t seed) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

uint64_t hash_settings(const Settings& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    h = mix_u(h, kGiBakeVersion);
    h = mix_u(h, s.samples); h = mix_u(h, s.bounces);
    h = mix_f(h, s.texel_density); h = mix_u(h, s.seed);
    h = mix_u(h, s.firefly_filter ? 1 : 0); h = mix_u(h, s.denoise ? 1 : 0);
    h = mix_u(h, s.dilate_texels); h = mix_u(h, s.prelit ? 1 : 0);
    h = mix_u(h, s.max_atlas_texels); h = mix_f(h, s.cone_deg);
    return h;
}

uint64_t hash_lighting(const Lighting& l) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 3; ++i) h = mix_f(h, l.sun_direction[i]);
    for (int i = 0; i < 3; ++i) h = mix_f(h, l.sun_color[i]);
    for (int i = 0; i < 3; ++i) h = mix_f(h, l.sky_color[i]);
    h = mix_f(h, l.sun_angular_diameter_deg);
    return h;
}

uint64_t hash_part(const Part& p) {
    if (p.hash != 0) return p.hash;
    uint64_t h = 0xcbf29ce484222325ull;
    for (const Tri& t : p.tris) {
        h = mix_f(h, t.vertex0.x); h = mix_f(h, t.vertex0.y); h = mix_f(h, t.vertex0.z);
        h = mix_f(h, t.vertex1.x); h = mix_f(h, t.vertex1.y); h = mix_f(h, t.vertex1.z);
        h = mix_f(h, t.vertex2.x); h = mix_f(h, t.vertex2.y); h = mix_f(h, t.vertex2.z);
    }
    for (const TriEx& e : p.triex) {
        h = mix_u(h, uint64_t(uint32_t(e.materialId)));
        h = mix_f(h, e.tint.x); h = mix_f(h, e.tint.y); h = mix_f(h, e.tint.z); h = mix_f(h, e.tint.w);
        h = mix_f(h, e.N0.x); h = mix_f(h, e.N0.y); h = mix_f(h, e.N0.z);
        h = mix_f(h, e.N1.x); h = mix_f(h, e.N1.y); h = mix_f(h, e.N1.z);
        h = mix_f(h, e.N2.x); h = mix_f(h, e.N2.y); h = mix_f(h, e.N2.z);
    }
    return h == 0 ? 1 : h;
}

uint64_t hash_placement(const Instance& i) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (int k = 0; k < 16; ++k) h = mix_f(h, i.transform[k]);
    return h;
}

uint64_t hash_scene(const Scene& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    std::vector<uint64_t> part_hashes;
    part_hashes.reserve(s.parts.size());
    for (const Part& p : s.parts) part_hashes.push_back(hash_part(p));
    for (const Instance& i : s.instances) {
        h = mix_u(h, i.part < part_hashes.size() ? part_hashes[i.part] : 0);
        h = mix_u(h, hash_placement(i));
    }
    return h;
}

std::string cache_key(const Scene& s, uint32_t instance, const Settings& st, const Lighting& l) {
    const Instance& i = s.instances[instance];
    return "gi" + std::to_string(kGiBakeVersion) +
           "|part=" + hex16(hash_part(s.parts[i.part])) +
           "|inst=" + hex16(hash_placement(i)) +
           "|scene=" + hex16(hash_scene(s)) +
           "|light=" + hex16(hash_lighting(l)) +
           "|set=" + hex16(hash_settings(st));
}

void mul_transform(const float a[16], const float b[16], float out[16]) {
    float tmp[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            tmp[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
    std::memcpy(out, tmp, sizeof tmp);
}

// ---------------------------------------------------------------------------
// Atlas.
// ---------------------------------------------------------------------------
// Same segmentation and planar projection as lod_bake::build_chart_rung (the
// renderer's chart-space VT), but a COMPACT pack: the VT packer page-aligns
// every chart to 128-texel pages because its pool is paged, which turns a
// rock's forty tiny charts into a 3-megatexel atlas that is 0.2 % covered. A
// lightmap has no pages, so charts are shelf-packed with just the gutter.
// The chart_atlas.h mapping (rect + gutter + (p.T - o.T) * tpm) is preserved
// exactly, so the same ChartEntry table describes the texels.
bool build_atlas(Part& part, const Settings& s, PartAtlas& out, std::string& err) {
    namespace mc = mesh_charting;
    out = PartAtlas{};
    if (part.tris.empty()) { err = "part '" + part.name + "' has no triangles"; return false; }
    if (part.triex.size() != part.tris.size()) {
        err = "part '" + part.name + "': triex is not parallel to tris";
        return false;
    }
    if (!(s.cone_deg > 0.0f) || s.cone_deg >= 90.0f) { err = "cone_deg must be in (0, 90)"; return false; }
    const int n = int(part.tris.size());

    std::vector<float> pos(size_t(n) * 9);
    std::vector<unsigned int> idx(size_t(n) * 3);
    for (int t = 0; t < n; ++t) {
        const float3* v[3] = {&part.tris[t].vertex0, &part.tris[t].vertex1, &part.tris[t].vertex2};
        for (int k = 0; k < 3; ++k) {
            const size_t corner = size_t(t) * 3 + k;
            pos[corner * 3 + 0] = v[k]->x; pos[corner * 3 + 1] = v[k]->y; pos[corner * 3 + 2] = v[k]->z;
            idx[corner] = (unsigned int)corner;
        }
    }
    const auto adj = mc::build_adjacency(pos.data(), idx.data(), n);
    int n_charts = 0;
    const auto cid = mc::segment_charts(pos.data(), idx.data(), n, adj, s.cone_deg, n_charts);
    if (n_charts <= 0) { err = "part '" + part.name + "': chart segmentation produced no charts"; return false; }
    const auto normals = mc::chart_average_normals(pos.data(), idx.data(), n, cid, n_charts);
    std::vector<float> T(size_t(n_charts) * 3), B(size_t(n_charts) * 3);
    for (int c = 0; c < n_charts; ++c)
        mc::plane_basis(&normals[size_t(c) * 3], &T[size_t(c) * 3], &B[size_t(c) * 3]);

    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> minU(size_t(n_charts), inf), minV(size_t(n_charts), inf);
    std::vector<float> maxU(size_t(n_charts), -inf), maxV(size_t(n_charts), -inf);
    auto project = [&](int c, const float3& p, float& u, float& v) {
        const float* tc = &T[size_t(c) * 3];
        const float* bc = &B[size_t(c) * 3];
        u = p.x * tc[0] + p.y * tc[1] + p.z * tc[2];
        v = p.x * bc[0] + p.y * bc[1] + p.z * bc[2];
    };
    for (int t = 0; t < n; ++t) {
        const int c = cid[t];
        const float3* v[3] = {&part.tris[t].vertex0, &part.tris[t].vertex1, &part.tris[t].vertex2};
        for (int k = 0; k < 3; ++k) {
            float u, w; project(c, *v[k], u, w);
            minU[c] = std::min(minU[c], u); maxU[c] = std::max(maxU[c], u);
            minV[c] = std::min(minV[c], w); maxV[c] = std::max(maxV[c], w);
        }
    }

    // Compact deterministic shelf pack at the requested density, halving the
    // density until the atlas fits Settings::max_atlas_texels.
    const int gutter = int(chart_atlas::kChartGutterTexels);
    const uint64_t max_dim_u = uint64_t(std::sqrt(double(std::max<uint64_t>(s.max_atlas_texels, 4096))));
    const int max_dim = int(std::min<uint64_t>(max_dim_u, 16384));
    float density = std::max(s.texel_density, 1.0f / 64.0f);
    const size_t chart_count = static_cast<size_t>(n_charts);
    std::vector<int> bw(chart_count), bh(chart_count), px(chart_count), py(chart_count);
    int atlas_w = 0, atlas_h = 0;
    bool packed = false;
    while (density >= 1.0f / 64.0f) {
        uint64_t area = 0;
        int widest = 0;
        for (int c = 0; c < n_charts; ++c) {
            bw[c] = std::max(1, int(std::ceil(double(maxU[c] - minU[c]) * density))) + 2 * gutter;
            bh[c] = std::max(1, int(std::ceil(double(maxV[c] - minV[c]) * density))) + 2 * gutter;
            area += uint64_t(bw[c]) * bh[c];
            widest = std::max(widest, bw[c]);
        }
        int W = 16;
        while (uint64_t(W) * W < uint64_t(double(area) * 1.25) && W < max_dim) W *= 2;
        W = std::max(W, widest);
        if (W <= max_dim) {
            std::vector<int> order(chart_count);
            for (int c = 0; c < n_charts; ++c) order[c] = c;
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                if (bh[a] != bh[b]) return bh[a] > bh[b];
                if (bw[a] != bw[b]) return bw[a] > bw[b];
                return a < b;
            });
            int x = 0, y = 0, shelf = 0;
            for (int c : order) {
                if (x + bw[c] > W) { y += shelf; x = 0; shelf = 0; }
                px[c] = x; py[c] = y;
                x += bw[c];
                shelf = std::max(shelf, bh[c]);
            }
            atlas_w = W;
            atlas_h = y + shelf;
            if (uint64_t(atlas_w) * atlas_h <= s.max_atlas_texels && atlas_h <= max_dim) { packed = true; break; }
        }
        density *= 0.5f;
    }
    if (!packed) { err = "part '" + part.name + "': chart atlas does not fit max_atlas_texels"; return false; }

    chart_atlas::ChartAtlasRung& rung = out.rung;
    rung.atlas_w = uint32_t(atlas_w);
    rung.atlas_h = uint32_t(atlas_h);
    std::vector<uint32_t> first(size_t(n_charts), 0), count(size_t(n_charts), 0);
    for (int t = 0; t < n; ++t) count[cid[t]]++;
    uint32_t running = 0;
    for (int c = 0; c < n_charts; ++c) { first[c] = running; running += count[c]; }
    rung.tri_order.resize(size_t(n));
    {
        std::vector<uint32_t> cursor = first;
        for (int t = 0; t < n; ++t) rung.tri_order[cursor[cid[t]]++] = uint32_t(t);
    }
    rung.charts.resize(size_t(n_charts));
    for (int c = 0; c < n_charts; ++c) {
        chart_atlas::ChartEntry& e = rung.charts[c];
        const float* tc = &T[size_t(c) * 3];
        const float* bc = &B[size_t(c) * 3];
        for (int i = 0; i < 3; ++i) {
            e.origin[i] = minU[c] * tc[i] + minV[c] * bc[i];
            e.tangent[i] = tc[i];
            e.bitangent[i] = bc[i];
        }
        e.rect_x = uint32_t(px[c]); e.rect_y = uint32_t(py[c]);
        e.rect_w = uint32_t(bw[c]); e.rect_h = uint32_t(bh[c]);
        e.texels_per_meter = density;
        e.first_tri = first[c];
        e.tri_count = count[c];
    }
    const float inv_w = 1.0f / float(atlas_w), inv_h = 1.0f / float(atlas_h);
    for (int t = 0; t < n; ++t) {
        const int c = cid[t];
        const float3* v[3] = {&part.tris[t].vertex0, &part.tris[t].vertex1, &part.tris[t].vertex2};
        float2* uv[3] = {&part.triex[t].uv0, &part.triex[t].uv1, &part.triex[t].uv2};
        for (int k = 0; k < 3; ++k) {
            float u, w; project(c, *v[k], u, w);
            const float tx = float(px[c] + gutter) + (u - minU[c]) * density;
            const float ty = float(py[c] + gutter) + (w - minV[c]) * density;
            uv[k]->x = tx * inv_w;
            uv[k]->y = ty * inv_h;
        }
    }

    out.width = rung.atlas_w;
    out.height = rung.atlas_h;
    out.texels_per_meter = density;
    out.tri_chart.assign(size_t(n), 0);
    for (int t = 0; t < n; ++t) out.tri_chart[t] = uint32_t(cid[t]);
    out.ok = out.width > 0 && out.height > 0;
    if (!out.ok) err = "part '" + part.name + "': empty atlas";
    return out.ok;
}

// ---------------------------------------------------------------------------
// The bake.
// ---------------------------------------------------------------------------
bool bake_scene(Scene& scene, const Settings& s, const Lighting& l, BakeResult& out,
                std::string& err, const ProgressFn& progress, const CacheHooks* cache) {
    out = BakeResult{};
    err.clear();
    if (scene.instances.empty()) { err = "scene has no instances"; return false; }
    if (s.samples == 0) { err = "samples must be >= 1"; return false; }
    for (const Instance& i : scene.instances)
        if (i.part >= scene.parts.size()) { err = "instance '" + i.name + "' references a missing part"; return false; }

    out.settings_hash = hash_settings(s);
    out.lighting_hash = hash_lighting(l);
    out.scene_hash = hash_scene(scene);

    // Atlases (one per part; instances of the same part share it).
    out.atlases.resize(scene.parts.size());
    for (uint32_t p = 0; p < scene.parts.size(); ++p) {
        if (progress) progress(Progress{p, "atlas", p, uint32_t(scene.parts.size())});
        if (!build_atlas(scene.parts[p], s, out.atlases[p], err)) return false;
    }

    // Scene tracer over every instance, borrowing the parts' BLAS entries.
    std::unordered_map<uint64_t, uint32_t> part_by_hash;
    for (uint32_t p = 0; p < scene.parts.size(); ++p) part_by_hash.emplace(hash_part(scene.parts[p]), p);
    world_tracer::WorldTracer tracer;
    tracer.set_resident_source([&](uint64_t hash, world_tracer::ResidentPart& rp) {
        auto it = part_by_hash.find(hash);
        if (it == part_by_hash.end()) return false;
        rp.entries = scene.parts[it->second].entries;
        rp.children = nullptr;
        rp.expand_children = false;
        return !rp.entries.empty();
    });
    std::vector<world_tracer::TraceInstance> trace_instances;
    trace_instances.reserve(scene.instances.size());
    float max_coord = 1.0f;
    for (const Instance& i : scene.instances) {
        world_tracer::TraceInstance ti;
        ti.part_hash = hash_part(scene.parts[i.part]);
        std::memcpy(ti.transform, i.transform, sizeof ti.transform);
        trace_instances.push_back(ti);
        max_coord = std::max(max_coord, std::max(std::fabs(i.transform[3]),
                             std::max(std::fabs(i.transform[7]), std::fabs(i.transform[11]))));
    }
    {
        std::string terr;
        if (!tracer.build(std::string(), trace_instances, terr)) {
            err = "tracer build failed: " + terr;
            return false;
        }
    }
    for (const Part& p : scene.parts)
        for (const Tri& t : p.tris) {
            max_coord = std::max(max_coord, std::fabs(t.vertex0.x));
            max_coord = std::max(max_coord, std::fabs(t.vertex0.y));
            max_coord = std::max(max_coord, std::fabs(t.vertex0.z));
        }

    const uint32_t thread_count = s.threads ? s.threads
        : std::max(1u, std::thread::hardware_concurrency());
    out.lightmaps.resize(scene.instances.size());

    for (uint32_t ii = 0; ii < scene.instances.size(); ++ii) {
        const Instance& inst = scene.instances[ii];
        const Part& part = scene.parts[inst.part];
        const PartAtlas& atlas = out.atlases[inst.part];
        Lightmap& map = out.lightmaps[ii];

        const std::string key = cache_key(scene, ii, s, l);
        if (cache && cache->lookup) {
            Lightmap cached;
            if (cache->lookup(key, cached) && cached.width == atlas.width &&
                cached.height == atlas.height && cached.rgb.size() == size_t(atlas.width) * atlas.height * 3) {
                map = std::move(cached);
                map.cache_hit = true;
                if (progress) progress(Progress{ii, "trace", 1, 1});
                continue;
            }
        }

        if (progress) progress(Progress{ii, "raster", 0, 1});
        Raster raster;
        rasterize_instance(part, atlas, inst, raster);

        map.width = atlas.width; map.height = atlas.height;
        const size_t texels = size_t(map.width) * map.height;
        map.rgb.assign(texels * 3, 0.0f);
        map.coverage.assign(texels, kTexelEmpty);
        if (s.prelit) map.albedo.assign(texels * 3, 0.0f);
        map.covered_texels = uint32_t(raster.recs.size());
        for (size_t ti = 0; ti < texels; ++ti) {
            const int32_t ri = raster.index[ti];
            if (ri < 0) continue;
            map.coverage[ti] = kTexelCovered;
            if (s.prelit) {
                map.albedo[ti * 3] = raster.recs[ri].albedo.x;
                map.albedo[ti * 3 + 1] = raster.recs[ri].albedo.y;
                map.albedo[ti * 3 + 2] = raster.recs[ri].albedo.z;
            }
        }

        // Trace.
        const float texel_m = atlas.texels_per_meter > 0.0f ? 1.0f / atlas.texels_per_meter : 0.1f;
        TraceCtx ctx;
        ctx.tracer = &tracer;
        ctx.sun = make_sun(l);
        ctx.samples = s.samples;
        ctx.bounces = s.bounces;
        ctx.eps = std::max(1e-4f, std::max(1e-3f * texel_m, 2e-6f * max_coord));
        ctx.seed = s.seed ^ (hash_part(part) * 0x9E3779B97F4A7C15ull) ^ hash_placement(inst);

        const auto t0 = std::chrono::steady_clock::now();
        std::vector<float> variance(texels, 0.0f);
        std::atomic<uint32_t> next_row{0};
        std::atomic<uint64_t> done_texels{0};
        std::atomic<uint64_t> rays_total{0};
        auto worker = [&]() {
            uint64_t rays = 0;
            for (;;) {
                const uint32_t y = next_row.fetch_add(1);
                if (y >= map.height) break;
                uint64_t row_done = 0;
                for (uint32_t x = 0; x < map.width; ++x) {
                    const size_t ti = size_t(y) * map.width + x;
                    const int32_t ri = raster.index[ti];
                    if (ri < 0) continue;
                    Rng rng(ctx.seed ^ (uint64_t(ti) * 0xD6E8FEB86659FD93ull));
                    float var = 0.0f;
                    const float3 v = trace_texel(ctx, raster.recs[ri], rng, rays, var);
                    map.rgb[ti * 3] = v.x; map.rgb[ti * 3 + 1] = v.y; map.rgb[ti * 3 + 2] = v.z;
                    variance[ti] = var;
                    ++row_done;
                }
                done_texels.fetch_add(row_done);
            }
            rays_total.fetch_add(rays);
        };
        std::vector<std::thread> pool;
        const uint32_t workers = std::max(1u, std::min(thread_count, map.height));
        for (uint32_t t = 0; t < workers; ++t) pool.emplace_back(worker);
        if (progress) {
            const uint64_t total = raster.recs.size();
            uint64_t last = ~0ull;
            for (;;) {
                const uint64_t done = done_texels.load();
                if (done != last) { progress(Progress{ii, "trace", done, total}); last = done; }
                if (next_row.load() >= map.height + workers) break;   // every worker exited its loop
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        for (std::thread& t : pool) t.join();
        if (progress) progress(Progress{ii, "trace", raster.recs.size(), raster.recs.size()});
        map.rays = rays_total.load();
        map.trace_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        // Post passes.
        if (progress) progress(Progress{ii, "filter", 0, 3});
        if (s.firefly_filter) firefly_pass(raster, map.rgb);
        if (progress) progress(Progress{ii, "filter", 1, 3});
        if (s.denoise) denoise_pass(raster, texel_m, map.rgb, variance);
        if (progress) progress(Progress{ii, "filter", 2, 3});
        dilate_pass(raster, s.dilate_texels, map.rgb, map.coverage, s.prelit ? &map.albedo : nullptr);
        if (progress) progress(Progress{ii, "filter", 3, 3});

        map.content_hash = fnv1a64(map.rgb.data(), map.rgb.size() * sizeof(float));
        if (cache && cache->store) cache->store(key, map);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Sampling.
// ---------------------------------------------------------------------------
bool sample_at(const Scene& scene, const BakeResult& r, uint32_t instance,
               const float world_point[3], float out_rgb[3], uint32_t* out_chart) {
    if (instance >= scene.instances.size() || instance >= r.lightmaps.size()) return false;
    const Instance& inst = scene.instances[instance];
    const Part& part = scene.parts[inst.part];
    const PartAtlas& atlas = r.atlases[inst.part];
    const Lightmap& map = r.lightmaps[instance];
    if (!atlas.ok || map.rgb.empty()) return false;

    // World -> part-local through the full inverse (affine).
    const float* m = inst.transform;
    float inv[16];
    {
        // Gauss-Jordan on a 4x4 copy; instance transforms are affine and well
        // conditioned in practice, and a singular one simply fails the sample.
        float a[4][8];
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { a[i][j] = m[i * 4 + j]; a[i][j + 4] = (i == j) ? 1.0f : 0.0f; }
        for (int col = 0; col < 4; ++col) {
            int piv = col;
            for (int i = col + 1; i < 4; ++i) if (std::fabs(a[i][col]) > std::fabs(a[piv][col])) piv = i;
            if (std::fabs(a[piv][col]) < 1e-12f) return false;
            if (piv != col) for (int j = 0; j < 8; ++j) std::swap(a[piv][j], a[col][j]);
            const float s = 1.0f / a[col][col];
            for (int j = 0; j < 8; ++j) a[col][j] *= s;
            for (int i = 0; i < 4; ++i) {
                if (i == col) continue;
                const float f = a[i][col];
                if (f == 0.0f) continue;
                for (int j = 0; j < 8; ++j) a[i][j] -= f * a[col][j];
            }
        }
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) inv[i * 4 + j] = a[i][j + 4];
    }
    const float3 lp = xform_point(inv, make_float3(world_point[0], world_point[1], world_point[2]));

    const float fw = float(atlas.width), fh = float(atlas.height);
    // Search charts whose plane projection of lp lands inside a member triangle.
    float best_d2 = std::numeric_limits<float>::infinity();
    float best_u = 0.0f, best_v = 0.0f;
    uint32_t best_chart = ~0u;
    for (uint32_t c = 0; c < atlas.rung.charts.size(); ++c) {
        const chart_atlas::ChartEntry& e = atlas.rung.charts[c];
        const float3 T = make_float3(e.tangent[0], e.tangent[1], e.tangent[2]);
        const float3 B = make_float3(e.bitangent[0], e.bitangent[1], e.bitangent[2]);
        const float3 O = make_float3(e.origin[0], e.origin[1], e.origin[2]);
        const float px = float(e.rect_x + chart_atlas::kChartGutterTexels) + (dot(lp, T) - dot(O, T)) * e.texels_per_meter;
        const float py = float(e.rect_y + chart_atlas::kChartGutterTexels) + (dot(lp, B) - dot(O, B)) * e.texels_per_meter;
        for (uint32_t k = e.first_tri; k < e.first_tri + e.tri_count && k < atlas.rung.tri_order.size(); ++k) {
            const uint32_t t = atlas.rung.tri_order[k];
            const TriEx& ex = part.triex[t];
            const float a[2] = {ex.uv0.x * fw, ex.uv0.y * fh};
            const float b[2] = {ex.uv1.x * fw, ex.uv1.y * fh};
            const float cc[2] = {ex.uv2.x * fw, ex.uv2.y * fh};
            const float p[2] = {px, py};
            const float area = (b[0] - a[0]) * (cc[1] - a[1]) - (b[1] - a[1]) * (cc[0] - a[0]);
            if (!(std::fabs(area) > 1e-12f)) continue;
            // Only triangles that actually contain the 3D point: check the
            // out-of-plane distance too, so a chart whose plane happens to
            // project the point inside another surface's triangle is rejected.
            const Tri& tri = part.tris[t];
            const float3 n = safe_normalize(cross(tri.vertex1 - tri.vertex0, tri.vertex2 - tri.vertex0), make_float3(0, 1, 0));
            const float off = std::fabs(dot(lp - tri.vertex0, n));
            float bary[3]; float d2 = 0.0f;
            closest_bary(a, b, cc, p, bary, d2);
            const float inv_area = 1.0f / area;
            const float w0 = ((b[0] - p[0]) * (cc[1] - p[1]) - (b[1] - p[1]) * (cc[0] - p[0])) * inv_area;
            const float w1 = ((cc[0] - p[0]) * (a[1] - p[1]) - (cc[1] - p[1]) * (a[0] - p[0])) * inv_area;
            const float w2 = 1.0f - w0 - w1;
            const bool inside = w0 >= -1e-4f && w1 >= -1e-4f && w2 >= -1e-4f;
            const float score = (inside ? 0.0f : d2) + off * off * (e.texels_per_meter * e.texels_per_meter);
            if (score < best_d2 && (inside || d2 <= 1.0f)) {
                best_d2 = score; best_u = px; best_v = py; best_chart = c;
            }
        }
    }
    if (best_chart == ~0u) return false;
    if (out_chart) *out_chart = best_chart;

    // Bilinear over texel centres.
    const float x = std::min(std::max(best_u - 0.5f, 0.0f), fw - 1.0f);
    const float y = std::min(std::max(best_v - 0.5f, 0.0f), fh - 1.0f);
    const uint32_t x0 = uint32_t(x), y0 = uint32_t(y);
    const uint32_t x1 = std::min(x0 + 1, atlas.width - 1), y1 = std::min(y0 + 1, atlas.height - 1);
    const float fx = x - float(x0), fy = y - float(y0);
    for (int ch = 0; ch < 3; ++ch) {
        const float v00 = map.rgb[(size_t(y0) * atlas.width + x0) * 3 + ch];
        const float v10 = map.rgb[(size_t(y0) * atlas.width + x1) * 3 + ch];
        const float v01 = map.rgb[(size_t(y1) * atlas.width + x0) * 3 + ch];
        const float v11 = map.rgb[(size_t(y1) * atlas.width + x1) * 3 + ch];
        out_rgb[ch] = (v00 * (1 - fx) + v10 * fx) * (1 - fy) + (v01 * (1 - fx) + v11 * fx) * fy;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Serialization.
// ---------------------------------------------------------------------------
namespace {
constexpr char kMagic[4] = {'G', 'I', 'L', 'M'};
constexpr uint32_t kBlobVersion = 1;
template <class T> void put(std::vector<uint8_t>& out, const T& v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof v);
}
template <class T> bool get(const uint8_t*& p, const uint8_t* end, T& v) {
    if (size_t(end - p) < sizeof v) return false;
    std::memcpy(&v, p, sizeof v); p += sizeof v; return true;
}
}

std::vector<uint8_t> serialize_lightmap(const Lightmap& map) {
    std::vector<uint8_t> out;
    out.insert(out.end(), kMagic, kMagic + 4);
    put(out, kBlobVersion);
    put(out, map.width); put(out, map.height);
    put(out, uint32_t(map.albedo.empty() ? 0 : 1));
    put(out, map.covered_texels);
    put(out, map.rays);
    put(out, map.content_hash);
    const size_t texels = size_t(map.width) * map.height;
    const uint8_t* rgb = reinterpret_cast<const uint8_t*>(map.rgb.data());
    out.insert(out.end(), rgb, rgb + texels * 3 * sizeof(float));
    out.insert(out.end(), map.coverage.begin(), map.coverage.end());
    if (!map.albedo.empty()) {
        const uint8_t* alb = reinterpret_cast<const uint8_t*>(map.albedo.data());
        out.insert(out.end(), alb, alb + texels * 3 * sizeof(float));
    }
    return out;
}

bool deserialize_lightmap(const uint8_t* data, size_t len, Lightmap& out) {
    const uint8_t* p = data;
    const uint8_t* end = data + len;
    if (len < 4 || std::memcmp(p, kMagic, 4) != 0) return false;
    p += 4;
    uint32_t version = 0, has_albedo = 0;
    Lightmap m;
    if (!get(p, end, version) || version != kBlobVersion) return false;
    if (!get(p, end, m.width) || !get(p, end, m.height) || !get(p, end, has_albedo) ||
        !get(p, end, m.covered_texels) || !get(p, end, m.rays) || !get(p, end, m.content_hash))
        return false;
    const size_t texels = size_t(m.width) * m.height;
    if (texels == 0 || texels > (1ull << 28)) return false;
    const size_t need = texels * 3 * sizeof(float) + texels + (has_albedo ? texels * 3 * sizeof(float) : 0);
    if (size_t(end - p) != need) return false;
    m.rgb.resize(texels * 3);
    std::memcpy(m.rgb.data(), p, texels * 3 * sizeof(float)); p += texels * 3 * sizeof(float);
    m.coverage.assign(p, p + texels); p += texels;
    if (has_albedo) { m.albedo.resize(texels * 3); std::memcpy(m.albedo.data(), p, texels * 3 * sizeof(float)); }
    if (fnv1a64(m.rgb.data(), m.rgb.size() * sizeof(float)) != m.content_hash) return false;
    out = std::move(m);
    return true;
}

} // namespace gi_bake
