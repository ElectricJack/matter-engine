// sector_setup_profile.cpp — investigation harness (not a test).
//
// Question: of the wall time a streamed WorldSector bake costs, how much is
// fixed script-host setup/teardown (fresh JSRuntime + intrinsics + part-base
// prelude + native bindings + re-compiling the sector source and its imports,
// then freeing all of it) versus work that actually depends on the sector?
//
// Method: drive script_host::bake_source exactly the way
// WorldSession::Impl::bake_and_stage_sector does (a FRESH ScriptHost per
// bake), with a bake_trace::Collector installed so the existing
// fold/ctx/eval/merge/build/mesh/save spans are read straight off the trace
// tree — the same numbers MATTER_BAKE_PROFILE prints, aggregated over N bakes.
// `free` is the residual (part-bake total minus the phases): that is where
// JS_FreeContext + JS_FreeRuntime land.
//
// Also measures, for contrast:
//   - an empty Part with no imports  -> the host's absolute floor
//   - the same sector with a REUSED ScriptHost -> isolates the per-bake fold
//     (shared-lib disk read) that the streaming path repeats every sector
//     because its ScriptHost is a stack local.

#include "../src/script_host.h"
#include "../src/terrain_field.h"
#include "../src/part_graph.h"
#include "../src/bake_trace.h"
#include "../src/bake_trace_names.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace script_host;

namespace {

struct Acc {
    double total = 0, fold = 0, ctx = 0, eval = 0, merge = 0,
           build = 0, mesh = 0, save = 0;
    int n = 0;

    double phases() const { return fold + ctx + eval + merge + build + mesh + save; }
    double free_ms() const { return total - phases(); }
    // Fixed cost = everything that does not depend on which sector this is:
    // fold + ctx + eval + merge on the way in, the runtime teardown on the way out.
    double setup() const { return fold + ctx + eval + merge; }
    double fixed() const { return setup() + free_ms(); }
    double work() const { return build + mesh + save; }
};

void accumulate(const bake_trace::Span& root, Acc& a) {
    for (const bake_trace::Span& pb : root.children) {
        if (!pb.name || std::strcmp(pb.name, bake_trace::kSpanPartBake) != 0) continue;
        ++a.n;
        a.total += pb.end_ms - pb.begin_ms;
        for (const bake_trace::Span& ph : pb.children) {
            const double ms = ph.end_ms - ph.begin_ms;
            if (!ph.name) continue;
            if      (!std::strcmp(ph.name, bake_trace::kSpanFold))  a.fold  += ms;
            else if (!std::strcmp(ph.name, bake_trace::kSpanCtx))   a.ctx   += ms;
            else if (!std::strcmp(ph.name, bake_trace::kSpanEval))  a.eval  += ms;
            else if (!std::strcmp(ph.name, bake_trace::kSpanMerge)) a.merge += ms;
            else if (!std::strcmp(ph.name, bake_trace::kSpanBuild)) a.build += ms;
            else if (!std::strcmp(ph.name, bake_trace::kSpanMesh))  a.mesh  += ms;
            else if (!std::strcmp(ph.name, bake_trace::kSpanSave))  a.save  += ms;
        }
    }
}

void report(const char* label, const Acc& a) {
    if (a.n == 0) { std::printf("%-24s  (no bakes)\n", label); return; }
    const double n = a.n;
    std::printf("%-24s %8.2f %7.2f %6.2f %7.2f %6.2f %8.2f %7.2f %6.2f %6.2f   %5.1f%%\n",
                label, a.total / n, a.fold / n, a.ctx / n, a.eval / n,
                a.merge / n, a.build / n, a.mesh / n, a.save / n,
                a.free_ms() / n,
                a.total > 0 ? 100.0 * a.fixed() / a.total : 0.0);
}

void header() {
    std::printf("\n%-24s %8s %7s %6s %7s %6s %8s %7s %6s %6s   %6s\n",
                "scenario (ms/bake)", "total", "fold", "ctx", "eval",
                "merge", "build", "mesh", "save", "free", "fixed");
    std::printf("%-24s %8s %7s %6s %7s %6s %8s %7s %6s %6s   %6s\n",
                "------------------------", "--------", "-------", "------",
                "-------", "------", "--------", "-------", "------",
                "------", "------");
}

std::string slurp(const char* path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const char* kEmptyPart =
    "class Empty extends Part {\n"
    "  static params = { tx: 0, tz: 0 };\n"
    "  build(p) {}\n"
    "}\n";

}  // namespace

int main(int argc, char** argv) {
    const int iters = argc > 1 ? std::max(1, std::atoi(argv[1])) : 25;

    // StreamMountain-shaped field: the world whose ~5,000-sector disc motivated
    // the bake pool. Op text mirrors tests/sector_bake_tests.cpp.
    terrain_field::FieldProgram prog;
    std::string err;
    if (!terrain_field::FieldProgram::parse(
            "noise2 42 0.005 4 0.5 2.0\nconst 60\nmul r0 r1\nconst 0.6\nconst 0.3\n"
            "height r2\nmoisture r3\nrelief r4\nseaLevel -80\nbiome 0.65 0.35\n",
            prog, err)) {
        std::fprintf(stderr, "field parse failed: %s\n", err.c_str());
        return 1;
    }
    terrain_field::FieldRuntime field(std::move(prog));

    const std::string src = slurp("../../projects/world_demo/scenes/StreamMountain/objects/WorldSector.js");
    if (src.empty()) {
        std::fprintf(stderr, "cannot read WorldSector.js (run from MatterEngine3/tests)\n");
        return 1;
    }

    // argv[2] overrides where artifacts are written: `save` is disk I/O, so the
    // filesystem it lands on moves that column a lot on Windows.
    const char* kPartsDir = argc > 2 ? argv[2] : "build/sectorprof";
    const std::string shared_lib = "../shared-lib";

    // StreamMountain world constants.
    auto make_opts = [&]() {
        BakeOptions o;
        o.parts_dir = kPartsDir;
        o.world.field = &field;
        o.world.sector_size = 64.0f;
        o.world.y_min = -96.0f;
        o.world.y_max = 704.0f;
        return o;
    };

    // Full declared variant table, canonicalized the way placeChild expects
    // (same construction as sector_bake_tests.cpp).
    std::vector<std::string> mods, cparams;
    std::vector<uint64_t> hashes;
    {
        auto canon = [](const std::string& raw) {
            return raw.empty() ? std::string()
                : part_graph::params_to_json(part_graph::params_from_json(raw));
        };
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
    }
    const char* kBiomes =
        "{\\\"meadow\\\":{\\\"rocks\\\":4,\\\"grass\\\":5,\\\"trees\\\":2},"
        "\\\"foothills\\\":{\\\"rocks\\\":4,\\\"grass\\\":5,\\\"trees\\\":2},"
        "\\\"mountains\\\":{\\\"rocks\\\":4,\\\"grass\\\":5,\\\"trees\\\":1}}";

    // One scenario: bake `iters` sectors (tx varies, as in a streaming disc),
    // fresh ScriptHost per bake unless `reuse_host` is set.
    auto run = [&](const char* label, int terrain_lod, int rung, bool with_biomes,
                   bool reuse_host, bool empty_part) {
        Acc acc;
        ScriptHost shared;
        if (!empty_part) shared.set_shared_lib_root(shared_lib);

        for (int i = -3; i < iters; ++i) {           // 3 warmup bakes discarded
            char params[2048];
            if (empty_part) {
                std::snprintf(params, sizeof(params), R"({"tx":%d,"tz":0})", i);
            } else if (with_biomes) {
                std::snprintf(params, sizeof(params),
                    R"({"tx":%d,"tz":0,"rung":%d,"terrainLod":%d,"edgeMask":0,)"
                    R"("worldSeed":42,"fieldHash":"abc","biomes":"%s"})",
                    i, rung, terrain_lod, kBiomes);
            } else {
                std::snprintf(params, sizeof(params),
                    R"({"tx":%d,"tz":0,"rung":%d,"terrainLod":%d,"edgeMask":0,)"
                    R"("worldSeed":42,"fieldHash":"abc","biomes":""})",
                    i, rung, terrain_lod);
            }

            bake_trace::Collector col;
            bake_trace::set_current(&col);

            BakeResult r;
            if (reuse_host) {
                r = with_biomes
                    ? shared.bake_source(empty_part ? kEmptyPart : src, params,
                                         make_opts(), hashes.data(), hashes.size(),
                                         mods.data(), cparams.data())
                    : shared.bake_source(empty_part ? kEmptyPart : src, params,
                                         make_opts());
            } else {
                // Exactly what bake_and_stage_sector does per sector.
                ScriptHost fresh;
                if (!empty_part) fresh.set_shared_lib_root(shared_lib);
                r = with_biomes
                    ? fresh.bake_source(src, params, make_opts(),
                                        hashes.data(), hashes.size(),
                                        mods.data(), cparams.data())
                    : fresh.bake_source(empty_part ? kEmptyPart : src, params,
                                        make_opts());
            }

            bake_trace::set_current(nullptr);

            if (!r.error.ok) {
                std::fprintf(stderr, "[%s] bake failed: %s\n", label,
                             r.error.message.c_str());
                return;
            }
            if (i >= 0) accumulate(col.snapshot(), acc);
        }
        report(label, acc);
    };

    std::printf("sector bake setup/teardown profile — %d bakes per scenario\n", iters);
    std::printf("fixed = fold+ctx+eval+merge+free (per-bake JS host cost, "
                "independent of the sector)\n");

    header();
    run("empty part (floor)",   0, 0, false, false, /*empty*/true);
    run("hf lod0 terrain-only", 0, 0, false, false, false);
    run("hf lod2 terrain-only", 2, 0, false, false, false);
    run("hf lod4 terrain-only", 4, 0, false, false, false);
    run("voxel terrain-only",   5, 0, false, false, false);
    // The disc's bulk: far tiles still get the world's biomes table and the
    // full 30-entry child table, they just place fewer things.
    run("hf lod0 + biomes r0",  0, 0, true,  false, false);
    run("hf lod1 + biomes r0",  1, 0, true,  false, false);
    run("hf lod2 + scatter r2", 2, 2, true,  false, false);
    run("voxel + scatter r2",   5, 2, true,  false, false);
    std::printf("\n-- same, but ScriptHost REUSED across bakes (fold cache warm) --\n");
    run("hf lod2 terrain-only*", 2, 0, false, true, false);
    run("voxel terrain-only*",   5, 0, false, true, false);
    std::printf("\n");
    return 0;
}
