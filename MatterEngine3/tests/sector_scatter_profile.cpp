// sector_scatter_profile.cpp — investigation harness (not a test).
//
// Question: inside a StreamMountain sector bake, where does the scatter time
// actually go?
//
// This exists because that question was answered wrongly twice, both times by
// arithmetic over quantities measured OUTSIDE the loop being reasoned about:
//
//   1. "sampleHabitat is ~99% of a far sector's bake, so a native tape is a
//      ~50x win." The candidate count came from a naive grid (3,385) while
//      candidatesInRect is a Poisson sampler whose real output is ~963. The
//      tape landed -35.6%, not -98%.
//   2. "The residual is the O(viable^2) tree exclusion pass." The estimate
//      matched the measured 180.7 ms to within 3%. An exact spatial-hash
//      replacement, verified to emit a bitwise-identical placement list,
//      measured +1.4% / -1.9% / -2.9%. Nothing. `viable` had been read as the
//      ~963 candidates that REACH the habitat sample rather than the far
//      smaller subset that passes it.
//
// So this harness measures rather than projects, using ScriptProfile
// (dsl_bindings.h) counters that WorldSector.js and alpine_ecology.js open
// around their own phases.
//
// It drives the REAL sources -- ../../projects/world_demo/objects/WorldSector.js
// and the real StreamMountain habitat tape, compiled from the real world
// definition through the same eval_world -> SurfaceProgram::parse path
// install_world uses. A harness that stubs the ecology would just be a third
// way to measure the wrong thing.
//
// Known limits, stated because a previous harness in this repo was trusted
// past them: this runs ONE bake at a time on ONE thread, so it says nothing
// about pool throughput, publish-side cost, or how the fill schedules -- only
// about the composition of a single bake. Cross-check the shape against the
// engine's own [script-profile] block (printed at [stream.rate] and at
// [stream.fill] under MATTER_SCRIPT_PROFILE=1) before acting on it.
//
// Usage, from MatterEngine3/tests:
//   make run-scatterprof          # or: ./build/sector_scatter_profile [iters]

#include "../src/script_host.h"
#include "../src/terrain_field.h"
#include "../src/part_graph.h"
#include "../src/dsl_bindings.h"
#include "../src/script/world_definition_loader.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <string>
#include <vector>

using namespace script_host;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
    const int iters = argc > 1 ? std::max(1, std::atoi(argv[1])) : 8;

    // ScriptProfile is env-gated. Set it here rather than making the caller
    // remember: a harness whose whole output is the profile has no other mode.
#ifdef _WIN32
    _putenv_s("MATTER_SCRIPT_PROFILE", "1");
#else
    setenv("MATTER_SCRIPT_PROFILE", "1", 1);
#endif

    const std::string world_src =
        slurp("../../projects/world_demo/scenes/StreamMountain/StreamMountain.js");
    const std::string sector_src =
        slurp("../../projects/world_demo/scenes/StreamMountain/objects/WorldSector.js");
    if (world_src.empty() || sector_src.empty()) {
        std::fprintf(stderr,
                     "cannot read StreamMountain.js / WorldSector.js "
                     "(run from MatterEngine3/tests)\n");
        return 1;
    }

    // BOTH shared-lib roots, project first -- the resolver order the engine
    // uses. StreamMountain imports alpine_ecology from the project root and
    // WorldSector imports rng/scatter_grid from the engine's.
    const std::vector<std::string> roots = {
        "../../projects/world_demo/shared-lib", "../shared-lib"};

    // ---- the REAL field and the REAL habitat tape ---------------------------
    // Compiled from the world definition exactly as install_world does, so the
    // tape under measurement is the one that ships, not a stand-in.
    //
    // load_world_definition FIRST, and not for its return value: StreamMountain
    // declares materials with defineMaterial, and the loader is what assigns
    // their handles. Skip it and eval_world fails with "not registered", no
    // habitat tape compiles, and the harness would quietly profile the
    // interpreted JS fallback while claiming to measure the tape.
    {
        matter::WorldLoadDesc desc;
        desc.world_path = "../../projects/world_demo/scenes/StreamMountain/StreamMountain.js";
        desc.objects_dir = "../../projects/world_demo/objects";
        desc.project_shared_lib_dir = "../../projects/world_demo/shared-lib";
        desc.engine_shared_lib_dir = "../shared-lib";
        matter::WorldDefinition definition;
        matter::WorldLoadError lerr;
        if (!matter::load_world_definition(desc, definition, lerr)) {
            std::fprintf(stderr, "load_world_definition failed: %s\n",
                         lerr.message.c_str());
            return 1;
        }
    }

    ScriptHost world_host;
    world_host.set_shared_lib_roots(roots);
    WorldEvalResult wr = world_host.eval_world(world_src, "{}");
    if (!wr.ok) {
        std::fprintf(stderr, "eval_world failed: %s\n", wr.message.c_str());
        return 1;
    }

    terrain_field::FieldProgram fprog;
    std::string perr;
    if (!terrain_field::FieldProgram::parse(wr.field_program, fprog, perr)) {
        std::fprintf(stderr, "field parse failed: %s\n", perr.c_str());
        return 1;
    }
    terrain_field::FieldRuntime field(std::move(fprog));

    std::unique_ptr<terrain_field::SurfaceRuntime> habitat;
    if (!wr.habitat_program.empty()) {
        terrain_field::SurfaceProgram hprog;
        if (!terrain_field::SurfaceProgram::parse(
                wr.habitat_program, hprog, perr,
                terrain_field::TapeMode::Habitat)) {
            std::fprintf(stderr, "habitat parse failed: %s\n", perr.c_str());
            return 1;
        }
        habitat = std::make_unique<terrain_field::SurfaceRuntime>(
            std::move(hprog));
        std::printf("habitat tape: %d channels, %zu ops\n",
                    habitat->channel_count(), wr.habitat_channels.size());
    } else {
        std::printf("habitat tape: NONE (ecology will use the JS fallback)\n");
    }

    // The world's own biomes table, JSON-escaped into the sector params the
    // way the engine passes it down.
    std::string biomes_escaped;
    for (char c : wr.biomes_json) {
        if (c == '"') biomes_escaped += "\\\"";
        else if (c == '\\') biomes_escaped += "\\\\";
        else biomes_escaped += c;
    }
    if (biomes_escaped.empty()) {
        std::fprintf(stderr, "world declared no biomes table\n");
        return 1;
    }

    // ---- child variant table ------------------------------------------------
    // placeChild does a strict composite-key lookup, so the alpine variants
    // have to be installed and canonicalized the way the engine canonicalizes
    // them, or every placement fails and the bake dies.
    //
    // eval_requires gets the BIOMES PARAM, not "{}". WorldSector's requires()
    // branches on the table -- selectVegetationCatalog returns the 19-form
    // alpine catalog only when __vegetation.profile is alpine-lush, and the
    // legacy Rock/Grass/Tree list otherwise. Called with empty params it
    // returns 24 legacy variants, every AlpineConifer placement misses, and
    // the bake fails on the first tree.
    std::vector<std::string> mods, cparams;
    std::vector<uint64_t> hashes;
    {
        const std::string req_params =
            "{\"biomes\":\"" + biomes_escaped + "\"}";
        auto variants = world_host.eval_requires(sector_src, req_params);
        for (const auto& v : variants) {
            mods.push_back(v.module_specifier);
            cparams.push_back(v.params_json.empty()
                ? std::string()
                : part_graph::params_to_json(
                      part_graph::params_from_json(v.params_json)));
            hashes.push_back(0x1000ull + hashes.size());
        }
        std::printf("child variants: %zu\n", mods.size());
        if (mods.empty()) {
            std::fprintf(stderr,
                         "no child variants -- placements would all miss; "
                         "refusing to report a meaningless profile\n");
            return 1;
        }
    }

    // save_v2 does not create its own directory tree.
    {
        std::error_code ec;
        std::filesystem::create_directories("build/scatterprof/parts", ec);
    }

    auto run = [&](const char* label, int terrain_lod, int rung,
                   double sector_size) {
        dsl::script_profile::reset();
        const auto t0 = std::chrono::steady_clock::now();
        int ok = 0;
        for (int i = 0; i < iters; ++i) {
            char params[8192];
            std::snprintf(params, sizeof(params),
                R"({"tx":%d,"tz":%d,"rung":%d,"terrainLod":%d,"edgeMask":0,)"
                R"("sectorSize":%.1f,"worldSeed":42,"fieldHash":"abc",)"
                R"("biomes":"%s"})",
                i, i * 7, rung, terrain_lod, sector_size,
                biomes_escaped.c_str());

            // Fresh ScriptHost per bake: what bake_and_stage_sector does.
            ScriptHost h;
            h.set_shared_lib_roots(roots);
            BakeOptions o;
            o.parts_dir = "build/scatterprof";
            o.world.field = &field;
            o.world.habitat = habitat.get();
            o.world.sector_size = (float)sector_size;
            o.world.y_min = -96.0f;
            o.world.y_max = 704.0f;
            BakeResult r = h.bake_source(sector_src, params, o,
                                         hashes.data(), hashes.size(),
                                         mods.data(), cparams.data());
            if (!r.error.ok) {
                std::fprintf(stderr, "[%s] bake failed: %s\n", label,
                             r.error.message.c_str());
                return;
            }
            ++ok;
        }
        const double wall_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        std::printf("\n=== %s (%d bakes, %.1f ms/bake wall) ===\n",
                    label, ok, ok ? wall_ms / ok : 0.0);
        const std::string rep = dsl::script_profile::report();
        std::fputs(rep.empty() ? "  (no profile data)\n" : rep.c_str(), stdout);
    };

    std::printf("\nStreamMountain sector scatter profile — %d bakes per row\n",
                iters);
    std::printf("Single-threaded and single-bake: this is the COMPOSITION of a "
                "bake,\nnot the throughput of a fill. Cross-check against the "
                "engine's own\n[script-profile] block before acting on it.\n");

    // terrainLod is the distance band; VEGETATION_MIN_LOD gates planting at 3.
    // Band 3 is the far edge of where vegetation is planted at all -- the bulk
    // of the disc -- and band 5 is underfoot.
    run("band 5 (nearest, rung 2)", 5, 2, 64.0);
    run("band 4 (rung 1)",          4, 1, 64.0);
    run("band 3 (far edge, rung 0)", 3, 0, 64.0);
    run("band 2 (gated: no vegetation)", 2, 0, 128.0);
    std::printf("\n");
    return 0;
}
