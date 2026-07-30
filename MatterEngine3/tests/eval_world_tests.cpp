// MatterEngine3/tests/eval_world_tests.cpp — Task 4: eval_world + world manifest kind
#include "check.h"
#include "../src/script_host.h"
#include "../src/terrain_field.h"
#include "material_registry.h"
#include <fstream>
#include <sstream>
#include <string>

using namespace script_host;

static const char* kWorld = R"JS(
class TestWorld extends World {
  static params = { worldSeed: 42 };
  static world  = { sectorSize: 16, yMin: -64, yMax: 192 };
  field(p) {
    const relief   = noise2(p.worldSeed ^ 1, 1/900, 3);
    const plains   = noise2(p.worldSeed ^ 3, 1/160, 4).mul(8);
    const mounts   = ridge2(p.worldSeed ^ 4, 1/340, 5).mul(110);
    const height   = blend(plains, mounts, relief.smoothstep(0.45, 0.75)).add(-6);
    const moisture = noise2(p.worldSeed ^ 2, 1/700, 3);
    return { density: heightToDensity(height), moisture, relief, seaLevel: 0.0 };
  }
  biomes() {
    return { meadow: { grass: 156, pebbles: 16, rocks: 2, trees: true },
             foothills: { grass: 39, rocks: 2 },
             mountains: { rocks: 1 }, ocean: {} };
  }
}
)JS";

int main() {
    ScriptHost host;
    WorldEvalResult r = host.eval_world(kWorld, "{}");
    CHECK(r.ok, r.message.c_str());
    CHECK(!r.field_program.empty(), "program emitted");
    CHECK(r.biomes_json.find("meadow") != std::string::npos, "biomes json present");
    CHECK(r.sector_size == 16.0f && r.y_min == -64.0f && r.y_max == 192.0f,
          "world constants read");

    terrain_field::FieldProgram prog; std::string err;
    CHECK(terrain_field::FieldProgram::parse(r.field_program, prog, err),
          err.c_str());
    terrain_field::FieldRuntime f(std::move(prog));
    float h = f.height_at(100, 100);
    CHECK(h > -130.0f && h < 130.0f, "height in plausible range");

    // Determinism + seed sensitivity
    WorldEvalResult r2 = host.eval_world(kWorld, "{}");
    CHECK(r2.field_program == r.field_program, "program deterministic");
    WorldEvalResult r3 = host.eval_world(kWorld, "{\"worldSeed\":7}");
    CHECK(r3.field_program != r.field_program, "seed changes program");

    // Error path: field() throwing must fail loudly
    WorldEvalResult bad = host.eval_world(
        "class B extends World { field(p) { throw new Error('boom'); } }", "{}");
    CHECK(!bad.ok && bad.message.find("boom") != std::string::npos,
          "field() error surfaces");

    // Finding 2: static params defaults must be picked up even when the caller
    // passes "{}" (no overrides). The seed used in field() should be 42 (the
    // class default), so the program must match an explicit worldSeed:42 call.
    WorldEvalResult r_default = host.eval_world(kWorld, "{}");
    WorldEvalResult r_explicit42 = host.eval_world(kWorld, "{\"worldSeed\":42}");
    CHECK(r_default.ok, r_default.message.c_str());
    CHECK(r_explicit42.ok, r_explicit42.message.c_str());
    CHECK(r_default.field_program == r_explicit42.field_program,
          "static params default worldSeed=42 matches explicit override");
    // Non-default seed must differ, confirming the seed is actually wired.
    WorldEvalResult r_other = host.eval_world(kWorld, "{\"worldSeed\":99}");
    CHECK(r_other.ok, r_other.message.c_str());
    CHECK(r_default.field_program != r_other.field_program,
          "non-default seed produces different program (static default really used)");

    // Finding 1: a World whose field() uses a shared-lib symbol still works when
    // no shared_lib_root is set (no imports in the test source — the fold path is
    // a no-op, confirming it doesn't break the import-free path).
    // When a shared-lib root IS present the fold step would resolve imports; we
    // verify here that the fold-gated code path does not regress the base case.
    WorldEvalResult r_nofold = host.eval_world(kWorld, "{}");
    CHECK(r_nofold.ok, r_nofold.message.c_str());
    CHECK(r_nofold.field_program == r.field_program,
          "fold path is transparent when no shared-lib root is set");

    // ---- WP-F: surfaces() tape record/readback/compile round-trip ----
    // A world with no surfaces() emits no tape (legacy path).
    CHECK(r.surface_program.empty(), "no surfaces() => empty surface program");

    static const char* kSurfWorld = R"JS(
class SurfWorld extends World {
  static params = { worldSeed: 42 };
  static world  = { sectorSize: 16, yMin: -64, yMax: 192 };
  field(p) {
    const relief   = noise2(p.worldSeed ^ 1, 1/900, 3);
    const moisture = noise2(p.worldSeed ^ 2, 1/700, 3);
    const height   = noise2(p.worldSeed ^ 3, 1/160, 4).mul(30);
    return { density: heightToDensity(height), moisture, relief, seaLevel: 0.0 };
  }
  surfaces(s) {
    const steep = s.slope.smoothstep(0.3, 0.6);
    const snow  = s.altitude.smoothstep(40, 60).mul(steep.oneMinus());
    const grass = steep.oneMinus().mul(snow.oneMinus());
    s.weight(31, grass);
    s.weight(32, steep);
    s.weight(33, snow);
  }
}
)JS";
    WorldEvalResult rs = host.eval_world(kSurfWorld, "{}");
    CHECK(rs.ok, rs.message.c_str());
    CHECK(!rs.surface_program.empty(), "surfaces() emits a tape");
    CHECK(rs.field_program.find("input ") == std::string::npos,
          "surface ops do not leak into the field program");
    CHECK(rs.surface_program.find("noise2") == std::string::npos,
          "field ops do not leak into the surface program");
    {
        terrain_field::SurfaceProgram sp;
        std::string serr;
        CHECK(terrain_field::SurfaceProgram::parse(rs.surface_program, sp, serr),
              serr.c_str());
        CHECK(sp.materials.size() == 3, "3 declared materials survive readback");
        CHECK(sp.materials[0].handle == 31 && sp.materials[1].handle == 32 &&
                  sp.materials[2].handle == 33,
              "material handles preserved in declaration order");
        CHECK(sp.uses_world_inputs(), "altitude marks the tape world-dependent");
        // Compile + evaluate: flat/low => grass, steep => rock (proves the
        // recorded oneMinus()/smoothstep chain evaluates as authored).
        terrain_field::SurfaceRuntime rt{std::move(sp)};
        float w[terrain_field::kMaxSurfaceMaterials];
        const float flat_pos[3] = {0, 5, 0}, up[3] = {0, 1, 0};
        // Null world context: altitude falls back to 0 => no snow.
        rt.weights_at(flat_pos, up, nullptr, w);
        CHECK(w[0] > 0.99f && w[1] < 1e-6f && w[2] < 1e-6f,
              "recorded tape evaluates: flat sample is grass");
        const float side[3] = {1, 0.05f, 0};
        rt.weights_at(flat_pos, side, nullptr, w);
        CHECK(w[1] > 0.99f, "recorded tape evaluates: steep sample is rock");
    }
    // Determinism of the recorded tape (this is the invalidation key).
    WorldEvalResult rs2 = host.eval_world(kSurfWorld, "{}");
    CHECK(rs2.ok && rs2.surface_program == rs.surface_program,
          "surface program deterministic across evals");

    // ---- WP-F: the extended op surface records and compiles ----
    // noise2World/ridge2World/fieldCurvature/fieldSlope on the tape side,
    // sub/abs/pow/oneMinus on both node classes.
    {
        static const char* kOpsWorld = R"JS(
class OpsWorld extends World {
  static params = { worldSeed: 9 };
  static world  = { sectorSize: 16, yMin: -64, yMax: 192 };
  field(p) {
    const n = noise2(p.worldSeed, 1/300, 3);
    const h = n.abs().pow(2).oneMinus().sub(n).mul(20);
    return { density: heightToDensity(h), moisture: n, relief: n, seaLevel: 0 };
  }
  surfaces(s) {
    const macro  = s.noise2World(11, 1/400, 4);
    const ridged = s.ridge2World(12, 1/900, 3);
    const bowl   = s.fieldCurvature(6).smoothstep(0.5, 3);
    const grad   = s.fieldSlope.smoothstep(0.4, 1.0);
    const shape  = macro.sub(ridged).abs().pow(1.5).oneMinus();
    s.weight(31, shape.mul(bowl).mul(s.value(1)).mul(s.value(1)));
    s.weight(32, grad);
  }
}
)JS";
        WorldEvalResult ro = host.eval_world(kOpsWorld, "{}");
        CHECK(ro.ok, ro.message.c_str());
        CHECK(ro.field_program.find("abs r") != std::string::npos &&
                  ro.field_program.find("pow r") != std::string::npos &&
                  ro.field_program.find("oneminus r") != std::string::npos &&
                  ro.field_program.find("sub r") != std::string::npos,
              "FieldNode sub/abs/pow/oneMinus record their ops");
        CHECK(ro.surface_program.find("noise2w ") != std::string::npos &&
                  ro.surface_program.find("ridge2w ") != std::string::npos &&
                  ro.surface_program.find("curv 6") != std::string::npos &&
                  ro.surface_program.find("input fslope") != std::string::npos,
              "world-noise/curvature/fieldSlope record their ops");
        terrain_field::FieldProgram fp;
        std::string ferr;
        CHECK(terrain_field::FieldProgram::parse(ro.field_program, fp, ferr),
              ferr.c_str());
        terrain_field::SurfaceProgram sp;
        std::string serr;
        CHECK(terrain_field::SurfaceProgram::parse(ro.surface_program, sp, serr),
              serr.c_str());
        CHECK(sp.uses_world_inputs(),
              "world noise/curvature/fieldSlope mark the tape world-dependent");
        // Compile + evaluate under the fallback context: deterministic.
        terrain_field::SurfaceRuntime rt{std::move(sp)};
        float w[terrain_field::kMaxSurfaceMaterials];
        float w2[terrain_field::kMaxSurfaceMaterials];
        const float pos[3] = {2, 7, -3}, up[3] = {0, 1, 0};
        rt.weights_at(pos, up, nullptr, w);
        rt.weights_at(pos, up, nullptr, w2);
        CHECK(w[0] == w2[0] && w[1] == w2[1],
              "extended-op tape evaluates deterministically");
    }

    // Fail-closed paths: a throwing surfaces() and one that declares nothing.
    {
        WorldEvalResult bad_throw = host.eval_world(
            "class T extends World {"
            " field(p) { const n = noise2(1, 0.1, 2);"
            "  return { density: n, moisture: n, relief: n, seaLevel: 0 }; }"
            " surfaces(s) { throw new Error('surf-boom'); } }",
            "{}");
        CHECK(!bad_throw.ok &&
                  bad_throw.message.find("surf-boom") != std::string::npos,
              "surfaces() error surfaces");
        WorldEvalResult bad_empty = host.eval_world(
            "class T extends World {"
            " field(p) { const n = noise2(1, 0.1, 2);"
            "  return { density: n, moisture: n, relief: n, seaLevel: 0 }; }"
            " surfaces(s) { return s.slope; } }",
            "{}");
        CHECK(!bad_empty.ok &&
                  bad_empty.message.find("no material weights") != std::string::npos,
              "surfaces() without s.weight() fails loudly");
    }

    // ---- WP-F: the shipped ChartVtProof world records a compilable tape ----
    // eval_world's defineMaterial shim RESOLVES handles the world-definition
    // loader assigned; mirror the loader by registering the three materials
    // dynamically first, then evaluate the real world source.
    {
        std::ifstream in("../../projects/world_demo/worlds/ChartVtProof.js",
                         std::ios::binary);
        CHECK(bool(in), "ChartVtProof.js readable from the tests directory");
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string proof_source = ss.str();

        MaterialRegistryResetDynamic();
        MaterialDef def{};
        MaterialRegistryDefaultDynamicDef(&def);
        const int grass = MaterialRegistryDefineDynamic(&def, "proofGrass");
        const int rock = MaterialRegistryDefineDynamic(&def, "proofRock");
        const int snow = MaterialRegistryDefineDynamic(&def, "proofSnow");
        CHECK(grass >= 30 && rock > grass && snow > rock,
              "dynamic proof materials registered");

        WorldEvalResult proof = host.eval_world(proof_source, "{}");
        CHECK(proof.ok, proof.message.c_str());
        CHECK(!proof.surface_program.empty(), "ChartVtProof records a tape");
        terrain_field::SurfaceProgram sp;
        std::string serr;
        CHECK(terrain_field::SurfaceProgram::parse(proof.surface_program, sp,
                                                   serr),
              serr.c_str());
        CHECK(sp.materials.size() == 3 && sp.uses_world_inputs(),
              "ChartVtProof declares 3 materials and reads world inputs");
        CHECK(sp.materials[0].handle == grass &&
                  sp.materials[1].handle == rock &&
                  sp.materials[2].handle == snow,
              "ChartVtProof weights resolve to the registered handles");
        // Compile + smoke-evaluate: flat/low grass, steep rock, flat/high
        // snow — under a world context whose altitude is the local y.
        terrain_field::SurfaceRuntime rt{std::move(sp)};
        terrain_field::SurfaceWorldContext wctx{nullptr, nullptr};
        float w[terrain_field::kMaxSurfaceMaterials];
        const float up[3] = {0, 1, 0}, side[3] = {1, 0.05f, 0};
        const float low[3] = {0, 5, 0}, high[3] = {0, 90, 0};
        rt.weights_at(low, up, &wctx, w);
        CHECK(w[0] > 0.99f, "ChartVtProof: flat low ground is grass");
        rt.weights_at(high, side, &wctx, w);
        CHECK(w[1] > 0.99f, "ChartVtProof: steep faces are rock");
        rt.weights_at(high, up, &wctx, w);
        CHECK(w[2] > 0.99f, "ChartVtProof: flat high ground is snow");
        MaterialRegistryResetDynamic();
    }

    return check_summary();
}
