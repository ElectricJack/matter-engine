// MatterEngine3/tests/terrain_verb_tests.cpp — Task 5: terrainVolume verb through bake_source
#include "check.h"
#include "../src/script_host.h"
#include "../src/terrain_field.h"
#include "../src/part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

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

static const char* kSector = R"JS(
class S extends Part {
  static params = { tx: 0, tz: 0, rung: 0 };
  build(p) {
    this.terrainVolume(p.tx, p.tz, p.rung, [MAT.grass, MAT.dirt, MAT.rock, MAT.snow]);
  }
}
)JS";

int main() {
    terrain_field::FieldProgram prog; std::string err;
    CHECK(terrain_field::FieldProgram::parse(
        "const 5\nconst 0.5\nconst 0.5\n"
        "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n",
        prog, err), err.c_str());
    terrain_field::FieldRuntime field(std::move(prog));

    const std::string parts_dir = sandbox_dir("me3_terrain_verb");
    ScriptHost host;

    // No world bound -> loud error
    {
        BakeOptions opts; opts.parts_dir = parts_dir;
        BakeResult r = host.bake_source(kSector, "{}", opts);
        CHECK(!r.error.ok, "terrainVolume without world binding must fail");
        CHECK(r.error.message.find("terrainVolume") != std::string::npos, "names the verb");
    }
    // Bound -> bakes; artifact holds 128 surface tris. Was 192 (128 surface +
    // 64 border skirt) until skirts were removed on 2026-07-30; see
    // terrain_mesher.cpp.
    {
        BakeOptions opts; opts.parts_dir = parts_dir;
        opts.world.field = &field;   // sector_size / y bounds = defaults (16, -64, 192)
        BakeResult r = host.bake_source(kSector, "{}", opts);
        CHECK(r.error.ok, r.error.message.c_str());

        // Load the artifact and count triangles.
        BLASManager blas;
        TLASManager tlas(64);
        std::vector<part_asset::ChildInstance> children;
        part_asset::LodLevels lods;
        bool loaded = part_asset::load_v2(r.written_path, r.resolved_hash, blas, tlas, children, lods);
        CHECK(loaded, "load artifact");
        int total_tris = blas.get_total_triangle_count();
        printf("  terrainVolume total triangles: %d (expect 128)\n", total_tris);
        CHECK(total_tris == 128, "128 triangles (surface only, no skirts)");
        CHECK(blas.get_unique_blas_count() >= 1, "at least 1 material bucket");
    }
    return check_summary();
}
