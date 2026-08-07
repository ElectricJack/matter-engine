// lod_normal_consistency — mesh rung N vs mesh rung 0 shading-normal gate.
// Born from issue 7b64dc27 (interleaved pale/dark tree populations, all MESH
// instances). NOTE: the user-visible HUE split there measured out as an
// ALBEDO difference (present in the unlit G-buffer, Pearson 0.96 lit-vs-
// albedo), which is tracked separately; what this suite gates is the real
// normals defect found alongside it — coarse rungs of faceted authored
// geometry losing the faceted shading character.
//
// THE CONTRACT UNDER TEST: every rung of an LOD ladder must inherit the
// SOURCE'S SHADING CHARACTER and shade alike:
//   * a faceted authored field (DSL triangle_emit: N0=N1=N2 = geometric face
//     normal — every beginShape(SHAPE.triangles) canopy) stays FACETED on
//     every rung, and the rung's mean lit brightness stays with rep 0's;
//   * a smooth authored field (mesher SDF-gradient normals) stays smooth
//     (issue ef7053be: recomputing smooth normals melted box edges — the
//     guard for that lives here too, as the subdivided-cube case).
//
// WHY THIS EXISTED AS A BUG: reproject_triex(SampleSource) samples the
// authored normal field at each target CORNER and the rasterizer then
// interpolates between the three samples. For a smooth field that
// reconstructs the field; for a piecewise-CONSTANT (faceted) field it
// manufactures a smooth gradient that never existed — every coarse rung of a
// faceted canopy shaded soft and pale next to rep 0's hard dark facets.
//
// MEASUREMENT (shared by both modes):
//   * normal(deg): area-weighted mean angle between the rung's interpolated
//     shading normal and the source's authored normal at the nearest source
//     surface point, sampled over each rung triangle.
//   * lit mean: area-weighted mean of the composite direct+ambient shading
//     factor (unit albedo) under the engine-default sun; the number the
//     screenshot's pale-vs-dark split IS.
//   * facet fraction: fraction of rung triangles whose three corner normals
//     agree (a faceted output triangle).
//
// MODES
//   (no args)      fixture gate: deterministic canopy / cube / ellipsoid
//                  ladders through the production decimate+reproject path.
//                  Exit 1 on CHECK failure.
//   --tree [form]  instrument: installs the world_demo AlpineDeciduous
//                  (default form 0) via the script host, flattens it, loads
//                  the FLAT ladder and prints per-rung numbers. Not a gate.
//
//   make -C MatterEngine3/tests run-lod-normal-consistency

#include "lod_bake.h"
#include "mesh_transform.hpp"
#include "mesh_indexed.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
#define CHECK(cond, msg)                                   \
    do {                                                   \
        if (cond) {                                        \
            printf("  ok: %s\n", msg);                     \
        } else {                                           \
            printf("FAIL: %s\n", msg);                     \
            ++g_failures;                                  \
        }                                                  \
    } while (0)

inline float3 f3(float x, float y, float z) { return make_float3(x, y, z); }
inline float3 sub(const float3& a, const float3& b) { return f3(a.x-b.x, a.y-b.y, a.z-b.z); }
inline float dot3(const float3& a, const float3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline float3 cross3(const float3& a, const float3& b) {
    return f3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
inline float3 unit3(const float3& v) {
    const float l = std::sqrt(dot3(v, v));
    if (!(l > 1e-20f)) return f3(0, 1, 0);
    return f3(v.x/l, v.y/l, v.z/l);
}
inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

float3 face_normal(const Tri& t) {
    return unit3(cross3(sub(t.vertex1, t.vertex0), sub(t.vertex2, t.vertex0)));
}
float tri_area(const Tri& t) {
    const float3 c = cross3(sub(t.vertex1, t.vertex0), sub(t.vertex2, t.vertex0));
    return 0.5f * std::sqrt(dot3(c, c));
}

Tri make_tri(float3 a, float3 b, float3 c) {
    Tri t{};
    std::memset(&t, 0, sizeof(Tri));
    t.vertex0 = a; t.vertex1 = b; t.vertex2 = c;
    t.centroid = f3((a.x+b.x+c.x)/3.f, (a.y+b.y+c.y)/3.f, (a.z+b.z+c.z)/3.f);
    return t;
}

// Closest point on triangle (a,b,c) to p as clamped barycentrics (Ericson
// 5.1.5) — the same construction mesh_transform.cpp uses, copied so the test
// measures against an independent implementation of "the authored field at
// the nearest source point".
void closest_bary(const float3& a, const float3& b, const float3& c,
                  const float3& p, float& u, float& v, float& w) {
    const float3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    const float d1 = dot3(ab, ap), d2 = dot3(ac, ap);
    if (d1 <= 0 && d2 <= 0) { u = 1; v = 0; w = 0; return; }
    const float3 bp = sub(p, b);
    const float d3 = dot3(ab, bp), d4 = dot3(ac, bp);
    if (d3 >= 0 && d4 <= d3) { u = 0; v = 1; w = 0; return; }
    const float vc = d1*d4 - d3*d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        const float t = (d1 - d3) != 0 ? d1/(d1 - d3) : 0;
        u = 1 - t; v = t; w = 0; return;
    }
    const float3 cp = sub(p, c);
    const float d5 = dot3(ab, cp), d6 = dot3(ac, cp);
    if (d6 >= 0 && d5 <= d6) { u = 0; v = 0; w = 1; return; }
    const float vb = d5*d2 - d1*d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        const float t = (d2 - d6) != 0 ? d2/(d2 - d6) : 0;
        u = 1 - t; v = 0; w = t; return;
    }
    const float va = d3*d6 - d5*d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const float den = (d4 - d3) + (d5 - d6);
        const float t = den != 0 ? (d4 - d3)/den : 0;
        u = 0; v = 1 - t; w = t; return;
    }
    const float den = va + vb + vc;
    if (std::fabs(den) < 1e-30f) { u = v = w = 1.f/3.f; return; }
    v = vb/den; w = vc/den; u = 1.f - v - w;
}

// The authored shading normal of (tris,triex) at the point of the set nearest
// to p. Brute force — this is a test, correctness over speed.
float3 source_normal_at(const std::vector<Tri>& tris,
                        const std::vector<TriEx>& triex, const float3& p) {
    float best_d2 = 3.4e38f;
    float3 best_n = f3(0, 1, 0);
    for (size_t i = 0; i < tris.size(); ++i) {
        const Tri& t = tris[i];
        float u, v, w;
        closest_bary(t.vertex0, t.vertex1, t.vertex2, p, u, v, w);
        const float3 q = f3(u*t.vertex0.x + v*t.vertex1.x + w*t.vertex2.x,
                            u*t.vertex0.y + v*t.vertex1.y + w*t.vertex2.y,
                            u*t.vertex0.z + v*t.vertex1.z + w*t.vertex2.z);
        const float3 d = sub(q, p);
        const float d2 = dot3(d, d);
        if (d2 < best_d2) {
            best_d2 = d2;
            const TriEx& e = triex[i];
            best_n = unit3(f3(u*e.N0.x + v*e.N1.x + w*e.N2.x,
                              u*e.N0.y + v*e.N1.y + w*e.N2.y,
                              u*e.N0.z + v*e.N1.z + w*e.N2.z));
        }
    }
    return best_n;
}

// composite.frag's shading factor at unit albedo, RT off: what a pixel with
// this normal brightens to, up to the material constants that are identical
// between two rungs of the same part.
float lit_factor(const float3& n, const float3& to_sun) {
    const float direct = std::max(dot3(n, to_sun), 0.f);
    const float sky_t = 0.2f + 0.8f * clamp01(n.y * 0.5f + 0.5f);
    const float sun_g = 2.05f, sky_g = 0.43f;
    return sky_g * sky_t + sun_g * direct;
}

// Per-rung metrics against a source field.
struct RungMetrics {
    double mean_angle_deg = 0;   // rung normal vs authored source normal
    double lit_mean = 0;         // area-weighted composite factor
    double lit_stddev = 0;       // area-weighted spread — shading CONTRAST
    double facet_fraction = 0;   // triangles with N0==N1==N2
    double own_face_fraction = 0;// triangles whose constant normal is their own
                                 // geometric face normal (rep 0's recipe)
    size_t tris = 0;
};

// Fixed interior barycentric sample set (no corners: a corner sits ON the
// crease between donors and its nearest-source normal is ambiguous).
constexpr float kBary[][3] = {
    {1.f/3, 1.f/3, 1.f/3},
    {0.6f, 0.2f, 0.2f}, {0.2f, 0.6f, 0.2f}, {0.2f, 0.2f, 0.6f},
    {0.45f, 0.45f, 0.1f}, {0.1f, 0.45f, 0.45f}, {0.45f, 0.1f, 0.45f},
};
constexpr int kBaryN = int(sizeof(kBary) / sizeof(kBary[0]));

RungMetrics measure_rung(const std::vector<Tri>& rung_tris,
                         const std::vector<TriEx>& rung_ex,
                         const std::vector<Tri>& src_tris,
                         const std::vector<TriEx>& src_ex,
                         const float3& to_sun,
                         int material_filter = -1) {
    RungMetrics m;
    double wsum = 0, angle_sum = 0, lit_sum = 0, lit_sq_sum = 0;
    double facet_sum = 0, ownface_sum = 0, facet_n = 0;
    for (size_t i = 0; i < rung_tris.size(); ++i) {
        const TriEx& e = rung_ex[i];
        if (material_filter >= 0 && e.materialId != material_filter) continue;
        const Tri& t = rung_tris[i];
        const float area = tri_area(t);
        if (!(area > 0)) continue;
        const float w = area / kBaryN;
        const float c0 = dot3(e.N0, e.N1), c1 = dot3(e.N1, e.N2);
        const bool faceted = c0 > 0.9999f && c1 > 0.9999f;
        facet_sum += faceted ? 1.0 : 0.0;
        ownface_sum += (faceted && dot3(unit3(e.N0), face_normal(t)) > 0.999f)
                           ? 1.0 : 0.0;
        facet_n += 1.0;
        for (int s = 0; s < kBaryN; ++s) {
            const float u = kBary[s][0], v = kBary[s][1], ww = kBary[s][2];
            const float3 p = f3(u*t.vertex0.x + v*t.vertex1.x + ww*t.vertex2.x,
                                u*t.vertex0.y + v*t.vertex1.y + ww*t.vertex2.y,
                                u*t.vertex0.z + v*t.vertex1.z + ww*t.vertex2.z);
            const float3 n = unit3(f3(u*e.N0.x + v*e.N1.x + ww*e.N2.x,
                                      u*e.N0.y + v*e.N1.y + ww*e.N2.y,
                                      u*e.N0.z + v*e.N1.z + ww*e.N2.z));
            const float3 ref = source_normal_at(src_tris, src_ex, p);
            const float d = std::min(std::max(dot3(n, ref), -1.f), 1.f);
            angle_sum += std::acos(d) * 57.29577951308232 * w;
            const double lit = lit_factor(n, to_sun);
            lit_sum += lit * w;
            lit_sq_sum += lit * lit * w;
            wsum += w;
        }
        ++m.tris;
    }
    if (wsum > 0) {
        m.mean_angle_deg = angle_sum / wsum;
        m.lit_mean = lit_sum / wsum;
        const double var = lit_sq_sum / wsum - m.lit_mean * m.lit_mean;
        m.lit_stddev = var > 0 ? std::sqrt(var) : 0.0;
    }
    if (facet_n > 0) {
        m.facet_fraction = facet_sum / facet_n;
        m.own_face_fraction = ownface_sum / facet_n;
    }
    return m;
}

// Production ladder step: decimate then reproject, exactly as bake_lods /
// part_flatten do it (from_tri weld, SampleSource, to_tri unweld).
void ladder_step(const std::vector<Tri>& src_tris,
                 const std::vector<TriEx>& src_ex, float keep,
                 std::vector<Tri>& out_tris, std::vector<TriEx>& out_ex) {
    out_tris = lod_bake::decimate_tris(src_tris, keep);
    if (out_tris.empty()) out_tris = src_tris;
    MeshIndexed src_m = from_tri(src_tris, &src_ex);
    MeshIndexed tgt_m = from_tri(out_tris, nullptr);
    ::reproject_triex(src_m, tgt_m, ReprojectNormals::SampleSource);
    std::vector<Tri> ignored;
    to_tri(tgt_m, ignored, out_ex);
}

// ---------------------------------------------------------------------------
// Fixtures. Deterministic LCG, no libc rand.
struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed) {}
    float next() {   // [0,1)
        s = s * 1664525u + 1013904223u;
        return float(s >> 8) * (1.0f / 16777216.0f);
    }
    float range(float lo, float hi) { return lo + (hi - lo) * next(); }
};

// One AlpineDeciduous-style canopy cloud: bottom apex, `rings` rings of
// `sides` irregular points, top apex — authored FACETED, exactly like DSL
// triangle_emit (N0=N1=N2 = geometric face normal).
void add_canopy_cloud(Lcg& r, const float3& center, float rx, float ry, float rz,
                      std::vector<Tri>& tris, std::vector<TriEx>& triex) {
    const int sides = 8, rings = 3;
    const float axial[rings] = {-0.38f, 0.02f, 0.40f};
    const float bulge[rings] = {0.77f, 1.0f, 0.75f};
    float3 ring_pts[rings][sides];
    const float phase = r.range(-0.42f, 0.42f);
    for (int ri = 0; ri < rings; ++ri) {
        const float3 rc = f3(center.x + r.range(-0.12f, 0.12f) * rx,
                             center.y + axial[ri] * ry + r.range(-0.05f, 0.06f) * ry,
                             center.z + r.range(-0.12f, 0.12f) * rz);
        for (int p = 0; p < sides; ++p) {
            const float th = phase + ri * 0.27f + p * 6.2831853f / sides;
            const float irr = r.range(0.80f, 1.18f) * bulge[ri];
            ring_pts[ri][p] = f3(rc.x + std::cos(th) * rx * irr,
                                 rc.y + r.range(-0.10f, 0.10f) * ry,
                                 rc.z + std::sin(th) * rz * r.range(0.82f, 1.17f) * bulge[ri]);
        }
    }
    const float3 bottom = f3(center.x, center.y - 0.70f * ry, center.z);
    const float3 top    = f3(center.x, center.y + 0.72f * ry, center.z);
    auto push = [&](float3 a, float3 b, float3 c) {
        Tri t = make_tri(a, b, c);
        TriEx e{};
        std::memset(&e, 0, sizeof(TriEx));
        const float3 n = face_normal(t);
        e.N0 = e.N1 = e.N2 = n;                     // faceted, like the DSL
        e.materialId = 29;                          // foliageThin
        e.tint = make_float4(0.18f, 0.40f, 0.13f, 1.f);
        e.ao0 = e.ao1 = e.ao2 = 1.f;
        tris.push_back(t);
        triex.push_back(e);
    };
    for (int p = 0; p < sides; ++p) {
        const int q = (p + 1) % sides;
        push(bottom, ring_pts[0][p], ring_pts[0][q]);
        for (int ri = 0; ri < rings - 1; ++ri) {
            const float3 a = ring_pts[ri][p], b = ring_pts[ri][q];
            const float3 c = ring_pts[ri+1][q], d = ring_pts[ri+1][p];
            push(a, c, b);
            push(a, d, c);
        }
        push(ring_pts[rings-1][p], top, ring_pts[rings-1][q]);
    }
}

void build_canopy(std::vector<Tri>& tris, std::vector<TriEx>& triex) {
    Lcg r(0xC0FFEEu);
    add_canopy_cloud(r, f3(0, 1.72f, 0), 0.72f, 0.62f, 0.72f, tris, triex);
    for (int region = 0; region < 5; ++region) {
        const float a = region * 6.2831853f / 5 + r.range(-0.16f, 0.16f);
        add_canopy_cloud(r,
            f3(std::cos(a) * 0.47f, r.range(1.48f, 1.68f), std::sin(a) * 0.47f),
            r.range(0.55f, 0.66f), r.range(0.46f, 0.56f), r.range(0.55f, 0.66f),
            tris, triex);
    }
}

// Subdivided cube, faceted authored normals: the ef7053be guard. Each face is
// an n x n grid; the decimated rung must keep every normal ON an axis.
void build_cube(std::vector<Tri>& tris, std::vector<TriEx>& triex) {
    const int n = 4;
    auto face = [&](float3 origin, float3 du, float3 dv) {
        for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const float u0 = float(i)/n, u1 = float(i+1)/n;
            const float v0 = float(j)/n, v1 = float(j+1)/n;
            const float3 p00 = f3(origin.x + du.x*u0 + dv.x*v0,
                                  origin.y + du.y*u0 + dv.y*v0,
                                  origin.z + du.z*u0 + dv.z*v0);
            const float3 p10 = f3(origin.x + du.x*u1 + dv.x*v0,
                                  origin.y + du.y*u1 + dv.y*v0,
                                  origin.z + du.z*u1 + dv.z*v0);
            const float3 p01 = f3(origin.x + du.x*u0 + dv.x*v1,
                                  origin.y + du.y*u0 + dv.y*v1,
                                  origin.z + du.z*u0 + dv.z*v1);
            const float3 p11 = f3(origin.x + du.x*u1 + dv.x*v1,
                                  origin.y + du.y*u1 + dv.y*v1,
                                  origin.z + du.z*u1 + dv.z*v1);
            auto push = [&](float3 a, float3 b, float3 c) {
                Tri t = make_tri(a, b, c);
                TriEx e{};
                std::memset(&e, 0, sizeof(TriEx));
                e.N0 = e.N1 = e.N2 = face_normal(t);
                e.materialId = 3;
                e.tint = make_float4(1, 1, 1, 0);
                e.ao0 = e.ao1 = e.ao2 = 1.f;
                tris.push_back(t);
                triex.push_back(e);
            };
            push(p00, p10, p11);
            push(p00, p11, p01);
        }
    };
    face(f3(-1,-1,-1), f3(2,0,0), f3(0,2,0));   // z- (inward; winding irrelevant here)
    face(f3(-1,-1, 1), f3(0,2,0), f3(2,0,0));   // z+
    face(f3(-1,-1,-1), f3(0,0,2), f3(2,0,0));   // y-
    face(f3(-1, 1,-1), f3(2,0,0), f3(0,0,2));   // y+
    face(f3(-1,-1,-1), f3(0,2,0), f3(0,0,2));   // x-
    face(f3( 1,-1,-1), f3(0,0,2), f3(0,2,0));   // x+
}

// Octahedron-subdivision sphere with SMOOTH authored normals (radial): the
// mesher-shaped source. Rungs must stay smooth — the fix must NOT facet it.
void build_smooth_sphere(std::vector<Tri>& tris, std::vector<TriEx>& triex) {
    struct T3 { float3 a, b, c; };
    std::vector<T3> cur = {
        {f3(0,1,0), f3(1,0,0), f3(0,0,1)},  {f3(0,1,0), f3(0,0,1), f3(-1,0,0)},
        {f3(0,1,0), f3(-1,0,0), f3(0,0,-1)},{f3(0,1,0), f3(0,0,-1), f3(1,0,0)},
        {f3(0,-1,0), f3(0,0,1), f3(1,0,0)}, {f3(0,-1,0), f3(-1,0,0), f3(0,0,1)},
        {f3(0,-1,0), f3(0,0,-1), f3(-1,0,0)},{f3(0,-1,0), f3(1,0,0), f3(0,0,-1)},
    };
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<T3> next;
        next.reserve(cur.size() * 4);
        for (const T3& t : cur) {
            const float3 ab = unit3(f3(t.a.x+t.b.x, t.a.y+t.b.y, t.a.z+t.b.z));
            const float3 bc = unit3(f3(t.b.x+t.c.x, t.b.y+t.c.y, t.b.z+t.c.z));
            const float3 ca = unit3(f3(t.c.x+t.a.x, t.c.y+t.a.y, t.c.z+t.a.z));
            next.push_back({t.a, ab, ca});
            next.push_back({ab, t.b, bc});
            next.push_back({ca, bc, t.c});
            next.push_back({ab, bc, ca});
        }
        cur.swap(next);
    }
    const float R = 1.4f;
    for (const T3& t : cur) {
        Tri tri = make_tri(f3(t.a.x*R, t.a.y*R, t.a.z*R),
                           f3(t.b.x*R, t.b.y*R, t.b.z*R),
                           f3(t.c.x*R, t.c.y*R, t.c.z*R));
        TriEx e{};
        std::memset(&e, 0, sizeof(TriEx));
        e.N0 = t.a; e.N1 = t.b; e.N2 = t.c;        // smooth radial field
        e.materialId = 5;
        e.tint = make_float4(1, 1, 1, 0);
        e.ao0 = e.ao1 = e.ao2 = 1.f;
        tris.push_back(tri);
        triex.push_back(e);
    }
}

const float3 kToSun = unit3(f3(0.45f, 0.80f, 0.35f));   // engine default, negated dir

void print_metrics(const char* label, size_t rung, const RungMetrics& m,
                   const RungMetrics& base) {
    const double rel = base.lit_mean > 0
        ? (m.lit_mean - base.lit_mean) / base.lit_mean : 0.0;
    printf("  %-10s rung %zu: tris=%5zu  normal=%6.2f deg  lit=%.4f "
           "(%+.1f%% vs rung0)  sd=%.4f  facet=%4.2f  ownface=%4.2f\n",
           label, rung, m.tris, m.mean_angle_deg, m.lit_mean, 100.0 * rel,
           m.lit_stddev, m.facet_fraction, m.own_face_fraction);
}

int run_fixture() {
    printf("lod_normal_consistency: fixture mode\n");

    // ---- canopy (faceted authored) --------------------------------------
    {
        std::vector<Tri> tris; std::vector<TriEx> triex;
        build_canopy(tris, triex);
        const RungMetrics base = measure_rung(tris, triex, tris, triex, kToSun);
        printf("canopy: %zu source tris\n", tris.size());
        print_metrics("canopy", 0, base, base);
        const float keeps[] = {0.45f, 0.20f};
        size_t rung_i = 1;
        for (float keep : keeps) {
            std::vector<Tri> rt; std::vector<TriEx> re;
            ladder_step(tris, triex, keep, rt, re);
            const RungMetrics m = measure_rung(rt, re, tris, triex, kToSun);
            print_metrics("canopy", rung_i, m, base);
            // The gates encode the CONTRACT, not pointwise normal agreement:
            // decimation itself reorients facets, so a faceted field has an
            // irreducible pointwise angle floor (the printed normal(deg) is
            // informative only). What must hold is that every rung computes
            // its normals by rep 0's RULE (own geometric face normal,
            // constant per face) and that the aggregate shading — mean
            // brightness and contrast — stays with rep 0's.
            char buf[160];
            std::snprintf(buf, sizeof buf,
                          "canopy rung %zu uses rep 0's recipe: constant "
                          "per-face own-geometric normals (%.2f > 0.90)",
                          rung_i, m.own_face_fraction);
            CHECK(m.own_face_fraction > 0.90, buf);
            const double rel = std::fabs(m.lit_mean - base.lit_mean) / base.lit_mean;
            std::snprintf(buf, sizeof buf,
                          "canopy rung %zu lit mean within 8%% of rung 0 "
                          "(%.1f%%)", rung_i, 100.0 * rel);
            CHECK(rel < 0.08, buf);
            const double sd_ratio = base.lit_stddev > 0
                ? m.lit_stddev / base.lit_stddev : 1.0;
            std::snprintf(buf, sizeof buf,
                          "canopy rung %zu keeps shading contrast (lit stddev "
                          "ratio %.2f > 0.60 — no smooth wash)", rung_i, sd_ratio);
            CHECK(sd_ratio > 0.60, buf);
            ++rung_i;
        }
    }

    // ---- subdivided cube (ef7053be guard) -------------------------------
    {
        std::vector<Tri> tris; std::vector<TriEx> triex;
        build_cube(tris, triex);
        printf("cube: %zu source tris\n", tris.size());
        std::vector<Tri> rt; std::vector<TriEx> re;
        ladder_step(tris, triex, 0.15f, rt, re);
        const RungMetrics m = measure_rung(rt, re, tris, triex, kToSun);
        const RungMetrics base = measure_rung(tris, triex, tris, triex, kToSun);
        print_metrics("cube", 1, m, base);
        // Every rung normal must still BE a cube face normal: hard 90-degree
        // edges, no melting. Max deviation from the nearest axis, any corner.
        double worst_axis_dot = 1.0;
        for (const TriEx& e : re) {
            for (const float3* n : {&e.N0, &e.N1, &e.N2}) {
                const double ax = std::max(std::fabs(n->x),
                                  std::max(std::fabs(n->y), std::fabs(n->z)));
                worst_axis_dot = std::min(worst_axis_dot, ax);
            }
        }
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "cube rung normals stay axis-aligned (worst |axis dot| "
                      "%.4f > 0.995) — box edges do not melt (ef7053be)",
                      worst_axis_dot);
        CHECK(worst_axis_dot > 0.995, buf);
        CHECK(m.facet_fraction > 0.99, "cube rung stays faceted per face");
    }

    // ---- smooth sphere (mesher-shaped source) ---------------------------
    {
        std::vector<Tri> tris; std::vector<TriEx> triex;
        build_smooth_sphere(tris, triex);
        printf("sphere: %zu source tris\n", tris.size());
        std::vector<Tri> rt; std::vector<TriEx> re;
        ladder_step(tris, triex, 0.25f, rt, re);
        const RungMetrics m = measure_rung(rt, re, tris, triex, kToSun);
        const RungMetrics base = measure_rung(tris, triex, tris, triex, kToSun);
        print_metrics("sphere", 1, m, base);
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "smooth sphere rung stays smooth (facet fraction %.2f "
                      "< 0.10 — the faceted path must not fire)",
                      m.facet_fraction);
        CHECK(m.facet_fraction < 0.10, buf);
        std::snprintf(buf, sizeof buf,
                      "smooth sphere rung normal error %.2f deg < 10",
                      m.mean_angle_deg);
        CHECK(m.mean_angle_deg < 10.0, buf);
    }

    if (g_failures == 0) {
        printf("lod_normal_consistency: ALL PASS\n");
        return 0;
    }
    printf("lod_normal_consistency: %d FAILURE(S)\n", g_failures);
    return 1;
}

}  // namespace

// ---------------------------------------------------------------------------
// --tree mode: the real AlpineDeciduous through install + flatten. Instrument,
// not a gate — prints the same metrics per FLAT rung.
#ifdef MATTER_HAVE_SCRIPT_HOST
#include "part_graph.h"
#include "part_asset_v2.h"
#include "part_flatten.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "portable_realpath.h"
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define ME3_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define ME3_MKDIR(p) mkdir((p), 0777)
#endif

namespace {

int run_tree(int form) {
    using namespace part_graph;
    const std::string schemas = abspath("../../projects/world_demo/objects");
    // Both shared-lib roots: the engine's (shared-lib/rng) and the project's
    // (shared-lib/vegetation) — the same pair the editor resolves against.
    const std::vector<std::string> shared_libs = {
        abspath("../shared-lib"),
        abspath("../../projects/world_demo/shared-lib"),
    };

    const std::string sandbox = "build/lod_normal_tree_sandbox";
    ME3_MKDIR("build");
    ME3_MKDIR(sandbox.c_str());
    ME3_MKDIR((sandbox + "/parts").c_str());
    if (chdir(sandbox.c_str()) != 0) { printf("FAIL: chdir sandbox\n"); return 1; }

    script_host::ScriptHost host;
    host.set_shared_lib_roots(shared_libs);
    FileModuleResolver resolver(host, schemas);
    HostBaker baker(host, ".");
    PartGraph graph(resolver, baker);

    Params params;
    params["seed"]    = ParamValue::number(3201);
    params["form"]    = ParamValue::number(form);
    params["size"]    = ParamValue::number(1.90);
    params["dryness"] = ParamValue::number(0.210);
    std::vector<ChildRequest> roots = { ChildRequest{ "AlpineDeciduous", params } };

    InstallResult ir = graph.install(roots);
    if (!ir.ok || ir.root_hashes.empty()) {
        printf("FAIL: install: %s\n", ir.error.c_str());
        return 1;
    }
    const uint64_t h = ir.root_hashes[0];
    printf("AlpineDeciduous form %d resolved %016llx\n", form,
           (unsigned long long)h);

    part_flatten::FlattenResult fr = part_flatten::flatten_part(".", h);
    if (!fr.ok) { printf("FAIL: flatten: %s\n", fr.error.c_str()); return 1; }
    printf("flatten: levels=%zu clusters=%zu coarsest=%zu\n",
           fr.levels, fr.clusters, fr.coarsest_tris);

    BLASManager blas;
    TLASManager tlas(4096);
    std::vector<part_asset::FlatCluster> clusters;
    const std::string bundle = part_asset::cache_path_flat(h);
    if (!part_asset::load_flat_v3(bundle, h, blas, tlas, clusters)) {
        printf("FAIL: load_flat_v3 %s\n", bundle.c_str());
        return 1;
    }
    const auto& entries = blas.get_entries();
    for (size_t c = 0; c < clusters.size(); ++c) {
        const auto& lods = clusters[c].lods;
        if (lods.empty()) continue;
        // Gather every rung of this cluster.
        std::vector<std::vector<Tri>>   rt(lods.size());
        std::vector<std::vector<TriEx>> re(lods.size());
        for (size_t l = 0; l < lods.size(); ++l) {
            for (uint32_t bi : lods[l].blas_indices) {
                if (bi >= entries.size()) continue;
                const auto& e = entries[bi];
                rt[l].insert(rt[l].end(), e->triangles.begin(), e->triangles.end());
                re[l].insert(re[l].end(), e->tri_extra.begin(), e->tri_extra.end());
            }
        }
        if (rt[0].empty() || re[0].size() != rt[0].size()) continue;
        printf("cluster %zu: %zu rungs, rung0=%zu tris\n", c, lods.size(),
               rt[0].size());
        const RungMetrics base =
            measure_rung(rt[0], re[0], rt[0], re[0], kToSun);
        const RungMetrics base_fol =
            measure_rung(rt[0], re[0], rt[0], re[0], kToSun, 29);
        print_metrics("all", 0, base, base);
        print_metrics("foliage", 0, base_fol, base_fol);
        for (size_t l = 1; l < lods.size(); ++l) {
            if (rt[l].empty() || re[l].size() != rt[l].size()) continue;
            // Billboard rung: 2 triangles — skip, it's not a mesh rung.
            if (rt[l].size() <= 2) {
                printf("  rung %zu: %zu tris (billboard) — skipped\n", l,
                       rt[l].size());
                continue;
            }
            const RungMetrics m =
                measure_rung(rt[l], re[l], rt[0], re[0], kToSun);
            const RungMetrics mf =
                measure_rung(rt[l], re[l], rt[0], re[0], kToSun, 29);
            print_metrics("all", l, m, base);
            print_metrics("foliage", l, mf, base_fol);
        }
    }
    return 0;
}

}  // namespace
#endif  // MATTER_HAVE_SCRIPT_HOST

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::string(argv[1]) == "--tree") {
#ifdef MATTER_HAVE_SCRIPT_HOST
        const int form = argc > 2 ? std::atoi(argv[2]) : 0;
        return run_tree(form);
#else
        fprintf(stderr, "--tree requires a script-host build\n");
        return 1;
#endif
    }
    return run_fixture();
}
