// impostor_ab_probe — the mesh-vs-impostor A/B instrument (issue e9e7397c).
//
// Renders the SAME cluster twice from the SAME baked view frame at the SAME
// on-screen resolution — once as a mesh rung, once as its impostor card —
// and reports a NUMBER per G-buffer channel plus the lit/display result.
//
// WHICH mesh rung matters, and is the --mesh-rung flag:
//   --mesh-rung terminal   (default) the COARSEST mesh rung — the rung the
//                          billboard actually takes over from on screen
//                          (part_flatten.cpp: term_idx = level_metas.back()).
//                          This measures the on-screen switch the user sees.
//   --mesh-rung 0          rep 0 — the rung the atlas is BAKED from
//                          (part_flatten.cpp: src_idx = level_metas.front(),
//                          "Rep 0, not the coarsest rung"). This measures
//                          whether the atlas is faithful to its source.
//   --mesh-rung N          any specific mesh rung index.
// The card side is ALWAYS baked from rep 0, exactly as production does; the
// flag moves only the mesh reference. The two questions are different: a card
// can be a perfect picture of rep 0 (rung 0 diff ~ 0) and still pop against
// the decimated rung it replaces (terminal diff >> 0).
// Whichever channel diverges is the answer; the tool ranks them so nobody has
// to squint at pixels. Born on the third round of "impostor trees are the
// wrong colour", where two plausible causes in a row died on measurement:
// the per-channel diff is the measurement, made permanent.
//
// WHAT IT MIRRORS (and must be kept in step with — this is a CPU model of the
// GPU path, the price of running headless with no swapchain):
//   * impostor_bake.cpp     — the card content comes from the REAL bake_cluster,
//                             not a model of it. Only the two shading paths are
//                             mirrored by hand.
//   * shaders_vk/gbuffer.frag
//       mesh branch         — base_color = resolveBaseColor(material, tint),
//                             ao = 1 for DSL geometry (TriEx::ao is 1),
//                             roughness/metallic from the PER-PIXEL material.
//       impostor branch     — bilinear atlas tap clamped to the cell, alpha
//                             cutout 0.25, resolveBaseColor against the
//                             per-cluster VOTED material, octahedral normal
//                             decode. (Parallax uv offset and the conservative
//                             depth write are NOT modelled: they move where a
//                             texel lands, not what colour it resolves to.)
//   * shaders_vk/composite.frag (RT off: visibility == 1, raw_diffuse == 0)
//       direct/ambient      — direct = max(dot(N,to_sun),0);
//                             ambient = diffuse * sky_irradiance(N) * ao;
//                             sun scaled by mix(1, 0.65, roughness).
//       subsurface          — thin-walled backlit term for MESH pixels only:
//                             since aeb797ad an impostor card gets no
//                             subsurface term (the card depicts mixed
//                             materials; the voted one must not glow).
//   * shaders_vk/display_transform.frag — exposure -2 EV, ACES, sRGB encode.
//
// The MESH reference is supersampled (shade per subsample, then average),
// which is the ensemble expectation of the 1-sample-per-pixel GPU raster over
// jittered instances. The CARD is sampled exactly once per pixel like the GPU.
// The two images share one orthographic frame — the card quad spans the same
// [-h,+h]^2 about the same centre the bake projected — so pixel (x,y) of both
// images is the same world footprint and a per-pixel diff is meaningful.
//
// USAGE
//   impostor_ab_probe                                   # fixture self-test
//   impostor_ab_probe --bundle <path.bundle> --hash <16-hex>
//                     [--cluster N] [--view N] [--px N]
//                     [--mesh-rung {0|terminal|N}]
//                     [--rung-vs A,B]
//                     [--sun az_deg,el_deg] [--sun-mult M]
//
//   --rung-vs A,B  MESH-vs-MESH mode (issue 7b64dc27): no impostor at all.
//             Renders mesh rung A into the "mesh" column and mesh rung B into
//             the "card" column of the same report, from the same frame — the
//             direct rung-N-vs-rung-M diff for LOD shading discontinuities.
//             `t` means the terminal rung (e.g. --rung-vs 0,t).
//
//   --view    baked view index 0..47 (azimuth-major, 16 per ring; rings at
//             0/30/60 deg). Default probes 0, 20 and 40 — one per ring.
//   --px      on-screen card size in pixels (default 32; use the size the
//             defect is seen at).
//   --sun     sun AZIMUTH/ELEVATION in degrees, matter/sun_angles.h
//             convention. Default: the engine-default sun vector.
//   --sun-mult  scalar on sun_color (render.lighting sun_multiplier).
//
//   Makefile: make -C MatterEngine3/tests run-impostor-ab   (fixture mode)
//   Bundle mode example (the StreamMountain conifer):
//     ./build/impostor_ab_probe --hash <h> --view 40 --px 32
//         --bundle ../../projects/world_demo/.cache/StreamMountain/parts/<h>.bundle
//
// Exit code: fixture mode fails (1) on its CHECKs; bundle mode fails only on
// I/O errors — it is an instrument, not a gate.

#include "impostor_bake.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "material_registry.h"

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

// ---------------------------------------------------------------------------
// Small math, mirroring impostor_bake.cpp's deterministic helpers.
inline float dot3(const float3& a, const float3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline float3 cross3(const float3& a, const float3& b) {
    return make_float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                       a.x * b.y - a.y * b.x);
}
inline float3 unit3(const float3& v) {
    const float len = std::sqrt(dot3(v, v));
    if (!(len > 1e-20f)) return make_float3(0.0f, 0.0f, 1.0f);
    return make_float3(v.x / len, v.y / len, v.z / len);
}
inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// Octahedral decode — gbuffer.frag's branch structure.
inline float3 oct_decode(float ox, float oy) {
    float ex = ox * 2.f - 1.f, ey = oy * 2.f - 1.f;
    const float ez = 1.f - std::fabs(ex) - std::fabs(ey);
    if (ez < 0.f) {
        const float tx = (1.f - std::fabs(ey)) * (ex >= 0.f ? 1.f : -1.f);
        const float ty = (1.f - std::fabs(ex)) * (ey >= 0.f ? 1.f : -1.f);
        ex = tx; ey = ty;
    }
    return unit3(make_float3(ex, ey, ez));
}

// ---------------------------------------------------------------------------
// Lighting model (composite.frag, RT off) + display transform.
struct Lighting {
    float3 to_sun{};                      // unit, toward the sun
    float  sun_color[3] = {2.2f, 2.05f, 1.8f};
    float  sky_color[3] = {0.38f, 0.43f, 0.52f};
    float  sun_mult = 1.0f;
    float  exposure = 0.25f;              // -2 EV
};

struct Shaded {
    float hdr[3];
    float display[3];
};

inline float aces1(float x) {
    x = x < 0.f ? 0.f : x;
    const float n = x * (2.51f * x + 0.03f);
    const float d = x * (2.43f * x + 0.59f) + 0.14f;
    return clamp01(n / (d < 1e-6f ? 1e-6f : d));
}
inline float srgb1(float c) {
    return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

// One G-buffer sample -> lit pixel. `allow_subsurface` is the aeb797ad seam:
// composite skips the subsurface block for impostor-identity pixels.
Shaded composite_pixel(const float albedo[3], const float3& n, float roughness,
                       float metallic, float ao, int material_id,
                       bool allow_subsurface, const Lighting& L) {
    Shaded out{};
    const float ndl = dot3(n, L.to_sun);
    const float direct = ndl > 0.f ? ndl : 0.f;
    const float sky_t = 0.2f + 0.8f * clamp01(n.y * 0.5f + 0.5f);
    float ss_add[3] = {0.f, 0.f, 0.f};
    if (allow_subsurface && material_id >= 0) {
        const MaterialDef* m = MaterialRegistryGet(material_id);
        if (m && m->subsurface > 0.f) {
            if (m->surfaceFlags & MATERIAL_THIN_WALLED) {
                float backlit = clamp01(-ndl);
                const float aniso = clamp01(m->anisotropy);
                backlit = std::pow(backlit, 1.f + 3.f * aniso);
                const float falloff = clamp01(m->scatteringDistance * 4.f);
                for (int k = 0; k < 3; ++k)
                    ss_add[k] = m->scatteringColor[k] * m->subsurface *
                                backlit * falloff;
            } else {
                const float wrapped =
                    clamp01((ndl + m->subsurface) / (1.f + m->subsurface));
                const float extra = std::max(wrapped - direct, 0.f);
                for (int k = 0; k < 3; ++k)
                    ss_add[k] = albedo[k] * (1.f - metallic) *
                                m->scatteringColor[k] * extra;
            }
        }
    }
    const float sun_scale = 1.f - 0.35f * clamp01(roughness);   // mix(1,.65,r)
    for (int k = 0; k < 3; ++k) {
        const float diffuse = albedo[k] * (1.f - clamp01(metallic));
        const float ambient = diffuse * L.sky_color[k] * sky_t * clamp01(ao);
        const float sun = (diffuse * direct + ss_add[k]) *
                          L.sun_color[k] * L.sun_mult * sun_scale;   // vis = 1
        out.hdr[k] = ambient + sun;
        out.display[k] = srgb1(aces1(out.hdr[k] * L.exposure));
    }
    return out;
}

// ---------------------------------------------------------------------------
// One rendered pixel's G-buffer + lit values, both paths share this shape.
struct Pixel {
    bool   drawn = false;
    float  weight = 0.f;      // mesh: subsample coverage; card: 1 (post-cutout)
    float  albedo[3] = {0, 0, 0};
    float3 normal{};
    float  roughness = 0.f, metallic = 0.f, ao = 1.f;
    int    material = -1;     // mesh: modal per pixel; card: the vote
    float  hdr[3] = {0, 0, 0};
    float  display[3] = {0, 0, 0};
};

// resolveBaseColor (material_common.glsl).
void resolve_base_color(int material_id, const float4& tint, float out3[3]) {
    float base[3] = {0.5f, 0.5f, 0.5f};
    if (const MaterialDef* m = MaterialRegistryGet(material_id))
        for (int k = 0; k < 3; ++k) base[k] = m->albedo[k];
    const float a = clamp01(tint.w);
    out3[0] = base[0] + (tint.x - base[0]) * a;
    out3[1] = base[1] + (tint.y - base[1]) * a;
    out3[2] = base[2] + (tint.z - base[2]) * a;
}

void material_rough_metal(int material_id, float& rough, float& metal) {
    rough = 0.5f; metal = 0.f;
    if (const MaterialDef* m = MaterialRegistryGet(material_id)) {
        rough = m->roughness; metal = m->metallic;
    }
}

// The baked view frame for view index v — the same construction as
// bake_cluster and raster.vert's rebuilt baked frame.
struct ViewFrame {
    float3 view_z, right, up;
};
ViewFrame view_frame(uint32_t view) {
    const uint32_t az_i = view % impostor::kAzimuths;
    const uint32_t el_i = view / impostor::kAzimuths;
    const float angle = 6.28318530717958647692f * float(az_i) /
                        float(impostor::kAzimuths);
    const float elev = float(el_i) * impostor::kElevationStep;
    const float ce = std::cos(elev), se = std::sin(elev);
    ViewFrame f;
    f.view_z = make_float3(std::sin(angle) * ce, se, std::cos(angle) * ce);
    f.right = unit3(cross3(make_float3(0.f, 1.f, 0.f), f.view_z));
    f.up = cross3(f.view_z, f.right);
    return f;
}

// ---------------------------------------------------------------------------
// MESH reference: rasterize rep-0 triangles into px*px pixels with ss*ss
// shaded subsamples per pixel (shade-then-average — the ensemble expectation
// of the GPU's one-sample raster). Same scanline math as bake_cluster.
void render_mesh(const std::vector<Tri>& tris, const std::vector<TriEx>& triex,
                 const float3& center, float half_extent, uint32_t view,
                 int px, int ss, const Lighting& L, std::vector<Pixel>& out) {
    const ViewFrame f = view_frame(view);
    const int edge = px * ss;
    struct Sub {
        float depth = -3.4e38f;
        bool hit = false;
        float3 normal{};
        float4 tint{};
        int material = -1;
    };
    std::vector<Sub> sub(size_t(edge) * edge);
    const float inv_h = 1.f / half_extent;
    const bool have_ex = triex.size() == tris.size();
    for (size_t ti = 0; ti < tris.size(); ++ti) {
        const Tri& t = tris[ti];
        const float3 verts[3] = {t.vertex0, t.vertex1, t.vertex2};
        float sx[3], sy[3], sz[3];
        for (int c = 0; c < 3; ++c) {
            const float3 rel = make_float3(verts[c].x - center.x,
                                           verts[c].y - center.y,
                                           verts[c].z - center.z);
            const float nx = dot3(rel, f.right) * inv_h;
            const float ny = dot3(rel, f.up) * inv_h;
            sx[c] = (nx * 0.5f + 0.5f) * float(edge);
            sy[c] = (0.5f - ny * 0.5f) * float(edge);
            sz[c] = dot3(rel, f.view_z);
        }
        const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) -
                           (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (std::fabs(area) < 1e-12f) continue;
        const float inv_area = 1.f / area;
        int x0 = int(std::floor(std::min(sx[0], std::min(sx[1], sx[2]))));
        int x1 = int(std::ceil(std::max(sx[0], std::max(sx[1], sx[2]))));
        int y0 = int(std::floor(std::min(sy[0], std::min(sy[1], sy[2]))));
        int y1 = int(std::ceil(std::max(sy[0], std::max(sy[1], sy[2]))));
        x0 = std::max(x0, 0); y0 = std::max(y0, 0);
        x1 = std::min(x1, edge - 1); y1 = std::min(y1, edge - 1);
        for (int py = y0; py <= y1; ++py) {
            const float fy = float(py) + 0.5f;
            for (int pxx = x0; pxx <= x1; ++pxx) {
                const float fx = float(pxx) + 0.5f;
                const float w1 = ((fx - sx[0]) * (sy[2] - sy[0]) -
                                  (sx[2] - sx[0]) * (fy - sy[0])) * inv_area;
                const float w2 = ((sx[1] - sx[0]) * (fy - sy[0]) -
                                  (fx - sx[0]) * (sy[1] - sy[0])) * inv_area;
                const float w0 = 1.f - w1 - w2;
                if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
                const float depth = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                Sub& s = sub[size_t(py) * edge + pxx];
                if (s.hit && !(depth > s.depth)) continue;
                s.hit = true;
                s.depth = depth;
                if (have_ex) {
                    const TriEx& e = triex[ti];
                    s.normal = make_float3(
                        w0 * e.N0.x + w1 * e.N1.x + w2 * e.N2.x,
                        w0 * e.N0.y + w1 * e.N1.y + w2 * e.N2.y,
                        w0 * e.N0.z + w1 * e.N1.z + w2 * e.N2.z);
                    s.tint = e.tint;
                    s.material = e.materialId;
                } else {
                    s.normal = unit3(cross3(t.vertex1 - t.vertex0,
                                            t.vertex2 - t.vertex0));
                    s.tint = make_float4(1.f, 1.f, 1.f, 0.f);
                    s.material = -1;
                }
            }
        }
    }
    out.assign(size_t(px) * px, Pixel{});
    std::vector<int> mat_ids;
    std::vector<int> mat_counts;
    for (int y = 0; y < px; ++y) {
        for (int x = 0; x < px; ++x) {
            Pixel& p = out[size_t(y) * px + x];
            int hits = 0;
            float3 nsum = make_float3(0, 0, 0);
            mat_ids.clear(); mat_counts.clear();
            for (int sy = 0; sy < ss; ++sy) {
                for (int sxp = 0; sxp < ss; ++sxp) {
                    const Sub& s =
                        sub[size_t(y * ss + sy) * edge + (x * ss + sxp)];
                    if (!s.hit) continue;
                    ++hits;
                    const float3 n = unit3(s.normal);
                    float alb[3];
                    resolve_base_color(s.material, s.tint, alb);
                    float rough, metal;
                    material_rough_metal(s.material, rough, metal);
                    const Shaded sh = composite_pixel(
                        alb, n, rough, metal, 1.f, s.material,
                        /*allow_subsurface=*/true, L);
                    for (int k = 0; k < 3; ++k) {
                        p.albedo[k] += alb[k];
                        p.hdr[k] += sh.hdr[k];
                        p.display[k] += sh.display[k];
                    }
                    nsum = make_float3(nsum.x + n.x, nsum.y + n.y, nsum.z + n.z);
                    p.roughness += rough;
                    p.metallic += metal;
                    bool found = false;
                    for (size_t m = 0; m < mat_ids.size(); ++m)
                        if (mat_ids[m] == s.material) {
                            ++mat_counts[m]; found = true; break;
                        }
                    if (!found) { mat_ids.push_back(s.material); mat_counts.push_back(1); }
                }
            }
            if (hits == 0) continue;
            const float inv = 1.f / float(hits);
            p.drawn = true;
            p.weight = float(hits) / float(ss * ss);
            for (int k = 0; k < 3; ++k) {
                p.albedo[k] *= inv; p.hdr[k] *= inv; p.display[k] *= inv;
            }
            p.normal = unit3(nsum);
            p.roughness *= inv;
            p.metallic *= inv;
            p.ao = 1.f;
            int best = 0;
            for (size_t m = 1; m < mat_ids.size(); ++m)
                if (mat_counts[m] > mat_counts[best]) best = int(m);
            p.material = mat_ids.empty() ? -1 : mat_ids[best];
        }
    }
}

// ---------------------------------------------------------------------------
// CARD path: sample the freshly baked atlas exactly as gbuffer.frag's
// impostor branch does — one bilinear tap per pixel, clamped to the cell,
// cutout 0.25, voted material, no subsurface.
void render_card(const impostor::ClusterImpostor& imp, uint32_t view, int px,
                 const Lighting& L, std::vector<Pixel>& out) {
    const uint32_t cell = impostor::cell_px();
    const uint32_t layer_edge = impostor::layer_px();
    const size_t layer_sz = impostor::layer_bytes();
    const uint32_t cx0 = (view % impostor::kGridDim) * cell;
    const uint32_t cy0 = (view / impostor::kGridDim) * cell;
    const uint8_t* shade_layer = imp.atlas.data();
    const uint8_t* tint_layer = imp.atlas.data() + layer_sz;
    const auto sample = [&](const uint8_t* layer, float u, float v,
                            float out4[4]) {
        const float fx = u * cell - 0.5f, fy = v * cell - 0.5f;
        const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
        const float wx = fx - x0, wy = fy - y0;
        for (int k = 0; k < 4; ++k) out4[k] = 0.f;
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                int x = std::min(std::max(x0 + dx, 0), int(cell) - 1);
                int y = std::min(std::max(y0 + dy, 0), int(cell) - 1);
                const uint8_t* t =
                    layer + (size_t(cy0 + y) * layer_edge + (cx0 + x)) * 4;
                const float w = (dx ? wx : 1.f - wx) * (dy ? wy : 1.f - wy);
                for (int k = 0; k < 4; ++k) out4[k] += w * (t[k] / 255.f);
            }
        }
    };
    const int vote = int(imp.material_index);
    float rough, metal;
    material_rough_metal(vote, rough, metal);
    out.assign(size_t(px) * px, Pixel{});
    for (int y = 0; y < px; ++y) {
        for (int x = 0; x < px; ++x) {
            const float u = (x + 0.5f) / px, v = (y + 0.5f) / px;
            float shade[4], tint[4];
            sample(shade_layer, u, v, shade);
            if (shade[3] < 0.25f) continue;   // kImpostorAlphaCutout
            sample(tint_layer, u, v, tint);
            Pixel& p = out[size_t(y) * px + x];
            p.drawn = true;
            p.weight = 1.f;
            const float4 t4 = make_float4(tint[0], tint[1], tint[2], tint[3]);
            resolve_base_color(vote, t4, p.albedo);
            p.normal = oct_decode(shade[0], shade[1]);
            p.roughness = rough;
            p.metallic = metal;
            p.ao = 1.f;   // gbuffer.frag impostor branch sets ao = 1
            p.material = vote;
            const Shaded sh = composite_pixel(
                p.albedo, p.normal, p.roughness, p.metallic, p.ao, vote,
                /*allow_subsurface=*/false, L);   // aeb797ad
            for (int k = 0; k < 3; ++k) { p.hdr[k] = sh.hdr[k]; p.display[k] = sh.display[k]; }
        }
    }
}

// ---------------------------------------------------------------------------
// The report: per-channel weighted means for each path, the delta, and a
// per-pixel |delta| over jointly drawn pixels. Returns the verdict score set.
struct ChannelRow {
    const char* name;
    double mesh = 0, card = 0, joint_abs = 0;
    double norm_score = 0;   // 0..1-ish, for the verdict ranking
};

void report(const std::vector<Pixel>& mesh, const std::vector<Pixel>& card,
            int px, uint32_t view, std::vector<ChannelRow>& verdict_rows) {
    auto wmean = [&](const std::vector<Pixel>& img, auto&& get) {
        double sum = 0, w = 0;
        for (const Pixel& p : img)
            if (p.drawn) { sum += double(get(p)) * p.weight; w += p.weight; }
        return w > 0 ? sum / w : 0.0;
    };
    auto jointmean = [&](auto&& get) {
        double sum = 0; size_t n = 0;
        for (size_t i = 0; i < mesh.size(); ++i)
            if (mesh[i].drawn && card[i].drawn) {
                sum += std::fabs(double(get(mesh[i])) - double(get(card[i])));
                ++n;
            }
        return n ? sum / double(n) : 0.0;
    };
    double mesh_area = 0, card_area = 0; size_t joint = 0;
    for (size_t i = 0; i < mesh.size(); ++i) {
        if (mesh[i].drawn) mesh_area += mesh[i].weight;
        if (card[i].drawn) card_area += 1.0;
        if (mesh[i].drawn && card[i].drawn) ++joint;
    }
    const double total = double(px) * px;

    // normal divergence: mean angle over joint pixels
    double angle_sum = 0; size_t angle_n = 0;
    for (size_t i = 0; i < mesh.size(); ++i)
        if (mesh[i].drawn && card[i].drawn) {
            const float d = clamp01(dot3(mesh[i].normal, card[i].normal));
            angle_sum += std::acos(std::min(std::max(d, -1.f), 1.f)) *
                         57.29577951308232;
            ++angle_n;
        }
    const double mean_angle = angle_n ? angle_sum / double(angle_n) : 0.0;

    // material mismatch over joint pixels
    size_t mat_diff = 0;
    for (size_t i = 0; i < mesh.size(); ++i)
        if (mesh[i].drawn && card[i].drawn &&
            mesh[i].material != card[i].material)
            ++mat_diff;
    const double mat_frac = joint ? double(mat_diff) / double(joint) : 0.0;

    printf("\n-- view %u (ring %u, azimuth %u), %dx%d px --\n", view,
           view / impostor::kAzimuths, view % impostor::kAzimuths, px, px);
    printf("coverage: mesh %.1f%%  card %.1f%%  joint %.1f%%  (card/mesh %.2fx)\n",
           100.0 * mesh_area / total, 100.0 * card_area / total,
           100.0 * double(joint) / total,
           mesh_area > 0 ? card_area / mesh_area : 0.0);
    printf("%-18s %10s %10s %10s %12s\n", "channel", "mesh", "card", "delta",
           "joint|d|");

    verdict_rows.clear();
    auto row = [&](const char* name, auto&& get, double scale) {
        ChannelRow r; r.name = name;
        r.mesh = wmean(mesh, get); r.card = wmean(card, get);
        r.joint_abs = jointmean(get);
        r.norm_score = r.joint_abs / scale;
        printf("%-18s %10.4f %10.4f %+10.4f %12.4f\n", name, r.mesh, r.card,
               r.card - r.mesh, r.joint_abs);
        verdict_rows.push_back(r);
    };
    row("albedo.r", [](const Pixel& p) { return p.albedo[0]; }, 1.0);
    row("albedo.g", [](const Pixel& p) { return p.albedo[1]; }, 1.0);
    row("albedo.b", [](const Pixel& p) { return p.albedo[2]; }, 1.0);
    row("roughness", [](const Pixel& p) { return p.roughness; }, 1.0);
    row("metallic", [](const Pixel& p) { return p.metallic; }, 1.0);
    row("ao", [](const Pixel& p) { return p.ao; }, 1.0);
    {
        ChannelRow r; r.name = "normal(deg)";
        r.mesh = 0; r.card = 0; r.joint_abs = mean_angle;
        r.norm_score = mean_angle / 90.0;
        printf("%-18s %10s %10s %10s %12.2f\n", r.name, "-", "-", "-", mean_angle);
        verdict_rows.push_back(r);
    }
    {
        ChannelRow r; r.name = "material-id";
        r.mesh = 0; r.card = 0; r.joint_abs = mat_frac;
        r.norm_score = mat_frac;
        printf("%-18s %10s %10s %10s %11.1f%%\n", r.name, "-", "-", "-",
               100.0 * mat_frac);
        verdict_rows.push_back(r);
    }
    printf("%-18s %10.4f %10.4f %10s %12s   (RT off: both constant)\n",
           "visibility", 1.0, 1.0, "-", "-");
    row("lit hdr.r", [](const Pixel& p) { return p.hdr[0]; }, 1.0);
    row("lit hdr.g", [](const Pixel& p) { return p.hdr[1]; }, 1.0);
    row("lit hdr.b", [](const Pixel& p) { return p.hdr[2]; }, 1.0);
    row("display.r", [](const Pixel& p) { return p.display[0]; }, 1.0);
    row("display.g", [](const Pixel& p) { return p.display[1]; }, 1.0);
    row("display.b", [](const Pixel& p) { return p.display[2]; }, 1.0);
    printf("display RGB (0-255): mesh %3.0f %3.0f %3.0f   card %3.0f %3.0f %3.0f\n",
           255 * wmean(mesh, [](const Pixel& p) { return p.display[0]; }),
           255 * wmean(mesh, [](const Pixel& p) { return p.display[1]; }),
           255 * wmean(mesh, [](const Pixel& p) { return p.display[2]; }),
           255 * wmean(card, [](const Pixel& p) { return p.display[0]; }),
           255 * wmean(card, [](const Pixel& p) { return p.display[1]; }),
           255 * wmean(card, [](const Pixel& p) { return p.display[2]; }));

    // Verdict: the G-BUFFER channel with the largest normalized divergence.
    // The lit/display rows are outputs, not causes, so they are excluded from
    // the ranking but printed above for the eyeball number.
    size_t best = 0;
    for (size_t i = 0; i < verdict_rows.size(); ++i) {
        const std::string n = verdict_rows[i].name;
        if (n.rfind("lit", 0) == 0 || n.rfind("display", 0) == 0) continue;
        if (verdict_rows[i].norm_score > verdict_rows[best].norm_score) best = i;
    }
    printf("VERDICT view %u: largest G-buffer divergence = %s (score %.4f)\n",
           view, verdict_rows[best].name, verdict_rows[best].norm_score);

    // AB_PROBE_TRACE=<row>: dump one pixel row's normals and lit values, the
    // drill-down for "the verdict says normals, WHICH normals".
    if (const char* tr = std::getenv("AB_PROBE_TRACE")) {
        const int row = std::atoi(tr);
        if (row >= 0 && row < px) {
            for (int x = 0; x < px; ++x) {
                const Pixel& m = mesh[size_t(row) * px + x];
                const Pixel& c = card[size_t(row) * px + x];
                if (!m.drawn && !c.drawn) continue;
                printf("  x=%2d mesh[%c n=(%+.2f,%+.2f,%+.2f) alb=%.2f,%.2f,%.2f"
                       " hdr.g=%.3f] card[%c n=(%+.2f,%+.2f,%+.2f)"
                       " alb=%.2f,%.2f,%.2f hdr.g=%.3f]\n",
                       x, m.drawn ? '*' : '-', m.normal.x, m.normal.y,
                       m.normal.z, m.albedo[0], m.albedo[1], m.albedo[2],
                       m.hdr[1], c.drawn ? '*' : '-', c.normal.x, c.normal.y,
                       c.normal.z, c.albedo[0], c.albedo[1], c.albedo[2],
                       c.hdr[1]);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Fixture: a two-material toy tree — trunk (bark 14, brown tint) + canopy
// shelves (foliageThin 29, dark green tint). Small on purpose.
void build_fixture(std::vector<Tri>& tris, std::vector<TriEx>& triex) {
    auto quad = [&](float3 a, float3 b, float3 c, float3 d, int mat,
                    float4 tint) {
        auto push = [&](float3 v0, float3 v1, float3 v2) {
            Tri t{};
            t.vertex0 = v0; t.vertex1 = v1; t.vertex2 = v2;
            t.centroid = make_float3((v0.x + v1.x + v2.x) / 3.f,
                                     (v0.y + v1.y + v2.y) / 3.f,
                                     (v0.z + v1.z + v2.z) / 3.f);
            TriEx e{};
            const float3 n = unit3(cross3(v1 - v0, v2 - v0));
            e.N0 = e.N1 = e.N2 = n;
            e.materialId = mat;
            e.tint = tint;
            e.ao0 = e.ao1 = e.ao2 = 1.f;
            tris.push_back(t);
            triex.push_back(e);
        };
        push(a, b, c);
        push(a, c, d);
    };
    // The fixture's vertex tint IS its material's registry albedo, and that
    // equality is load-bearing for this probe rather than cosmetic.
    //
    // The card's tint layer is baked from the MATERIAL albedo now (impostor
    // format v8) because that is what the VT page carries for a slotless
    // material, while this probe's mesh reference models resolveBaseColor --
    // the FLAT path, which returns the vertex tint. This file has no virtual
    // texture and cannot have one, so authoring the two differently would make
    // "mesh albedo vs card albedo" measure the gap between two colour
    // authorities instead of the thing this probe exists for: whether the
    // atlas round-trips a colour faithfully through baking, padding and
    // sampling. Tying them together removes that confound; a real divergence
    // in sampling or padding still shows up exactly as before.
    const auto mat_tint = [](int id) {
        const MaterialDef* m = MaterialRegistryGet(id);
        return make_float4(m->albedo[0], m->albedo[1], m->albedo[2], 1.f);
    };
    const float4 bark = mat_tint(14);
    const float4 leaf = mat_tint(29);
    // trunk: a 0.2 m square column, 2 m tall (4 side faces)
    const float r = 0.1f;
    for (int s = 0; s < 4; ++s) {
        const float a0 = 1.57079632679f * s, a1 = a0 + 1.57079632679f;
        const float3 p0 = make_float3(std::cos(a0) * r, 0.f, std::sin(a0) * r);
        const float3 p1 = make_float3(std::cos(a1) * r, 0.f, std::sin(a1) * r);
        quad(p0, p1, make_float3(p1.x, 2.f, p1.z), make_float3(p0.x, 2.f, p0.z),
             14, bark);
    }
    // canopy: 3 horizontal shelf quads of decreasing size, 1.2..2.0 m up
    const float sizes[3] = {1.0f, 0.75f, 0.5f};
    for (int i = 0; i < 3; ++i) {
        const float y = 1.2f + 0.4f * i;
        const float s = sizes[i];
        quad(make_float3(-s, y, -s), make_float3(s, y, -s),
             make_float3(s, y, s), make_float3(-s, y, s), 29, leaf);
        // underside, wound the other way, so the shelf is visible from below
        quad(make_float3(-s, y, s), make_float3(s, y, s),
             make_float3(s, y, -s), make_float3(-s, y, -s), 29, leaf);
    }
}

bool parse_hex64(const char* s, uint64_t& out) {
    char* end = nullptr;
    out = std::strtoull(s, &end, 16);
    return end && *end == '\0';
}

// Concatenate one LOD level's BLAS entries into a flat (tris, triex) pair —
// the same walk the original rep-0 gather did, now shared by every rung.
template <typename Entries>
void gather_level(const Entries& entries, const std::vector<uint32_t>& idxs,
                  std::vector<Tri>& tris, std::vector<TriEx>& triex) {
    for (uint32_t bi : idxs) {
        if (bi >= entries.size()) continue;
        const auto& e = entries[bi];
        tris.insert(tris.end(), e->triangles.begin(), e->triangles.end());
        triex.insert(triex.end(), e->tri_extra.begin(), e->tri_extra.end());
    }
}

// One gathered mesh rung of a ladder (billboard rungs are excluded before
// this table is built, so index i here == mesh rung i of the ladder).
struct Rung {
    std::vector<Tri> tris;
    std::vector<TriEx> triex;
};

// Gather every MESH rung of a ladder, finest first. Billboard rungs (the
// card's own two-triangle quad, part of the same ladder since M2.5) are
// dropped: they are the B side of the A/B, not a mesh reference.
template <typename Entries>
void gather_mesh_rungs(const Entries& entries,
                       const part_asset::LodLevels& lods,
                       std::vector<Rung>& out) {
    for (const auto& lvl : lods) {
        Rung r;
        gather_level(entries, lvl.blas_indices, r.tris, r.triex);
        if (r.tris.empty() || r.triex.size() != r.tris.size()) continue;
        if (impostor::is_billboard_rung(r.tris, r.triex)) continue;
        out.push_back(std::move(r));
    }
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::string bundle;
    uint64_t hash = 0;
    int cluster_arg = -1;
    int mesh_rung_arg = -1;   // -1 = terminal (the on-screen switch), N = rung N
    bool rung_vs = false;     // --rung-vs A,B mesh-vs-mesh mode
    int rung_a = 0, rung_b = -1;   // -1 = terminal
    std::vector<uint32_t> views;
    int px = 32;
    Lighting L;
    // engine default sun vector {-0.45,-0.80,-0.35} -> to_sun = -normalize
    {
        const float3 d = unit3(make_float3(-0.45f, -0.80f, -0.35f));
        L.to_sun = make_float3(-d.x, -d.y, -d.z);
    }
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            return i + 1 < argc ? argv[++i] : "";
        };
        if (a == "--bundle") bundle = next();
        else if (a == "--hash") {
            if (!parse_hex64(next(), hash)) {
                fprintf(stderr, "bad --hash\n");
                return 1;
            }
        } else if (a == "--cluster") cluster_arg = std::atoi(next());
        else if (a == "--mesh-rung") {
            const std::string v = next();
            if (v == "terminal") mesh_rung_arg = -1;
            else if (!v.empty() &&
                     v.find_first_not_of("0123456789") == std::string::npos)
                mesh_rung_arg = std::atoi(v.c_str());
            else {
                fprintf(stderr, "bad --mesh-rung (want 0|terminal|N)\n");
                return 1;
            }
        } else if (a == "--rung-vs") {
            const std::string v = next();
            const size_t comma = v.find(',');
            if (comma == std::string::npos) {
                fprintf(stderr, "bad --rung-vs (want A,B; B may be t)\n");
                return 1;
            }
            rung_a = std::atoi(v.c_str());
            const std::string b2 = v.substr(comma + 1);
            rung_b = (b2 == "t" || b2 == "terminal") ? -1 : std::atoi(b2.c_str());
            rung_vs = true;
        } else if (a == "--view") views.push_back(uint32_t(std::atoi(next())));
        else if (a == "--px") px = std::atoi(next());
        else if (a == "--sun") {
            float az = 0, el = 0;
            if (std::sscanf(next(), "%f,%f", &az, &el) == 2) {
                const float ar = az * 0.01745329252f, er = el * 0.01745329252f;
                const float ce = std::cos(er);
                L.to_sun = make_float3(ce * std::sin(ar), std::sin(er),
                                       -ce * std::cos(ar));
            }
        } else if (a == "--sun-mult") L.sun_mult = float(std::atof(next()));
        else {
            fprintf(stderr, "unknown arg %s\n", a.c_str());
            return 1;
        }
    }
    if (views.empty()) views = {0u, 20u, 40u};
    if (px < 4 || px > 512) { fprintf(stderr, "bad --px\n"); return 1; }

    printf("impostor_ab_probe: cell_px=%u  to_sun=(%.3f,%.3f,%.3f)  sun_mult=%.2f\n",
           impostor::cell_px(), L.to_sun.x, L.to_sun.y, L.to_sun.z, L.sun_mult);

    // Gather clusters to probe. The BAKE source (tris/triex) is ALWAYS rep 0
    // — that is what production bakes the atlas from (part_flatten.cpp
    // src_idx = level_metas.front()). The MESH reference (mesh_tris/
    // mesh_triex) is the --mesh-rung selection; by default the terminal rung
    // the billboard replaces on screen.
    struct Probe {
        std::vector<Tri> tris;         // rep 0 — the atlas bake source
        std::vector<TriEx> triex;
        std::vector<Tri> mesh_tris;    // the mesh reference (--mesh-rung)
        std::vector<TriEx> mesh_triex;
        std::string label;
    };
    std::vector<Probe> probes;
    // Turn a gathered ladder into a Probe. Returns false when the ladder has
    // no usable mesh rung.
    auto make_probe = [&](std::vector<Rung>& rungs, const char* kind,
                          size_t cluster_idx, Probe& p) -> bool {
        if (rungs.empty()) return false;
        const int n = int(rungs.size());
        if (rung_vs) {
            // MESH-vs-MESH: rung A -> the "mesh" column, rung B -> the "card"
            // column. No impostor is baked or sampled in this mode.
            const int ai = std::min(std::max(rung_a, 0), n - 1);
            const int bi = rung_b < 0 ? n - 1 : std::min(rung_b, n - 1);
            p.tris = rungs[size_t(ai)].tris;
            p.triex = rungs[size_t(ai)].triex;
            p.mesh_tris = rungs[size_t(bi)].tris;
            p.mesh_triex = rungs[size_t(bi)].triex;
            char buf[128];
            std::snprintf(buf, sizeof buf,
                          "%s %zu: MESH rung %d (%zu tris) vs MESH rung %d/%d "
                          "(%zu tris%s)",
                          kind, cluster_idx, ai, p.tris.size(), bi, n - 1,
                          p.mesh_tris.size(), bi == n - 1 ? ", terminal" : "");
            p.label = buf;
            return true;
        }
        int mi = mesh_rung_arg < 0 ? n - 1 : mesh_rung_arg;
        if (mi >= n) {
            fprintf(stderr,
                    "note: --mesh-rung %d out of range (ladder has %d mesh "
                    "rungs); clamped to terminal %d\n",
                    mi, n, n - 1);
            mi = n - 1;
        }
        p.mesh_tris = rungs[size_t(mi)].tris;      // copy: rungs[0] may alias
        p.mesh_triex = rungs[size_t(mi)].triex;
        p.tris = std::move(rungs[0].tris);
        p.triex = std::move(rungs[0].triex);
        char buf[128];
        std::snprintf(buf, sizeof buf,
                      "%s %zu: bake rep0 (%zu tris) vs mesh rung %d/%d (%zu "
                      "tris%s)",
                      kind, cluster_idx, p.tris.size(), mi, n - 1,
                      p.mesh_tris.size(), mi == n - 1 ? ", terminal" : "");
        p.label = buf;
        return true;
    };
    if (bundle.empty()) {
        Probe p;
        build_fixture(p.tris, p.triex);
        p.mesh_tris = p.tris;
        p.mesh_triex = p.triex;
        p.label = "fixture (trunk 14 + canopy 29)";
        probes.push_back(std::move(p));
    } else {
        BLASManager blas;
        TLASManager tlas(4096);
        std::vector<part_asset::FlatCluster> clusters;
        if (part_asset::load_flat_v3(bundle, hash, blas, tlas, clusters)) {
            const auto& entries = blas.get_entries();
            for (size_t c = 0; c < clusters.size(); ++c) {
                if (cluster_arg >= 0 && int(c) != cluster_arg) continue;
                if (clusters[c].lods.empty()) continue;
                std::vector<Rung> rungs;
                gather_mesh_rungs(entries, clusters[c].lods, rungs);
                Probe p;
                if (make_probe(rungs, "flat cluster", c, p))
                    probes.push_back(std::move(p));
            }
        } else {
            // The FLAT section is salted with the ladder-shape digest, so a
            // flat baked under different MATTER_IMPOSTOR_DISTANCE /
            // MATTER_LOD_MAX_MESH_RUNGS env is (correctly) rejected here.
            // REP0 is deliberately unsalted — the authored geometry no ladder
            // knob touches — and it is exactly what the A/B needs, so fall
            // back to it rather than requiring the caller to reproduce the
            // editor's env.
            fprintf(stderr,
                    "note: FLAT rejected (ladder shape/env mismatch); "
                    "falling back to REP0\n");
            std::vector<part_asset::ChildInstance> children;
            part_asset::LodLevels lods;
            if (!part_asset::load_v2(bundle, hash, blas, tlas, children,
                                     lods)) {
                fprintf(stderr, "load_v2 (REP0) also failed for %s\n",
                        bundle.c_str());
                return 1;
            }
            if (!children.empty())
                fprintf(stderr,
                        "note: part has %zu child instances; REP0 probes only "
                        "its own geometry\n",
                        children.size());
            const auto& entries = blas.get_entries();
            std::vector<Rung> rungs;
            if (!lods.empty()) gather_mesh_rungs(entries, lods, rungs);
            if (rungs.empty()) {
                // No ladder in the REP0 section: everything there is rep 0.
                Rung r;
                std::vector<uint32_t> all;
                for (uint32_t i = 0; i < entries.size(); ++i) all.push_back(i);
                gather_level(entries, all, r.tris, r.triex);
                if (!r.tris.empty() && r.triex.size() == r.tris.size() &&
                    !impostor::is_billboard_rung(r.tris, r.triex))
                    rungs.push_back(std::move(r));
                if (mesh_rung_arg != 0 && !rungs.empty())
                    fprintf(stderr,
                            "note: REP0 section carries no ladder — the mesh "
                            "reference IS rep 0 here, not the terminal rung. "
                            "For the on-screen A/B, load FLAT (reproduce the "
                            "editor's MATTER_IMPOSTOR_* env).\n");
            }
            Probe p;
            if (make_probe(rungs, "REP0", 0, p))
                probes.push_back(std::move(p));
        }
        if (probes.empty()) {
            fprintf(stderr, "no usable clusters in %s\n", bundle.c_str());
            return 1;
        }
    }

    for (const Probe& p : probes) {
        printf("\n==== %s ====\n", p.label.c_str());
        if (rung_vs) {
            // Shared orthographic frame from the FINE rung's AABB (the union
            // is dominated by it; both sides use the same centre/extent so a
            // per-pixel diff is meaningful).
            float mn[3] = {3.4e38f, 3.4e38f, 3.4e38f};
            float mx[3] = {-3.4e38f, -3.4e38f, -3.4e38f};
            for (const Tri& t : p.tris) {
                for (const float3* v : {&t.vertex0, &t.vertex1, &t.vertex2}) {
                    mn[0] = std::min(mn[0], v->x); mx[0] = std::max(mx[0], v->x);
                    mn[1] = std::min(mn[1], v->y); mx[1] = std::max(mx[1], v->y);
                    mn[2] = std::min(mn[2], v->z); mx[2] = std::max(mx[2], v->z);
                }
            }
            const float3 center = make_float3((mn[0] + mx[0]) * 0.5f,
                                              (mn[1] + mx[1]) * 0.5f,
                                              (mn[2] + mx[2]) * 0.5f);
            float half = 0.f;
            for (int k = 0; k < 3; ++k)
                half = std::max(half, (mx[k] - mn[k]) * 0.5f);
            half *= 1.10f;   // margin so silhouettes don't clip
            printf("frame: center=(%.2f,%.2f,%.2f) half_extent=%.3f\n",
                   center.x, center.y, center.z, half);
            for (uint32_t v : views) {
                if (v >= impostor::kViews) continue;
                std::vector<Pixel> a_img, b_img;
                render_mesh(p.tris, p.triex, center, half, v, px, 4, L, a_img);
                render_mesh(p.mesh_tris, p.mesh_triex, center, half, v, px, 4,
                            L, b_img);
                std::vector<ChannelRow> rows;
                printf("(columns: mesh = rung A, card = rung B)\n");
                report(a_img, b_img, px, v, rows);
            }
            continue;
        }
        impostor::ClusterImpostor imp;
        if (!impostor::bake_cluster(0, p.tris, p.triex, imp)) {
            printf("bake_cluster failed\n");
            ++g_failures;
            continue;
        }
        const float3 center =
            make_float3(imp.center[0], imp.center[1], imp.center[2]);
        printf("vote=%u half_extent=%.3f source_tris=%u\n", imp.material_index,
               imp.half_extent, imp.source_tris);
        for (uint32_t v : views) {
            if (v >= impostor::kViews) continue;
            std::vector<Pixel> mesh_img, card_img;
            render_mesh(p.mesh_tris, p.mesh_triex, center, imp.half_extent, v,
                        px, 4, L, mesh_img);
            render_card(imp, v, px, L, card_img);
            std::vector<ChannelRow> rows;
            report(mesh_img, card_img, px, v, rows);
            if (bundle.empty()) {
                // Fixture mode doubles as the instrument's regression test:
                // the properties asserted are the ones the tool's own logic
                // guarantees, not a claim that mesh and card look identical.
                auto find = [&](const char* n) -> const ChannelRow& {
                    for (const auto& r : rows)
                        if (std::strcmp(r.name, n) == 0) return r;
                    static ChannelRow dummy;
                    return dummy;
                };
                CHECK(find("albedo.g").joint_abs < 0.08,
                      "fixture: albedo agrees within tolerance (padding holds)");
                CHECK(find("metallic").joint_abs < 1e-6,
                      "fixture: metallic identical (both zero)");
                CHECK(find("ao").joint_abs < 1e-6,
                      "fixture: ao identical (both one)");
                // Only where the trunk is visible at all: from the top ring
                // the canopy hides it and every joint pixel is the voted
                // material on both sides.
                if (v < impostor::kAzimuths)
                    CHECK(find("material-id").joint_abs > 0.0,
                          "fixture: the vote flattens a two-material part "
                          "(the known per-cluster-material limitation)");
            }
        }
    }
    if (bundle.empty()) {
        if (g_failures == 0) {
            printf("\nimpostor_ab_probe: ALL PASS\n");
            return 0;
        }
        printf("\nimpostor_ab_probe: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    return 0;
}
