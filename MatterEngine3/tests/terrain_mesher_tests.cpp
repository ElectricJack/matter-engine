// MatterEngine3/tests/terrain_mesher_tests.cpp — Task 5: native surface nets
#include "check.h"
#include "../src/terrain_field.h"
#include "../src/terrain_mesher.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace terrain_field;
using namespace terrain_mesher;

static FieldRuntime make(const char* text) {
    FieldProgram p; std::string err;
    if (!FieldProgram::parse(text, p, err)) printf("parse err: %s\n", err.c_str());
    return FieldRuntime(std::move(p));
}
static const char* kFlat5 =
    "const 5\nconst 0.5\nconst 0.5\n"
    "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n";
static const char* kNoise =
    "noise2 1234 0.02 4 0.5 2.0\nconst 20\nmul r0 r1\nconst 0.5\nconst 0.5\n"
    "height r2\nmoisture r3\nrelief r4\nseaLevel -100\nbiome 0.65 0.35\n";

// Count tris whose (stored) normal satisfies pred.
template <typename P>
static size_t count_tris(const SectorMesh& m, P pred) {
    size_t n = 0;
    for (const auto& b : m.buckets)
        for (size_t t = 0; t * 9 < b.normals.size(); ++t)
            if (pred(b.normals[t*9+0], b.normals[t*9+1], b.normals[t*9+2])) ++n;
    return n;
}

int main() {
    // --- flat field, rung 0: counts, height, orientation -------------------
    {
        FieldRuntime f = make(kFlat5);
        SectorMesh m; std::string err;
        CHECK(mesh_sector(f, 0, 0, 0, 16.0f, -64.0f, 192.0f, m, err), err.c_str());
        size_t up    = count_tris(m, [](float, float ny, float){ return ny >  0.9f; });
        size_t skirt = count_tris(m, [](float, float ny, float){ return std::fabs(ny) < 0.3f; });
        CHECK(up == 128, "flat rung0: 8x8 cells -> 128 surface tris");
        CHECK(skirt == 64, "flat rung0: 4 sides x 8 segs x 2 = 64 skirt tris");
        bool y_ok = true, xz_ok = true;
        for (const auto& b : m.buckets)
            for (size_t t = 0; t * 9 < b.positions.size(); ++t) {
                float ny = b.normals[t*9+1];
                for (int v = 0; v < 3; ++v) {
                    float x = b.positions[t*9+v*3+0], y = b.positions[t*9+v*3+1],
                          z = b.positions[t*9+v*3+2];
                    if (ny > 0.9f && std::fabs(y - 5.0f) > 1e-3f) y_ok = false;
                    if (x < -2.1f || x > 18.1f || z < -2.1f || z > 18.1f) xz_ok = false;
                }
            }
        CHECK(y_ok, "surface verts at y=5");
        CHECK(xz_ok, "verts in sector-local range");
    }
    // --- rung scaling: rung1 = 4x surface tris of rung0 --------------------
    {
        FieldRuntime f = make(kFlat5);
        SectorMesh m0, m1; std::string err;
        CHECK(mesh_sector(f, 0, 0, 0, 16.0f, -64, 192, m0, err), "rung0");
        CHECK(mesh_sector(f, 0, 0, 1, 16.0f, -64, 192, m1, err), "rung1");
        size_t up0 = count_tris(m0, [](float,float ny,float){ return ny > 0.9f; });
        size_t up1 = count_tris(m1, [](float,float ny,float){ return ny > 0.9f; });
        CHECK(up1 == 4 * up0, "rung1 surface = 4x rung0");
    }
    // --- determinism --------------------------------------------------------
    {
        FieldRuntime f = make(kNoise);
        SectorMesh a, b; std::string err;
        CHECK(mesh_sector(f, 3, -2, 2, 16.0f, -64, 192, a, err), "a");
        CHECK(mesh_sector(f, 3, -2, 2, 16.0f, -64, 192, b, err), "b");
        CHECK(a.buckets.size() == b.buckets.size(), "same bucket count");
        bool same = true;
        for (size_t i = 0; i < a.buckets.size(); ++i)
            if (a.buckets[i].positions != b.buckets[i].positions) same = false;
        CHECK(same, "byte-identical positions");
    }
    // --- same-rung seam: sector (0,0) +x skirt == sector (1,0) -x skirt ------
    // Surface-nets with ownership-based face emission leaves a one-voxel gap at the
    // shared border (A owns faces for x in [0,S), B owns faces for its own [0,S)).
    // Skirts fill this gap. Verify that A's +x skirt and B's -x skirt are
    // world-compatible: skirt top-vertex heights must match (same height_at queries).
    // Collect all skirt segments from each side (|ny| < 0.3 = horizontal normal).
    {
        FieldRuntime f = make(kNoise);
        SectorMesh a, b; std::string err;
        CHECK(mesh_sector(f, 0, 0, 1, 16.0f, -64, 192, a, err), "a");
        CHECK(mesh_sector(f, 1, 0, 1, 16.0f, -64, 192, b, err), "b");
        // A's +x skirt verts have local x = 16.0 and nx > 0.
        // B's -x skirt verts have local x = 0.0  and nx < 0 (world x = 16.0).
        auto collect_border = [](const SectorMesh& m, float border_x,
                                 bool pos_normal) -> std::vector<float> {
            std::vector<float> ys;
            for (const auto& bkt : m.buckets)
                for (size_t t = 0; t * 9 < bkt.positions.size(); ++t) {
                    float nx = bkt.normals[t*9+0], ny = bkt.normals[t*9+1];
                    if (std::fabs(ny) > 0.3f) continue; // not a skirt tri
                    for (int v = 0; v < 3; ++v) {
                        float px = bkt.positions[t*9+v*3+0];
                        float py = bkt.positions[t*9+v*3+1];
                        if (std::fabs(px - border_x) < 0.01f)
                            ys.push_back(py);
                    }
                }
            std::sort(ys.begin(), ys.end());
            ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
            return ys;
        };
        auto ys_a = collect_border(a, 16.0f, true);  // A's +x border, local x=16
        auto ys_b = collect_border(b,  0.0f, false); // B's -x border, local x=0
        CHECK(!ys_a.empty(), "+x skirt verts in A");
        CHECK(!ys_b.empty(), "-x skirt verts in B");
        // Both sectors query height_at the same world x=16 positions — heights must match.
        CHECK(ys_a.size() == ys_b.size(), "same number of skirt vert heights at x=16");
        bool match = true;
        for (size_t i = 0; i < ys_a.size() && i < ys_b.size(); ++i)
            if (std::fabs(ys_a[i] - ys_b[i]) > 0.01f) { match = false; break; }
        CHECK(match, "border skirt heights match between neighbors");
    }
    // --- degenerate config fails loudly -------------------------------------
    {
        FieldRuntime f = make(kFlat5);
        SectorMesh m; std::string err;
        CHECK(!mesh_sector(f, 0, 0, 4, 16.0f, -64, 192, m, err), "rung 4 rejected");
        CHECK(!mesh_sector(f, 0, 0, 0, 16.0f, 10, -10, m, err), "bad slab rejected");
    }
    return check_summary();
}
