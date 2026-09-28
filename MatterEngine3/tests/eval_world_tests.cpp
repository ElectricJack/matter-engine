#include "matter/project_layout.h"
// MatterEngine3/tests/eval_world_tests.cpp — Task 4: eval_world + world manifest kind
#include "check.h"
#include "../src/script_host.h"
#include "../src/terrain_field.h"
#include "../src/render/vt_surface_tape.h"
#include "material_registry.h"
#include <cmath>
#include <algorithm>
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
    {
        const char* source_world = R"JS(
class DirectMaterial extends World {
  field() { return { density: heightToDensity(0), moisture: 0.5, relief: 0 }; }
  surfaces(s) {
    s.source(30, { baseColor: [s.x.mul(0.1), s.cellNoise2(4294967295, s.x.sub(2.1), s.z), s.footprint], roughness: 0.7,
                   height: s.x.mul(0.01), heightRange: [-1, 1] });
  }
}
)JS";
        auto generated = host.eval_world(source_world, "{}");
        CHECK(generated.ok, generated.message.c_str());
        terrain_field::SurfaceProgram direct;
        std::string source_error;
        CHECK(terrain_field::SurfaceProgram::parse(generated.surface_program, direct, source_error),
              source_error.c_str());
        CHECK(direct.source.version == 1, "source authoring: the complete material output is retained");
        if (direct.source.version == 1) {
            terrain_field::SurfaceRuntime runtime(direct);
            terrain_field::SurfaceSourceSample sample;
            const float p[3] = {2, 0, 0}, n[3] = {0, 1, 0};
            CHECK(runtime.source_at(p, n, nullptr, .025f, sample),
                  "source authoring: native evaluation accepts the JS recipe");
            CHECK(std::fabs(sample.albedo[0] - .2f) < 1e-6f &&
                      sample.albedo[1] == 7621890.f / 16777216.f &&
                      sample.albedo[2] == .025f && std::fabs(sample.height_m - .02f) < 1e-6f &&
                      sample.orm[0] == 1 && sample.orm[1] == .7f && sample.orm[2] == 0,
                  "source authoring: channels, defaults and physical coordinates agree");
        }
        CHECK(host.eval_world(source_world, "{}").surface_program == generated.surface_program,
              "source authoring: fresh-context generation is deterministic");
    }
    {
        const std::string world=R"JS(
class ReceiverHeight extends World {
  field() { return { density: heightToDensity(0), moisture: 0.5, relief: 0, seaLevel: 0 }; }
  surfaces(s) { s.source(30,{baseColor:[0.2,0.3,0.4],roughness:0.8,
    height:s.slope.mul(-0.02),heightRange:[-0.02,0],heightContext:'receiver'}); }
})JS";
        auto generated=host.eval_world(world,"{}");
        terrain_field::SurfaceProgram program;std::string error;
        CHECK(generated.ok,generated.message.c_str());
        CHECK(terrain_field::SurfaceProgram::parse(generated.surface_program,program,error),error.c_str());
        CHECK(program.source.version==2,"receiver height: explicit JS option reaches the native contract");
        auto invalid=world;invalid.replace(invalid.find("'receiver'"),10,"'unknown'");
        CHECK(!host.eval_world(invalid,"{}").ok,"receiver height: unknown context mode is rejected");
    }
    // Native JS authoring -> canonical scalar program -> CPU semantic oracle.
    // The reference is evaluated from the published physical blend formula.
    for (const char* operation : {"replace", "deposit", "appearance"}) {
        const std::string world = std::string("const LAYER_OP = '") + operation + "';\n" + R"JS(
class LayerMaterial extends World {
  field() { return { density: heightToDensity(0), moisture: 0.5, relief: 0 }; }
  surfaces(s) {
    const base = { baseColor: [0.2, 0.4, 0.6], roughness: 0.8,
                   height: 0.002, heightRange: [0.002, 0.002] };
    const layer = { baseColor: [0.8, 0.2, 0.1], roughness: 0.2,
                    height: 0.004, heightRange: [0.004, 0.004] };
    s.source(30, s.layer(base, layer, { coverage: s.x, operation: LAYER_OP, width: 0.008 }));
  }
}
)JS";
        auto generated = host.eval_world(world, "{}");
        CHECK(generated.ok, generated.message.c_str());
        terrain_field::SurfaceProgram program;
        std::string error;
        const bool parsed = terrain_field::SurfaceProgram::parse(generated.surface_program, program, error);
        CHECK(parsed, "layer authoring: native recipe parses");
        if (!parsed) continue;
        terrain_field::SurfaceRuntime runtime(program);
        for (float x : {-1.f, 0.f, .5f, 1.f, 2.f}) {
            const bool appearance = std::string(operation) == "appearance";
            const bool deposit = std::string(operation) == "deposit";
            const float c = std::max(0.f, std::min(1.f, x));
            float t = c + c * (1.f - c) * (deposit ? .004f : .002f) / .008f;
            t = std::max(0.f, std::min(1.f, t));
            const float weight = appearance ? c : t * t * (3.f - 2.f * t);
            const float pos[3] = {x, 0, 0};
            terrain_field::SurfaceSourceSample sample;
            runtime.source_at(pos, nullptr, nullptr, .001f, sample);
            CHECK(std::fabs(sample.albedo[0] - (.2f + .6f * weight)) < 1e-6f,
                  "layer authoring: coverage endpoints and height bias follow reference");
            CHECK(std::fabs(sample.orm[1] - std::sqrt(.64f - .60f * weight)) < 1e-6f,
                  "layer authoring: roughness blends in squared convention");
            const float height = appearance ? .002f : .002f + (deposit ? .004f : .002f) * weight;
            CHECK(std::fabs(sample.height_m - height) < 1e-6f,
                  "layer authoring: replacement, thickness and appearance-only heights agree");
        }
    }
    {
        const char* layered_world = R"JS(
class OrderedLayers extends World {
  field() { return { density: heightToDensity(0), moisture: 0.5, relief: 0 }; }
  surfaces(s) {
    let material = { baseColor: [0.2, 0.4, 0.6], roughness: 0.8,
                     height: 0.002, heightRange: [0.002, 0.002] };
    for (let i = 0; i < 10; ++i) {
      const h = 0.0001 * (i + 1);
      material = s.layer(material,
        { baseColor: [(i + 1) / 20, 0.2, 0.1], roughness: 0.2 + i * 0.03,
          height: h, heightRange: [h, h] },
        { coverage: s.x.mul((i + 1) / 10), operation: 'deposit', width: 0.008 });
    }
    s.source(30, material);
  }
}
)JS";
        const auto generated = host.eval_world(layered_world, "{}");
        CHECK(generated.ok, generated.message.c_str());
        terrain_field::SurfaceProgram program;
        vt::VtSurfaceTapePack packed;
        std::string error;
        const bool parsed = terrain_field::SurfaceProgram::parse(generated.surface_program, program, error);
        CHECK(parsed, error.c_str());
        if (parsed) {
            std::printf("ordered layer fixture: %zu operations\n", program.ops.size());
            CHECK(program.ops.size() > 96 && vt::vt_pack_surface_tape(program, false, packed),
                  "layer authoring: ten complete ordered layers compile beyond the legacy cap");
            terrain_field::SurfaceRuntime runtime(program);
            for (float x : {0.f, .25f, .75f, 1.5f}) {
                float red = .2f, rough_squared = .64f, height = .002f;
                for (int i = 0; i < 10; ++i) {
                    const float h = .0001f * (i + 1);
                    const float c = std::max(0.f, std::min(1.f, x * (i + 1) / 10.f));
                    const float t = std::max(0.f, std::min(1.f, c + c * (1.f - c) * h / .008f));
                    const float w = t * t * (3.f - 2.f * t);
                    red += ((i + 1) / 20.f - red) * w;
                    const float roughness = .2f + i * .03f;
                    rough_squared += (roughness * roughness - rough_squared) * w;
                    height += h * w;
                }
                const float p[3] = {x, 0, 0};
                terrain_field::SurfaceSourceSample sample;
                CHECK(runtime.source_at(p, nullptr, nullptr, .001f, sample) &&
                          std::fabs(sample.albedo[0] - red) < 2e-6f &&
                          std::fabs(sample.orm[1] - std::sqrt(rough_squared)) < 2e-6f &&
                          std::fabs(sample.height_m - height) < 2e-6f,
                      "layer authoring: no tail layers disappear from color, roughness or height");
            }
        }
    }
    // Independent shape/placement + blend oracle, including signed coordinates,
    // rotated axes, world translation, mip footprints and all height operations.
    for (const std::string shape : {"box", "ellipsoid"})
    for (const std::string anchor : {"local", "world"})
    for (const std::string operation : {"appearance", "deposit", "replace"}) {
        const std::string code = "const SHAPE='" + shape + "', ANCHOR='" + anchor +
            "', OP='" + operation + "';\n" + R"JS(
class BoundedPatch extends World {
  field() { return { density: heightToDensity(0), moisture: 0.5, relief: 0 }; }
  surfaces(s) {
    const base = { baseColor: [0.2, 0.4, 0.6], roughness: 0.8,
      height: 0.002, heightRange: [0.002, 0.002], occlusion: 0.6, metallic: 0 };
    const layer = { baseColor: [0.8, 0.2, 0.1], roughness: 0.2,
      height: 0.004, heightRange: [0.004, 0.004], occlusion: 0.9, metallic: 0.25 };
    s.source(30, s.splat(base, layer,
      { shape: SHAPE, anchor: ANCHOR, center: [0.2, -0.3, 0.1], halfSize: [1, 2, 0.5],
        axes: [[0,0,1], [0,1,0], [-1,0,0]], feather: 0.2, footprintScale: 1.5 },
      { operation: OP, coverage: 0.75, width: 0.008 }));
  }
}
)JS";
        const auto generated = host.eval_world(code, "{}");
        CHECK(generated.ok, generated.message.c_str());
        terrain_field::SurfaceProgram program;
        vt::VtSurfaceTapePack packed;
        std::string error;
        const bool parsed = terrain_field::SurfaceProgram::parse(generated.surface_program, program, error);
        CHECK(parsed, error.c_str());
        if (!parsed) continue;
        CHECK(vt::vt_pack_surface_tape(program, true, packed), packed.err.c_str());
        CHECK(host.eval_world(code, "{}").surface_program == generated.surface_program,
              "bounded splat: fresh authoring contexts produce identical programs");
        terrain_field::SurfaceRuntime runtime(program);
        const float matrix[16] = {1,0,0,-1, 0,1,0,.5f, 0,0,1,.25f, 0,0,0,1};
        terrain_field::SurfaceWorldContext world{nullptr, matrix};
        for (float x : {-1.3f, -.3f, .2f, .7f, 1.7f})
        for (float y : {-.3f, 1.7f})
        for (float z : {-.9f, .1f, 1.1f})
        for (float footprint : {.02f, .6f}) {
            const bool anchored = anchor == "world";
            const float dx = x + (anchored ? -1.f : 0.f) - .2f;
            const float dy = y + (anchored ? .5f : 0.f) + .3f;
            const float dz = z + (anchored ? .25f : 0.f) - .1f;
            const float q[3] = {dz, dy, -dx}, half[3] = {1,2,.5f};
            float distance;
            if (shape == "box") {
                float outside = 0, inside = -100;
                for (int i = 0; i < 3; ++i) {
                    const float d = std::fabs(q[i]) - half[i];
                    outside += std::max(d, 0.f) * std::max(d, 0.f);
                    inside = std::max(inside, d);
                }
                distance = std::sqrt(outside) + std::min(inside, 0.f);
            } else {
                distance = (std::sqrt(q[0]*q[0] + q[1]*q[1]/4 + q[2]*q[2]*4) - 1) * .5f;
            }
            const auto smooth = [](float t) {
                t = std::max(0.f, std::min(1.f, t));
                return t*t*(3-2*t);
            };
            const float coverage = .75f * smooth(.5f - distance / std::max(.2f, footprint*1.5f));
            const float w = operation == "appearance" ? coverage : smooth(coverage +
                coverage*(1-coverage)*(operation == "deposit" ? .004f : .002f)/.008f);
            const float height = .002f + (operation == "appearance" ? 0 :
                w*(operation == "deposit" ? .004f : .002f));
            const float pos[3] = {x,y,z};
            terrain_field::SurfaceSourceSample sample;
            CHECK(runtime.source_at(pos, nullptr, &world, footprint, sample) &&
                      std::fabs(sample.albedo[0] - (.2f + .6f*w)) < 3e-6f &&
                      std::fabs(sample.orm[0] - (.6f + .3f*w)) < 3e-6f &&
                      std::fabs(sample.orm[1] - std::sqrt(.64f - .6f*w)) < 3e-6f &&
                      std::fabs(sample.orm[2] - .25f*w) < 3e-6f &&
                      std::fabs(sample.height_m - height) < 3e-6f,
                  "bounded splat: anchored shape and complete material follow independent oracle");
        }
    }
    for (const char* invalid : {
        "p.halfSize[0]=0", "p.center[1]=NaN", "p.anchor='camera'", "p.shape='unknown'",
        "p.feather=0", "p.footprintScale=-1", "p.axes=[[1,0,0],[1,0,0],[0,0,1]]"}) {
        const std::string code = std::string(R"JS(
class InvalidPatch extends World {
  field() { return { density: heightToDensity(0), moisture: 0.5, relief: 0 }; }
  surfaces(s) {
    const p = {shape:'box', anchor:'local', center:[0,0,0], halfSize:[1,1,1], feather:0.1};
)JS") + invalid + "; s.coverageShape(p); } }";
        const auto generated = host.eval_world(code, "{}");
        CHECK(!generated.ok && generated.message.find("coverageShape()") != std::string::npos,
              "bounded splat: malformed placement fails at authoring with a useful error");
    }
    for (const char* scene : {"ProceduralBrickProof", "ProceduralTerrainProof"}) {
        // Match world-definition loading: field evaluation resolves handles
        // already assigned at module scope; it does not register materials.
        MaterialRegistryResetDynamic();
        MaterialDef def{};
        MaterialRegistryDefaultDynamicDef(&def);
        const char* material_name = std::string(scene) == "ProceduralBrickProof"
            ? "ProceduralProof.BrickPaint" : "ProceduralProof.RockSoilMoss";
        CHECK(MaterialRegistryDefineDynamic(&def, material_name) >= 30,
              "material proof: loader material handle registered");
        const std::string path = matter::project_layout::scene_script("../../projects/world_demo", scene).string();
        std::ifstream file(path);
        CHECK(file.good(), "material proof: scene source exists");
        if (!file.good()) continue;
        std::stringstream text; text << file.rdbuf();
        const auto generated = host.eval_world(text.str(), "{}");
        CHECK(generated.ok, generated.message.c_str());
        terrain_field::SurfaceProgram program;
        vt::VtSurfaceTapePack packed;
        std::string error;
        const bool parsed = terrain_field::SurfaceProgram::parse(generated.surface_program, program, error);
        CHECK(parsed, error.c_str());
        if (parsed) {
            CHECK(vt::vt_pack_surface_tape(program, true, packed), packed.err.c_str());
            std::printf("%s: %zu material operations\n", scene, program.ops.size());
        }
    }
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

    WorldEvalResult field_collision = host.eval_world(R"JS(
class FieldCollision extends World {
  field() { terrainCollision({ cellSize: 0.5 }); }
}
)JS", "{}");
    CHECK(!field_collision.ok &&
              field_collision.message.find("terrainCollision") != std::string::npos &&
              field_collision.message.find("field()") != std::string::npos,
          "field() terrainCollision misuse reports the active phase");

    WorldEvalResult biome_collision = host.eval_world(R"JS(
class BiomeCollision extends World {
  field() {
    const zero = blend(0, 0, 0);
    return { density: heightToDensity(zero), moisture: zero, relief: zero, seaLevel: 0 };
  }
  biomes() { terrainCollision({ cellSize: 0.5 }); }
}
)JS", "{}");
    CHECK(!biome_collision.ok &&
              biome_collision.message.find("terrainCollision") != std::string::npos &&
              biome_collision.message.find("biomes()") != std::string::npos,
          "biomes() terrainCollision misuse propagates the active-phase error");

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
        std::ifstream in("../../projects/world_demo/scenes/texturing/virtual_texture/ChartVtProof/ChartVtProof.js",
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
        std::ifstream in("../../projects/world_demo/scenes/streaming/StreamMountain/StreamMountain.js",
                         std::ios::binary);
        CHECK(bool(in), "StreamMountain.js readable from the tests directory");
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string mountain_source = ss.str();

        // StreamMountain imports its ECOLOGY (habitat(h) delegates to
        // shared-lib/alpine_ecology), so resolving imports is now part of
        // evaluating it. Without roots the import fails, the eval fails, and
        // every downstream assertion here reads an empty tape -- which surfaces
        // as an out-of-range materials[0] rather than as "the world did not
        // load", so it is worth stating why the roots are here.
        host.set_shared_lib_roots({"../../projects/world_demo/shared-lib",
                                   "../shared-lib"});

        MaterialRegistryResetDynamic();
        MaterialDef def{};
        MaterialRegistryDefaultDynamicDef(&def);
        const int ground = MaterialRegistryDefineDynamic(&def, "AlpineGround");
        const int rock = MaterialRegistryDefineDynamic(&def, "AlpineRock");
        const int scree = MaterialRegistryDefineDynamic(&def, "Scree");
        const int snow = MaterialRegistryDefineDynamic(&def, "AlpineSnow");
        const int meadow = MaterialRegistryDefineDynamic(&def, "AlpineMeadow");
        // The shipped world now also declares the forest's materials while
        // loading its module; mirror those loader assignments for eval_world.
        for (const char* name : {"mountain.forest.bark", "mountain.forest.branch",
                                 "mountain.forest.needles", "mountain.forest.cone",
                                 "mountain.forest.redwood"})
            CHECK(MaterialRegistryDefineDynamic(&def, name) >= 30,
                  "dynamic forest material registered for StreamMountain evaluation");
        CHECK(ground >= 30 && rock > ground && scree > rock && snow > scree &&
                  meadow > snow,
              "dynamic alpine materials registered");

        WorldEvalResult mtn = host.eval_world(mountain_source, "{}");
        CHECK(mtn.ok, mtn.message.c_str());
        // Report the world-load failure instead of dereferencing an empty
        // material vector and losing the diagnostic to an access violation.
        if (!mtn.ok) return check_summary();
        CHECK(!mtn.surface_program.empty(), "StreamMountain records a tape");
        terrain_field::SurfaceProgram sp;
        std::string serr;
        CHECK(terrain_field::SurfaceProgram::parse(mtn.surface_program, sp, serr),
              serr.c_str());
        CHECK(sp.source.version==2 && sp.materials.size()==1 && sp.uses_world_inputs(),
              "StreamMountain exposes one coherent receiver-context source");
        if(sp.materials.empty())return check_summary();
        CHECK(sp.materials[0].handle==ground,"StreamMountain retains its ground fallback carrier");
        vt::VtSurfaceTapePack packed;
        CHECK(vt::vt_pack_surface_tape(sp,true,packed),"StreamMountain fits actual GPU op/register budgets");
        terrain_field::FieldProgram fp;std::string ferr;
        CHECK(terrain_field::FieldProgram::parse(mtn.field_program,fp,ferr),ferr.c_str());
        terrain_field::FieldRuntime mf(std::move(fp));
        terrain_field::SurfaceRuntime rt{std::move(sp)};
        terrain_field::SurfaceWorldContext wctx{&mf,nullptr};
        const float up[]={0.f,1.f,0.f},wall[]={1.f,0.f,0.f};
        const float valley[]={130.f,20.f,-70.f},face[]={130.f,260.f,-70.f},summit[]={130.f,700.f,-70.f};
        terrain_field::SurfaceSourceSample ground_sample,rock_sample,snow_sample,wall_sample;
        CHECK(rt.source_at(valley,up,&wctx,1.f/128,ground_sample) &&
              rt.source_at(face,wall,&wctx,1.f/128,rock_sample) &&
              rt.source_at(summit,up,&wctx,1.f/128,snow_sample) &&
              rt.source_at(summit,wall,&wctx,1.f/128,wall_sample),
              "StreamMountain evaluates the authored physical materials");
        CHECK(ground_sample.albedo[0]<.3f && ground_sample.orm[1]>.65f,
              "StreamMountain valley remains rough earth/mineral");
        CHECK(rock_sample.orm[1]>.85f && rock_sample.orm[1]<.91f,
              "StreamMountain steep faces select bedrock roughness");
        CHECK(*std::min_element(snow_sample.albedo,snow_sample.albedo+3)>.65f,
              "StreamMountain flat summit carries bright snow");
        CHECK(*std::max_element(wall_sample.albedo,wall_sample.albedo+3)<.3f,
              "StreamMountain snow sheds from summit walls");
        int total=0;float min_red=1,max_red=0;bool bounded=true,carrier=true,snow_altitude=true;
        for(int ix=-24;ix<=24;++ix)for(int iz=-24;iz<=24;++iz) {
            const float x=float(ix)*50,z=float(iz)*50,alt=mf.height_at(x,z),g=mf.slope_at(x,z);
            const float inv=1/std::sqrt(1+g*g),pos[]={x,alt,z},normal[]={g*inv,inv,0};
            terrain_field::SurfaceSourceSample value;float weight=0;
            bounded &= rt.source_at(pos,normal,&wctx,1.f/128,value);
            rt.weights_at(pos,normal,&wctx,&weight);carrier &= weight==1;
            for(float c:value.albedo)bounded &= std::isfinite(c)&&c>=0&&c<=1;
            bounded &= value.height_m>=-.09f && value.height_m<=0 && value.orm[1]>=.65f;
            min_red=std::min(min_red,value.albedo[0]);max_red=std::max(max_red,value.albedo[0]);
            if(value.albedo[0]>.6f)snow_altitude &= alt>300;
            ++total;
        }
        CHECK(total==49*49 && bounded && carrier,"StreamMountain grid retains finite channels, relief bounds and one carrier");
        CHECK(max_red-min_red>.01f,"StreamMountain material varies across the range");
        CHECK(snow_altitude,"StreamMountain snow-dominant samples stay above the lowlands");
        MaterialRegistryResetDynamic();
    }

    // =======================================================================
    // habitat() tape round-trip (docs/habitat-tape-sketch-2026-08-08.md)
    //
    // Same record/readback/compile shape as surfaces() above, and the point of
    // it: an ECOLOGY expressed as data, so the engine can evaluate it natively
    // without learning what a forest is.
    // =======================================================================
    {
        // A world with no habitat() emits no tape -- opt-in, unlike surfaces()
        // whose absence is also fine but whose presence must declare weights.
        CHECK(r.habitat_program.empty(),
              "no habitat() => empty habitat program");
        CHECK(r.habitat_channels.empty(), "and no channel names");
    }
    static const char* kHabWorld = R"JS(
class HabWorld extends World {
  static params = { worldSeed: 7 };
  field(p) {
    const height = noise2(p.worldSeed ^ 3, 1/160, 4).mul(30);
    const zero = blend(0.0, 0.0, 0.0);
    return { density: heightToDensity(height), moisture: zero, relief: zero,
             seaLevel: 0.0 };
  }
  habitat(h) {
    const moisture = h.value(0.18)
      .add(h.noise2World(11, 1 / 300).mul(0.62))
      .add(h.noise2World(17, 1 / 55).mul(0.20))
      .clamp(0, 1);
    const forestSignal = h.noise2World(31, 1 / 520, 4).mul(0.72)
      .add(h.noise2World(37, 1 / 140).mul(0.23));
    // dryness reads the TERRAIN inputs -- the two the scatter planner calls
    // heightAt/slopeAt for today, folded into the same evaluation.
    const dryness = moisture.oneMinus().mul(0.45)
      .add(h.height.sub(100).mul(1 / 420).clamp(0, 1).mul(0.20))
      .add(h.fieldSlope.mul(1 / 0.8).clamp(0, 1).mul(0.10))
      .clamp(0, 1);
    h.channel('moisture', moisture);
    h.channel('forest', forestSignal.smoothstep(0.40, 0.61));
    h.channel('dryness', dryness);
  }
}
)JS";
    {
        WorldEvalResult rh = host.eval_world(kHabWorld, "{}");
        CHECK(rh.ok, rh.message.c_str());
        CHECK(!rh.habitat_program.empty(), "habitat() emits a tape");
        CHECK(rh.surface_program.empty(),
              "a habitat tape is not mistaken for a surfaces tape");
        CHECK(rh.habitat_program.find("noise2w") != std::string::npos,
              "world noise reached the habitat tape");
        CHECK(rh.field_program.find("channel ") == std::string::npos,
              "channel directives do not leak into the field program");

        // Channel NAMES are the ecology's contract, in declaration order.
        CHECK(rh.habitat_channels.size() == 3, "three channels reported");
        CHECK(rh.habitat_channels[0] == "moisture" &&
                  rh.habitat_channels[1] == "forest" &&
                  rh.habitat_channels[2] == "dryness",
              "channel names come back in declaration order");

        terrain_field::SurfaceProgram hp;
        std::string herr;
        CHECK(terrain_field::SurfaceProgram::parse(
                  rh.habitat_program, hp, herr,
                  terrain_field::TapeMode::Habitat), herr.c_str());
        CHECK(hp.channel_count == 3, "3 declared channels survive readback");

        // And it evaluates: bound to the world's own field, every channel is a
        // finite [0,1] value that varies across the world. A tape that pinned
        // to the origin -- the failure the surfaces fallback has by design --
        // would show up as no variation.
        terrain_field::FieldProgram fp;
        std::string ferr;
        CHECK(terrain_field::FieldProgram::parse(rh.field_program, fp, ferr),
              ferr.c_str());
        terrain_field::FieldRuntime field(std::move(fp));
        terrain_field::SurfaceRuntime hrt(std::move(hp));
        float ch[terrain_field::kMaxHabitatChannels] = {};
        float first[3] = {-1, -1, -1};
        bool in_range = true, varies = false;
        for (int i = 0; i < 40; ++i) {
            hrt.channels_at(float(i) * 53.0f, float(i) * -31.0f, &field, ch);
            for (int c = 0; c < 3; ++c) {
                if (!(ch[c] >= 0.0f && ch[c] <= 1.0f)) in_range = false;
                if (first[c] < 0) first[c] = ch[c];
                else if (std::fabs(ch[c] - first[c]) > 1e-4f) varies = true;
            }
        }
        CHECK(in_range, "every habitat channel stays in [0,1]");
        CHECK(varies, "habitat channels vary across the world");

        // Seed wiring: a habitat tape is part of the world's program, so a
        // different world seed must not silently produce the same ecology when
        // the author threads the seed through (this one hardcodes its seeds, so
        // the tape is deliberately seed-INdependent -- assert that rather than
        // leave it ambiguous).
        WorldEvalResult rh2 = host.eval_world(kHabWorld, "{\"worldSeed\":99}");
        CHECK(rh2.ok, rh2.message.c_str());
        CHECK(rh2.habitat_program == rh.habitat_program,
              "a habitat tape with literal seeds is seed-independent, as "
              "written");
    }

    // --- the two tapes coexist without renumbering each other --------------
    // Register numbers are positions WITHIN a tape. If both recorded into one
    // array, the second tape's refs would point into the first's ops -- so a
    // world declaring both is the case that catches it.
    {
        static const char* kBoth = R"JS(
class BothWorld extends World {
  static params = { worldSeed: 5 };
  field(p) {
    const height = noise2(p.worldSeed ^ 3, 1/160, 4).mul(30);
    const zero = blend(0.0, 0.0, 0.0);
    return { density: heightToDensity(height), moisture: zero, relief: zero,
             seaLevel: 0.0 };
  }
  surfaces(s) {
    const steep = s.slope.smoothstep(0.3, 0.6);
    s.weight(31, steep);
    s.weight(32, steep.oneMinus());
  }
  habitat(h) {
    h.channel('forest', h.noise2World(31, 1 / 520, 4).smoothstep(0.4, 0.61));
  }
}
)JS";
        MaterialRegistryResetDynamic();
        WorldEvalResult rb = host.eval_world(kBoth, "{}");
        CHECK(rb.ok, rb.message.c_str());
        CHECK(!rb.surface_program.empty() && !rb.habitat_program.empty(),
              "a world may declare both tapes");
        terrain_field::SurfaceProgram sp, hp;
        std::string e1, e2;
        CHECK(terrain_field::SurfaceProgram::parse(rb.surface_program, sp, e1),
              e1.c_str());
        CHECK(terrain_field::SurfaceProgram::parse(
                  rb.habitat_program, hp, e2,
                  terrain_field::TapeMode::Habitat), e2.c_str());
        CHECK(sp.materials.size() == 2 && hp.channel_count == 1,
              "each tape kept its own outputs");
        CHECK(rb.habitat_program.find("slope") == std::string::npos,
              "the surfaces tape's ops did not leak into the habitat tape");
        MaterialRegistryResetDynamic();
    }

    // =======================================================================
    // 3D DENSITY: the no-re-bake guarantee, and the volumetric path.
    //
    // The field program is a function of (x, y, z). A heightfield is the
    // density `h(x, z) - y`, and heightToDensity() is LAZY precisely so that a
    // world which never touches it emits no ops for the subtraction: the
    // canonical text -- whose hash gates sector re-bakes -- comes out byte for
    // byte what it was before the field program became 3D.
    //
    // This block is that guarantee. If it fails, every streamed world in the
    // project re-bakes on the next load.
    // =======================================================================
    {
        ScriptHost h2;
        WorldEvalResult r2 = h2.eval_world(kWorld, "{}");
        CHECK(r2.ok, r2.message.c_str());
        CHECK(r2.field_program.find("input wy") == std::string::npos,
              "a heightfield world emits NO 'input wy' op");
        CHECK(r2.field_program.find("\ndensity ") == std::string::npos,
              "a heightfield world emits NO 'density' directive");
        // Byte-identity, pinned. The `height r6` line is what proves the
        // directive still names the surface register and not something the
        // laziness shifted.
        const char* kExpected =
            "noise2 43 0.0011111111111111111 3 0.5 2\n"
            "noise2 41 0.00625 4 0.5 2\n"
            "const 8\n"
            "mul r1 r2\n"
            "ridge2 46 0.0029411764705882353 5 0.5 2\n"
            "const 110\n"
            "mul r4 r5\n"
            "smoothstep 0.45 0.75 r0\n"
            "blend r3 r6 r7\n"
            "const -6\n"
            "add r8 r9\n"
            "noise2 40 0.0014285714285714286 3 0.5 2\n"
            "height r10\n"
            "moisture r11\n"
            "relief r0\n"
            "seaLevel 0\n"
            "biome 0.65 0.35\n";
        CHECK(r2.field_program == kExpected,
              "the heightfield canonical text is byte-identical -- no world "
              "re-bakes because the field program became 3D");

        terrain_field::FieldProgram hp2; std::string he;
        CHECK(terrain_field::FieldProgram::parse(r2.field_program, hp2, he),
              he.c_str());
        CHECK(hp2.is_heightfield, "and it parses back as a heightfield");
    }

    // --- a volumetric world round-trips -------------------------------------
    // heightToDensity(h).min(caves): the node materialises `h - y` on the first
    // operator, the host emits both directives, and the parser reports a
    // volumetric program whose surface height is still the authored one.
    {
        static const char* kCaveWorld = R"JS(
class CaveWorld extends World {
  static params = { worldSeed: 7 };
  static world  = { sectorSize: 64, yMin: -512, yMax: 128 };
  field(p) {
    const height = noise2(p.worldSeed ^ 1, 1/400, 3).mul(30).add(60);
    const caves  = ridge3(p.worldSeed ^ 2, 1/90, 2, 0.5, 2.0)
                     .sub(0.55).mul(-1);
    const floor  = worldY().sub(-500).mul(-1);
    const inert  = blend(0.0, 0.0, 0.0);
    return {
      density: heightToDensity(height).min(caves).max(floor),
      moisture: inert, relief: inert, seaLevel: -1000.0,
    };
  }
  biomes() { return { foothills: {}, ocean: {} }; }
}
)JS";
        ScriptHost h3;
        WorldEvalResult r3 = h3.eval_world(kCaveWorld, "{}");
        CHECK(r3.ok, r3.message.c_str());
        CHECK(r3.field_program.find("input wy") != std::string::npos,
              "a volumetric world emits 'input wy'");
        CHECK(r3.field_program.find("ridge3 ") != std::string::npos,
              "and its 3D noise op");
        CHECK(r3.field_program.find("\ndensity r") != std::string::npos,
              "and the 'density' directive that distinguishes it from height");

        terrain_field::FieldProgram cp; std::string ce;
        CHECK(terrain_field::FieldProgram::parse(r3.field_program, cp, ce),
              ce.c_str());
        CHECK(!cp.is_heightfield, "it parses as volumetric");
        CHECK(cp.height_reg != cp.density_reg,
              "height and density are different registers");
        CHECK(!cp.y_dependent[cp.height_reg] && cp.y_dependent[cp.density_reg],
              "the surface is y-independent and the density is not");

        terrain_field::FieldRuntime cf(std::move(cp));
        // The surface still reads as the authored 30..90 m, and there is air
        // under it somewhere -- otherwise the caves carved nothing.
        const float hgt = cf.height_at(100, 100);
        CHECK(hgt > 20.0f && hgt < 100.0f, "authored surface height survives");
        bool any_air = false, floor_solid = true;
        for (int i = 0; i < 200 && (!any_air || floor_solid); ++i) {
            const float x = float(i * 13), z = float(i * -7);
            for (float y = 0; y > -480.0f; y -= 4.0f)
                if (cf.density_at(x, y, z) <= 0) { any_air = true; break; }
            if (cf.density_at(x, -505.0f, z) <= 0) floor_solid = false;
        }
        CHECK(any_air, "the cave field actually carves air below the surface");
        CHECK(floor_solid, "and the authored floor is solid below -500");

        // Determinism, the same property the heightfield path has.
        WorldEvalResult r3b = h3.eval_world(kCaveWorld, "{}");
        CHECK(r3b.field_program == r3.field_program,
              "volumetric program deterministic");
    }

    return check_summary();
}
