// MatterEngine3/tests/sector_bake_tests.cpp
#include "check.h"
#include "../src/script_host.h"
#include "../src/terrain_field.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

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
        // 8 rocks + 8 boulders + 6 pebbles + 5 grass + 1 tree = 28
        CHECK(req_a.size() == 28, "full variant list");
    }

    system("rm -rf /tmp/sector_bake_parts && mkdir -p /tmp/sector_bake_parts");
    auto bake = [&](const char* params) {
        BakeOptions opts;
        opts.parts_dir = "/tmp/sector_bake_parts";
        opts.world.field = &field;
        return host.bake_source(src, params, opts);
    };
    // rung 0 bakes terrain-only (no placeChild -> bakes even without child hashes)
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

    // scatter bake: rung >= 2 enables vegetation scatter (see WorldSector.js line 34).
    // Provide a biome table with non-zero grass/rocks/pebbles counts so the
    // scatter code paths execute even at this deterministic seed/tile.
    // We cannot assert child instance geometry here (child hashes are not wired
    // up in this harness), but we CAN assert the bake succeeds and its hash
    // differs from a rung-0 bake of the same tile (rung is a hash input).
    {
        const char* p_scatter =
            R"({"tx":0,"tz":0,"rung":2,"worldSeed":42,"fieldHash":"abc",)"
            R"("biomes":"{\"meadow\":{\"rocks\":4,\"pebbles\":4,\"grass\":5}}"})";
        BakeResult rs = bake(p_scatter);
        CHECK(rs.error.ok, rs.error.message.c_str());
        // rung is folded into the hash, so rung-2 must differ from rung-0
        CHECK(rs.resolved_hash != r0.resolved_hash, "scatter-rung hash differs from terrain-only rung");
    }

    return check_summary();
}
