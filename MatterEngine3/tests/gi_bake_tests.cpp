// gi_bake_tests.cpp — headless suite for the GI lightmap bake (gi_bake.h).
//
// GL-free: BLASManager is only used to build BVHs, raylib rides along for the
// Tri<->mesh bridge like every other BLAS-linking suite. Everything here is a
// synthetic in-memory scene, so the suite runs without a project cache.
//
//   1. image writers: CRC/Adler vectors, PNG framing, RGBE round trip.
//   2. two-room fixture (open courtyard + closed room with one doorway):
//      numeric irradiance assertions — lit courtyard floor vs the sunlit strip
//      through the door vs the shadowed deep floor vs the bounce-lit wall.
//   3. seam continuity across chart edges on a curved vault that the normal
//      cone forces into several charts.
//   4. determinism: threads 1 vs 2 -> identical hashes and identical .hdr bytes.
//   5. cache hooks + blob round trip.

#include "gi_bake.h"
#include "gi_bake_image.h"

#include "blas_manager.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "check.h"

namespace {

// ---------------------------------------------------------------------------
// Fixture geometry
// ---------------------------------------------------------------------------
struct FixturePart {
    gi_bake::Part part;
    std::unique_ptr<BLASManager> blas;
};

float3 v3(float x, float y, float z) { return make_float3(x, y, z); }

// Two triangles for the quad (p0 p1 p2 p3), wound so the geometric normal
// agrees with `n`; the TriEx tint with a = 1 pins the albedo exactly.
void add_quad(gi_bake::Part& part, float3 p0, float3 p1, float3 p2, float3 p3,
              float3 n, float3 albedo) {
    const float3 g = cross(p1 - p0, p2 - p0);
    if (dot(g, n) < 0.0f) std::swap(p1, p3);
    auto push = [&](float3 a, float3 b, float3 c) {
        Tri t{};
        t.vertex0 = a; t.vertex1 = b; t.vertex2 = c;
        t.centroid = (a + b + c) * (1.0f / 3.0f);
        TriEx e{};
        e.N0 = e.N1 = e.N2 = normalize(n);
        e.materialId = 1;   // builtin 1: emission 0 (builtin 0 is faintly emissive)
        e.tint = make_float4(albedo.x, albedo.y, albedo.z, 1.0f);
        e.uv0 = e.uv1 = e.uv2 = make_float2(0.0f);
        part.tris.push_back(t);
        part.triex.push_back(e);
    };
    push(p0, p1, p2);
    push(p0, p2, p3);
}

// Same quad, but with smooth per-vertex normals (a curved surface whose
// lighting must be continuous across triangle and chart edges).
void add_quad_smooth(gi_bake::Part& part, float3 p0, float3 p1, float3 p2, float3 p3,
                     float3 n0, float3 n1, float3 n2, float3 n3, float3 face_n, float3 albedo) {
    const float3 g = cross(p1 - p0, p2 - p0);
    if (dot(g, face_n) < 0.0f) { std::swap(p1, p3); std::swap(n1, n3); }
    auto push = [&](float3 a, float3 b, float3 c, float3 na, float3 nb, float3 nc) {
        Tri t{};
        t.vertex0 = a; t.vertex1 = b; t.vertex2 = c;
        t.centroid = (a + b + c) * (1.0f / 3.0f);
        TriEx e{};
        e.N0 = normalize(na); e.N1 = normalize(nb); e.N2 = normalize(nc);
        e.materialId = 1;   // builtin 1: emission 0 (builtin 0 is faintly emissive)
        e.tint = make_float4(albedo.x, albedo.y, albedo.z, 1.0f);
        e.uv0 = e.uv1 = e.uv2 = make_float2(0.0f);
        part.tris.push_back(t);
        part.triex.push_back(e);
    };
    push(p0, p1, p2, n0, n1, n2);
    push(p0, p2, p3, n0, n2, n3);
}

void finish_part(FixturePart& fp) {
    fp.blas = std::make_unique<BLASManager>();
    const BLASHandle h = fp.blas->register_triangles(fp.part.tris, fp.part.triex, true);
    fp.part.entries.clear();
    fp.part.entries.push_back(fp.blas->get_entry(h));
}

// Courtyard: 40x40 m ground at y = 0. Room: x in [-8, 0], z in [-4, 4],
// y in [0, 4], interior faces only, one doorway on the +x wall
// (z in [-1, 1], y < 2.5). The sun comes from +x, so light enters the door
// and lands on a floor strip just inside it; the room's shadow falls on the
// -x side of the courtyard.
struct TwoRooms {
    FixturePart ground, room;
    gi_bake::Scene scene;
    gi_bake::Lighting lighting;

    TwoRooms() {
        const float3 white = v3(0.6f, 0.6f, 0.6f);
        ground.part.name = "ground";
        add_quad(ground.part, v3(-20, 0, -20), v3(20, 0, -20), v3(20, 0, 20), v3(-20, 0, 20),
                 v3(0, 1, 0), white);
        finish_part(ground);

        room.part.name = "room";
        gi_bake::Part& r = room.part;
        // -x wall (faces +x, toward the door)
        add_quad(r, v3(-8, 0, -4), v3(-8, 4, -4), v3(-8, 4, 4), v3(-8, 0, 4), v3(1, 0, 0), white);
        // -z and +z walls
        add_quad(r, v3(-8, 0, -4), v3(0, 0, -4), v3(0, 4, -4), v3(-8, 4, -4), v3(0, 0, 1), white);
        add_quad(r, v3(-8, 0, 4), v3(0, 0, 4), v3(0, 4, 4), v3(-8, 4, 4), v3(0, 0, -1), white);
        // ceiling (faces down)
        add_quad(r, v3(-8, 4, -4), v3(0, 4, -4), v3(0, 4, 4), v3(-8, 4, 4), v3(0, -1, 0), white);
        // +x wall with the doorway: two side pieces + lintel
        add_quad(r, v3(0, 0, -4), v3(0, 4, -4), v3(0, 4, -1), v3(0, 0, -1), v3(-1, 0, 0), white);
        add_quad(r, v3(0, 0, 1), v3(0, 4, 1), v3(0, 4, 4), v3(0, 0, 4), v3(-1, 0, 0), white);
        add_quad(r, v3(0, 2.5f, -1), v3(0, 4, -1), v3(0, 4, 1), v3(0, 2.5f, 1), v3(-1, 0, 0), white);
        finish_part(room);

        scene.parts.push_back(ground.part);
        scene.parts.push_back(room.part);
        gi_bake::Instance gi; gi.part = 0; gi.name = "ground";
        gi_bake::Instance ri; ri.part = 1; ri.name = "room";
        scene.instances.push_back(gi);
        scene.instances.push_back(ri);

        const float3 to_sun = normalize(v3(0.75f, 0.55f, 0.0f));
        lighting.sun_direction[0] = -to_sun.x;
        lighting.sun_direction[1] = -to_sun.y;
        lighting.sun_direction[2] = -to_sun.z;
    }
};

float lum(const float* rgb) { return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]; }

bool sample(const gi_bake::Scene& s, const gi_bake::BakeResult& r, uint32_t inst,
            float x, float y, float z, float out[3]) {
    const float p[3] = {x, y, z};
    return gi_bake::sample_at(s, r, inst, p, out);
}

// ---------------------------------------------------------------------------
// 1. Image writers
// ---------------------------------------------------------------------------
void test_image_writers() {
    using namespace gi_bake_image;
    CHECK(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u, "crc32 check vector");
    CHECK(adler32(reinterpret_cast<const uint8_t*>("Wikipedia"), 9) == 0x11E60398u, "adler32 check vector");

    std::vector<uint8_t> png;
    const uint8_t px8[2 * 2 * 3] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 10, 20, 30};
    CHECK(encode_png_rgb8(2, 2, px8, png), "png8 encodes");
    CHECK(png.size() > 33 && png[0] == 137 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G',
          "png8 signature");
    CHECK(std::memcmp(&png[12], "IHDR", 4) == 0 && png[19] == 2 && png[23] == 2 && png[24] == 8 && png[25] == 2,
          "png8 IHDR: 2x2, depth 8, RGB");
    {
        // IHDR CRC covers type + data (17 bytes at offset 12).
        const uint32_t crc = crc32(&png[12], 17);
        const uint32_t stored = (uint32_t(png[29]) << 24) | (uint32_t(png[30]) << 16) | (uint32_t(png[31]) << 8) | png[32];
        CHECK(crc == stored, "png8 IHDR chunk CRC");
    }
    const uint16_t px16[3] = {65535, 32768, 0};
    CHECK(encode_png_rgb16(1, 1, px16, png) && png[24] == 16, "png16 depth 16");

    // RGBE round trip, both the RLE (width >= 8) and flat (width < 8) paths.
    for (uint32_t w : {4u, 16u}) {
        std::vector<float> rgb(size_t(w) * 3 * 3);
        for (size_t i = 0; i < rgb.size(); ++i) rgb[i] = (i % 7 == 0) ? 0.0f : 0.01f * float(i % 97) + float(i / 50);
        std::vector<uint8_t> hdr;
        CHECK(encode_hdr(w, 3, rgb.data(), hdr), "hdr encodes");
        uint32_t dw = 0, dh = 0;
        std::vector<float> back;
        CHECK(decode_hdr(hdr.data(), hdr.size(), dw, dh, back), "hdr decodes");
        CHECK(dw == w && dh == 3, "hdr dimensions round trip");
        float worst = 0.0f;
        for (size_t i = 0; i < rgb.size() && i < back.size(); ++i) {
            const float ref = rgb[i];
            const float err = std::fabs(back[i] - ref) / std::max(ref, 1e-3f);
            if (ref > 1e-3f) worst = std::max(worst, err);
            else worst = std::max(worst, back[i]);
        }
        CHECK(worst < 0.01f, "hdr round trip within RGBE precision (1%)");
    }
    // A long run of identical texels must survive the RLE path exactly.
    {
        std::vector<float> flat(size_t(300) * 3, 1.5f);
        std::vector<uint8_t> hdr;
        CHECK(encode_hdr(300, 1, flat.data(), hdr), "hdr encodes a run");
        uint32_t dw = 0, dh = 0; std::vector<float> back;
        CHECK(decode_hdr(hdr.data(), hdr.size(), dw, dh, back) && dw == 300, "hdr run decodes");
        bool same = true;
        for (float v : back) same = same && std::fabs(v - 1.5f) < 0.01f;
        CHECK(same, "hdr run values");
    }
    CHECK(std::fabs(tonemap_ldr(4.0f) - 1.0f) < 1e-5f, "tonemap white point maps 4.0 to 1.0");
    CHECK(tonemap_ldr(0.0f) == 0.0f && tonemap_ldr(-1.0f) == 0.0f, "tonemap clamps");
}

// ---------------------------------------------------------------------------
// 2. Two-room fixture
// ---------------------------------------------------------------------------
void test_two_rooms() {
    TwoRooms fx;
    gi_bake::Settings s;
    s.samples = 48; s.bounces = 2; s.texel_density = 4.0f; s.threads = 2; s.denoise = true;

    gi_bake::BakeResult r;
    std::string err;
    CHECK(gi_bake::bake_scene(fx.scene, s, fx.lighting, r, err), ("bake_scene: " + err).c_str());
    if (r.lightmaps.size() != 2) { CHECK(false, "two lightmaps"); return; }
    CHECK(r.atlases[0].ok && r.atlases[1].ok, "atlases built");
    CHECK(r.lightmaps[0].covered_texels > 1000, "ground lightmap has coverage");
    CHECK(r.lightmaps[1].covered_texels > 500, "room lightmap has coverage");
    printf("[gi] ground %ux%u covered=%u rays=%llu %.0fms | room %ux%u covered=%u rays=%llu %.0fms\n",
           r.lightmaps[0].width, r.lightmaps[0].height, r.lightmaps[0].covered_texels,
           (unsigned long long)r.lightmaps[0].rays, r.lightmaps[0].trace_ms,
           r.lightmaps[1].width, r.lightmaps[1].height, r.lightmaps[1].covered_texels,
           (unsigned long long)r.lightmaps[1].rays, r.lightmaps[1].trace_ms);

    // Analytic courtyard value: sun_color * cos(theta) + sky_color (E/pi units).
    const float3 to_sun = normalize(v3(0.75f, 0.55f, 0.0f));
    const float cos_t = to_sun.y;
    float court[3], strip[3], deep[3], shadow[3], wall[3];
    CHECK(sample(fx.scene, r, 0, 10.0f, 0.0f, 10.0f, court), "sample courtyard");
    CHECK(sample(fx.scene, r, 0, -1.5f, 0.0f, 0.0f, strip), "sample sunlit strip inside the door");
    CHECK(sample(fx.scene, r, 0, -7.0f, 0.0f, 0.0f, deep), "sample deep room floor");
    CHECK(sample(fx.scene, r, 0, -10.0f, 0.0f, 0.0f, shadow), "sample courtyard in the room's shadow");
    CHECK(sample(fx.scene, r, 1, -7.999f, 1.5f, 0.0f, wall), "sample door-facing interior wall");
    printf("[gi] court=(%.3f %.3f %.3f) strip=%.3f deep=%.3f shadow=%.3f wall=%.3f\n",
           court[0], court[1], court[2], lum(strip), lum(deep), lum(shadow), lum(wall));

    for (int c = 0; c < 3; ++c) {
        const float expect = fx.lighting.sun_color[c] * cos_t + fx.lighting.sky_color[c];
        CHECK(std::fabs(court[c] - expect) <= 0.08f * expect,
              "open courtyard floor matches sun*cos + sky within 8%");
    }
    const float court_l = lum(court);
    CHECK(lum(deep) < 0.15f * court_l, "deep room floor is shadowed (< 15% of the courtyard)");
    CHECK(lum(strip) > 0.65f * court_l && lum(strip) < 1.02f * court_l,
          "sunlit strip through the doorway carries the direct sun term");
    CHECK(lum(shadow) < 0.45f * court_l && lum(shadow) > 0.10f * court_l,
          "the room's cast shadow on the courtyard is sky-lit only");

    // Direct-only bake: the interior wall loses its bounce fill.
    gi_bake::Settings direct = s; direct.bounces = 0;
    gi_bake::BakeResult r0;
    CHECK(gi_bake::bake_scene(fx.scene, direct, fx.lighting, r0, err), "direct-only bake");
    float wall0[3], deep0[3];
    CHECK(sample(fx.scene, r0, 1, -7.999f, 1.5f, 0.0f, wall0), "sample wall (direct only)");
    CHECK(sample(fx.scene, r0, 0, -7.0f, 0.0f, 0.0f, deep0), "sample deep floor (direct only)");
    printf("[gi] direct-only wall=%.3f deep=%.3f\n", lum(wall0), lum(deep0));
    CHECK(lum(wall0) > 0.0f, "direct-only wall still sees sky through the door");
    CHECK(lum(wall) > 1.5f * lum(wall0), "bounces light the door-facing wall (> 1.5x direct-only)");
    CHECK(lum(deep) >= lum(deep0), "bounces never remove energy from the deep floor");

    // Coverage bookkeeping: dilation only adds texels, never removes.
    uint32_t covered = 0, dilated = 0;
    for (uint8_t c : r.lightmaps[0].coverage) { covered += c == gi_bake::kTexelCovered; dilated += c == gi_bake::kTexelDilated; }
    CHECK(covered == r.lightmaps[0].covered_texels, "coverage count matches covered texels");
    CHECK(dilated > 0, "dilation padded the chart gutters");
}

// ---------------------------------------------------------------------------
// 3. Seam continuity across chart boundaries
// ---------------------------------------------------------------------------
// Bilinear sample through one triangle's own chart UVs (no chart search), so
// the two sides of a chart edge can be compared explicitly.
bool sample_tri(const gi_bake::Part& part, const gi_bake::PartAtlas& atlas,
                const gi_bake::Lightmap& map, uint32_t t, const float bary[3], float out[3]) {
    const TriEx& e = part.triex[t];
    const float u = (e.uv0.x * bary[0] + e.uv1.x * bary[1] + e.uv2.x * bary[2]) * float(atlas.width);
    const float v = (e.uv0.y * bary[0] + e.uv1.y * bary[1] + e.uv2.y * bary[2]) * float(atlas.height);
    const float x = std::min(std::max(u - 0.5f, 0.0f), float(atlas.width - 1));
    const float y = std::min(std::max(v - 0.5f, 0.0f), float(atlas.height - 1));
    const uint32_t x0 = uint32_t(x), y0 = uint32_t(y);
    const uint32_t x1 = std::min(x0 + 1, atlas.width - 1), y1 = std::min(y0 + 1, atlas.height - 1);
    const float fx = x - float(x0), fy = y - float(y0);
    for (int c = 0; c < 3; ++c) {
        const float v00 = map.rgb[(size_t(y0) * atlas.width + x0) * 3 + c];
        const float v10 = map.rgb[(size_t(y0) * atlas.width + x1) * 3 + c];
        const float v01 = map.rgb[(size_t(y1) * atlas.width + x0) * 3 + c];
        const float v11 = map.rgb[(size_t(y1) * atlas.width + x1) * 3 + c];
        out[c] = (v00 * (1 - fx) + v10 * fx) * (1 - fy) + (v01 * (1 - fx) + v11 * fx) * fy;
    }
    return true;
}

struct VKey { float x, y, z; bool operator<(const VKey& o) const { return std::memcmp(this, &o, sizeof *this) < 0; } };

void test_seams() {
    // A half-cylinder vault (axis z, radius 5) over the ground: 36 segments of
    // 5 degrees, so the 45-degree normal cone splits it into several charts
    // whose boundaries cut across a smoothly varying lighting field.
    FixturePart ground, vault;
    ground.part.name = "ground";
    add_quad(ground.part, v3(-20, 0, -20), v3(20, 0, -20), v3(20, 0, 20), v3(-20, 0, 20), v3(0, 1, 0), v3(0.5f, 0.5f, 0.5f));
    finish_part(ground);
    vault.part.name = "vault";
    const int segs = 36, rows = 4;
    const float R = 5.0f;
    for (int i = 0; i < segs; ++i) {
        const float a0 = 3.14159265f * float(i) / segs, a1 = 3.14159265f * float(i + 1) / segs;
        for (int j = 0; j < rows; ++j) {
            const float z0 = -6.0f + 12.0f * float(j) / rows, z1 = -6.0f + 12.0f * float(j + 1) / rows;
            const float3 p0 = v3(R * std::cos(a0), R * std::sin(a0), z0);
            const float3 p1 = v3(R * std::cos(a1), R * std::sin(a1), z0);
            const float3 p2 = v3(R * std::cos(a1), R * std::sin(a1), z1);
            const float3 p3 = v3(R * std::cos(a0), R * std::sin(a0), z1);
            const float am = 0.5f * (a0 + a1);
            // Smooth normals: the true cylinder normal at each vertex, so the
            // lighting is continuous and any step at a chart edge is a seam.
            add_quad_smooth(vault.part, p0, p1, p2, p3,
                            v3(std::cos(a0), std::sin(a0), 0), v3(std::cos(a1), std::sin(a1), 0),
                            v3(std::cos(a1), std::sin(a1), 0), v3(std::cos(a0), std::sin(a0), 0),
                            v3(std::cos(am), std::sin(am), 0), v3(0.7f, 0.7f, 0.7f));
        }
    }
    finish_part(vault);
    gi_bake::Scene scene;
    scene.parts.push_back(ground.part);
    scene.parts.push_back(vault.part);
    gi_bake::Instance gi; gi.part = 0; gi.name = "ground";
    gi_bake::Instance vi; vi.part = 1; vi.name = "vault";
    scene.instances.push_back(gi);
    scene.instances.push_back(vi);
    gi_bake::Lighting l;
    const float3 to_sun = normalize(v3(0.3f, 0.9f, 0.2f));
    l.sun_direction[0] = -to_sun.x; l.sun_direction[1] = -to_sun.y; l.sun_direction[2] = -to_sun.z;

    gi_bake::Settings s;
    s.samples = 64; s.bounces = 1; s.texel_density = 4.0f; s.threads = 2;
    gi_bake::BakeResult r;
    std::string err;
    CHECK(gi_bake::bake_scene(scene, s, l, r, err), ("seam bake: " + err).c_str());
    const gi_bake::PartAtlas& atlas = r.atlases[1];
    CHECK(atlas.rung.charts.size() >= 2, "the vault splits into several charts");
    printf("[gi] vault charts=%zu atlas=%ux%u\n", atlas.rung.charts.size(), atlas.width, atlas.height);

    // Shared edges between triangles: welded by exact position.
    const gi_bake::Part& part = scene.parts[1];
    std::map<std::pair<VKey, VKey>, std::vector<std::pair<uint32_t, int>>> edges;
    auto key = [](const float3& p) { return VKey{p.x, p.y, p.z}; };
    for (uint32_t t = 0; t < part.tris.size(); ++t) {
        const float3 v[3] = {part.tris[t].vertex0, part.tris[t].vertex1, part.tris[t].vertex2};
        for (int e = 0; e < 3; ++e) {
            VKey a = key(v[e]), b = key(v[(e + 1) % 3]);
            if (b < a) std::swap(a, b);
            edges[{a, b}].push_back({t, e});
        }
    }
    std::vector<float> seam_deltas, inner_deltas, seam_values;
    for (const auto& kv : edges) {
        if (kv.second.size() != 2) continue;
        const auto [ta, ea] = kv.second[0];
        const auto [tb, eb] = kv.second[1];
        const bool cross_chart = atlas.tri_chart[ta] != atlas.tri_chart[tb];
        for (int k = 1; k <= 3; ++k) {
            const float f = float(k) / 4.0f;
            // Point on the edge at fraction f, nudged 1/4 texel into each triangle.
            auto bary_for = [&](uint32_t t, int e, float frac, float inset, float out[3]) {
                float b[3] = {0, 0, 0};
                b[e] = 1.0f - frac; b[(e + 1) % 3] = frac;
                const int opp = (e + 2) % 3;
                b[e] -= inset * (1.0f - frac); b[(e + 1) % 3] -= inset * frac; b[opp] += inset;
                (void)t;
                out[0] = b[0]; out[1] = b[1]; out[2] = b[2];
            };
            float ba[3], bb[3], va[3], vb[3];
            // Fraction is measured from vertex e of each triangle; the two
            // triangles traverse the shared edge in opposite directions.
            const float3 a_start = (ea == 0 ? part.tris[ta].vertex0 : ea == 1 ? part.tris[ta].vertex1 : part.tris[ta].vertex2);
            const float3 b_start = (eb == 0 ? part.tris[tb].vertex0 : eb == 1 ? part.tris[tb].vertex1 : part.tris[tb].vertex2);
            const bool same_dir = std::memcmp(&a_start, &b_start, sizeof(float) * 3) == 0;
            bary_for(ta, ea, f, 0.05f, ba);
            bary_for(tb, eb, same_dir ? f : 1.0f - f, 0.05f, bb);
            sample_tri(part, atlas, r.lightmaps[1], ta, ba, va);
            sample_tri(part, atlas, r.lightmaps[1], tb, bb, vb);
            const float d = std::fabs(lum(va) - lum(vb));
            (cross_chart ? seam_deltas : inner_deltas).push_back(d);
            if (cross_chart) seam_values.push_back(0.5f * (lum(va) + lum(vb)));
            if (cross_chart && std::getenv("GI_SEAM_DEBUG")) {
                auto pos_of = [&](uint32_t t, const float* b) {
                    return part.tris[t].vertex0 * b[0] + part.tris[t].vertex1 * b[1] + part.tris[t].vertex2 * b[2];
                };
                auto uv_of = [&](uint32_t t, const float* b, float& u, float& v) {
                    const TriEx& e = part.triex[t];
                    u = (e.uv0.x * b[0] + e.uv1.x * b[1] + e.uv2.x * b[2]) * float(atlas.width);
                    v = (e.uv0.y * b[0] + e.uv1.y * b[1] + e.uv2.y * b[2]) * float(atlas.height);
                };
                const float3 pa = pos_of(ta, ba), pb = pos_of(tb, bb);
                float ua, vaa, ub, vbb;
                uv_of(ta, ba, ua, vaa); uv_of(tb, bb, ub, vbb);
                const size_t ia = size_t(std::min(int(vaa), int(atlas.height) - 1)) * atlas.width + std::min(int(ua), int(atlas.width) - 1);
                const size_t ib = size_t(std::min(int(vbb), int(atlas.height) - 1)) * atlas.width + std::min(int(ub), int(atlas.width) - 1);
                printf("[seam] ta=%u(c%u) tb=%u(c%u) pa=(%.3f %.3f %.3f) pb=(%.3f %.3f %.3f) uva=(%.2f %.2f cov%d) uvb=(%.2f %.2f cov%d) va=%.4f vb=%.4f\n",
                       ta, atlas.tri_chart[ta], tb, atlas.tri_chart[tb], pa.x, pa.y, pa.z, pb.x, pb.y, pb.z,
                       ua, vaa, int(r.lightmaps[1].coverage[ia]), ub, vbb, int(r.lightmaps[1].coverage[ib]),
                       lum(va), lum(vb));
                const chart_atlas::ChartEntry& ca = atlas.rung.charts[atlas.tri_chart[ta]];
                const chart_atlas::ChartEntry& cb = atlas.rung.charts[atlas.tri_chart[tb]];
                printf("[seam] chart a rect=(%u %u %u %u) n=(%.2f %.2f %.2f)x(%.2f %.2f %.2f) | chart b rect=(%u %u %u %u) n=(%.2f %.2f %.2f)x(%.2f %.2f %.2f)\n",
                       ca.rect_x, ca.rect_y, ca.rect_w, ca.rect_h, ca.tangent[0], ca.tangent[1], ca.tangent[2], ca.bitangent[0], ca.bitangent[1], ca.bitangent[2],
                       cb.rect_x, cb.rect_y, cb.rect_w, cb.rect_h, cb.tangent[0], cb.tangent[1], cb.tangent[2], cb.bitangent[0], cb.bitangent[1], cb.bitangent[2]);
            }
        }
    }
    CHECK(!seam_deltas.empty() && !inner_deltas.empty(), "found chart-boundary and interior edges");
    if (seam_deltas.empty() || inner_deltas.empty()) return;
    std::sort(seam_deltas.begin(), seam_deltas.end());
    std::sort(inner_deltas.begin(), inner_deltas.end());
    std::sort(seam_values.begin(), seam_values.end());
    const float seam_max = seam_deltas.back();
    const float seam_p95 = seam_deltas[size_t(0.95f * float(seam_deltas.size() - 1))];
    const float seam_level = seam_values[seam_values.size() / 2];
    const float inner_med = inner_deltas[inner_deltas.size() / 2];
    const float inner_p95 = inner_deltas[size_t(0.95f * float(inner_deltas.size() - 1))];
    printf("[gi] seam deltas: n=%zu max=%.4f p95=%.4f level=%.3f | interior: n=%zu median=%.4f p95=%.4f\n",
           seam_deltas.size(), seam_max, seam_p95, seam_level, inner_deltas.size(), inner_med, inner_p95);
    // A visible seam is a step the interior never shows, and a step under ~2%
    // of the local level (4% at the very worst sample) is below the contrast
    // threshold a viewer can see on a smooth gradient.
    CHECK(seam_p95 <= std::max(2.0f * inner_p95, 0.02f * seam_level),
          "chart-boundary p95 delta within 2x the interior p95 (or 2% of the level)");
    CHECK(seam_max <= std::max(3.0f * inner_p95, 0.04f * seam_level),
          "no chart-boundary delta exceeds 3x the interior p95 (or 4% of the level)");
}

// ---------------------------------------------------------------------------
// 4. Determinism
// ---------------------------------------------------------------------------
void test_determinism() {
    TwoRooms a, b;
    gi_bake::Settings sa; sa.samples = 12; sa.bounces = 2; sa.texel_density = 3.0f; sa.threads = 1;
    gi_bake::Settings sb = sa; sb.threads = 2;
    gi_bake::BakeResult ra, rb;
    std::string err;
    CHECK(gi_bake::bake_scene(a.scene, sa, a.lighting, ra, err), "bake A");
    CHECK(gi_bake::bake_scene(b.scene, sb, b.lighting, rb, err), "bake B");
    if (ra.lightmaps.size() != rb.lightmaps.size()) { CHECK(false, "same lightmap count"); return; }
    for (size_t i = 0; i < ra.lightmaps.size(); ++i) {
        CHECK(ra.lightmaps[i].content_hash == rb.lightmaps[i].content_hash,
              "identical content hash across thread counts");
        CHECK(ra.lightmaps[i].rgb == rb.lightmaps[i].rgb, "identical texels across thread counts");
        std::vector<uint8_t> ha, hb;
        gi_bake_image::encode_hdr(ra.lightmaps[i].width, ra.lightmaps[i].height, ra.lightmaps[i].rgb.data(), ha);
        gi_bake_image::encode_hdr(rb.lightmaps[i].width, rb.lightmaps[i].height, rb.lightmaps[i].rgb.data(), hb);
        CHECK(ha == hb, "identical .hdr bytes");
    }
    CHECK(ra.scene_hash == rb.scene_hash && ra.settings_hash == rb.settings_hash &&
              ra.lighting_hash == rb.lighting_hash,
          "scene/settings/lighting hashes are stable");
    CHECK(gi_bake::cache_key(a.scene, 0, sa, a.lighting) == gi_bake::cache_key(b.scene, 0, sb, b.lighting),
          "cache key ignores thread count");
    gi_bake::Settings sc = sa; sc.seed += 1;
    CHECK(gi_bake::cache_key(a.scene, 0, sc, a.lighting) != gi_bake::cache_key(a.scene, 0, sa, a.lighting),
          "cache key changes with the seed");
    gi_bake::Scene moved = a.scene;
    moved.instances[1].transform[3] += 0.5f;
    CHECK(gi_bake::cache_key(moved, 0, sa, a.lighting) != gi_bake::cache_key(a.scene, 0, sa, a.lighting),
          "moving any instance invalidates every instance's key");
}

// ---------------------------------------------------------------------------
// 5. Cache hooks + blob round trip
// ---------------------------------------------------------------------------
void test_cache() {
    TwoRooms fx;
    gi_bake::Settings s; s.samples = 8; s.bounces = 1; s.texel_density = 3.0f; s.threads = 2; s.prelit = true;
    std::map<std::string, std::vector<uint8_t>> blobs;
    int lookups = 0, stores = 0;
    gi_bake::CacheHooks hooks;
    hooks.lookup = [&](const std::string& key, gi_bake::Lightmap& out) {
        ++lookups;
        auto it = blobs.find(key);
        return it != blobs.end() && gi_bake::deserialize_lightmap(it->second.data(), it->second.size(), out);
    };
    hooks.store = [&](const std::string& key, const gi_bake::Lightmap& map) {
        ++stores;
        blobs[key] = gi_bake::serialize_lightmap(map);
    };
    gi_bake::BakeResult r1, r2;
    std::string err;
    CHECK(gi_bake::bake_scene(fx.scene, s, fx.lighting, r1, err, {}, &hooks), "cached bake 1");
    CHECK(stores == 2 && lookups == 2, "first bake stores both lightmaps");
    CHECK(gi_bake::bake_scene(fx.scene, s, fx.lighting, r2, err, {}, &hooks), "cached bake 2");
    CHECK(stores == 2 && lookups == 4, "second bake hits and stores nothing");
    CHECK(r2.lightmaps[0].cache_hit && r2.lightmaps[1].cache_hit, "second bake reports cache hits");
    CHECK(r2.lightmaps[0].rgb == r1.lightmaps[0].rgb && r2.lightmaps[0].albedo == r1.lightmaps[0].albedo,
          "cached lightmap round-trips texels and albedo");
    CHECK(r2.lightmaps[0].content_hash == r1.lightmaps[0].content_hash, "cached content hash");
    // A corrupt blob must miss, not crash.
    for (auto& kv : blobs) if (kv.second.size() > 64) kv.second[40] ^= 0x5A;
    gi_bake::BakeResult r3;
    CHECK(gi_bake::bake_scene(fx.scene, s, fx.lighting, r3, err, {}, &hooks), "bake after corruption");
    CHECK(!r3.lightmaps[0].cache_hit, "corrupt blob is a miss");
}

} // namespace

int main() {
    test_image_writers();
    test_two_rooms();
    test_seams();
    test_determinism();
    test_cache();
    return check_summary();
}
