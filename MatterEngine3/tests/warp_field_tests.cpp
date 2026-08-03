// Warp-field (VT Phase 2) gates + fixture tooling.
//
// Default mode: run the §4.3 solver quality gates over the committed
// real-mesh fixtures (tests/fixtures/warp_field/*.wfx), print the world-XZ
// baseline measured with the SAME metric code, and verify determinism
// (byte-identical re-solve) and the shared-border pin rule (identical pins
// from both sectors of a committed neighbouring pair).
//
// Tooling modes (not part of the automated gate):
//   warp_field_tests --scan-transient <dir>
//       Scan a transient parts directory (%TEMP%/matter_transient/<pid>/parts)
//       for real voxel sector meshes and print their metrics (area ratio,
//       mean slope, non-graph fraction) so fixture candidates can be chosen.
//   warp_field_tests --cut-fixtures <dir> <out_dir>
//       Cut the §15 fixture set from a transient dataset: one typical rolling
//       sector (area ratio ~1.0-1.3), one mid steep (~2.6), one extreme wall
//       (max ratio), and one shared-border pair (matched by border-chain
//       geometry) — serialized as .wfx files.
//
// .wfx format (WFX1): little-endian, "WFX1" magic, u32 tri_count,
// f64 anchor_x, f64 anchor_z, f32 sector_size, f32 voxel, then tri_count * 9
// f32 corner positions (sector-local, bitwise from the .part), then
// tri_count u8 skirt flags. Triangle soup on purpose: fixtures exercise the
// same weld the production path runs.

#include "part_asset_v2.h"
#include "warp_field.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/stb_image_write.h"  // raylib's vendored copy (PNG dump)

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond, msg)                                        \
    do {                                                        \
        const bool ok_ = (cond);                                \
        std::printf("%s %s\n", ok_ ? "[ok] " : "[FAIL]", msg);  \
        if (!ok_) ++g_failures;                                 \
    } while (0)

// ---------------------------------------------------------------------------
// Fixture IO.
// ---------------------------------------------------------------------------
struct Fixture {
    std::vector<Tri> tris;
    std::vector<uint8_t> skirt;
    double anchor_x = 0.0, anchor_z = 0.0;
    float sector_size = 64.0f;
    float voxel = 2.0f;
};

static bool save_wfx(const std::string& path, const Fixture& f) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    const uint32_t n = (uint32_t)f.tris.size();
    bool ok = std::fwrite("WFX1", 1, 4, fp) == 4 &&
              std::fwrite(&n, 4, 1, fp) == 1 &&
              std::fwrite(&f.anchor_x, 8, 1, fp) == 1 &&
              std::fwrite(&f.anchor_z, 8, 1, fp) == 1 &&
              std::fwrite(&f.sector_size, 4, 1, fp) == 1 &&
              std::fwrite(&f.voxel, 4, 1, fp) == 1;
    for (uint32_t t = 0; ok && t < n; ++t) {
        const float c[9] = {
            f.tris[t].vertex0.x, f.tris[t].vertex0.y, f.tris[t].vertex0.z,
            f.tris[t].vertex1.x, f.tris[t].vertex1.y, f.tris[t].vertex1.z,
            f.tris[t].vertex2.x, f.tris[t].vertex2.y, f.tris[t].vertex2.z};
        ok = std::fwrite(c, 4, 9, fp) == 9;
    }
    if (ok && n)
        ok = std::fwrite(f.skirt.data(), 1, n, fp) == n;
    std::fclose(fp);
    return ok;
}

static bool load_wfx(const std::string& path, Fixture& f) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    char magic[4];
    uint32_t n = 0;
    bool ok = std::fread(magic, 1, 4, fp) == 4 &&
              std::memcmp(magic, "WFX1", 4) == 0 &&
              std::fread(&n, 4, 1, fp) == 1 &&
              std::fread(&f.anchor_x, 8, 1, fp) == 1 &&
              std::fread(&f.anchor_z, 8, 1, fp) == 1 &&
              std::fread(&f.sector_size, 4, 1, fp) == 1 &&
              std::fread(&f.voxel, 4, 1, fp) == 1 && n < (1u << 24);
    if (ok) {
        f.tris.assign(n, Tri{});
        for (uint32_t t = 0; ok && t < n; ++t) {
            float c[9];
            ok = std::fread(c, 4, 9, fp) == 9;
            f.tris[t].vertex0 = make_float3(c[0], c[1], c[2]);
            f.tris[t].vertex1 = make_float3(c[3], c[4], c[5]);
            f.tris[t].vertex2 = make_float3(c[6], c[7], c[8]);
        }
        f.skirt.assign(n, 0);
        if (ok && n) ok = std::fread(f.skirt.data(), 1, n, fp) == n;
    }
    std::fclose(fp);
    return ok;
}

// Local mirror of lod_bake::count_terrain_skirt_tris' rule (kept local so the
// gate binary links without the whole lod_bake closure): a skirt triangle
// contains a vertical edge — two corners with bitwise-equal x AND z but
// different y. Only legacy pre-2026-07-30 artifacts contain any.
static void skirt_mask_of(const std::vector<Tri>& tris,
                          std::vector<uint8_t>& mask) {
    mask.assign(tris.size(), 0);
    for (size_t t = 0; t < tris.size(); ++t) {
        const float3 c[3] = {tris[t].vertex0, tris[t].vertex1,
                             tris[t].vertex2};
        for (int e = 0; e < 3; ++e) {
            const float3 &a = c[e], &b = c[(e + 1) % 3];
            if (a.x == b.x && a.z == b.z && a.y != b.y) {
                mask[t] = 1;
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Transient .part scanning.
// ---------------------------------------------------------------------------
struct SectorInfo {
    std::string path;
    uint64_t hash = 0;
    std::vector<Tri> tris;       // finest LOD union
    std::vector<uint8_t> skirt;
    float ext_x = 0, ext_z = 0, ext_y = 0;
    double area = 0.0;           // non-skirt 3D area
    double area_ratio = 0.0;     // area / footprint
    double mean_slope_deg = 0.0; // area-weighted
    double nongraph_pct = 0.0;   // tris with n.y < 0.05 (overhung/vertical+)
    size_t solved_tris = 0;
};

static bool load_sector_part(const std::string& path, SectorInfo& out) {
    const std::string base = fs::path(path).filename().string();
    uint64_t hash = 0;
    if (base.size() < 16 ||
        std::sscanf(base.c_str(), "%16llx", (unsigned long long*)&hash) != 1)
        return false;
    BLASManager blas;
    TLASManager tlas(65536);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    if (!part_asset::load_v2(path, hash, blas, tlas, children, lods))
        return false;
    const auto& entries = blas.get_entries();
    out.path = path;
    out.hash = hash;
    out.tris.clear();
    if (!lods.empty() && !lods[0].blas_indices.empty()) {
        for (uint32_t bi : lods[0].blas_indices) {
            if (bi >= entries.size()) continue;
            out.tris.insert(out.tris.end(), entries[bi]->triangles.begin(),
                            entries[bi]->triangles.end());
        }
    } else {
        for (const auto& e : entries)
            out.tris.insert(out.tris.end(), e->triangles.begin(),
                            e->triangles.end());
    }
    return !out.tris.empty();
}

static void measure_sector(SectorInfo& s) {
    skirt_mask_of(s.tris, s.skirt);
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    double area = 0.0, slope_w = 0.0, nongraph_area = 0.0;
    size_t solved = 0;
    for (size_t t = 0; t < s.tris.size(); ++t) {
        const float3 c[3] = {s.tris[t].vertex0, s.tris[t].vertex1,
                             s.tris[t].vertex2};
        for (int k = 0; k < 3; ++k) {
            mn[0] = std::min(mn[0], c[k].x); mx[0] = std::max(mx[0], c[k].x);
            mn[1] = std::min(mn[1], c[k].y); mx[1] = std::max(mx[1], c[k].y);
            mn[2] = std::min(mn[2], c[k].z); mx[2] = std::max(mx[2], c[k].z);
        }
        if (s.skirt[t]) continue;
        const float3 n = cross(c[1] - c[0], c[2] - c[0]);
        const double a2 = std::sqrt((double)dot(n, n));
        if (a2 <= 1e-12) continue;
        ++solved;
        const double a = 0.5 * a2;
        const double ny = (double)n.y / a2 * 2.0 * 0.5;  // unit n.y
        area += a;
        slope_w += a * std::acos(std::min(1.0, std::max(-1.0, ny))) *
                   (180.0 / 3.14159265358979);
        if (ny < 0.05) nongraph_area += a;
    }
    s.ext_x = mx[0] - mn[0];
    s.ext_y = mx[1] - mn[1];
    s.ext_z = mx[2] - mn[2];
    s.area = area;
    const double footprint = (double)s.ext_x * (double)s.ext_z;
    s.area_ratio = footprint > 1.0 ? area / footprint : 0.0;
    s.mean_slope_deg = area > 0.0 ? slope_w / area : 0.0;
    s.nongraph_pct = area > 0.0 ? 100.0 * nongraph_area / area : 0.0;
    s.solved_tris = solved;
}

static bool is_voxel_sector(const SectorInfo& s) {
    return s.ext_x > 58.0f && s.ext_x < 70.0f && s.ext_z > 58.0f &&
           s.ext_z < 70.0f && s.solved_tris >= 1500;
}

// Border-chain signature for neighbour matching: quantized sorted (y, z) or
// (y, x) pairs of the given side's border-band vertices. The shared cell
// column of two neighbours yields (near-)identical vertex sets, so equal
// signatures identify a shared border.  side: 0=-x 1=+x 2=-z 3=+z.
static uint64_t side_signature(const SectorInfo& s, int side) {
    const float S = 64.0f, voxel = 2.0f;
    std::vector<std::pair<int64_t, int64_t>> pts;
    auto add = [&](const float3& p) {
        const bool in_band =
            side == 0 ? (p.x <= 1e-3f)
            : side == 1 ? (p.x >= S - voxel - 1e-3f)
            : side == 2 ? (p.z <= 1e-3f)
                        : (p.z >= S - voxel - 1e-3f);
        if (!in_band) return;
        const float along = (side < 2) ? p.z : p.x;
        pts.emplace_back((int64_t)std::llround((double)along * 1024.0),
                         (int64_t)std::llround((double)p.y * 1024.0));
    };
    for (const Tri& t : s.tris) {
        add(t.vertex0);
        add(t.vertex1);
        add(t.vertex2);
    }
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    uint64_t h = 1469598103934665603ull;
    for (const auto& p : pts) {
        for (int64_t v : {p.first, p.second}) {
            for (int b = 0; b < 8; ++b) {
                h ^= (uint64_t)((v >> (b * 8)) & 0xFF);
                h *= 1099511628211ull;
            }
        }
    }
    return pts.size() >= 8 ? h : 0;
}

static int scan_transient(const char* dir, std::vector<SectorInfo>& sectors,
                          bool verbose) {
    size_t files = 0, loaded = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".part") continue;
        ++files;
        if (entry.file_size() < 150 * 1024) continue;  // sector parts are big
        SectorInfo s;
        if (!load_sector_part(entry.path().string(), s)) continue;
        ++loaded;
        measure_sector(s);
        if (!is_voxel_sector(s)) continue;
        if (verbose)
            std::printf("%s tris=%zu ext=%.1fx%.1fx%.1f ratio=%.2f "
                        "slope=%.1f nongraph=%.2f%%\n",
                        fs::path(s.path).filename().string().c_str(),
                        s.tris.size(), s.ext_x, s.ext_y, s.ext_z,
                        s.area_ratio, s.mean_slope_deg, s.nongraph_pct);
        sectors.push_back(std::move(s));
    }
    std::printf("scan: %zu files, %zu large-enough loads, %zu voxel sector "
                "meshes\n",
                files, loaded, sectors.size());
    return 0;
}

static int cut_fixtures(const char* dir, const char* out_dir) {
    std::vector<SectorInfo> sectors;
    scan_transient(dir, sectors, false);
    if (sectors.size() < 4) {
        std::printf("cut-fixtures: only %zu sector meshes; refusing\n",
                    sectors.size());
        return 2;
    }
    std::sort(sectors.begin(), sectors.end(),
              [](const SectorInfo& a, const SectorInfo& b) {
                  return a.area_ratio < b.area_ratio;
              });
    auto nearest = [&](double target) -> const SectorInfo& {
        size_t best = 0;
        double bd = 1e30;
        for (size_t i = 0; i < sectors.size(); ++i) {
            const double d = std::fabs(sectors[i].area_ratio - target);
            if (d < bd) {
                bd = d;
                best = i;
            }
        }
        return sectors[best];
    };
    const SectorInfo& typical = nearest(1.15);
    const SectorInfo& mid = nearest(2.6);
    const SectorInfo& extreme = sectors.back();

    fs::create_directories(out_dir);
    auto write_one = [&](const SectorInfo& s, const char* name,
                         double ax, double az) {
        Fixture f;
        f.tris = s.tris;
        f.skirt = s.skirt;
        f.anchor_x = ax;
        f.anchor_z = az;
        const std::string path = std::string(out_dir) + "/" + name + ".wfx";
        const bool ok = save_wfx(path, f);
        std::printf("%s <- %s  tris=%zu ratio=%.2f slope=%.1f nongraph=%.2f%% "
                    "%s\n",
                    name, fs::path(s.path).filename().string().c_str(),
                    s.tris.size(), s.area_ratio, s.mean_slope_deg,
                    s.nongraph_pct, ok ? "" : "WRITE FAILED");
        return ok;
    };
    bool ok = write_one(typical, "typical", 0.0, 0.0) &&
              write_one(mid, "mid_steep", 0.0, 0.0) &&
              write_one(extreme, "extreme_wall", 0.0, 0.0);

    // Shared-border pair: match side signatures (+x of A against -x of B,
    // +z of A against -z of B). Prefer the steepest matched pair.
    std::map<uint64_t, size_t> minus_x, minus_z;
    for (size_t i = 0; i < sectors.size(); ++i) {
        const uint64_t sx = side_signature(sectors[i], 0);
        const uint64_t sz = side_signature(sectors[i], 2);
        if (sx) minus_x.emplace(sx, i);
        if (sz) minus_z.emplace(sz, i);
    }
    double best_score = -1.0;
    size_t pa = SIZE_MAX, pb = SIZE_MAX;
    int pair_axis = 0;  // 0: B at +x of A; 1: B at +z of A
    for (size_t i = 0; i < sectors.size(); ++i) {
        const uint64_t px = side_signature(sectors[i], 1);
        const uint64_t pz = side_signature(sectors[i], 3);
        auto mx = px ? minus_x.find(px) : minus_x.end();
        if (mx != minus_x.end() && mx->second != i) {
            const double score =
                sectors[i].area_ratio + sectors[mx->second].area_ratio;
            if (score > best_score) {
                best_score = score;
                pa = i;
                pb = mx->second;
                pair_axis = 0;
            }
        }
        auto mz = pz ? minus_z.find(pz) : minus_z.end();
        if (mz != minus_z.end() && mz->second != i) {
            const double score =
                sectors[i].area_ratio + sectors[mz->second].area_ratio;
            if (score > best_score) {
                best_score = score;
                pa = i;
                pb = mz->second;
                pair_axis = 1;
            }
        }
    }
    if (pa == SIZE_MAX) {
        std::printf("cut-fixtures: NO shared-border pair found\n");
        ok = false;
    } else {
        std::printf("border pair (axis %s):\n", pair_axis == 0 ? "x" : "z");
        ok = write_one(sectors[pa], "border_a", 0.0, 0.0) && ok;
        ok = write_one(sectors[pb], "border_b",
                       pair_axis == 0 ? 64.0 : 0.0,
                       pair_axis == 1 ? 64.0 : 0.0) &&
             ok;
    }
    return ok ? 0 : 2;
}

// ---------------------------------------------------------------------------
// Gates.
// ---------------------------------------------------------------------------
static void print_stats(const char* tag, const warp_field::FieldStats& s) {
    std::printf(
        "  %-10s tris=%u folds=%u (%.2f%%)  stretch p50/p95/max "
        "%.2f/%.2f/%.2f  compress p50/p95 %.2f/%.2f  aniso p50/p95 "
        "%.2f/%.2f  dJ p50/p95 %.2f/%.2f  (%.1f ms)\n",
        tag, s.tris, s.folds,
        s.tris ? 100.0 * s.folds / s.tris : 0.0, s.stretch_p50, s.stretch_p95,
        s.stretch_max, s.compress_p50, s.compress_p95, s.aniso_p50,
        s.aniso_p95, s.dj_p50, s.dj_p95, s.solve_ms);
}

static std::string fixture_dir() {
    // Relative to the tests/ working directory the Makefile runs from.
    return "fixtures/warp_field";
}

static bool gate_fixture(const char* name, bool apply_gates, float stretch_gate,
                         float aniso_gate, float fold_pct_gate,
                         float dj_gate) {
    Fixture f;
    const std::string path = fixture_dir() + "/" + name + ".wfx";
    if (!load_wfx(path, f)) {
        CHECK(false, (std::string("fixture loads: ") + path).c_str());
        return false;
    }
    warp_field::SolveOptions opts;
    opts.anchor_x = f.anchor_x;
    opts.anchor_z = f.anchor_z;
    opts.sector_size = f.sector_size;
    opts.voxel = f.voxel;

    std::printf("[%s] %zu tris\n", name, f.tris.size());
    const warp_field::FieldStats base = warp_field::world_xz_stats(
        f.tris.data(), f.tris.size(), f.skirt.data(), opts);
    print_stats("world-xz", base);

    warp_field::Field field;
    const bool solved = warp_field::solve(f.tris.data(), f.tris.size(),
                                          f.skirt.data(), opts, field);
    CHECK(solved, (std::string(name) + ": solve succeeds").c_str());
    if (!solved) return false;
    print_stats("warp", field.stats);

    // Diagnostic: where do the folds live? (corner knots vs distributed)
    {
        const size_t nt = field.indices.size() / 3;
        std::vector<float2> uv(field.verts.size());
        for (size_t v = 0; v < field.verts.size(); ++v)
            uv[v] = field.verts[v].uv;
        double pos_area = 0.0, neg_area = 0.0;
        std::vector<float> areas(nt);
        for (size_t t = 0; t < nt; ++t) {
            const uint32_t* i = &field.indices[t * 3];
            const float2 A = uv[i[0]], B = uv[i[1]], C = uv[i[2]];
            const float a2 =
                (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
            areas[t] = a2;
            if (a2 > 0) pos_area += 1;
            else neg_area += 1;
        }
        const float orient = pos_area >= neg_area ? 1.0f : -1.0f;
        int fold_border = 0, fold_corner = 0, fold_interior = 0;
        for (size_t t = 0; t < nt; ++t) {
            if (areas[t] * orient > 0.0f) continue;
            const uint32_t* i = &field.indices[t * 3];
            float cx = 0, cz = 0;
            for (int c = 0; c < 3; ++c) {
                cx += field.positions[i[c]].x / 3.0f;
                cz += field.positions[i[c]].z / 3.0f;
            }
            const float dx = std::min(cx - (-2.0f), 64.0f - cx);
            const float dz = std::min(cz - (-2.0f), 64.0f - cz);
            if (dx < 6.0f && dz < 6.0f) ++fold_corner;
            else if (dx < 6.0f || dz < 6.0f) ++fold_border;
            else ++fold_interior;
        }
        std::printf("  folds: corner=%d border=%d interior=%d\n", fold_corner,
                    fold_border, fold_interior);
    }

    if (apply_gates) {
        char msg[160];
        std::snprintf(msg, sizeof msg, "%s: stretch p95 %.2f <= %.2f", name,
                      field.stats.stretch_p95, stretch_gate);
        CHECK(field.stats.stretch_p95 <= stretch_gate, msg);
        std::snprintf(msg, sizeof msg, "%s: aniso p95 %.2f <= %.2f", name,
                      field.stats.aniso_p95, aniso_gate);
        CHECK(field.stats.aniso_p95 <= aniso_gate, msg);
        const double fold_pct =
            field.stats.tris ? 100.0 * field.stats.folds / field.stats.tris
                             : 0.0;
        std::snprintf(msg, sizeof msg, "%s: folds %.3f%% <= %.3f%%", name,
                      fold_pct, fold_pct_gate);
        CHECK(fold_pct <= fold_pct_gate, msg);
        std::snprintf(msg, sizeof msg, "%s: dJ p95 %.2f <= %.2f", name,
                      field.stats.dj_p95, dj_gate);
        CHECK(field.stats.dj_p95 <= dj_gate, msg);
    }

    // Byte-identical re-solve (determinism gate).
    warp_field::Field again;
    warp_field::solve(f.tris.data(), f.tris.size(), f.skirt.data(), opts,
                      again);
    const bool same_uv =
        again.verts.size() == field.verts.size() &&
        std::memcmp(again.verts.data(), field.verts.data(),
                    field.verts.size() * sizeof(warp_field::VertexField)) == 0;
    CHECK(same_uv, (std::string(name) + ": re-solve byte-identical").c_str());

    // evaluate() exactness on solve vertices + fail-soft on a far position.
    if (!field.positions.empty()) {
        const size_t nv = std::min<size_t>(field.positions.size(), 64);
        std::vector<float> pos(nv * 3), nrm(nv * 3, 0.0f), uv(nv * 2);
        std::vector<uint32_t> frame(nv * 2);
        for (size_t i = 0; i < nv; ++i) {
            pos[i * 3 + 0] = field.positions[i].x;
            pos[i * 3 + 1] = field.positions[i].y;
            pos[i * 3 + 2] = field.positions[i].z;
            nrm[i * 3 + 1] = 1.0f;
        }
        warp_field::evaluate(field, pos.data(), nrm.data(), nv, uv.data(),
                             frame.data());
        bool exact = true;
        for (size_t i = 0; i < nv; ++i) {
            if (uv[i * 2 + 0] != field.verts[i].uv.x ||
                uv[i * 2 + 1] != field.verts[i].uv.y)
                exact = false;
        }
        CHECK(exact,
              (std::string(name) + ": evaluate() exact on solve verts").c_str());
    }
    return true;
}

static void gate_border_pair() {
    Fixture a, b;
    if (!load_wfx(fixture_dir() + "/border_a.wfx", a) ||
        !load_wfx(fixture_dir() + "/border_b.wfx", b)) {
        CHECK(false, "border pair fixtures load");
        return;
    }
    warp_field::SolveOptions oa, ob;
    oa.anchor_x = a.anchor_x;
    oa.anchor_z = a.anchor_z;
    ob.anchor_x = b.anchor_x;
    ob.anchor_z = b.anchor_z;
    std::vector<float3> pa, pb;
    std::vector<warp_field::BorderPin> pinsa, pinsb;
    CHECK(warp_field::border_pins(a.tris.data(), a.tris.size(),
                                  a.skirt.data(), oa, pa, pinsa),
          "border_a pins computable");
    CHECK(warp_field::border_pins(b.tris.data(), b.tris.size(),
                                  b.skirt.data(), ob, pb, pinsb),
          "border_b pins computable");

    // Match pinned vertices by WORLD position across the pair and compare
    // pinned uv. The shared chain is the only overlap.
    struct WKey {
        float x, y, z;
        bool operator<(const WKey& o) const {
            if (x != o.x) return x < o.x;
            if (y != o.y) return y < o.y;
            return z < o.z;
        }
    };
    std::map<WKey, float2> a_pins;
    for (const auto& p : pinsa) {
        const float3& l = pa[p.vertex];
        a_pins[{float(oa.anchor_x + double(l.x)), l.y,
                float(oa.anchor_z + double(l.z))}] = p.uv;
    }
    size_t shared = 0, exact = 0;
    double max_du = 0.0;
    for (const auto& p : pinsb) {
        const float3& l = pb[p.vertex];
        const WKey k{float(ob.anchor_x + double(l.x)), l.y,
                     float(ob.anchor_z + double(l.z))};
        auto it = a_pins.find(k);
        if (it == a_pins.end()) continue;
        ++shared;
        if (it->second.x == p.uv.x && it->second.y == p.uv.y) ++exact;
        max_du = std::max(
            max_du, std::max(std::fabs(double(it->second.x) - p.uv.x),
                             std::fabs(double(it->second.y) - p.uv.y)));
    }
    std::printf("[border pair] shared pinned verts=%zu bitwise=%zu "
                "max |duv|=%.3g m\n",
                shared, exact, max_du);
    CHECK(shared >= 16, "border pair: shared chain found (>= 16 verts)");
    char msg[128];
    std::snprintf(msg, sizeof msg,
                  "border pair: pins byte-identical (%zu/%zu)", exact, shared);
    CHECK(shared > 0 && exact == shared, msg);
}

// Solve both sectors of the committed border pair and compare the per-vertex
// FRAME (gu/gv) on the shared chain, the way gate_border_pair() compares the
// pinned uv. The frame is a shading BASIS since issue b005ca2e, so a frame
// that steps across a border is a visible normal seam even where the uv is
// continuous. Before apply_chain_frames() this measured 40.1 degrees of
// grad-u disagreement; the gate is set an order of magnitude below what the
// eye can find on flat ground at grazing angles.
static void gate_frame_pair() {
    Fixture a, b;
    if (!load_wfx(fixture_dir() + "/border_a.wfx", a) ||
        !load_wfx(fixture_dir() + "/border_b.wfx", b)) {
        CHECK(false, "frame pair fixtures load");
        return;
    }
    warp_field::SolveOptions oa, ob;
    oa.anchor_x = a.anchor_x; oa.anchor_z = a.anchor_z;
    ob.anchor_x = b.anchor_x; ob.anchor_z = b.anchor_z;
    warp_field::Field fa, fb;
    if (!warp_field::solve(a.tris.data(), a.tris.size(), a.skirt.data(), oa,
                           fa) ||
        !warp_field::solve(b.tris.data(), b.tris.size(), b.skirt.data(), ob,
                           fb)) {
        CHECK(false, "frame pair: both sectors solve");
        return;
    }
    struct WKey { float x, y, z;
        bool operator<(const WKey& o) const {
            if (x != o.x) return x < o.x;
            if (y != o.y) return y < o.y;
            return z < o.z; } };
    std::map<WKey, uint32_t> amap;
    for (uint32_t v = 0; v < fa.positions.size(); ++v) {
        const float3& l = fa.positions[v];
        amap[{float(oa.anchor_x + double(l.x)), l.y,
              float(oa.anchor_z + double(l.z))}] = v;
    }
    size_t n = 0;
    double max_duv = 0, max_dgu = 0, max_dgv = 0, max_ang = 0;
    for (uint32_t v = 0; v < fb.positions.size(); ++v) {
        const float3& l = fb.positions[v];
        auto it = amap.find({float(ob.anchor_x + double(l.x)), l.y,
                             float(ob.anchor_z + double(l.z))});
        if (it == amap.end()) continue;
        const warp_field::VertexField& va = fa.verts[it->second];
        const warp_field::VertexField& vb = fb.verts[v];
        ++n;
        max_duv = std::max<double>(max_duv,
            std::max(std::fabs(va.uv.x - vb.uv.x), std::fabs(va.uv.y - vb.uv.y)));
        auto len = [](float3 q){ return std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z); };
        float3 du{va.gu.x-vb.gu.x, va.gu.y-vb.gu.y, va.gu.z-vb.gu.z};
        float3 dv{va.gv.x-vb.gv.x, va.gv.y-vb.gv.y, va.gv.z-vb.gv.z};
        max_dgu = std::max<double>(max_dgu, len(du));
        max_dgv = std::max<double>(max_dgv, len(dv));
        const float la = len(va.gu), lb = len(vb.gu);
        if (la > 1e-6f && lb > 1e-6f) {
            double c = (va.gu.x*vb.gu.x + va.gu.y*vb.gu.y + va.gu.z*vb.gu.z)
                       / (double(la) * lb);
            c = std::max(-1.0, std::min(1.0, c));
            max_ang = std::max(max_ang, std::acos(c) * 57.2957795);
        }
    }
    std::printf("[frame pair] shared solve verts=%zu  max|duv|=%.4g  "
                "max|d gu|=%.4g  max|d gv|=%.4g  max angle(gu)=%.2f deg\n",
                n, max_duv, max_dgu, max_dgv, max_ang);
    CHECK(n >= 16, "frame pair: shared chain found (>= 16 verts)");
    char msg[160];
    std::snprintf(msg, sizeof msg,
                  "frame pair: grad-u agrees across the border "
                  "(%.2f deg <= 2.00)", max_ang);
    CHECK(n > 0 && max_ang <= 2.0, msg);
    std::snprintf(msg, sizeof msg,
                  "frame pair: grad-v agrees across the border "
                  "(|d| %.4g <= 0.02)", max_dgv);
    CHECK(n > 0 && max_dgv <= 0.02, msg);
}

// ---------------------------------------------------------------------------
// Seam smoothness: the C1 half of a border join.
//
// gate_border_pair() proves the uv VALUE agrees across a border bitwise, and
// gate_frame_pair() proves the stored per-vertex FRAME agrees. Neither sees
// the quantity the eye actually reads off a brick lattice.
//
// The shader addresses ground content at `march_uv0 = in_warp_uv_scales.xy`
// (gbuffer.frag), which is the rasteriser's barycentric interpolation of the
// per-vertex uv — so inside a triangle the course direction is set by THAT
// TRIANGLE's Jacobian, not by the stored frame (the frame only steers the POM
// march offset off that base). Two triangles meeting at a sector border share
// their two border vertices, hence agree on d(uv) ALONG the border exactly,
// and are free to disagree ACROSS it: each sector's interior solve is an
// independent Dirichlet problem that only sees the pinned VALUES. The result
// is C0 but not C1 — courses run through the border at a slight angle to each
// other. A kink, not a tear, and invisible to every gate that existed before.
//
// Measured here as: for each border edge shared by the committed pair, take
// the two incident triangles (one per sector), parallel-transport sector B's
// Jacobian rows into sector A's triangle plane by the minimal rotation
// carrying n_B onto n_A — so a genuine crease in the SURFACE is not charged to
// the map — and report the angle between grad-u and between grad-v. The same
// statistic over interior manifold edges is the baseline: whatever a sector's
// own triangles do to each other is the smoothness the border should match.
// ---------------------------------------------------------------------------
namespace seam {

struct WKey {
    float x, y, z;
    bool operator<(const WKey& o) const {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        return z < o.z;
    }
};

struct TriFrame {
    float3 gu{}, gv{}, n{};
    float3 centroid{};
    bool ok = false;
};

static inline float3 sub3(const float3& a, const float3& b) {
    return make_float3(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline float dot3(const float3& a, const float3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline float3 cross3(const float3& a, const float3& b) {
    return make_float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                       a.x * b.y - a.y * b.x);
}
static inline float len3(const float3& a) { return std::sqrt(dot3(a, a)); }
static inline float3 scale3(const float3& a, float s) {
    return make_float3(a.x * s, a.y * s, a.z * s);
}

// Forward Jacobian rows (world -> uv) of one triangle; same closed form as
// warp_field.cpp's tri_jacobian.
static TriFrame tri_frame(const float3& p0, const float3& p1, const float3& p2,
                          const float2& uv0, const float2& uv1,
                          const float2& uv2) {
    TriFrame f;
    const float3 e1 = sub3(p1, p0), e2 = sub3(p2, p0);
    const float3 n = cross3(e1, e2);
    const float n2 = dot3(n, n);
    if (!(n2 > 1e-20f)) return f;
    const float3 c2 = cross3(e2, n), c1 = cross3(e1, n);
    const float inv = 1.0f / n2;
    f.gu = scale3(sub3(scale3(c2, uv1.x - uv0.x), scale3(c1, uv2.x - uv0.x)),
                  inv);
    f.gv = scale3(sub3(scale3(c2, uv1.y - uv0.y), scale3(c1, uv2.y - uv0.y)),
                  inv);
    f.n = scale3(n, 1.0f / std::sqrt(n2));
    f.centroid = make_float3((p0.x + p1.x + p2.x) / 3.0f,
                             (p0.y + p1.y + p2.y) / 3.0f,
                             (p0.z + p1.z + p2.z) / 3.0f);
    f.ok = true;
    return f;
}

// Rodrigues rotation of v by the minimal rotation carrying `from` onto `to`
// (both unit). Antiparallel inputs are rejected by the caller.
static float3 transport(const float3& v, const float3& from, const float3& to) {
    const float3 axis = cross3(from, to);
    const float s = len3(axis);
    const float c = dot3(from, to);
    if (s < 1e-7f) return v;  // already aligned
    const float3 k = scale3(axis, 1.0f / s);
    const float3 kxv = cross3(k, v);
    const float kv = dot3(k, v);
    return make_float3(v.x * c + kxv.x * s + k.x * kv * (1.0f - c),
                       v.y * c + kxv.y * s + k.y * kv * (1.0f - c),
                       v.z * c + kxv.z * s + k.z * kv * (1.0f - c));
}

static double angle_deg(const float3& a, const float3& b) {
    const float la = len3(a), lb = len3(b);
    if (!(la > 1e-9f) || !(lb > 1e-9f)) return 0.0;
    double c = double(dot3(a, b)) / (double(la) * double(lb));
    c = std::max(-1.0, std::min(1.0, c));
    return std::acos(c) * 57.29577951308232;
}

// One measured join between two triangles.
struct Join {
    double kink = 0.0;      // max(angle(grad-u), angle(grad-v)) after transport
    double dj = 0.0;        // |J1 - J2|_F after transport
    double dihedral = 0.0;  // surface crease at the edge, for context
};

static Join measure_join(const TriFrame& a, const TriFrame& b) {
    Join j;
    const float3 gu2 = transport(b.gu, b.n, a.n);
    const float3 gv2 = transport(b.gv, b.n, a.n);
    j.kink = std::max(angle_deg(a.gu, gu2), angle_deg(a.gv, gv2));
    const float3 du = sub3(a.gu, gu2), dv = sub3(a.gv, gv2);
    j.dj = std::sqrt(double(dot3(du, du)) + double(dot3(dv, dv)));
    j.dihedral = angle_deg(a.n, b.n);
    return j;
}

static double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t i = size_t(p * double(v.size() - 1) + 0.5);
    return v[std::min(i, v.size() - 1)];
}

struct Summary {
    size_t n = 0;
    double p50 = 0, p95 = 0, max = 0, mean = 0;
};

static Summary summarize(const std::vector<double>& v) {
    Summary s;
    s.n = v.size();
    if (v.empty()) return s;
    s.p50 = pct(v, 0.50);
    s.p95 = pct(v, 0.95);
    for (double x : v) {
        s.max = std::max(s.max, x);
        s.mean += x;
    }
    s.mean /= double(v.size());
    return s;
}

// World-space view of one solved sector, with a shared vertex numbering so
// edges from the two sectors can be compared by key.
struct SectorView {
    std::vector<float3> pos;   // world
    std::vector<float2> uv;
    std::vector<uint32_t> idx;
    std::vector<uint32_t> gid;  // per local vertex -> global id
    std::vector<TriFrame> tf;
};

static void build_view(const warp_field::Field& f, double ax, double az,
                       std::map<WKey, uint32_t>& gids, SectorView& v) {
    v.pos.resize(f.positions.size());
    v.uv.resize(f.verts.size());
    v.gid.resize(f.positions.size());
    for (size_t i = 0; i < f.positions.size(); ++i) {
        const float3& l = f.positions[i];
        v.pos[i] = make_float3(float(ax + double(l.x)), l.y,
                               float(az + double(l.z)));
        v.uv[i] = f.verts[i].uv;
        const WKey k{v.pos[i].x, v.pos[i].y, v.pos[i].z};
        auto it = gids.find(k);
        if (it == gids.end())
            it = gids.emplace(k, uint32_t(gids.size())).first;
        v.gid[i] = it->second;
    }
    v.idx = f.indices;
    const size_t nt = v.idx.size() / 3;
    v.tf.resize(nt);
    for (size_t t = 0; t < nt; ++t) {
        const uint32_t* i = &v.idx[t * 3];
        v.tf[t] = tri_frame(v.pos[i[0]], v.pos[i[1]], v.pos[i[2]], v.uv[i[0]],
                            v.uv[i[1]], v.uv[i[2]]);
    }
}

static inline uint64_t gkey(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (uint64_t(a) << 32) | uint64_t(b);
}

// Per-vertex area-weighted average of the incident triangles' Jacobian rows —
// exactly what solve() computes before apply_chain_frames() overwrites the
// border ring, so this recovers the ONE-SIDED estimate each sector would have
// shipped on its own.
static void one_sided_frames(const SectorView& v, std::vector<float3>& gu,
                             std::vector<float3>& gv) {
    gu.assign(v.pos.size(), make_float3(0, 0, 0));
    gv.assign(v.pos.size(), make_float3(0, 0, 0));
    std::vector<float> wsum(v.pos.size(), 0.0f);
    const size_t nt = v.idx.size() / 3;
    for (size_t t = 0; t < nt; ++t) {
        if (!v.tf[t].ok) continue;
        const uint32_t* i = &v.idx[t * 3];
        const float3 e1 = sub3(v.pos[i[1]], v.pos[i[0]]);
        const float3 e2 = sub3(v.pos[i[2]], v.pos[i[0]]);
        const float w = 0.5f * len3(cross3(e1, e2));
        for (int c = 0; c < 3; ++c) {
            gu[i[c]] = make_float3(gu[i[c]].x + v.tf[t].gu.x * w,
                                   gu[i[c]].y + v.tf[t].gu.y * w,
                                   gu[i[c]].z + v.tf[t].gu.z * w);
            gv[i[c]] = make_float3(gv[i[c]].x + v.tf[t].gv.x * w,
                                   gv[i[c]].y + v.tf[t].gv.y * w,
                                   gv[i[c]].z + v.tf[t].gv.z * w);
            wsum[i[c]] += w;
        }
    }
    for (size_t i = 0; i < v.pos.size(); ++i) {
        if (wsum[i] > 1e-12f) {
            gu[i] = scale3(gu[i], 1.0f / wsum[i]);
            gv[i] = scale3(gv[i], 1.0f / wsum[i]);
        }
    }
}

}  // namespace seam

// Gate: the uv-gradient join across a real sector border, against the same
// statistic inside the sectors. `kink_p95_gate` / `kink_max_gate` are degrees.
static void gate_seam_smoothness(double kink_p95_gate, double kink_max_gate) {
    using namespace seam;
    Fixture a, b;
    if (!load_wfx(fixture_dir() + "/border_a.wfx", a) ||
        !load_wfx(fixture_dir() + "/border_b.wfx", b)) {
        CHECK(false, "seam smoothness: border pair fixtures load");
        return;
    }
    warp_field::SolveOptions oa, ob;
    oa.anchor_x = a.anchor_x; oa.anchor_z = a.anchor_z;
    oa.sector_size = a.sector_size; oa.voxel = a.voxel;
    ob.anchor_x = b.anchor_x; ob.anchor_z = b.anchor_z;
    ob.sector_size = b.sector_size; ob.voxel = b.voxel;
    warp_field::Field fa, fb;
    if (!warp_field::solve(a.tris.data(), a.tris.size(), a.skirt.data(), oa,
                           fa) ||
        !warp_field::solve(b.tris.data(), b.tris.size(), b.skirt.data(), ob,
                           fb)) {
        CHECK(false, "seam smoothness: both sectors solve");
        return;
    }

    std::map<WKey, uint32_t> gids;
    SectorView va, vb;
    build_view(fa, oa.anchor_x, oa.anchor_z, gids, va);
    build_view(fb, ob.anchor_x, ob.anchor_z, gids, vb);

    // Edge -> incident triangles, per sector, in the shared numbering.
    auto edge_map = [](const SectorView& v) {
        std::map<uint64_t, std::vector<uint32_t>> m;
        const size_t nt = v.idx.size() / 3;
        for (size_t t = 0; t < nt; ++t) {
            const uint32_t* i = &v.idx[t * 3];
            for (int e = 0; e < 3; ++e)
                m[gkey(v.gid[i[e]], v.gid[i[(e + 1) % 3]])].push_back(
                    uint32_t(t));
        }
        return m;
    };
    const auto ea = edge_map(va), eb = edge_map(vb);

    // Seam joins: an edge that is a BOUNDARY edge of A and of B alike, i.e.
    // the two sectors' meshes butt against each other there.
    std::vector<double> seam_kink, seam_dj, seam_dihedral;
    size_t flipped = 0;
    for (const auto& [k, ta] : ea) {
        if (ta.size() != 1) continue;
        auto it = eb.find(k);
        if (it == eb.end() || it->second.size() != 1) continue;
        const TriFrame& fa_t = va.tf[ta[0]];
        const TriFrame& fb_t = vb.tf[it->second[0]];
        if (!fa_t.ok || !fb_t.ok) continue;
        if (dot3(fa_t.n, fb_t.n) <= 0.0f) {  // opposed winding: not a join
            ++flipped;
            continue;
        }
        const Join j = measure_join(fa_t, fb_t);
        seam_kink.push_back(j.kink);
        seam_dj.push_back(j.dj);
        seam_dihedral.push_back(j.dihedral);
    }

    // Interior baseline over BOTH sectors, and the near-border subset (both
    // triangles within 4 m of the shared border plane) as the like-for-like
    // comparison — same terrain, same triangle sizes, same solver, no border.
    std::vector<double> in_kink, near_kink;
    double border_coord = 0.0;
    int border_axis = 0;  // 0 = x, 2 = z
    {
        // The pair is cut so B sits at +x or +z of A; the shared plane is at
        // A's far edge in whichever axis the anchors differ.
        if (std::fabs(ob.anchor_x - oa.anchor_x) > 1e-9) {
            border_axis = 0;
            border_coord = oa.anchor_x + double(a.sector_size);
        } else {
            border_axis = 2;
            border_coord = oa.anchor_z + double(a.sector_size);
        }
    }
    auto interior_scan = [&](const SectorView& v,
                             const std::map<uint64_t, std::vector<uint32_t>>& m) {
        for (const auto& [k, ts] : m) {
            if (ts.size() != 2) continue;
            const TriFrame& t0 = v.tf[ts[0]];
            const TriFrame& t1 = v.tf[ts[1]];
            if (!t0.ok || !t1.ok) continue;
            if (dot3(t0.n, t1.n) <= 0.0f) continue;
            const double kink = measure_join(t0, t1).kink;
            in_kink.push_back(kink);
            const double c0 = border_axis == 0 ? t0.centroid.x : t0.centroid.z;
            const double c1 = border_axis == 0 ? t1.centroid.x : t1.centroid.z;
            if (std::fabs(c0 - border_coord) < 4.0 &&
                std::fabs(c1 - border_coord) < 4.0)
                near_kink.push_back(kink);
        }
    };
    interior_scan(va, ea);
    interior_scan(vb, eb);

    const Summary sk = summarize(seam_kink);
    const Summary sd = summarize(seam_dj);
    const Summary sh = summarize(seam_dihedral);
    const Summary ik = summarize(in_kink);
    const Summary nk = summarize(near_kink);
    std::printf("[seam] joins=%zu (skipped %zu opposed)  kink deg "
                "p50/p95/max %.2f/%.2f/%.2f  dJ p50/p95/max %.3f/%.3f/%.3f  "
                "surface dihedral p95/max %.1f/%.1f\n",
                sk.n, flipped, sk.p50, sk.p95, sk.max, sd.p50, sd.p95, sd.max,
                sh.p95, sh.max);
    std::printf("[seam] interior baseline: all edges n=%zu kink p50/p95/max "
                "%.2f/%.2f/%.2f | within 4 m of the border n=%zu "
                "%.2f/%.2f/%.2f\n",
                ik.n, ik.p50, ik.p95, ik.max, nk.n, nk.p50, nk.p95, nk.max);

    // Where does the frame's conformal completion actually land? The
    // apply_chain_frames() note claims it sits at the MEAN of the two
    // one-sided estimates; this measures that claim on real data, and
    // measures how far apart the one-sided estimates were to begin with.
    {
        std::vector<float3> gua, gva, gub, gvb;
        one_sided_frames(va, gua, gva);
        one_sided_frames(vb, gub, gvb);
        std::map<uint32_t, uint32_t> b_by_gid;
        for (uint32_t i = 0; i < vb.gid.size(); ++i)
            b_by_gid.emplace(vb.gid[i], i);
        size_t n = 0;
        double max_one_sided = 0, max_vs_mean = 0, max_vs_mean_ang = 0;
        for (uint32_t i = 0; i < va.gid.size(); ++i) {
            auto it = b_by_gid.find(va.gid[i]);
            if (it == b_by_gid.end()) continue;
            const uint32_t jb = it->second;
            ++n;
            max_one_sided =
                std::max(max_one_sided, double(angle_deg(gua[i], gub[jb])));
            const float3 mean_gu =
                make_float3(0.5f * (gua[i].x + gub[jb].x),
                            0.5f * (gua[i].y + gub[jb].y),
                            0.5f * (gua[i].z + gub[jb].z));
            const float3 chain = fa.verts[i].gu;  // post-overwrite = shared
            max_vs_mean = std::max(max_vs_mean, double(len3(sub3(chain, mean_gu))));
            max_vs_mean_ang =
                std::max(max_vs_mean_ang, double(angle_deg(chain, mean_gu)));
        }
        std::printf("[seam] chain frame vs one-sided averages on %zu shared "
                    "verts: one-sided disagreement max %.2f deg; chain vs "
                    "their mean max |d| %.4g (%.2f deg)\n",
                    n, max_one_sided, max_vs_mean, max_vs_mean_ang);
    }

    CHECK(sk.n >= 16, "seam smoothness: shared border joins found (>= 16)");
    char msg[192];
    std::snprintf(msg, sizeof msg,
                  "seam smoothness: uv-gradient kink p95 %.2f deg <= %.2f",
                  sk.p95, kink_p95_gate);
    CHECK(sk.n > 0 && sk.p95 <= kink_p95_gate, msg);
    std::snprintf(msg, sizeof msg,
                  "seam smoothness: uv-gradient kink max %.2f deg <= %.2f",
                  sk.max, kink_max_gate);
    CHECK(sk.n > 0 && sk.max <= kink_max_gate, msg);
}

// --dump-uv <fixture.wfx> <out.png>: render the solved uv layout for eyes-on
// debugging. uv-space triangles (gray = ok, red = folded), pins in blue.
static int dump_uv(const char* fixture_path, const char* out_png) {
    Fixture f;
    if (!load_wfx(fixture_path, f)) {
        std::printf("dump-uv: cannot load %s\n", fixture_path);
        return 2;
    }
    warp_field::SolveOptions opts;
    opts.anchor_x = f.anchor_x;
    opts.anchor_z = f.anchor_z;
    warp_field::Field field;
    if (!warp_field::solve(f.tris.data(), f.tris.size(), f.skirt.data(), opts,
                           field)) {
        std::printf("dump-uv: solve failed\n");
        return 2;
    }
    const int W = 1200, H = 1200;
    std::vector<unsigned char> img(size_t(W) * H * 3, 255);
    float mnu = 1e30f, mxu = -1e30f, mnv = 1e30f, mxv = -1e30f;
    for (const auto& v : field.verts) {
        mnu = std::min(mnu, v.uv.x); mxu = std::max(mxu, v.uv.x);
        mnv = std::min(mnv, v.uv.y); mxv = std::max(mxv, v.uv.y);
    }
    const float span = std::max(mxu - mnu, mxv - mnv) * 1.05f + 1e-6f;
    auto px = [&](const float2& uv, int& x, int& y) {
        x = int((uv.x - mnu + span * 0.02f) / span * W);
        y = int((uv.y - mnv + span * 0.02f) / span * H);
    };
    // Fold mask via majority orientation.
    const size_t nt = field.indices.size() / 3;
    std::vector<float> area2(nt);
    double pos = 0, neg = 0;
    for (size_t t = 0; t < nt; ++t) {
        const uint32_t* i = &field.indices[t * 3];
        const float2 A = field.verts[i[0]].uv, B = field.verts[i[1]].uv,
                     C = field.verts[i[2]].uv;
        area2[t] = (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
        (area2[t] > 0 ? pos : neg) += 1;
    }
    const float orient = pos >= neg ? 1.0f : -1.0f;
    auto put = [&](int x, int y, unsigned char r, unsigned char g,
                   unsigned char b) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        unsigned char* p = &img[(size_t(y) * W + x) * 3];
        p[0] = r; p[1] = g; p[2] = b;
    };
    auto line = [&](int x0, int y0, int x1, int y1, unsigned char r,
                    unsigned char g, unsigned char b) {
        const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0)) + 1;
        for (int s = 0; s <= steps; ++s) {
            put(x0 + (x1 - x0) * s / steps, y0 + (y1 - y0) * s / steps, r, g,
                b);
        }
    };
    for (size_t t = 0; t < nt; ++t) {
        const uint32_t* i = &field.indices[t * 3];
        int x[3], y[3];
        for (int c = 0; c < 3; ++c) px(field.verts[i[c]].uv, x[c], y[c]);
        const bool folded = area2[t] * orient <= 0.0f;
        const unsigned char r = folded ? 255 : 170,
                            g = folded ? 40 : 170,
                            b = folded ? 40 : 170;
        for (int e = 0; e < 3; ++e)
            line(x[e], y[e], x[(e + 1) % 3], y[(e + 1) % 3], r, g, b);
    }
    // Pins (border chains) in blue, on top.
    std::vector<float3> ppos;
    std::vector<warp_field::BorderPin> pins;
    warp_field::border_pins(f.tris.data(), f.tris.size(), f.skirt.data(), opts,
                            ppos, pins);
    for (const auto& p : pins) {
        int x, y;
        px(p.uv, x, y);
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) put(x + dx, y + dy, 20, 60, 255);
    }
    stbi_write_png(out_png, W, H, 3, img.data(), W * 3);
    std::printf("dump-uv: %s (uv range %.1f..%.1f x %.1f..%.1f, %zu pins)\n",
                out_png, mnu, mxu, mnv, mxv, pins.size());
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "--dump-uv") == 0)
        return dump_uv(argv[2], argv[3]);
    if (argc == 3 && std::strcmp(argv[1], "--scan-transient") == 0) {
        std::vector<SectorInfo> sectors;
        return scan_transient(argv[2], sectors, true);
    }
    if (argc == 4 && std::strcmp(argv[1], "--cut-fixtures") == 0)
        return cut_fixtures(argv[2], argv[3]);

    // §4.3 gates on the committed fixtures. The mid fixture is informative
    // (the spec pins gates on typical + extreme).
    gate_fixture("typical", true, 1.6f, 2.0f, 0.0f, 1.0f);
    gate_fixture("mid_steep", false, 0, 0, 0, 0);
    gate_fixture("extreme_wall", true, 2.5f, 3.0f, 0.3f, 1.0f);
    gate_border_pair();
    gate_frame_pair();
    // Seam smoothness. Gates in DEGREES of uv-gradient kink across the
    // committed pair's shared border. Reference points, all measured on this
    // fixture with the surface dihedral there at 0.0 deg (so every degree is
    // the map, not the geometry):
    //   Dirichlet-only border (border_gradient_weight = 0): p95 23.81, max 24.62
    //   shipped weight 4.0:                                 p95  7.11, max  7.58
    // The gate sits at ~1.4x the shipped value and ~0.4x the pre-fix value, so
    // it fails long before the border relapses to a value-only pin.
    gate_seam_smoothness(10.0, 12.0);

    std::printf("warp_field_tests: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
