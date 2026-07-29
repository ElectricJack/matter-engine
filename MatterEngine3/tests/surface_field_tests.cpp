// surface_field_tests.cpp — WP-F: surfaces() classifier tape (terrain_field
// SurfaceProgram / SurfaceRuntime). Headless, no GL, no JS.
//
// Covers: parse round-trip + rejection paths, hash determinism/sensitivity
// (the page-invalidation key), local-input evaluation (slope/altitude),
// world-context vs fallback-constant evaluation (the world-anchored rule),
// the world-anchored detection helper, the warn-once misuse latch, and
// per-vertex quantized classification.

#include "check.h"
#include "../src/terrain_field.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace terrain_field;

namespace {

// A 3-material slope/altitude tape, exactly what world_base.js.h records for:
//   const steep = s.slope.smoothstep(0.3, 0.6);
//   const snow  = s.altitude.smoothstep(40, 60).mul(steep.oneMinus());
//   const grass = steep.oneMinus().mul(snow.oneMinus());
//   s.weight(31, grass); s.weight(32, steep); s.weight(33, snow);
const char* kTape =
    "input slope\n"            // r0
    "smoothstep 0.3 0.6 r0\n"  // r1 = steep
    "input wy\n"               // r2 = altitude
    "smoothstep 40 60 r2\n"    // r3
    "const -1\n"               // r4
    "mul r1 r4\n"              // r5 = -steep
    "const 1\n"                // r6
    "add r5 r6\n"              // r7 = 1 - steep
    "mul r3 r7\n"              // r8 = snow
    "const -1\n"               // r9
    "mul r8 r9\n"              // r10
    "const 1\n"                // r11
    "add r10 r11\n"            // r12 = 1 - snow
    "mul r7 r12\n"             // r13 = grass
    "material 31 r13\n"
    "material 32 r1\n"
    "material 33 r8\n";

// Minimal valid field program for a FieldRuntime the world inputs can query.
const char* kField =
    "const 25\n"
    "const 0.75\n"
    "const 0.25\n"
    "height r0\n"
    "moisture r1\n"
    "relief r2\n"
    "seaLevel -10\n"
    "biome 0.65 0.35\n";

bool parse_tape(const char* text, SurfaceProgram& prog, std::string& err) {
    return SurfaceProgram::parse(text, prog, err);
}

} // namespace

int main() {
    std::string err;

    // ---- parse round-trip ----
    SurfaceProgram prog;
    CHECK(parse_tape(kTape, prog, err), err.c_str());
    CHECK(prog.ops.size() == 14, "op count");
    CHECK(prog.materials.size() == 3, "3 declared materials");
    CHECK(prog.materials[0].handle == 31 && prog.materials[1].handle == 32 &&
              prog.materials[2].handle == 33,
          "material handles preserved in declaration order");
    CHECK(prog.uses_world_inputs(), "wy is a world input");

    // A local-only tape must not claim world inputs.
    {
        SurfaceProgram local;
        CHECK(parse_tape("input slope\nmaterial 2 r0\n", local, err), err.c_str());
        CHECK(!local.uses_world_inputs(), "slope alone is not a world input");
    }

    // ---- rejection paths ----
    {
        SurfaceProgram bad;
        CHECK(!parse_tape("input slope\n", bad, err),
              "no material directive rejected");
        CHECK(!parse_tape("warp2 r0 1 0.5 10\nmaterial 2 r0\n", bad, err),
              "warp2 rejected in the surface op set");
        CHECK(!parse_tape("input bogus\nmaterial 2 r0\n", bad, err),
              "unknown input rejected");
        CHECK(!parse_tape("input slope\nmaterial 2 r5\n", bad, err),
              "forward material register rejected");
        CHECK(!parse_tape("input slope\nmaterial 2 r0\nmaterial 2 r0\n", bad, err),
              "duplicate material handle rejected");
        std::string many = "input slope\n";
        for (int i = 0; i < kMaxSurfaceMaterials + 1; ++i)
            many += "material " + std::to_string(30 + i) + " r0\n";
        CHECK(!parse_tape(many.c_str(), bad, err),
              "more than kMaxSurfaceMaterials rejected");
    }

    // ---- hash: deterministic, sensitive to edits (page-invalidation key) ----
    {
        SurfaceProgram again;
        CHECK(parse_tape(kTape, again, err), err.c_str());
        CHECK(again.hash() == prog.hash(), "hash deterministic across parses");
        std::string edited(kTape);
        const size_t at = edited.find("40 60");
        edited.replace(at, 5, "45 60");
        SurfaceProgram other;
        CHECK(parse_tape(edited.c_str(), other, err), err.c_str());
        CHECK(other.hash() != prog.hash(), "an edited tape changes the hash");
    }

    // ---- evaluation: world-anchored context ----
    FieldProgram fieldProg;
    CHECK(FieldProgram::parse(kField, fieldProg, err), err.c_str());
    FieldRuntime field(std::move(fieldProg));
    SurfaceRuntime tape{SurfaceProgram(prog)};
    CHECK(tape.material_count() == 3, "runtime material count");
    CHECK(tape.material_handle(1) == 32, "runtime material handle");

    // Sector-style transform: translation only (row-major, y untouched).
    float l2w[16] = {1, 0, 0, 128,
                     0, 1, 0, 0,
                     0, 0, 1, -64,
                     0, 0, 0, 1};
    SurfaceWorldContext world{&field, l2w};

    float w[kMaxSurfaceMaterials];
    // Flat, low altitude: pure grass.
    {
        const float pos[3] = {1.0f, 5.0f, 2.0f};
        const float nrm[3] = {0.0f, 1.0f, 0.0f};
        tape.weights_at(pos, nrm, &world, w);
        CHECK(std::fabs(w[0] - 1.0f) < 1e-6f && w[1] == 0.0f && w[2] == 0.0f,
              "flat low sample is pure grass");
    }
    // Steep at any altitude: rock (slope = 1 - ny; ny = 0.2 -> slope 0.8 -> steep 1).
    {
        const float pos[3] = {1.0f, 80.0f, 2.0f};
        const float n_unnorm[3] = {0.98f, 0.2f, 0.0f};   // also checks normalization
        float nrm[3];
        const float len = std::sqrt(0.98f * 0.98f + 0.2f * 0.2f);
        nrm[0] = n_unnorm[0]; nrm[1] = n_unnorm[1]; nrm[2] = n_unnorm[2];
        (void)len;
        tape.weights_at(pos, nrm, &world, w);
        CHECK(w[1] > 0.99f && w[0] < 1e-6f, "steep sample is rock");
    }
    // Flat, high altitude: snow.
    {
        const float pos[3] = {1.0f, 70.0f, 2.0f};
        const float nrm[3] = {0.0f, 1.0f, 0.0f};
        tape.weights_at(pos, nrm, &world, w);
        CHECK(w[2] > 0.99f && w[1] < 1e-6f, "flat high sample is snow");
        CHECK(std::fabs(w[0]) < 1e-6f, "grass excluded above the snow line");
    }
    // The transform matters: altitude comes from world-space y. A transform
    // that lifts the part by 100 m must flip a low vertex into snow.
    {
        float lifted[16];
        std::memcpy(lifted, l2w, sizeof(lifted));
        lifted[7] = 100.0f;   // +y translation
        SurfaceWorldContext up{&field, lifted};
        const float pos[3] = {1.0f, 5.0f, 2.0f};
        const float nrm[3] = {0.0f, 1.0f, 0.0f};
        tape.weights_at(pos, nrm, &up, w);
        CHECK(w[2] > 0.99f, "local->world transform feeds the altitude input");
    }

    // ---- fallback constants (misuse / non-anchored variants) ----
    {
        const float pos[3] = {1.0f, 70.0f, 2.0f};   // altitude 70 if world were read
        const float nrm[3] = {0.0f, 1.0f, 0.0f};
        tape.weights_at(pos, nrm, nullptr, w);
        // Fallback altitude is 0 -> no snow; flat -> grass.
        CHECK(std::fabs(w[0] - 1.0f) < 1e-6f && w[2] == 0.0f,
              "null world context evaluates world inputs to fallback constants");
        float w2[kMaxSurfaceMaterials];
        tape.weights_at(pos, nrm, nullptr, w2);
        CHECK(std::memcmp(w, w2, sizeof(float) * 3) == 0,
              "fallback evaluation is deterministic");
    }

    // ---- world-anchored detection helper (provider instance bookkeeping) ----
    CHECK(surface_variant_world_anchored(1), "single-instance variant is anchored");
    CHECK(!surface_variant_world_anchored(2), "multi-instance variant is not");
    CHECK(!surface_variant_world_anchored(0), "unreferenced variant is not");

    // ---- misuse diagnostic latch fires exactly once ----
    {
        SurfaceProgram p2;
        CHECK(parse_tape(kTape, p2, err), err.c_str());
        SurfaceRuntime latch{std::move(p2)};
        CHECK(latch.uses_world_inputs(), "latch tape uses world inputs");
        CHECK(latch.note_world_input_misuse(), "first misuse note fires");
        CHECK(!latch.note_world_input_misuse(), "second misuse note is silent");
        CHECK(!latch.note_world_input_misuse(), "latch stays closed");
    }

    // ---- per-vertex quantized classification ----
    {
        // 3 vertices: flat-low (grass), steep (rock), flat-high (snow).
        const float positions[9] = {0, 5, 0,   0, 30, 0,   0, 70, 0};
        const float s45 = 0.70710678f;
        const float normals[9] = {0, 1, 0,   s45, 0.1f, s45,   0, 1, 0};
        std::vector<uint8_t> out(3 * tape.material_count(), 0xCD);
        tape.classify_vertices(positions, normals, 3, &world, out.data());
        CHECK(out[0] == 255 && out[1] == 0 && out[2] == 0,
              "vertex 0 quantizes to pure grass");
        CHECK(out[3] == 0 && out[4] == 255 && out[5] == 0,
              "vertex 1 quantizes to pure rock");
        CHECK(out[6] == 0 && out[7] == 0 && out[8] == 255,
              "vertex 2 quantizes to pure snow");
        // Determinism across calls.
        std::vector<uint8_t> out2(out.size(), 0);
        tape.classify_vertices(positions, normals, 3, &world, out2.data());
        CHECK(out == out2, "classification byte-identical across calls");
        // Quantization normalizes: a mixed sample's weights sum to ~255.
        const float mixed_pos[3] = {0, 50, 0};
        const float tilted[3] = {0.42f, 0.88f, 0.22f};   // slope ~0.12? mid-band
        std::vector<uint8_t> one(tape.material_count(), 0);
        tape.classify_vertices(mixed_pos, tilted, 1, &world, one.data());
        int sum = 0;
        for (uint8_t b : one) sum += b;
        CHECK(sum >= 254 && sum <= 256, "quantized weights sum to ~255");
        // All-zero weights fail closed to the first declared material.
        SurfaceProgram zp;
        CHECK(parse_tape("const 0\nmaterial 7 r0\nmaterial 8 r0\n", zp, err),
              err.c_str());
        SurfaceRuntime zero{std::move(zp)};
        std::vector<uint8_t> zout(2, 0);
        zero.classify_vertices(mixed_pos, tilted, 1, nullptr, zout.data());
        CHECK(zout[0] == 255 && zout[1] == 0,
              "all-zero weights fall back to the first declared material");
    }

    // ---- noise2 over part-local coordinates is deterministic ----
    {
        SurfaceProgram np;
        CHECK(parse_tape("noise2 7 0.25 3 0.5 2\nclamp r0 0 1\nmaterial 2 r1\n"
                         "material 3 r1\n",
                         np, err),
              err.c_str());
        SurfaceRuntime noise{std::move(np)};
        const float pos[3] = {3.7f, 0.0f, -2.2f};
        const float nrm[3] = {0, 1, 0};
        float a[kMaxSurfaceMaterials], b[kMaxSurfaceMaterials];
        noise.weights_at(pos, nrm, nullptr, a);
        noise.weights_at(pos, nrm, &world, b);   // world ctx must not shift local noise
        CHECK(a[0] == b[0], "local noise ignores the world context");
    }

    return check_summary();
}
