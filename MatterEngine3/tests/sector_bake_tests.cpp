// MatterEngine3/tests/sector_bake_tests.cpp
#include "check.h"
#include "../src/script_host.h"
#include "../src/terrain_field.h"
#include "../src/part_graph.h"
#include "../src/dsl_bindings.h"
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

    std::string src = slurp("../../projects/world_demo/scenes/StreamMountain/objects/WorldSector.js");
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
        // Printed with its path because together they are the handle for the
        // scatter sub-celling bitwise gate (WP5): run this binary against two
        // versions of WorldSector.js and `cmp` the two artifacts.
        //
        // Compare the ARTIFACTS, not these hashes. The part hash covers the
        // module SOURCE -- adding one comment line to WorldSector.js moves it,
        // measured -- so it can tell you two bakes differ but never that two
        // bakes agree. The 2026-08-08 sub-celling run: both bundles 42,684
        // bytes, differing in 32 bytes at offsets 9-88, which is four copies of
        // the 8-byte content hash in the header and section table. Every byte
        // of geometry and of the child placement table was identical.
        printf("  scatter bake hash: %016llx  path: %s\n",
               (unsigned long long)rs.resolved_hash, rs.written_path.c_str());
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

    // --- nested scatter: a big tile IS its four small ones -------------------
    // WP5. Scatter is computed per fixed 64 m cell rather than per tile, so a
    // level-1 tile must emit exactly what its four level-0 tiles emit, in the
    // cell loop's order. This is the invariant that keeps placements stable
    // across a level transition: without it every `r`-driven placement -- the
    // rock scatter, every rotation, the grass -- reshuffles the moment a tile
    // changes size, at the ranges where that is most visible.
    //
    // Module-for-module and in order. Positions are not compared here (the
    // bake does not hand them back), but they derive from the same cell
    // coordinates that produce this sequence, and any reshuffle changes the
    // sequence: the RNG stream drives which variant each call draws.
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
            hashes.push_back(0x1000ull + hashes.size());
        };
        for (int s = 0; s < 8; ++s) add("Rock", "{\"seed\":" + std::to_string(s) + "}");
        for (const char* sz : {"2.5", "4.0"})
            for (int s = 0; s < 4; ++s)
                add("Rock", "{\"seed\":" + std::to_string(s) + ",\"size\":" + sz + "}");
        for (int s = 0; s < 6; ++s) add("Pebble", "{\"seed\":" + std::to_string(s) + "}");
        for (int s = 0; s < 5; ++s) add("Grass", "{\"seed\":" + std::to_string(s) + "}");
        for (int s = 0; s < 3; ++s) add("Tree", "{\"seed\":" + std::to_string(s) + "}");

        const char* biomes =
            R"(,"worldSeed":42,"fieldHash":"abc","biomes":)"
            R"("{\"meadow\":{\"rocks\":4,\"pebbles\":4,\"grass\":5},)"
            R"(\"foothills\":{\"rocks\":4,\"pebbles\":4,\"grass\":5},)"
            R"(\"mountains\":{\"rocks\":4,\"pebbles\":4,\"grass\":5}}"})";
        auto scatter_bake = [&](const std::string& head) {
            BakeOptions opts;
            opts.parts_dir = parts_dir;
            opts.world.field = &field;
            return host.bake_source(src, (head + biomes).c_str(), opts,
                                    hashes.data(), hashes.size(),
                                    mods.data(), cparams.data());
        };

        // One level-1 tile at (2, 4): 128 m, covering level-0 cells
        // (4,8) (5,8) (4,9) (5,9).
        BakeResult big = scatter_bake(
            R"({"tx":2,"tz":4,"rung":2,"terrainLod":4,"sectorSize":128)");
        CHECK(big.error.ok, big.error.message.c_str());

        std::vector<std::string> union_mods;
        bool small_ok = true;
        // Cell loop order: cz outer, cx inner.
        const int cells[4][2] = {{4, 8}, {5, 8}, {4, 9}, {5, 9}};
        for (const auto& c : cells) {
            BakeResult one = scatter_bake(
                "{\"tx\":" + std::to_string(c[0]) +
                ",\"tz\":" + std::to_string(c[1]) +
                R"(,"rung":2,"terrainLod":5,"sectorSize":64)");
            if (!one.error.ok) { small_ok = false; break; }
            union_mods.insert(union_mods.end(),
                              one.child_modules_placed.begin(),
                              one.child_modules_placed.end());
        }
        CHECK(small_ok, "all four level-0 cells baked");
        printf("  nested scatter: level-1 tile placed %zu children, its four "
               "level-0 cells %zu\n",
               big.child_modules_placed.size(), union_mods.size());
        CHECK(!union_mods.empty(), "the cells actually scattered something");
        CHECK(big.child_modules_placed == union_mods,
              "nested scatter: a level-1 tile emits exactly what its four "
              "level-0 cells emit, in the same order");
    }

    // --- vegetation stops where it cannot be resolved -----------------------
    // WorldSector plants nothing past VEGETATION_MIN_LOD. That gate is ~99% of
    // a far sector's bake: measured at 334 ms per 64 m cell at the far scatter
    // tier, over ~90,000 cells inside StreamMountain's authored 10 km reach.
    // The far tier is NOT the cheap one -- familiesForRung(<=0) is ['tree'],
    // and the tree planner runs an O(viable^2) exclusion pass with three field
    // queries per candidate, while grass (gated to the nearest tier) is a flat
    // loop. The gating was backwards with respect to cost.
    //
    // Landmark boulders are deliberately ABOVE the gate: 25-40 m across, still
    // several pixels at 5 km, and nearly free at a 180 m minimum distance.
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
            hashes.push_back(0x1000ull + hashes.size());
        };
        for (int s = 0; s < 8; ++s) add("Rock", "{\"seed\":" + std::to_string(s) + "}");
        for (const char* sz : {"2.5", "4.0"})
            for (int s = 0; s < 4; ++s)
                add("Rock", "{\"seed\":" + std::to_string(s) + ",\"size\":" + sz + "}");
        for (int s = 0; s < 6; ++s) add("Pebble", "{\"seed\":" + std::to_string(s) + "}");
        for (int s = 0; s < 5; ++s) add("Grass", "{\"seed\":" + std::to_string(s) + "}");
        for (int s = 0; s < 3; ++s) add("Tree", "{\"seed\":" + std::to_string(s) + "}");

        const char* biomes =
            R"(,"worldSeed":42,"fieldHash":"abc","biomes":)"
            R"("{\"meadow\":{\"rocks\":4,\"pebbles\":4,\"grass\":5,\"trees\":6},)"
            R"(\"foothills\":{\"rocks\":4,\"pebbles\":4,\"grass\":5,\"trees\":6},)"
            R"(\"mountains\":{\"rocks\":4,\"pebbles\":4,\"grass\":5,\"trees\":6}}"})";
        auto bake_band = [&](int tx, int terrain_lod) {
            BakeOptions opts;
            opts.parts_dir = parts_dir;
            opts.world.field = &field;
            const std::string params =
                std::string(R"({"tx":)") + std::to_string(tx) +
                R"(,"tz":0,"rung":2,"terrainLod":)" +
                std::to_string(terrain_lod) + R"(,"sectorSize":64)" + biomes;
            return host.bake_source(src, params.c_str(), opts,
                                    hashes.data(), hashes.size(),
                                    mods.data(), cparams.data());
        };
        auto count_of = [](const BakeResult& r, const char* module) {
            size_t n = 0;
            for (const auto& m : r.child_modules_placed) if (m == module) ++n;
            return n;
        };

        // Both properties are checked over a SCAN, not a fixed tile. Placement
        // is world-position-keyed, so most individual cells contain neither a
        // boulder nor a tree -- a fixed tile lets this pass by being empty,
        // which is exactly how the first version of it "passed".
        //
        // Past the gate a Rock can only be a landmark boulder: the scree-rock
        // scatter sits BELOW the gate, and child_modules_placed carries module
        // names without params, so the two are otherwise indistinguishable.
        // That is what makes the band-2 rock count a clean boulder probe.
        size_t veg_near = 0, veg_far = 0, boulders_far = 0, tiles = 0;
        for (int tx = 0; tx < 40; ++tx) {
            BakeResult near = bake_band(tx, 3);
            BakeResult far  = bake_band(tx, 2);
            if (!near.error.ok || !far.error.ok) continue;
            ++tiles;
            veg_near += count_of(near, "Tree") + count_of(near, "Grass");
            veg_far  += count_of(far, "Tree") + count_of(far, "Grass");
            boulders_far += count_of(far, "Rock");
        }
        printf("  vegetation gate: over %zu tiles -- band 3 planted %zu, "
               "band 2 planted %zu and kept %zu landmark boulders\n",
               tiles, veg_near, veg_far, boulders_far);
        CHECK(tiles > 30, "the gate scan actually baked its tiles");
        CHECK(veg_near > 0,
              "the near band still plants -- otherwise this proves nothing");
        CHECK(veg_far == 0,
              "past the gate a sector plants no vegetation at all");
        CHECK(boulders_far > 0,
              "landmark boulders sit ABOVE the gate and still place at range "
              "-- they are large, visible and nearly free");
    }

    // --- habitatAt: the verb agrees with the runtime it reads ---------------
    // The last link in the habitat chain (docs/habitat-tape-sketch-2026-08-08).
    // A Part reads channels through this.habitatAt(x, z, out) and THROWS on any
    // disagreement with the values C++ computes from the same tape, so a bake
    // that succeeds is the assertion. Cross-checking rather than asserting a
    // constant means a change to the noise, the register machine or the verb's
    // argument order all show up here.
    {
        const char* tape =
            "noise2w 11 0.00333 3 0.5 2\n"        // r0
            "const 0.62\n"                         // r1
            "mul r0 r1\n"                          // r2
            "input height\n"                       // r3
            "const 0.01\n"                         // r4
            "mul r3 r4\n"                          // r5
            "input fslope\n"                       // r6
            "channel 0 r2\nchannel 1 r5\nchannel 2 r6\n";
        terrain_field::SurfaceProgram hp;
        std::string herr;
        CHECK(terrain_field::SurfaceProgram::parse(
                  tape, hp, herr, terrain_field::TapeMode::Habitat),
              herr.c_str());
        terrain_field::SurfaceRuntime hrt(std::move(hp));

        // What the runtime says, at the probe points the JS will use.
        const float probes[4][2] = {
            {0.0f, 0.0f}, {137.0f, -89.0f}, {-420.5f, 260.25f}, {910.0f, 910.0f},
        };
        std::string expect = "[";
        for (int i = 0; i < 4; ++i) {
            float ch[terrain_field::kMaxHabitatChannels] = {};
            hrt.channels_at(probes[i][0], probes[i][1], &field, ch);
            for (int c = 0; c < 3; ++c) {
                if (i || c) expect += ",";
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.9g", (double)ch[c]);
                expect += buf;
            }
        }
        expect += "]";

        const char* probe_src = R"JS(
class HabProbe extends Part {
  static params = { expect: '', tol: 0.0005 };
  build(p) {
    const want = JSON.parse(p.expect);
    const probes = [[0,0],[137,-89],[-420.5,260.25],[910,910]];
    const out = [];
    let k = 0;
    for (const [x, z] of probes) {
      const n = this.habitatAt(x, z, out);
      if (n !== 3) throw new Error('habitatAt returned ' + n + ' channels, want 3');
      for (let c = 0; c < 3; ++c) {
        const got = out[c], exp = want[k++];
        if (!(Math.abs(got - exp) <= p.tol))
          throw new Error('channel ' + c + ' at ' + x + ',' + z +
                          ': verb ' + got + ' vs runtime ' + exp);
      }
    }
    this.terrainVolume(0, 0, 0, 0, [16,16,16,16]);
  }
}
)JS";
        BakeOptions opts;
        opts.parts_dir = parts_dir;
        opts.world.field = &field;
        opts.world.habitat = &hrt;
        opts.world.sector_size = 16.0f;
        std::string params = "{\"expect\":\"";
        for (char c : expect) {           // escape for embedding in JSON
            if (c == '"') params += "\\\"";
            else params += c;
        }
        params += "\"}";
        BakeResult hb = host.bake_source(probe_src, params.c_str(), opts);
        CHECK(hb.error.ok, hb.error.message.c_str());
        printf("  habitatAt: verb matches the runtime at 4 probes x 3 channels\n");

        // And with no tape bound the verb REPORTS it rather than handing back
        // zeros -- an ecology reading all-zero channels would place nothing and
        // look like a scatter bug rather than a missing habitat().
        //
        // A bare probe, not the cross-check above: the verb follows the same
        // convention heightAt/slopeAt do (set the DSL error, return undefined,
        // harvest at the end) rather than throwing, so a script that keeps
        // running after the failed call can raise its OWN error first and mask
        // the host's. That is worth knowing about the convention, and it is why
        // this case gets a probe that does nothing after the call.
        static const char* bare_src = R"JS(
class BareHab extends Part {
  build(p) {
    const out = [];
    this.habitatAt(0, 0, out);
    this.terrainVolume(0, 0, 0, 0, [16,16,16,16]);
  }
}
)JS";
        BakeOptions bare = opts;
        bare.world.habitat = nullptr;
        BakeResult nb = host.bake_source(bare_src, "{}", bare);
        CHECK(!nb.error.ok, "habitatAt with no tape bound fails the bake");
        CHECK(nb.error.message.find("no habitat tape") != std::string::npos,
              nb.error.message.c_str());
    }

    // ---- terrainVolume forwards ALL FIVE arguments ---------------------------
    //
    // The wrapper in part_base.js.h was left at its pre-edgeMask four-parameter
    // form after the mask was added to the C binding and to WorldSector's call
    // site. The shape of the mistake is why it survived: WorldSector passes
    // (tx, tz, rung, edgeMask, mats), so edgeMask bound to the wrapper's `mats`
    // parameter and was forwarded into the binding's 4th slot -- which IS
    // edgeMask. Masking worked perfectly. The MATERIAL ARRAY fell off the end,
    // every voxel sector meshed with the binding's default raw ids 0..3 instead
    // of the authored [grass, dirt, rock, snow], and a world's material
    // override did nothing on the voxel path.
    //
    // Tested by intercepting the native binding from JS, because that is where
    // the defect lives -- the C side was always correct, and a test that only
    // checked the rendered result would have blamed the mesher.
    {
        printf("== terrainVolume argument forwarding ==\n");
        static const char* fwd_src = R"JS(
class MatFwd extends Part {
  build(p) {
    let seen = null;
    const real = globalThis.__terrainVolume;
    globalThis.__terrainVolume = function(...args) { seen = args; return real.apply(null, args); };
    try {
      this.terrainVolume(3, 4, 0, 5, [16, 11, 17, 2]);
    } finally {
      globalThis.__terrainVolume = real;
    }
    if (!seen) throw new Error('binding was never called');
    if (seen.length !== 5)
      throw new Error('forwarded ' + seen.length + ' args, expected 5');
    if (seen[3] !== 5)
      throw new Error('edgeMask arrived as ' + seen[3] + ', expected 5');
    if (!Array.isArray(seen[4]) || seen[4][0] !== 16 || seen[4][3] !== 2)
      throw new Error('material array did not arrive: ' + JSON.stringify(seen[4]));
    // And omitting mats entirely must still be legal (null, not undefined).
    let seen2 = null;
    const real2 = globalThis.__terrainVolume;
    globalThis.__terrainVolume = function(...args) { seen2 = args; return real2.apply(null, args); };
    try { this.terrainVolume(0, 0, 0, 0); } finally { globalThis.__terrainVolume = real2; }
    if (seen2[4] !== null)
      throw new Error('omitted mats should forward null, got ' + seen2[4]);
  }
}
)JS";
        BakeOptions fopts;
        fopts.parts_dir = parts_dir;
        fopts.world.field = &field;
        fopts.world.sector_size = 16.0f;
        BakeResult fr = host.bake_source(fwd_src, "{}", fopts);
        CHECK(fr.error.ok, fr.error.message.c_str());
        printf("  terrainVolume(tx,tz,rung,edgeMask,mats): all five reach the "
               "binding\n");
    }

    // ---- __candidatesInRect is BITWISE identical to the JS ------------------
    //
    // Every scatter placement in every world is derived from these hashes, so
    // a native implementation that is merely close moves trees, rocks and
    // grass everywhere -- silently, and only visible as "the world changed".
    // The JS reference stays in scatter_grid.js as candidatesInRectJs
    // precisely so this comparison can exist.
    //
    // Compared inside a real bake (rather than by reimplementing the hash in
    // C++ here) because that is the only place both implementations are
    // reachable at once, and because it also proves the binding is installed
    // where the ecology actually calls it.
    //
    // The rects are the REAL ones: the tree grid over a 64 m cell padded by
    // 16 m on every side at 1.65 m spacing, and the grass grid at 0.63 m --
    // the densest in the world, and the one where an off-by-one in the cell
    // loop would first show.
    {
        printf("== candidatesInRect: native vs JS ==\n");
        static const char* cmp_src = R"JS(
import { candidatesInRect, candidatesInRectJs } from 'shared-lib/scatter_grid';
class GridCmp extends Part {
  build(p) {
    const cases = [
      [31, 1.65, -16, -16, 96, 96],     // trees, padded cell
      [47, 0.63, 0, 0, 64, 64],         // grass, densest grid
      [2, 180.0, 0, 0, 64, 64],         // landmark boulders, sparsest
      [37, 1.58, -640, 448, 64, 64],    // shrubs, away from the origin
      [41, 1.26, 63.5, -0.5, 64, 64],   // non-integer rect origin
    ];
    let total = 0;
    for (const [kind, minDist, x0, z0, w, h] of cases) {
      const a = candidatesInRect(42, kind, minDist, x0, z0, w, h);
      const b = candidatesInRectJs(42, kind, minDist, x0, z0, w, h);
      if (a.length !== b.length)
        throw new Error('count ' + kind + ': ' + a.length + ' vs ' + b.length);
      for (let i = 0; i < a.length; ++i) {
        // Strict equality on doubles, deliberately: the claim under test is
        // bitwise identity, and a tolerance here would pass exactly the drift
        // this exists to catch.
        if (a[i].x !== b[i].x || a[i].z !== b[i].z || a[i].rot !== b[i].rot ||
            a[i].u !== b[i].u || a[i].v !== b[i].v)
          throw new Error('candidate ' + kind + '[' + i + '] differs');
      }
      total += a.length;
    }
    if (total < 100) throw new Error('suspiciously few candidates: ' + total);
    this.terrainVolume(0, 0, 0, 0, [16,16,16,16]);
  }
}
)JS";
        BakeOptions gopts;
        gopts.parts_dir = parts_dir;
        gopts.world.field = &field;
        gopts.world.sector_size = 16.0f;
        BakeResult gr = host.bake_source(cmp_src, "{}", gopts);
        CHECK(gr.error.ok, gr.error.message.c_str());
        printf("  native == JS over 5 real rects (trees, grass, boulders, "
               "shrubs, off-origin)\n");

        // And the binding is actually REACHED -- a `typeof` guard that
        // silently fell through to the JS would pass every check above while
        // delivering none of the speedup. Ask the module which path it took.
        static const char* path_src = R"JS(
class GridPath extends Part {
  build(p) {
    if (typeof __candidatesInRect !== 'function')
      throw new Error('__candidatesInRect is not installed in a part bake');
    this.terrainVolume(0, 0, 0, 0, [16,16,16,16]);
  }
}
)JS";
        BakeResult pr2 = host.bake_source(path_src, "{}", gopts);
        CHECK(pr2.error.ok, pr2.error.message.c_str());
    }

    // ---- __planCandidates agrees with __candidatesInRect + __habitatAt ------
    //
    // __planCandidates is the fused, one-crossing form of the two verbs proven
    // above: same cell loop, same survival test, same order, plus the bound
    // habitat tape's channels evaluated per survivor before the result crosses
    // into JS. This test proves the fusion changed NOTHING observable: same
    // candidates in the same order as __candidatesInRect, and for each
    // candidate the same channel doubles __habitatAt would report at that
    // point. Bitwise equality throughout -- the candidatesInRect precedent
    // above proved that is achievable for exactly this shape.
    {
        printf("== planCandidates: native fused vs native two-step ==\n");
        const char* tape =
            "noise2w 11 0.00333 3 0.5 2\n"        // r0
            "const 0.62\n"                         // r1
            "mul r0 r1\n"                          // r2
            "input height\n"                       // r3
            "const 0.01\n"                         // r4
            "mul r3 r4\n"                          // r5
            "input fslope\n"                       // r6
            "channel 0 r2\nchannel 1 r5\nchannel 2 r6\n";
        terrain_field::SurfaceProgram pp;
        std::string perr;
        CHECK(terrain_field::SurfaceProgram::parse(
                  tape, pp, perr, terrain_field::TapeMode::Habitat),
              perr.c_str());
        terrain_field::SurfaceRuntime prt(std::move(pp));

        BakeOptions popts;
        popts.parts_dir = parts_dir;
        popts.world.field = &field;
        popts.world.habitat = &prt;
        popts.world.sector_size = 16.0f;

        static const char* plan_src = R"JS(
class PlanCmp extends Part {
  build(p) {
    const cases = [
      [31, 1.65, -16, -16, 96, 96],     // trees, padded cell
      [47, 0.63, 0, 0, 64, 64],         // grass, densest grid
      [2, 180.0, 0, 0, 64, 64],         // landmark boulders, sparsest
      [37, 1.58, -640, 448, 64, 64],    // shrubs, away from the origin
      [41, 1.26, 63.5, -0.5, 64, 64],   // non-integer rect origin
    ];
    let total = 0;
    for (const [kind, minDist, x0, z0, w, h] of cases) {
      const flat = __planCandidates(42, kind, minDist, x0, z0, w, h);
      const channelCount = flat[0];
      const count = flat[1];
      const stride = 5 + channelCount;
      const ref = __candidatesInRect(42, kind, minDist, x0, z0, w, h);
      if (count !== ref.length)
        throw new Error('count ' + kind + ': ' + count + ' vs ' + ref.length);
      if (channelCount !== 3)
        throw new Error('channelCount ' + kind + ': ' + channelCount + ' want 3');
      const out = [];
      for (let i = 0; i < count; ++i) {
        const base = 2 + i * stride;
        const x = flat[base], z = flat[base+1], rot = flat[base+2],
              u = flat[base+3], v = flat[base+4];
        if (x !== ref[i].x || z !== ref[i].z || rot !== ref[i].rot ||
            u !== ref[i].u || v !== ref[i].v)
          throw new Error('candidate ' + kind + '[' + i + '] position differs');
        const n = this.habitatAt(x, z, out);
        if (n !== 3) throw new Error('habitatAt returned ' + n);
        for (let c = 0; c < 3; ++c) {
          if (flat[base + 5 + c] !== out[c])
            throw new Error('candidate ' + kind + '[' + i + '] channel ' + c +
                            ' differs: ' + flat[base + 5 + c] + ' vs ' + out[c]);
        }
      }
      total += count;
    }
    if (total < 100) throw new Error('suspiciously few candidates: ' + total);
    this.terrainVolume(0, 0, 0, 0, [16,16,16,16]);
  }
}
)JS";
        BakeResult plr = host.bake_source(plan_src, "{}", popts);
        CHECK(plr.error.ok, plr.error.message.c_str());
        printf("  planCandidates == candidatesInRect positions + habitatAt "
               "channels over 5 real rects\n");

        // No habitat tape bound: channelCount must be 0 and the bake must
        // NOT fail (unlike habitatAt, which fails loudly with no tape).
        static const char* no_tape_src = R"JS(
class PlanNoTape extends Part {
  build(p) {
    const flat = __planCandidates(42, 31, 1.65, -16, -16, 96, 96);
    const channelCount = flat[0], count = flat[1];
    if (channelCount !== 0)
      throw new Error('channelCount with no tape: ' + channelCount);
    if (count < 1) throw new Error('no candidates at all: ' + count);
    if (flat.length !== 2 + count * 5)
      throw new Error('unexpected flat length ' + flat.length +
                      ' for count ' + count);
    this.terrainVolume(0, 0, 0, 0, [16,16,16,16]);
  }
}
)JS";
        BakeOptions ntopts = popts;
        ntopts.world.habitat = nullptr;
        BakeResult ntr = host.bake_source(no_tape_src, "{}", ntopts);
        CHECK(ntr.error.ok, ntr.error.message.c_str());
        printf("  planCandidates with no habitat tape: channelCount=0, "
               "does not fail\n");

        static const char* plan_path_src = R"JS(
class PlanPath extends Part {
  build(p) {
    if (typeof __planCandidates !== 'function')
      throw new Error('__planCandidates is not installed in a part bake');
    this.terrainVolume(0, 0, 0, 0, [16,16,16,16]);
  }
}
)JS";
        BakeResult ppr = host.bake_source(plan_path_src, "{}", popts);
        CHECK(ppr.error.ok, ppr.error.message.c_str());
    }

    // ---- ScriptProfile: prof() from inside a bake ---------------------------
    //
    // This exists because the thing it measures had NO instrumentation: the
    // bake's `build` phase was one opaque block, and two successive theories
    // about where its time went were both wrong -- each backed by arithmetic
    // that matched a measured total to within a few percent, and each refuted
    // only after being implemented. A profiler that is itself unverified would
    // just be a third way to be confidently wrong, so its arithmetic is pinned
    // here rather than trusted.
    {
        printf("== script profile ==\n");
        // Off by default: a script can carry prof() calls permanently without
        // paying for them. Slot allocation is the gate -- an unenabled slot()
        // returns -1 and every begin/end after it is an early return.
        _putenv_s("MATTER_SCRIPT_PROFILE", "");
        CHECK(dsl::script_profile::slot("off") < 0,
              "slot() is -1 when the profile is off");
        CHECK(dsl::script_profile::report().empty(),
              "report is empty when nothing was recorded");

        _putenv_s("MATTER_SCRIPT_PROFILE", "1");
        // Toggling mid-process has to work, which is exactly why `enabled()`
        // reads the env var live instead of latching it: the first version
        // latched, and this suite could then only ever observe whichever state
        // it happened to probe first.
        CHECK(dsl::script_profile::enabled(), "MATTER_SCRIPT_PROFILE arms it");
        dsl::script_profile::reset();

        // The same name interns to the same slot; distinct names do not. This
        // is what lets a script hoist `profSlot` into a module constant.
        const int a = dsl::script_profile::slot("t.alpha");
        const int b = dsl::script_profile::slot("t.beta");
        CHECK(a >= 0 && b >= 0 && a != b, "distinct labels get distinct slots");
        CHECK(dsl::script_profile::slot("t.alpha") == a, "interning is stable");

        // SELF vs TOTAL is the property the whole table hangs on: nesting
        // alpha around beta must charge beta's time to beta once, and to
        // alpha's total but NOT alpha's self. Get this wrong and every parent
        // label reads as its own cost plus all its children's, which is
        // exactly the "137% of frame" illusion the render profiler once had.
        {
            dsl::script_profile::begin(a);
            dsl::script_profile::begin(b);
            const auto spin_until = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(20);
            while (std::chrono::steady_clock::now() < spin_until) { /* spin */ }
            dsl::script_profile::end(b);
            dsl::script_profile::end(a);
        }
        const std::string rep = dsl::script_profile::report();
        CHECK(!rep.empty(), "report is non-empty after recording");
        CHECK(rep.find("t.alpha") != std::string::npos, rep.c_str());
        CHECK(rep.find("t.beta") != std::string::npos, rep.c_str());
        CHECK(rep.find("WARNING") == std::string::npos,
              "balanced scopes report no mismatch");
        fputs(rep.c_str(), stdout);

        // An unbalanced end() must not corrupt the stack or silently produce a
        // plausible-looking table -- it says so. A script whose prof() calls
        // are wrong should be caught by the output, not by the reader assuming
        // the numbers are fine.
        dsl::script_profile::reset();
        dsl::script_profile::begin(a);
        dsl::script_profile::end(b);            // wrong label closes `a`
        CHECK(dsl::script_profile::report().find("WARNING") != std::string::npos,
              "an unbalanced scope is announced, not hidden");

        // End to end: prof() called from JS inside a real bake reaches the
        // native counters. This is the only case that proves the binding, the
        // part_base globals and the aggregation are wired to each other.
        dsl::script_profile::reset();
        static const char* prof_src = R"JS(
class ProfProbe extends Part {
  build(p) {
    const S = profSlot('js.loop');
    profBegin(S);
    let acc = 0;
    for (let i = 0; i < 200000; ++i) acc += Math.sqrt(i);
    profEnd(S);
    this.terrainVolume(0, 0, 0, 0, [16,16,16,16]);
  }
}
)JS";
        BakeOptions popts;
        popts.parts_dir = parts_dir;
        popts.world.field = &field;
        popts.world.sector_size = 16.0f;
        BakeResult pr = host.bake_source(prof_src, "{}", popts);
        CHECK(pr.error.ok, pr.error.message.c_str());
        const std::string js_rep = dsl::script_profile::report();
        CHECK(js_rep.find("js.loop") != std::string::npos,
              "prof() from JS reaches the native counters");
        CHECK(js_rep.find("WARNING") == std::string::npos, js_rep.c_str());
        fputs(js_rep.c_str(), stdout);

        // Leave the process as we found it so no later suite inherits an armed
        // profile (the latch means it could not be disarmed anyway, but the
        // env var should not leak to a child process either).
        _putenv_s("MATTER_SCRIPT_PROFILE", "");
        dsl::script_profile::reset();
    }

    return check_summary();
}
