// MatterEngine3/tests/sector_bake_tests.cpp
#include "check.h"
#include "../src/script_host.h"
#include "../src/terrain_field.h"
#include "../src/part_graph.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

    std::string src = slurp("../examples/world_demo/schemas/WorldSector.js");
    CHECK(!src.empty(), "WorldSector.js readable");

    ScriptHost host;
    host.set_shared_lib_root("../shared-lib");  // tests run from MatterEngine3/tests

    // requires: fixed asset list, independent of tx/tz
    {
        auto req_a = host.eval_requires(src, R"({"tx":0,"tz":0,"rung":3})");
        auto req_b = host.eval_requires(src, R"({"tx":900,"tz":-77,"rung":0})");
        CHECK(!req_a.empty(), "requires non-empty");
        CHECK(req_a.size() == req_b.size(), "requires independent of tx/tz");
        // 8 rocks + 8 boulders + 6 pebbles + 5 grass + 3 trees = 30
        CHECK(req_a.size() == 30, "full variant list");
    }

    system("rm -rf /tmp/sector_bake_parts && mkdir -p /tmp/sector_bake_parts");
    auto bake = [&](const char* params) {
        BakeOptions opts;
        opts.parts_dir = "/tmp/sector_bake_parts";
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
        opts.parts_dir = "/tmp/sector_bake_parts";
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

    return check_summary();
}
