// MatterEngine3/tests/sector_bake_tests.cpp
#include "check.h"
#include "../src/script_host.h"
#include "../src/terrain_field.h"
#include "../src/part_graph.h"
#include "../src/terrain_mesher.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Sandbox under the platform temp dir rather than a hardcoded POSIX "/tmp".
// The literal made this suite unrunnable on Windows -- every bake failed with
// errno=2 and the failures read like artifact bugs -- so it was red on main for
// a reason that had nothing to do with what it tests. Same fix async_bake_tests
// already carries.
static std::string sandbox_dir(const char* name) {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    const fs::path dir =
        fs::temp_directory_path() / (std::string(name) + "_" +
                                     std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir.string();
}

using namespace script_host;

static std::string slurp(const char* path) {
    std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

int main() {
    // Mountain-ish noise field so slopeAt/biomeAt exercise real variation.
    terrain_field::FieldProgram prog; std::string err;
    CHECK(terrain_field::FieldProgram::parse(
        "noise2 42 0.005 4 0.5 2.0\nconst 60\nmul r0 r1\nconst 0.6\nconst 0.3\n"
        "height r2\nmoisture r3\nrelief r4\nseaLevel -80\nbiome 0.65 0.35\n",
        prog, err), err.c_str());
    terrain_field::FieldRuntime field(std::move(prog));

    std::string src = slurp("../../projects/world_demo/objects/WorldSector.js");
    CHECK(!src.empty(), "WorldSector.js readable");

    ScriptHost host;
    // BOTH roots. WorldSector.js imports rng/scatter_grid from the engine's
    // shared-lib and alpine_ecology from the project's, so the single engine
    // root this used to set could never resolve a WorldSector bake -- every
    // case after `requires` failed with "module not found" and read like a
    // bake bug. Project root first, matching the resolver order the engine
    // itself uses (shared_lib_tests.cpp covers that precedence).
    host.set_shared_lib_roots({"../../projects/world_demo/shared-lib",
                               "../shared-lib"});  // run from MatterEngine3/tests

    // requires: fixed asset list, independent of tx/tz
    {
        auto req_a = host.eval_requires(src, R"({"tx":0,"tz":0,"rung":3})");
        auto req_b = host.eval_requires(src, R"({"tx":900,"tz":-77,"rung":0})");
        CHECK(!req_a.empty(), "requires non-empty");
        CHECK(req_a.size() == req_b.size(), "requires independent of tx/tz");
        // Composition rather than a bare total: the count has drifted twice
        // (pebbles commented out of WorldSector.js, trees made conditional on
        // the biome table) and each time this line failed for a reason that
        // had nothing to do with sector baking.
        size_t rocks = 0, grass = 0, trees = 0;
        for (const auto& r : req_a) {
            if (r.module_specifier == "Rock")  ++rocks;
            if (r.module_specifier == "Grass") ++grass;
            if (r.module_specifier == "Tree")  ++trees;
        }
        CHECK(rocks == 16, "8 rock seeds + 8 boulder variants");
        CHECK(grass == 5,  "5 grass variants");
        CHECK(trees == 3,  "3 tree variants (no biome table -> fail-open)");
        CHECK(req_a.size() == rocks + grass + trees,
              "variant list is exactly rocks + grass + trees");
    }

    const std::string parts_dir = sandbox_dir("me3_sector_bake");
    auto bake = [&](const char* params) {
        BakeOptions opts;
        opts.parts_dir = parts_dir;
        opts.world.field = &field;
        return host.bake_source(src, params, opts);
    };
    // Empty biomes -> terrain-only (no placeChild -> bakes even without child hashes)
    const char* p00 = R"({"tx":0,"tz":0,"rung":0,"worldSeed":42,"fieldHash":"abc","biomes":""})";
    BakeResult r0 = bake(p00);
    CHECK(r0.error.ok, r0.error.message.c_str());

    // determinism: same params -> same hash; different tx -> different hash
    BakeResult r0b = bake(p00);
    CHECK(r0b.error.ok && r0b.resolved_hash == r0.resolved_hash, "deterministic hash");
    BakeResult r1 = bake(
        R"({"tx":1,"tz":0,"rung":0,"worldSeed":42,"fieldHash":"abc","biomes":""})");
    CHECK(r1.error.ok && r1.resolved_hash != r0.resolved_hash, "tx changes hash");

    // fieldHash participates in the hash (world edits invalidate sectors)
    BakeResult rf = bake(
        R"({"tx":0,"tz":0,"rung":0,"worldSeed":42,"fieldHash":"xyz","biomes":""})");
    CHECK(rf.error.ok && rf.resolved_hash != r0.resolved_hash, "fieldHash changes hash");

    // ground geometry: the baked .part file must be significantly larger than a
    // bare header, indicating terrain triangles were serialized into it.
    {
        CHECK(!r0.written_path.empty(), "bake wrote a .part file");
        std::ifstream pf(r0.written_path, std::ios::binary | std::ios::ate);
        long long fsz = pf ? (long long)pf.tellg() : 0LL;
        CHECK(fsz > 256, "sector .part has non-trivial size (ground geometry present)");
    }

    // scatter bake: a non-empty biomes table + rung 2 runs the full placeChild path.
    // placeChild does a STRICT composite-key lookup (module \x1f canonical-params)
    // with no bare-module fallback, so we must install the schema's full declared
    // variant table (assetVariants: 30 entries). bake_source keys the table with
    // child_params[i] verbatim, while placeChild normalizes its JS params via
    // params_from_json->params_to_json — so we canonicalize each variant's params
    // through the SAME functions, guaranteeing the keys match. Dummy child hashes
    // are fine: the parent bake records instance refs alongside its terrain; it
    // does not re-bake the children. Counts are supplied under every land biome so
    // scatter fires whatever biome the tile center resolves to.
    {
        auto canon = [](const std::string& raw) {
            return raw.empty() ? std::string()
                : part_graph::params_to_json(part_graph::params_from_json(raw));
        };
        std::vector<std::string> mods, cparams;
        std::vector<uint64_t> hashes;
        auto add = [&](const char* module, const std::string& raw) {
            mods.push_back(module);
            cparams.push_back(canon(raw));
            hashes.push_back(0x1000ull + hashes.size());   // distinct dummy hashes
        };
        for (int s = 0; s < 8; ++s) add("Rock", "{\"seed\":" + std::to_string(s) + "}");
        for (const char* sz : {"2.5", "4.0"})
            for (int s = 0; s < 4; ++s)
                add("Rock", "{\"seed\":" + std::to_string(s) + ",\"size\":" + sz + "}");
        for (int s = 0; s < 6; ++s) add("Pebble", "{\"seed\":" + std::to_string(s) + "}");
        for (int s = 0; s < 5; ++s) add("Grass", "{\"seed\":" + std::to_string(s) + "}");
        for (int s = 0; s < 3; ++s) add("Tree", "{\"seed\":" + std::to_string(s) + "}");
        CHECK(mods.size() == 30, "installed full declared variant table");

        BakeOptions opts;
        opts.parts_dir = parts_dir;
        opts.world.field = &field;
        const char* p_scatter =
            R"({"tx":0,"tz":0,"rung":2,"worldSeed":42,"fieldHash":"abc","biomes":)"
            R"("{\"meadow\":{\"rocks\":4,\"pebbles\":4,\"grass\":5},)"
            R"(\"foothills\":{\"rocks\":4,\"pebbles\":4,\"grass\":5},)"
            R"(\"mountains\":{\"rocks\":4,\"pebbles\":4,\"grass\":5}}"})";
        BakeResult rs = host.bake_source(src, p_scatter, opts,
            hashes.data(), hashes.size(), mods.data(), cparams.data());
        CHECK(rs.error.ok, rs.error.message.c_str());
        // rung is folded into the hash, so rung-2 must differ from rung-0
        CHECK(rs.resolved_hash != r0.resolved_hash, "scatter-rung hash differs from terrain-only rung");
    }

    // --- nested sector LOD: a tile that is not S_0 across --------------------
    // WP2 of docs/superpowers/plans/2026-08-08-nested-sector-lod-migration.md.
    // A level-L request is (terrainLod 5-L, sectorSize S_0<<L), so the tile is
    // wider AND coarser by the same factor and the triangle count is unchanged.
    // The engine sends `sectorSize` per request because only the streamer knows
    // a request's level; everything below is what WorldSector must do with it.
    {
        // The bake path used above writes .part files but does not hand back
        // geometry, so extent is asserted through terrainVolume's own mesher
        // instead -- the same call WorldSector.build() makes, with the same
        // arguments the engine now supplies per request.
        auto tile = [&](int64_t tx, int64_t tz, int rung, float size,
                        terrain_mesher::SectorMesh& out) {
            std::string e;
            return terrain_mesher::mesh_sector(field, tx, tz, rung, 0, size,
                                               -300.0f, 300.0f, out, e);
        };
        terrain_mesher::SectorMesh l0, l1, l2;
        CHECK(tile(1, 0, 0, 64.0f, l0),   "level 0: 64 m tile at rung 0");
        CHECK(tile(1, 0, -1, 128.0f, l1), "level 1: 128 m tile at rung -1");
        CHECK(tile(1, 0, -2, 256.0f, l2), "level 2: 256 m tile at rung -2");
        // Constant cells-per-tile is the whole design: four times the ground
        // for about the same triangles is where the 65x fewer parts comes from.
        //
        // "About", not "exactly". The lattice is 32x32 at every level, so the
        // horizontal surface is the same count; what varies is the VERTICAL
        // faces surface nets emits where the field falls faster than one voxel
        // per cell, and a coarser voxel meets that condition in different
        // places. The gate is that the count stays within a small factor --
        // i.e. that it tracks the lattice and not the area, which is the claim
        // the part-count win rests on. Area grows 4x and 16x here.
        printf("  nested tris: L0=%zu L1=%zu L2=%zu (lattice 32x32 at each)\n",
               l0.triangle_count(), l1.triangle_count(), l2.triangle_count());
        CHECK(l1.triangle_count() < 2 * l0.triangle_count() &&
              l2.triangle_count() < 2 * l0.triangle_count(),
              "nested: a coarser level covers 4x the ground WITHOUT 4x the "
              "triangles -- the count tracks the lattice, not the area");
        // Extent: positions are tile-local, so a level-1 tile must reach twice
        // as far as a level-0 one.
        auto span = [](const terrain_mesher::SectorMesh& m) {
            float lo = 1e30f, hi = -1e30f;
            for (const auto& b : m.buckets)
                for (size_t i = 0; i + 2 < b.positions.size(); i += 3) {
                    lo = std::min(lo, b.positions[i]);
                    hi = std::max(hi, b.positions[i]);
                }
            return hi - lo;
        };
        CHECK(span(l1) > 1.9f * span(l0) && span(l1) < 2.1f * span(l0),
              "nested: a level-1 tile spans twice a level-0 tile");
        // Determinism at a non-default size -- the property the whole streaming
        // cache rests on, asserted where the size is NOT the world scalar.
        terrain_mesher::SectorMesh again;
        CHECK(tile(1, 0, -1, 128.0f, again), "re-mesh the level-1 tile");
        bool identical = again.buckets.size() == l1.buckets.size();
        for (size_t i = 0; identical && i < l1.buckets.size(); ++i)
            identical = again.buckets[i].positions == l1.buckets[i].positions;
        CHECK(identical, "nested: double-mesh at a non-default size is "
                         "byte-identical");

        // And the param reaches JS: a bake whose ONLY difference is sectorSize
        // must resolve to a different part, or a level change would silently
        // reuse the wrong geometry.
        BakeResult small = bake(
            R"({"tx":0,"tz":0,"rung":0,"terrainLod":5,"sectorSize":64,)"
            R"("worldSeed":42,"fieldHash":"abc","biomes":""})");
        BakeResult big = bake(
            R"({"tx":0,"tz":0,"rung":0,"terrainLod":4,"sectorSize":128,)"
            R"("worldSeed":42,"fieldHash":"abc","biomes":""})");
        CHECK(small.error.ok && big.error.ok, "both nested bakes succeeded");
        CHECK(small.resolved_hash != big.resolved_hash,
              "nested: sectorSize participates in the part hash");
    }

    return check_summary();
}
