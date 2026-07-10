// MatterEngine3/tests/eval_world_tests.cpp — Task 4: eval_world + world manifest kind
#include "check.h"
#include "../src/script_host.h"
#include "../src/terrain_field.h"
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

    return check_summary();
}
