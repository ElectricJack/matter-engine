// MatterEngine3/tests/terrain_verb_tests.cpp — Task 5: terrainVolume verb through bake_source
#include "check.h"
#include "../src/script_host.h"
#include "../src/terrain_field.h"
#include "../src/part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

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

    system("rm -rf /tmp/terrain_verb_parts && mkdir -p /tmp/terrain_verb_parts");
    ScriptHost host;

    // No world bound -> loud error
    {
        BakeOptions opts; opts.parts_dir = "/tmp/terrain_verb_parts";
        BakeResult r = host.bake_source(kSector, "{}", opts);
        CHECK(!r.error.ok, "terrainVolume without world binding must fail");
        CHECK(r.error.message.find("terrainVolume") != std::string::npos, "names the verb");
    }
    // Bound -> bakes; artifact holds 128 surface + 64 skirt = 192 tris total
    {
        BakeOptions opts; opts.parts_dir = "/tmp/terrain_verb_parts";
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
        printf("  terrainVolume total triangles: %d (expect 192)\n", total_tris);
        CHECK(total_tris == 192, "192 triangles (128 surface + 64 skirt)");
        CHECK(blas.get_unique_blas_count() >= 1, "at least 1 material bucket");
    }
    return check_summary();
}
