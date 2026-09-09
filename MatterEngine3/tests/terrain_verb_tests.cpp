// MatterEngine3/tests/terrain_verb_tests.cpp — Task 5: the terrain mesher verb
// through bake_source.
//
// RETARGETED FROM `terrainVolume` TO `terrainVolumeTiled` (2026-08-20). The
// column verb this suite was written for was commented out in 516f398b
// ("refactor: octree streaming everywhere; comment out the column path",
// 2026-08-12) -- in dsl_bindings.cpp AND in part_base.js.h, together, so a
// script calling `this.terrainVolume(...)` now dies with "TypeError: not a
// function". `terrainVolumeTiled` is the verb every streamed scene uses; it
// meshes the CUBE [ty*S, (ty+1)*S) instead of the column [yMin, surface],
// which is why the bake below asks for the tile that contains the surface
// rather than passing y bounds.
#include "check.h"
#include "../src/script_host.h"
#include "../src/terrain_field.h"
#include "../src/terrain_mesher.h"
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
  static params = { tx: 0, ty: 0, tz: 0, rung: 0 };
  build(p) {
    this.terrainVolumeTiled(p.tx, p.ty, p.tz, p.rung,
                            [MAT.grass, MAT.dirt, MAT.rock, MAT.snow]);
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
        CHECK(!r.error.ok, "terrainVolumeTiled without world binding must fail");
        CHECK(r.error.message.find("terrainVolumeTiled") != std::string::npos,
              "names the verb");
    }
    // Bound -> bakes the ty = 0 tile, which for this flat h = 5 field is the one
    // tile of the stack that holds the surface (sector_size 16 => the tile spans
    // world y in [0, 16)).
    //
    // WHAT THE COUNT IS COMPARED AGAINST, and why it is no longer a literal.
    // This suite tests the VERB -- that the DSL hop reaches the mesher and that
    // the mesher's soup reaches the artifact intact. It was pinned at 128, the
    // COLUMN mesher's output for this field, and the tiled mesher legitimately
    // produces a different tessellation of the same plane (158 today): a tiled
    // mesh is clamped to the tile box [0, S] in every axis so that a shared
    // lattice plane reads bitwise identical from both neighbours, which adds
    // border vertices the column path -- whose mesh sits at [-1, S-1] -- never
    // had. Re-pinning the new literal would just move the staleness, and the
    // tessellation itself is already pinned BITWISE, in six configurations, by
    // terrain_mesher_tests.cpp. So ask the mesher directly and require the bake
    // to agree with it exactly: nothing added, nothing dropped, no skirts.
    {
        terrain_mesher::SectorMesh direct;
        std::string mesh_err;
        CHECK(terrain_mesher::mesh_sector_tiled(field, 0, 0, 0, 0,
                                                dsl::WorldBinding{}.sector_size,
                                                direct, nullptr, mesh_err),
              mesh_err.c_str());
        const size_t expect_tris = direct.triangle_count();
        CHECK(expect_tris > 0,
              "the fixture tile holds the surface (a bake of nothing proves nothing)");

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
        printf("  terrainVolumeTiled total triangles: %d (mesher says %zu)\n",
               total_tris, expect_tris);
        CHECK(total_tris == static_cast<int>(expect_tris),
              "the artifact holds exactly the mesher's triangles, surface only");
        CHECK(blas.get_unique_blas_count() >= 1, "at least 1 material bucket");
    }
    return check_summary();
}
