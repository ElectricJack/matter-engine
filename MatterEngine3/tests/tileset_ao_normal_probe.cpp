// tileset_ao_normal_probe.cpp — measurement harness for the tileset AO bake's
// choice of trace hemisphere.
//
// shaders_vk/tileset_bake_ao.comp builds its cosine-hemisphere frame from the
// INTERPOLATED vertex normal whenever the hit material is not flatShading. This
// probe bakes the real ForestFloor part variants through the real part bake and
// answers, per part, two questions:
//
//   1. How far do the baked TriEx vertex normals tilt off their own triangle's
//      face plane? (scale-invariant; reported as a percentile histogram)
//   2. What does that cost the AO integral? For a sample of surface points it
//      runs the shader's OWN 64-ray cosine hemisphere (RNG chain and basis
//      transliterated from the .comp) twice — once around the interpolated
//      normal, once around the triangle's geometric normal — intersecting the
//      part's actual triangles with the shader's tMax = edge_strip_width, and
//      reports the difference.
//
// Not a regression test: it prints numbers and always exits 0 unless a bake
// fails. Run it to re-derive the tuning evidence, not in the regression floor.

#include "part_graph.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "material_registry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "portable_realpath.h"

using namespace part_graph;

// ---------------------------------------------------------------------------
// Minimal vec3 / mat3 (column-major, matching GLSL mat3(b, n, tt)) and the
// RNG / basis / sampler — byte-preserved from shaders_vk/tileset_bake_ao.comp,
// same transliteration tileset_bake_vk_ao_tests.cpp uses.
// ---------------------------------------------------------------------------
struct Vec3 { float x, y, z; };
static Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 cross3(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static float len3(Vec3 v) { return std::sqrt(dot3(v, v)); }
static Vec3 normalize3(Vec3 v) { float l = len3(v); return {v.x / l, v.y / l, v.z / l}; }

static uint32_t splitmix32(uint32_t x) {
    x += 0x9E3779B9u;
    x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
    x = (x ^ (x >> 13)) * 0xC2B2AE35u;
    return x ^ (x >> 16);
}
static float u01(uint32_t h) { return float(h & 0x00FFFFFFu) / 16777216.0f; }
static Vec3 cosine_hemi(float ux, float uy) {
    float r = std::sqrt(ux);
    float phi = 6.28318530718f * uy;
    return {r * std::cos(phi), std::sqrt(std::fmax(0.0f, 1.0f - ux)), r * std::sin(phi)};
}
struct Mat3 { Vec3 c0, c1, c2; };
static Mat3 make_basis(Vec3 n) {
    Vec3 t = (std::fabs(n.y) < 0.999f) ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    Vec3 b = normalize3(cross3(t, n));
    return {b, n, cross3(n, b)};
}
static Vec3 mat3_mul(const Mat3& m, Vec3 v) {
    return m.c0 * v.x + m.c1 * v.y + m.c2 * v.z;
}

// ---------------------------------------------------------------------------
// Geometry.
// ---------------------------------------------------------------------------
struct ProbeTri {
    Vec3 v0, v1, v2;      // positions
    Vec3 n0, n1, n2;      // TriEx vertex normals (unnormalized as stored)
    Vec3 face;            // normalized geometric normal (cross(v1-v0, v2-v0))
    Vec3 centroid;
    float radius;         // centroid -> farthest vertex
    int   material;
};

// Watertight-enough Moller-Trumbore, two-sided (the bake's rayQuery has no
// face culling: gl_RayFlagsOpaqueEXT only forces opaque handling).
static bool ray_tri(Vec3 O, Vec3 D, const ProbeTri& t, float tmax, float* out_t) {
    const Vec3 e1 = t.v1 - t.v0, e2 = t.v2 - t.v0;
    const Vec3 p = cross3(D, e2);
    const float det = dot3(e1, p);
    if (std::fabs(det) < 1e-20f) return false;
    const float inv = 1.0f / det;
    const Vec3 s = O - t.v0;
    const float u = dot3(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 q = cross3(s, e1);
    const float v = dot3(D, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float tt = dot3(e2, q) * inv;
    if (tt <= 0.0f || tt >= tmax) return false;
    *out_t = tt;
    return true;
}

// The shader's per-texel AO, with the trace frame supplied by the caller so the
// same surface point can be measured around either normal. `shortlist` is the
// set of triangles within reach of P (cap prefilter), which only removes
// triangles the full loop could not have hit.
static float trace_ao(Vec3 P_surface, Vec3 trace_n, uint32_t local_x, uint32_t local_y,
                      uint32_t seed, int samples, float cap,
                      const std::vector<const ProbeTri*>& shortlist) {
    const Mat3 F = make_basis(trace_n);
    const Vec3 P = P_surface + trace_n * 1e-3f;   // shader's bias: N * 1e-3
    int occluded = 0;
    for (int i = 0; i < samples; ++i) {
        const uint32_t h1 = splitmix32(local_x * 73856093u ^ local_y * 19349663u ^
                                       (uint32_t)i * 83492791u ^ seed);
        const uint32_t h2 = splitmix32(h1);
        const Vec3 dir = mat3_mul(F, cosine_hemi(u01(h1), u01(h2)));
        float t;
        for (const ProbeTri* tp : shortlist) {
            if (ray_tri(P, dir, *tp, cap, &t)) { ++occluded; break; }
        }
    }
    return 1.0f - float(occluded) / float(samples);
}

static float pct(std::vector<float>& v, double q) {
    if (v.empty()) return 0.0f;
    size_t i = (size_t)(q * (double)(v.size() - 1) + 0.5);
    std::nth_element(v.begin(), v.begin() + (ptrdiff_t)i, v.end());
    return v[i];
}

struct Case { const char* module; int seed; double size; double detail; };

int main(int argc, char** argv) {
    const std::string schemas    = abspath("../../projects/world_demo/objects");
    const std::string shared_lib = abspath("../shared-lib");

    // fs::temp_directory_path, not a "/tmp" literal: the latter is the reason a
    // family of suites in this directory is red on Windows.
    std::error_code ec;
    const std::filesystem::path sandbox =
        std::filesystem::temp_directory_path(ec) / "me3_ao_normal_probe";
    std::filesystem::remove_all(sandbox, ec);
    std::filesystem::create_directories(sandbox / "parts", ec);
    if (!std::filesystem::exists(sandbox / "parts")) {
        std::printf("FAIL: could not create sandbox %s\n", sandbox.string().c_str());
        return 1;
    }
    std::filesystem::current_path(sandbox, ec);
    if (ec) { std::printf("FAIL: chdir sandbox\n"); return 1; }

    script_host::ScriptHost host;
    host.set_shared_lib_root(shared_lib);
    FileModuleResolver resolver(host, schemas);
    HostBaker baker(host, ".");

    // The variants ForestFloor.js `requires`, plus Twig.
    //
    // Twig is NO LONGER scattered by ForestFloor — it was dropped because a long
    // thin capsule cannot rest flat when the tileset snaps each piece to a single
    // height over uneven soil. It stays here on purpose: it is the sharpest
    // measured case of SDF-gradient vertex normals leaving their own face plane
    // (39 deg at p90, 74 deg at p99) and the evidence tileset_bake_ao.comp's
    // trace-normal note cites, so removing it would make that note unreproducible.
    std::vector<Case> cases = {
        {"Pebble", 0, 2.0, 0.0}, {"Pebble", 3, 2.0, 0.0}, {"Pebble", 5, 2.0, 0.0},
        {"Rock",   0, 1.2, 2.5},
        {"Twig",   0, 4.0, 0.0}, {"Twig", 1, 4.0, 0.0}, {"Twig", 2, 4.0, 0.0},
        {"Leaf",  -1, 0.0, 0.0},
    };

    // ForestFloor.js: this.tile({ size: 2.0, texelsPerMeter: 512, seed: 1234 }),
    // edge_strip_width defaulted (tileset_spec.h) -> the AO pass' tMax.
    const uint32_t kSeed    = 1234u;
    const int      kSamples = 64;                 // tileset_bake_vk.cpp: aoSamples = 64
    float          cap      = 0.15f;              // TileConfig::edge_strip_width default
    if (argc > 1) cap = (float)atof(argv[1]);

    std::printf("tileset AO trace-normal probe   samples=%d  tMax=%.3f m  seed=%u\n",
                kSamples, cap, kSeed);
    std::printf("%-14s %6s %4s %5s   %-27s %-13s %-31s\n", "variant", "tris", "mat", "flat",
                "angle(vtxN, facePlane) deg", "rays below", "AO(geometric) - AO(shading)");
    std::printf("%-14s %6s %4s %5s   %6s %6s %6s %6s   %6s %6s   %7s %7s %7s %5s %5s\n",
                "", "", "", "", "p50", "p90", "p99", "max",
                "p50", "p99", "mean", "p99", "max", "black", "inv");

    for (const Case& c : cases) {
        Params p;
        if (c.seed >= 0) p["seed"] = ParamValue::number(c.seed);
        if (c.size != 0.0) p["size"] = ParamValue::number(c.size);
        if (c.detail != 0.0) p["detail"] = ParamValue::number(c.detail);

        PartGraph graph(resolver, baker);
        InstallResult ir = graph.install({ ChildRequest{ c.module, p } });
        if (!ir.ok) {
            std::printf("FAIL install %s: %s\n", c.module, ir.error.c_str());
            return 1;
        }

        BLASManager blas; TLASManager tlas(256);
        std::vector<part_asset::ChildInstance> children;
        part_asset::LodLevels lods;
        if (!part_asset::load_v2(part_asset::cache_path_resolved(ir.root_hashes[0]),
                                 ir.root_hashes[0], blas, tlas, children, lods)) {
            std::printf("FAIL load %s\n", c.module);
            return 1;
        }
        const auto& entries = blas.get_entries();
        if (entries.empty()) { std::printf("FAIL no entries %s\n", c.module); return 1; }

        // Collect the whole part surface. tileset_torus_bvh.cpp's
        // load_part_into_shared only registers entries.front(), so print the
        // per-entry split too: if front() is a small fraction of the part, the
        // tileset BVH is holding a fragment of it.
        std::vector<ProbeTri> tris;
        std::string split;
        for (size_t ei = 0; ei < entries.size(); ++ei) {
            const BLASManager::BLASEntry& e = *entries[ei];
            const bool have_ex = !e.tri_extra.empty();
            if (ei < 6) {
                split += (ei ? "+" : "") + std::to_string(e.triangles.size());
            } else if (ei == 6) {
                split += "+...";
            }
            for (size_t i = 0; i < e.triangles.size(); ++i) {
                const Tri& t = e.triangles[i];
                ProbeTri pt{};
                pt.v0 = {t.vertex0.x, t.vertex0.y, t.vertex0.z};
                pt.v1 = {t.vertex1.x, t.vertex1.y, t.vertex1.z};
                pt.v2 = {t.vertex2.x, t.vertex2.y, t.vertex2.z};
                const Vec3 fn = cross3(pt.v1 - pt.v0, pt.v2 - pt.v0);
                if (len3(fn) < 1e-20f) continue;   // degenerate: shader falls back
                pt.face = normalize3(fn);
                if (have_ex) {
                    const TriEx& x = e.tri_extra[i];
                    pt.n0 = {x.N0.x, x.N0.y, x.N0.z};
                    pt.n1 = {x.N1.x, x.N1.y, x.N1.z};
                    pt.n2 = {x.N2.x, x.N2.y, x.N2.z};
                    pt.material = x.materialId;
                } else {
                    pt.n0 = pt.n1 = pt.n2 = pt.face;
                    pt.material = -1;
                }
                pt.centroid = (pt.v0 + pt.v1 + pt.v2) * (1.0f / 3.0f);
                pt.radius = std::fmax(len3(pt.v0 - pt.centroid),
                            std::fmax(len3(pt.v1 - pt.centroid), len3(pt.v2 - pt.centroid)));
                tris.push_back(pt);
            }
        }
        if (tris.empty()) { std::printf("FAIL no tris %s\n", c.module); return 1; }
        std::printf("  [%s s=%d] %zu entries, tris/entry %s  (bake traces entry0 = %zu)\n",
                    c.module, c.seed, entries.size(), split.c_str(),
                    entries[0]->triangles.size());

        const int mat = tris[0].material;
        const MaterialDef* md = MaterialRegistryGet(mat);
        const bool flat = md && md->flatShading != 0;

        // (1) Angle between each stored vertex normal and its own face plane.
        // Oriented to the same side first, so this measures tilt (<=90 deg) and
        // not winding disagreement; `inv` below covers the sign case.
        std::vector<float> ang;
        ang.reserve(tris.size() * 3);
        for (const ProbeTri& t : tris) {
            const Vec3 ns[3] = {t.n0, t.n1, t.n2};
            for (const Vec3& n : ns) {
                if (len3(n) < 1e-12f) continue;
                Vec3 nn = normalize3(n);
                if (dot3(nn, t.face) < 0.0f) nn = nn * -1.0f;   // compare orientations
                ang.push_back(std::acos(std::fmin(1.0f, dot3(nn, t.face))) *
                              57.29577951f);
            }
        }

        // Ground plane. ForestFloor embeds its litter into the dirt (Twig
        // embed 0.05), and the bake's TLAS holds the base heightfield, so a
        // downward ray off a part's top surface has something to hit well
        // inside the 0.15 m cap. Tracing the part in isolation would leave that
        // out and understate every hit.
        float ymin = tris[0].v0.y, ymax = ymin, span = 0.0f;
        for (const ProbeTri& t : tris) {
            const Vec3 vs[3] = {t.v0, t.v1, t.v2};
            for (const Vec3& v : vs) { ymin = std::fmin(ymin, v.y); ymax = std::fmax(ymax, v.y); }
            span = std::fmax(span, len3(t.centroid));
        }
        const float gy = ymin + 0.05f * (ymax - ymin);
        const float gr = span + cap + 1.0f;
        {
            const Vec3 a{-gr, gy, -gr}, b{gr, gy, -gr}, d{gr, gy, gr}, e2{-gr, gy, gr};
            for (int k = 0; k < 2; ++k) {
                ProbeTri g{};
                g.v0 = a; g.v1 = k ? d : b; g.v2 = k ? e2 : d;
                g.face = {0, 1, 0};
                g.n0 = g.n1 = g.n2 = g.face;
                g.material = 16;                  // DIRT
                g.centroid = (g.v0 + g.v1 + g.v2) * (1.0f / 3.0f);
                g.radius = std::fmax(len3(g.v0 - g.centroid),
                           std::fmax(len3(g.v1 - g.centroid), len3(g.v2 - g.centroid)));
                tris.push_back(g);
            }
        }
        const size_t n_part = tris.size() - 2;

        // (2) The AO integral both ways. Sampled at four barycentric positions
        // per triangle: the centroid plus three near-vertex points. Texels are
        // spread across the whole triangle (a 1.5 cm twig facet covers ~7x7 at
        // 512 texels/m), and near a vertex the interpolation returns that
        // vertex's own normal — the full deviation — while the centroid
        // averages all three and cancels much of it.
        const float kBary[4][3] = {
            {1.f/3, 1.f/3, 1.f/3}, {0.8f, 0.1f, 0.1f}, {0.1f, 0.8f, 0.1f}, {0.1f, 0.1f, 0.8f},
        };
        const size_t kMaxPts = 1200;
        const size_t stride = std::max<size_t>(1, (n_part * 4) / kMaxPts);
        std::vector<float> delta, below;
        std::vector<const ProbeTri*> shortlist;
        int black = 0;                 // spurious: shading AO < 0.5 while geometric > 0.9
        int inverted = 0;              // trace hemisphere pointing into the solid
        size_t visible = 0;            // sample points the ortho bake actually reaches
        double sum = 0.0;
        size_t pt = 0;
        for (size_t i = 0; i < n_part; ++i) {
            const ProbeTri& t = tris[i];
            shortlist.clear();
            for (const ProbeTri& o : tris) {
                if (len3(o.centroid - t.centroid) <= cap + o.radius + t.radius)
                    shortlist.push_back(&o);
            }
            for (int bi = 0; bi < 4; ++bi, ++pt) {
                if (pt % stride) continue;
                const float bu = kBary[bi][0], bv = kBary[bi][1], bw = kBary[bi][2];
                const Vec3 P = t.v0 * bu + t.v1 * bv + t.v2 * bw;

                // Only the upper envelope exists in the atlas. The bake writes a
                // texel from the FIRST hit of a downward ortho ray, so a
                // triangle that is not the topmost surface at this (x,z) — an
                // underside, or anything behind another facet — is never
                // reconstructed and must not be counted. Without this the
                // sample set is the whole closed mesh and roughly doubles.
                const Vec3 O{P.x, ymax + 1.0f, P.z};
                const Vec3 D{0.0f, -1.0f, 0.0f};
                const float want_t = O.y - P.y;
                float best = want_t + 1e-3f, tt;
                for (const ProbeTri& o : tris) {
                    if (ray_tri(O, D, o, best, &tt)) best = tt;
                }
                if (best < want_t - 1e-4f) continue;   // occluded from above
                ++visible;
                Vec3 shading = t.n0 * bu + t.n1 * bv + t.n2 * bw;   // shader's blend
                if (dot3(shading, shading) < 1e-20f) shading = t.face;
                shading = normalize3(shading);
                if (shading.y < 0.0f) shading = shading * -1.0f;    // shader: N.y<0 -> -N
                Vec3 geom = t.face;
                if (geom.y < 0.0f) geom = geom * -1.0f;             // same up-orientation
                // Fully inverted hemisphere: after the shader's own N.y<0 flip
                // the shading normal ends up on the far side of the facet, so
                // the trace frame points into the solid and every ray hits.
                if (dot3(shading, geom) < 0.0f) ++inverted;

                // Scale-free statistic: what fraction of the cosine lobe built
                // around the shading normal starts out pointing below the
                // triangle's own plane?
                const Mat3 F = make_basis(shading);
                int nb = 0;
                for (int s = 0; s < kSamples; ++s) {
                    const uint32_t h1 = splitmix32((uint32_t)pt * 73856093u ^
                                                   (uint32_t)s * 83492791u ^ kSeed);
                    const uint32_t h2 = splitmix32(h1);
                    if (dot3(mat3_mul(F, cosine_hemi(u01(h1), u01(h2))), geom) < 0.0f) ++nb;
                }
                below.push_back(float(nb) / float(kSamples));

                const uint32_t lx = (uint32_t)((pt * 7919u) % 1024u);
                const uint32_t ly = (uint32_t)((pt * 104729u) % 1024u);
                const float ao_s = trace_ao(P, shading, lx, ly, kSeed, kSamples, cap, shortlist);
                const float ao_g = trace_ao(P, geom,    lx, ly, kSeed, kSamples, cap, shortlist);
                delta.push_back(ao_g - ao_s);
                sum += (double)(ao_g - ao_s);
                if (ao_s < 0.5f && ao_g > 0.9f) ++black;
            }
        }

        std::printf("%-8s s=%-3d %6zu %4d %5s   %6.1f %6.1f %6.1f %6.1f   "
                    "%5.1f%% %5.1f%%   %7.3f %7.3f %7.3f %5d %5d /%zu\n",
                    c.module, c.seed, n_part, mat, flat ? "yes" : "NO",
                    pct(ang, 0.50), pct(ang, 0.90), pct(ang, 0.99), pct(ang, 1.0),
                    100.0f * pct(below, 0.50), 100.0f * pct(below, 0.99),
                    sum / (double)std::max<size_t>(1, delta.size()),
                    pct(delta, 0.99), pct(delta, 1.0), black, inverted, visible);
        std::fflush(stdout);
    }
    return 0;
}
