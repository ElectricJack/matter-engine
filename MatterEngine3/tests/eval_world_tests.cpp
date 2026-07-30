// MatterEngine3/tests/eval_world_tests.cpp — Task 4: eval_world + world manifest kind
#include "check.h"
#include "../src/script_host.h"
#include "../src/terrain_field.h"
#include "material_registry.h"
#include <cmath>
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

    // ---- texel-tape P1: 3D noise recorders + warp tail + fract ----
    // s.noise3/ridge3/noise3World/ridge3World record the canonical 3D-noise
    // lines (defaults oct=3 gain=0.5 lac=2, optional {seed, freq, amp} warp
    // object appending the 3-token tail), and node.fract() records its unary.
    {
        static const char* kTape3World = R"JS(
class Tape3World extends World {
  static params = { worldSeed: 9 };
  static world  = { sectorSize: 16, yMin: -64, yMax: 192 };
  field(p) {
    const n = noise2(p.worldSeed, 1/300, 3);
    return { density: heightToDensity(n.mul(20)), moisture: n, relief: n, seaLevel: 0 };
  }
  surfaces(s) {
    const strata = s.noise3World(21, 0.02, 3, 0.5, 2.0, {seed: 5, freq: 0.11, amp: 6});
    const band   = s.altitude.add(strata.mul(6)).mul(0.125).fract();
    const local  = s.noise3(7, 0.25).add(s.ridge3World(12, 0.01));
    s.weight(31, band);
    s.weight(32, local.clamp(0, 1));
  }
}
)JS";
        WorldEvalResult t3 = host.eval_world(kTape3World, "{}");
        CHECK(t3.ok, t3.message.c_str());
        CHECK(t3.surface_program.find("noise3w 21 0.02 3 0.5 2 5 0.11 6\n") !=
                  std::string::npos,
              "noise3World records the warp tail in canonical token order");
        CHECK(t3.surface_program.find("noise3 7 0.25 3 0.5 2\n") !=
                  std::string::npos,
              "noise3 defaults record as oct=3 gain=0.5 lac=2, no tail");
        CHECK(t3.surface_program.find("ridge3w 12 0.01 3 0.5 2\n") !=
                  std::string::npos,
              "ridge3World records its canonical line");
        CHECK(t3.surface_program.find("fract r") != std::string::npos,
              "fract() records its unary op");
        CHECK(t3.field_program.find("noise3") == std::string::npos &&
                  t3.field_program.find("fract") == std::string::npos,
              "3D-noise/fract ops do not leak into the field program");
        terrain_field::SurfaceProgram sp;
        std::string serr;
        CHECK(terrain_field::SurfaceProgram::parse(t3.surface_program, sp, serr),
              serr.c_str());
        CHECK(sp.uses_world_inputs(),
              "noise3w/ridge3w mark the tape world-dependent");
        // Compile + evaluate under the fallback context: deterministic.
        terrain_field::SurfaceRuntime rt{std::move(sp)};
        float w[terrain_field::kMaxSurfaceMaterials];
        float w2[terrain_field::kMaxSurfaceMaterials];
        const float pos[3] = {2, 7, -3}, up[3] = {0, 1, 0};
        rt.weights_at(pos, up, nullptr, w);
        rt.weights_at(pos, up, nullptr, w2);
        CHECK(w[0] == w2[0] && w[1] == w2[1],
              "3D-noise tape evaluates deterministically");
        // The recorded tape is the invalidation key: byte-stable across evals.
        WorldEvalResult t3b = host.eval_world(kTape3World, "{}");
        CHECK(t3b.ok && t3b.surface_program == t3.surface_program,
              "3D-noise surface program deterministic across evals");
    }

    // ---- texel-tape P3: appearance-lane recorders ----
    // s.tint / s.roughnessBias / s.wetness record the canonical directive
    // lines AFTER every material line, in the fixed order tint, roughbias,
    // wetness — regardless of the order surfaces() called them, so the tape
    // hash (the page-invalidation key) is authoring-order independent.
    {
        static const char* kTapeApp = R"JS(
class TapeApp extends World {
  static params = { worldSeed: 4 };
  static world  = { sectorSize: 16, yMin: -64, yMax: 192 };
  field(p) {
    const n = noise2(p.worldSeed, 1/300, 3);
    return { density: heightToDensity(n.mul(20)), moisture: n, relief: n, seaLevel: 0 };
  }
  surfaces(s) {
    const steep = s.slope.smoothstep(0.3, 0.6);
    // Deliberately "wrong" call order: wetness, then a weight, then tint,
    // then roughbias — the recorder must still emit mats, tint, roughbias,
    // wetness.
    s.wetness(s.fieldCurvature(4).smoothstep(0.5, 2.5));
    s.metallic(s.noise3(0xE1, 1/0.7, 2).smoothstep(0.8, 0.95));
    s.weight(31, steep.oneMinus());
    const drift = s.noise3World(0xC4, 1/140, 3).mul(0.1).add(1.0);
    s.tint(drift, drift, 0.98);
    s.weight(32, steep);
    s.roughnessBias(0.25);
  }
}
)JS";
        WorldEvalResult ta = host.eval_world(kTapeApp, "{}");
        CHECK(ta.ok, ta.message.c_str());
        const size_t p_mat31 = ta.surface_program.find("material 31 r");
        const size_t p_mat32 = ta.surface_program.find("material 32 r");
        const size_t p_tint  = ta.surface_program.find("\ntint r");
        const size_t p_rough = ta.surface_program.find("\nroughbias r");
        const size_t p_wet   = ta.surface_program.find("\nwetness r");
        const size_t p_met   = ta.surface_program.find("\nmetallic r");
        CHECK(p_mat31 != std::string::npos && p_mat32 != std::string::npos &&
                  p_tint != std::string::npos && p_rough != std::string::npos &&
                  p_wet != std::string::npos && p_met != std::string::npos,
              "appearance: all four directives and both materials record");
        CHECK(p_mat31 < p_mat32 && p_mat32 < p_tint && p_tint < p_rough &&
                  p_rough < p_wet && p_wet < p_met,
              "appearance: canonical order is materials, tint, roughbias, "
              "wetness, metallic regardless of JS call order");
        CHECK(ta.surface_program.find("tint r") <
                  ta.surface_program.find("wetness r"),
              "appearance: tint precedes wetness (application order)");
        terrain_field::SurfaceProgram sap;
        std::string saerr;
        CHECK(terrain_field::SurfaceProgram::parse(ta.surface_program, sap,
                                                   saerr),
              saerr.c_str());
        CHECK(sap.has_tint() && sap.has_rough_bias() && sap.has_wetness() &&
                  sap.has_metallic(),
              "appearance: the recorded tape compiles with all four lanes");
        CHECK(sap.materials.size() == 2,
              "appearance: directives do not add material columns");
        // A plain number coerces through __sreg exactly like a SurfaceNode.
        CHECK(sap.tint_reg[2] != sap.tint_reg[0] && sap.tint_reg[0] >= 0,
              "appearance: tint accepts a plain number for one component");
        WorldEvalResult ta2 = host.eval_world(kTapeApp, "{}");
        CHECK(ta2.ok && ta2.surface_program == ta.surface_program,
              "appearance: recorded tape is byte-stable across evals");
        // The lanes are optional: the same world without them is a different
        // tape (the hash covers invalidation) but still compiles.
        static const char* kTapePlain = R"JS(
class TapePlain extends World {
  static params = { worldSeed: 4 };
  static world  = { sectorSize: 16, yMin: -64, yMax: 192 };
  field(p) {
    const n = noise2(p.worldSeed, 1/300, 3);
    return { density: heightToDensity(n.mul(20)), moisture: n, relief: n, seaLevel: 0 };
  }
  surfaces(s) {
    const steep = s.slope.smoothstep(0.3, 0.6);
    s.weight(31, steep.oneMinus());
    s.weight(32, steep);
  }
}
)JS";
        WorldEvalResult tp = host.eval_world(kTapePlain, "{}");
        CHECK(tp.ok, tp.message.c_str());
        CHECK(tp.surface_program.find("tint ") == std::string::npos &&
                  tp.surface_program.find("wetness ") == std::string::npos,
              "appearance: a world that declares none records none");
        terrain_field::SurfaceProgram spp;
        CHECK(terrain_field::SurfaceProgram::parse(tp.surface_program, spp,
                                                   saerr),
              saerr.c_str());
        CHECK(!spp.has_appearance() && spp.hash() != sap.hash(),
              "appearance: declaring lanes changes the tape hash");
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

    // ---- The shipped StreamMountain world: 4 materials + an alpine tape -----
    // Same shape as the ChartVtProof block above (register the dynamic
    // materials the world-definition loader would have assigned, then evaluate
    // the real world source), but the assertions are about the classification
    // itself: the four deterministic corners of the tape, and — evaluated
    // against StreamMountain's OWN field, which is where the tape's
    // relief/moisture channels come from — that all four classes actually occur
    // across the range and land where alpine photographs put them.
    {
        std::ifstream in("../../projects/world_demo/worlds/StreamMountain.js",
                         std::ios::binary);
        CHECK(bool(in), "StreamMountain.js readable from the tests directory");
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string mountain_source = ss.str();

        MaterialRegistryResetDynamic();
        MaterialDef def{};
        MaterialRegistryDefaultDynamicDef(&def);
        const int ground = MaterialRegistryDefineDynamic(&def, "AlpineGround");
        const int rock = MaterialRegistryDefineDynamic(&def, "AlpineRock");
        const int scree = MaterialRegistryDefineDynamic(&def, "Scree");
        const int snow = MaterialRegistryDefineDynamic(&def, "AlpineSnow");
        const int meadow = MaterialRegistryDefineDynamic(&def, "AlpineMeadow");
        CHECK(ground >= 30 && rock > ground && scree > rock && snow > scree &&
                  meadow > snow,
              "dynamic alpine materials registered");

        WorldEvalResult mtn = host.eval_world(mountain_source, "{}");
        CHECK(mtn.ok, mtn.message.c_str());
        CHECK(!mtn.surface_program.empty(), "StreamMountain records a tape");
        terrain_field::SurfaceProgram sp;
        std::string serr;
        CHECK(terrain_field::SurfaceProgram::parse(mtn.surface_program, sp, serr),
              serr.c_str());
        CHECK(sp.materials.size() == 5 && sp.uses_world_inputs(),
              "StreamMountain declares 5 materials and reads world inputs");
        CHECK(sp.materials[0].handle == ground &&
                  sp.materials[1].handle == rock &&
                  sp.materials[2].handle == scree &&
                  sp.materials[3].handle == snow &&
                  sp.materials[4].handle == meadow,
              "StreamMountain weights resolve in declaration order");
        // The tape shares the register budget with every literal it names;
        // leave the headroom visible so an edit that blows it fails here.
        CHECK((int)sp.ops.size() <= terrain_field::kMaxSurfaceOps,
              "StreamMountain tape fits the op budget");

        terrain_field::FieldProgram fp;
        std::string ferr;
        CHECK(terrain_field::FieldProgram::parse(mtn.field_program, fp, ferr),
              ferr.c_str());
        terrain_field::FieldRuntime mf(std::move(fp));
        terrain_field::SurfaceRuntime rt{std::move(sp)};
        // No local_to_world: world x/z are the sample's x/z, so the field's
        // relief/moisture channels are read at the point being classified.
        terrain_field::SurfaceWorldContext wctx{&mf, nullptr};
        float w[terrain_field::kMaxSurfaceMaterials];
        const float up[3] = {0, 1, 0}, wall[3] = {1, 0.05f, 0};

        // Corners that hold for EVERY (x, z) — the noise channels are bounded,
        // so these four are properties of the tape, not of a lucky sample.
        const float valley[3] = {130, 20, -70};
        rt.weights_at(valley, up, &wctx, w);
        CHECK(w[0] > 0.99f && w[1] < 1e-6f && w[2] < 1e-6f && w[3] < 1e-6f,
              "StreamMountain: flat valley floor is alpine ground");
        const float face[3] = {130, 260, -70};
        rt.weights_at(face, wall, &wctx, w);
        CHECK(w[1] > 0.99f, "StreamMountain: steep faces are rock");
        const float summit[3] = {130, 700, -70};
        rt.weights_at(summit, up, &wctx, w);
        CHECK(w[3] > 0.99f, "StreamMountain: flat summit ground is snow");
        rt.weights_at(summit, wall, &wctx, w);
        CHECK(w[1] > 0.99f && w[3] < 1e-6f,
              "StreamMountain: snow sheds off summit walls (they stay rock)");

        // Sweep the real terrain: altitude from the field, normal tilt from the
        // field gradient. Every class must occur, and snow/ground must not
        // occur where the alps would not put them.
        int wins[4] = {0, 0, 0, 0};
        int total = 0;
        float lowest_snow = 1e9f, highest_ground = -1e9f;
        for (int ix = -24; ix <= 24; ++ix) {
            for (int iz = -24; iz <= 24; ++iz) {
                const float x = (float)ix * 50.0f, z = (float)iz * 50.0f;
                const float h = mf.height_at(x, z);
                const float g = mf.slope_at(x, z);          // |grad h|
                const float inv_len = 1.0f / std::sqrt(1.0f + g * g);
                const float pos[3] = {x, h, z};
                const float nrm[3] = {g * inv_len, inv_len, 0.0f};
                rt.weights_at(pos, nrm, &wctx, w);
                int best = 0;
                for (int k = 1; k < 4; ++k)
                    if (w[k] > w[best]) best = k;
                ++wins[best];
                ++total;
                if (best == 3 && h < lowest_snow) lowest_snow = h;
                if (best == 0 && h > highest_ground) highest_ground = h;
            }
        }
        CHECK(total == 49 * 49, "swept the whole grid");
        CHECK(wins[0] > 0 && wins[1] > 0 && wins[2] > 0 && wins[3] > 0,
              "all four alpine classes occur across the range");
        // Valleys are the biggest single class (the range is mostly below the
        // tree line at this seed) and snow the smallest — the shape the alpine
        // reference photos have.
        CHECK(wins[0] > wins[3], "ground covers more ground than snow");
        CHECK(lowest_snow > 300.0f,
              "no snow-dominant sample below 300 m (noise-broken, not random)");
        CHECK(highest_ground < 560.0f,
              "no turf-dominant sample above 560 m (stony belt takes over)");
        MaterialRegistryResetDynamic();
    }

    return check_summary();
}
