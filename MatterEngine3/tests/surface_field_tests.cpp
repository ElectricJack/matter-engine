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

// A field with actual spatial variation, for fslope / curv / world-noise
// coverage (the constant kField has zero gradient everywhere).
const char* kNoiseField =
    "noise2 7 0.01 3 0.5 2\n"   // r0
    "const 60\n"                // r1
    "mul r0 r1\n"               // r2 = height, ~[-60, 60]
    "const 0.5\n"               // r3
    "height r2\n"
    "moisture r3\n"
    "relief r3\n"
    "seaLevel -100\n"
    "biome 2 2\n";

bool parse_tape(const char* text, SurfaceProgram& prog, std::string& err) {
    return SurfaceProgram::parse(text, prog, err);
}

} // namespace

int main() {
    std::string err;

    // ---- parse round-trip ----
    SurfaceProgram prog;
    CHECK(parse_tape(kTape, prog, err), err.c_str());
    // 14 source lines; the duplicate `const -1` / `const 1` pairs dedup to one
    // emitted op each (refs are remapped), so 12 ops carry the register budget.
    CHECK(prog.ops.size() == 12, "op count (const-dedup collapses 2 dups)");
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

    // ---- const-dedup: duplicates cost no register budget, refs remap ----
    {
        SurfaceProgram dp;
        CHECK(parse_tape("const 2\nconst 2\nadd r0 r1\nmaterial 2 r2\n", dp, err),
              err.c_str());
        CHECK(dp.ops.size() == 2, "duplicate const collapses to one op");
        SurfaceRuntime rt{std::move(dp)};
        const float pos[3] = {0, 0, 0}, up[3] = {0, 1, 0};
        float w[kMaxSurfaceMaterials];
        rt.weights_at(pos, up, nullptr, w);
        CHECK(std::fabs(w[0] - 4.0f) < 1e-6f, "remapped refs evaluate correctly");
        // 80 source lines of the same const parse fine — the 64-op cap applies
        // to EMITTED ops — while 65 distinct consts still trip it.
        std::string big;
        for (int i = 0; i < 80; ++i) big += "const 1\n";
        big += "material 2 r79\n";
        SurfaceProgram bp;
        CHECK(parse_tape(big.c_str(), bp, err), err.c_str());
        CHECK(bp.ops.size() == 1, "80 identical consts dedup to one op");
        std::string over;
        for (int i = 0; i < kMaxSurfaceOps + 1; ++i)
            over += "const " + std::to_string(i) + "\n";
        over += "material 2 r0\n";
        SurfaceProgram op_;
        CHECK(!parse_tape(over.c_str(), op_, err),
              "distinct ops past the cap rejected");
    }

    // ---- sub / abs / oneminus / pow ----
    {
        SurfaceProgram ap;
        CHECK(parse_tape("input wy\n"      // r0 = 70 under `world`
                         "const 100\n"     // r1
                         "sub r0 r1\n"     // r2 = -30
                         "abs r2\n"        // r3 = 30
                         "oneminus r3\n"   // r4 = -29
                         "abs r4\n"        // r5 = 29
                         "pow r5 2\n"      // r6 = 841
                         "material 2 r6\n",
                         ap, err),
              err.c_str());
        SurfaceRuntime rt{std::move(ap)};
        const float pos[3] = {1, 70, 2}, up[3] = {0, 1, 0};
        float w[kMaxSurfaceMaterials];
        rt.weights_at(pos, up, &world, w);
        CHECK(std::fabs(w[0] - 841.0f) < 1e-3f, "sub/abs/oneminus/pow chain");
        // pow clamps its base to >= 0 (fractional exponents stay total).
        SurfaceProgram pp;
        CHECK(parse_tape("const -2\npow r0 0.5\nmaterial 2 r1\n", pp, err),
              err.c_str());
        SurfaceRuntime prt{std::move(pp)};
        prt.weights_at(pos, up, nullptr, w);
        CHECK(w[0] == 0.0f, "pow clamps negative bases to 0");
    }

    // ---- noise2w: world-frame noise is continuous across sector variants ----
    {
        // weight = noise2w + 2 (kept positive so the >= 0 weight clamp never
        // masks a comparison below).
        const char* kWorldNoiseTape =
            "noise2w 7 0.25 3 0.5 2\nconst 2\nadd r0 r1\nmaterial 2 r2\n";
        SurfaceProgram np;
        CHECK(parse_tape(kWorldNoiseTape, np, err), err.c_str());
        CHECK(np.uses_world_inputs(), "noise2w marks the tape world-dependent");
        SurfaceRuntime wn{std::move(np)};
        const float up[3] = {0, 1, 0};
        float a[kMaxSurfaceMaterials], b[kMaxSurfaceMaterials];
        // Two sector transforms 64 m apart in x (l2w is +128; B is +192).
        float l2wB[16];
        std::memcpy(l2wB, l2w, sizeof(l2wB));
        l2wB[3] = 192.0f;
        SurfaceWorldContext worldB{&field, l2wB};
        const float pos[3] = {1.0f, 0.0f, 2.0f};
        wn.weights_at(pos, up, &world, a);
        wn.weights_at(pos, up, &worldB, b);
        CHECK(a[0] != b[0], "same local position, different sector: different value");
        // A local position compensating the 64 m delta lands on the same world
        // point and must reproduce sector A's value exactly.
        const float posComp[3] = {1.0f - 64.0f, 0.0f, 2.0f};
        wn.weights_at(posComp, up, &worldB, b);
        CHECK(a[0] == b[0], "same world position across sectors: same value");
        // Under an identity transform world noise matches local noise (one fbm
        // core), and a null context pins it to world (0, 0) — a constant.
        float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        SurfaceWorldContext identCtx{&field, ident};
        SurfaceProgram lp;
        CHECK(parse_tape("noise2 7 0.25 3 0.5 2\nconst 2\nadd r0 r1\n"
                         "material 2 r2\n",
                         lp, err),
              err.c_str());
        SurfaceRuntime ln{std::move(lp)};
        wn.weights_at(pos, up, &identCtx, a);
        ln.weights_at(pos, up, nullptr, b);
        CHECK(a[0] == b[0], "identity transform: world noise == local noise");
        wn.weights_at(pos, up, nullptr, a);
        wn.weights_at(posComp, up, nullptr, b);
        CHECK(a[0] == b[0], "null context pins world noise to a constant");
    }

    // ---- fslope: rung-independent slope from the terrain field ----
    {
        FieldProgram nfp;
        CHECK(FieldProgram::parse(kNoiseField, nfp, err), err.c_str());
        FieldRuntime nfield(std::move(nfp));
        SurfaceProgram sp;
        CHECK(parse_tape("input fslope\nmaterial 2 r0\n", sp, err), err.c_str());
        CHECK(sp.uses_world_inputs(), "fslope is a world input");
        SurfaceRuntime rt{std::move(sp)};
        SurfaceWorldContext wctx{&nfield, l2w};
        const float pos[3] = {3.0f, 10.0f, -7.0f}, up[3] = {0, 1, 0};
        float w[kMaxSurfaceMaterials];
        rt.weights_at(pos, up, &wctx, w);
        // The mesh normal is straight up (mesh slope 0); fieldSlope still reads
        // the field gradient at world (3+128, -7-64).
        CHECK(std::fabs(w[0] - nfield.slope_at(131.0f, -71.0f)) < 1e-6f,
              "fslope == FieldRuntime::slope_at at the sample's world (x, z)");
        CHECK(w[0] > 0.0f, "noise field has nonzero gradient here");
        rt.weights_at(pos, up, nullptr, w);
        CHECK(w[0] == 0.0f, "fslope falls back to 0 without a world context");
    }

    // ---- curv: field curvature (concave collection zones vs convex crests) ----
    {
        FieldProgram nfp;
        CHECK(FieldProgram::parse(kNoiseField, nfp, err), err.c_str());
        FieldRuntime nfield(std::move(nfp));
        // weight = curv(8) + 5: the offset keeps convex (negative) samples
        // visible through the >= 0 weight clamp.
        SurfaceProgram cp;
        CHECK(parse_tape("curv 8\nconst 5\nadd r0 r1\nmaterial 2 r2\n", cp, err),
              err.c_str());
        CHECK(cp.uses_world_inputs(), "curv marks the tape world-dependent");
        SurfaceRuntime rt{std::move(cp)};
        SurfaceWorldContext wctx{&nfield, l2w};
        const float pos[3] = {3.0f, 10.0f, -7.0f}, up[3] = {0, 1, 0};
        float w[kMaxSurfaceMaterials];
        rt.weights_at(pos, up, &wctx, w);
        const float wx = 131.0f, wz = -71.0f;
        CHECK(std::fabs(w[0] - (5.0f + nfield.curvature_at(wx, wz, 8.0f))) < 1e-4f,
              "curv == FieldRuntime::curvature_at at the sample's world (x, z)");
        // curvature_at is exactly the ring-average height deficit…
        const float manual =
            (nfield.height_at(wx + 8, wz) + nfield.height_at(wx - 8, wz) +
             nfield.height_at(wx, wz + 8) + nfield.height_at(wx, wz - 8)) * 0.25f -
            nfield.height_at(wx, wz);
        CHECK(std::fabs(nfield.curvature_at(wx, wz, 8.0f) - manual) < 1e-6f,
              "curvature_at matches the manual ring average");
        // …zero on a constant field, and 0 without a world context.
        CHECK(field.curvature_at(10.0f, 20.0f, 4.0f) == 0.0f,
              "constant field has zero curvature");
        rt.weights_at(pos, up, nullptr, w);
        CHECK(std::fabs(w[0] - 5.0f) < 1e-6f,
              "curv falls back to 0 without a world context");
    }

    // ---- 3D noise ops (texel-tape P1): parse + input implication ----
    {
        // Part-local 3D noise claims no world inputs; the world pair implies
        // worldX/altitude/worldZ (keeps the misuse diagnostic honest).
        SurfaceProgram lp;
        CHECK(parse_tape("noise3 7 0.25 3 0.5 2\nclamp r0 0 1\nmaterial 2 r1\n",
                         lp, err),
              err.c_str());
        CHECK(!lp.uses_world_inputs(), "noise3 is not a world input");
        constexpr uint32_t kWorld3Mask = (1u << kSurfInWorldX) |
                                         (1u << kSurfInAltitude) |
                                         (1u << kSurfInWorldZ);
        SurfaceProgram wp;
        CHECK(parse_tape("noise3w 7 0.25 3 0.5 2\nmaterial 2 r0\n", wp, err),
              err.c_str());
        CHECK(wp.uses_world_inputs(), "noise3w marks the tape world-dependent");
        CHECK((wp.input_mask() & kWorld3Mask) == kWorld3Mask,
              "noise3w implies worldX/altitude/worldZ in the input mask");
        SurfaceProgram rp;
        CHECK(parse_tape("ridge3w 7 0.25 3 0.5 2 5 0.11 6\nmaterial 2 r0\n",
                         rp, err),
              err.c_str());
        CHECK(rp.uses_world_inputs() && (rp.input_mask() & kWorld3Mask) == kWorld3Mask,
              "ridge3w (warped) implies worldX/altitude/worldZ too");

        // Warp tail is all-or-nothing: exactly [wseed wfreq wamp].
        SurfaceProgram bad;
        CHECK(!parse_tape("noise3 7 0.25 3 0.5 2 5\nmaterial 2 r0\n", bad, err),
              "1-token warp tail rejected");
        CHECK(!parse_tape("noise3 7 0.25 3 0.5 2 5 0.11\nmaterial 2 r0\n",
                          bad, err),
              "2-token warp tail rejected");
        CHECK(!parse_tape("ridge3w 7 0.25 3 0.5 2 5 0.11 6 9\nmaterial 2 r0\n",
                          bad, err),
              "4-token warp tail rejected");
        CHECK(!parse_tape("fract r0\nmaterial 2 r0\n", bad, err),
              "fract self/forward register ref rejected");

        // The tape-only 3D ops must NOT parse as field ops.
        FieldProgram fbad;
        CHECK(!FieldProgram::parse("noise3 7 0.25 3 0.5 2\nheight r0\n"
                                   "moisture r0\nrelief r0\nseaLevel 0\n"
                                   "biome 0.65 0.35\n",
                                   fbad, err),
              "FieldProgram rejects noise3");
        CHECK(!FieldProgram::parse("const 1\nfract r0\nheight r0\nmoisture r0\n"
                                   "relief r0\nseaLevel 0\nbiome 0.65 0.35\n",
                                   fbad, err),
              "FieldProgram rejects fract");
    }

    // ---- 3D noise: hash — a warp tail is a different tape ----
    {
        const char* plain =
            "noise3 7 0.25 3 0.5 2\nconst 2\nadd r0 r1\nmaterial 2 r2\n";
        const char* warped =
            "noise3 7 0.25 3 0.5 2 5 0.11 6\nconst 2\nadd r0 r1\nmaterial 2 r2\n";
        SurfaceProgram a, b, c;
        CHECK(parse_tape(plain, a, err), err.c_str());
        CHECK(parse_tape(plain, b, err), err.c_str());
        CHECK(parse_tape(warped, c, err), err.c_str());
        CHECK(a.hash() == b.hash(), "same 3D-noise text parses to the same hash");
        CHECK(a.hash() != c.hash(), "adding a warp tail changes the hash");
    }

    // ---- 3D noise: golden vectors pinned from the shipped implementation ----
    // Exact floats (%.9g round-trips binary32): any drift in hash3i/
    // value_noise3/fbm3/the warp displacement changes these, and that must be
    // a deliberate page-invalidating decision, not an accident. Every tape
    // adds 2 so the >= 0 weight clamp never masks a comparison.
    {
        const float pos[3] = {3.7f, 1.5f, -2.2f}, up[3] = {0, 1, 0};
        float w[kMaxSurfaceMaterials], w2[kMaxSurfaceMaterials];

        SurfaceProgram np;
        CHECK(parse_tape("noise3 7 0.25 3 0.5 2\nconst 2\nadd r0 r1\n"
                         "material 2 r2\n",
                         np, err),
              err.c_str());
        SurfaceRuntime nrt{std::move(np)};
        nrt.weights_at(pos, up, nullptr, w);
        CHECK(w[0] == 1.79261982f, "noise3 golden value");
        nrt.weights_at(pos, up, &world, w2);
        CHECK(w[0] == w2[0], "local 3D noise ignores the world context");

        SurfaceProgram rp;
        CHECK(parse_tape("ridge3 7 0.25 3 0.5 2\nconst 2\nadd r0 r1\n"
                         "material 2 r2\n",
                         rp, err),
              err.c_str());
        SurfaceRuntime rrt{std::move(rp)};
        rrt.weights_at(pos, up, nullptr, w);
        CHECK(w[0] == 2.52229452f, "ridge3 golden value");

        // Warp on/off: warped != unwarped, both bit-stable across calls.
        SurfaceProgram wp;
        CHECK(parse_tape("noise3 7 0.25 3 0.5 2 5 0.11 6\nconst 2\nadd r0 r1\n"
                         "material 2 r2\n",
                         wp, err),
              err.c_str());
        SurfaceRuntime wrt{std::move(wp)};
        wrt.weights_at(pos, up, nullptr, w);
        CHECK(w[0] == 1.7238971f, "warped noise3 golden value");
        CHECK(w[0] != 1.79261982f, "warp displaces the sample point");
        wrt.weights_at(pos, up, nullptr, w2);
        CHECK(w[0] == w2[0], "warped noise3 deterministic across calls");

        // fract: x - floor(x), including the negative branch.
        SurfaceProgram fp;
        CHECK(parse_tape("const 3.7\nfract r0\nmaterial 2 r1\n", fp, err),
              err.c_str());
        SurfaceRuntime frt{std::move(fp)};
        frt.weights_at(pos, up, nullptr, w);
        CHECK(w[0] == 0.700000048f, "fract golden value (3.7f -> frac part)");
        SurfaceProgram fnp;
        CHECK(parse_tape("const -1.25\nfract r0\nmaterial 2 r1\n", fnp, err),
              err.c_str());
        SurfaceRuntime fnrt{std::move(fnp)};
        fnrt.weights_at(pos, up, nullptr, w);
        CHECK(w[0] == 0.75f, "fract of a negative wraps up (-1.25 -> 0.75)");
    }

    // ---- noise3w: world-frame 3D noise, altitude axis, fallback constant ----
    {
        const char* kTape3w =
            "noise3w 7 0.25 3 0.5 2\nconst 2\nadd r0 r1\nmaterial 2 r2\n";
        SurfaceProgram np;
        CHECK(parse_tape(kTape3w, np, err), err.c_str());
        SurfaceRuntime wn{std::move(np)};
        const float pos[3] = {3.7f, 1.5f, -2.2f}, up[3] = {0, 1, 0};
        float a[kMaxSurfaceMaterials], b[kMaxSurfaceMaterials];
        wn.weights_at(pos, up, &world, a);
        CHECK(a[0] == 1.93807423f, "noise3w golden value under the sector transform");
        // The altitude axis participates: lifting the part changes the value.
        float lifted[16];
        std::memcpy(lifted, l2w, sizeof(lifted));
        lifted[7] = 100.0f;
        SurfaceWorldContext upCtx{&field, lifted};
        wn.weights_at(pos, up, &upCtx, b);
        CHECK(a[0] != b[0], "world Y feeds the 3D sample point");
        // Continuity across sectors: compensating the transform delta lands on
        // the same world point and reproduces the value exactly.
        float l2wB[16];
        std::memcpy(l2wB, l2w, sizeof(l2wB));
        l2wB[3] = 192.0f;
        SurfaceWorldContext worldB{&field, l2wB};
        wn.weights_at(pos, up, &worldB, b);
        CHECK(a[0] != b[0], "same local position, different sector: different value");
        const float posComp[3] = {3.7f - 64.0f, 1.5f, -2.2f};
        wn.weights_at(posComp, up, &worldB, b);
        CHECK(a[0] == b[0], "same world position across sectors: same value");
        // Identity transform: world 3D noise == local 3D noise (one fbm core).
        float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        SurfaceWorldContext identCtx{&field, ident};
        SurfaceProgram lp;
        CHECK(parse_tape("noise3 7 0.25 3 0.5 2\nconst 2\nadd r0 r1\n"
                         "material 2 r2\n",
                         lp, err),
              err.c_str());
        SurfaceRuntime ln{std::move(lp)};
        wn.weights_at(pos, up, &identCtx, a);
        ln.weights_at(pos, up, nullptr, b);
        CHECK(a[0] == b[0], "identity transform: world 3D noise == local 3D noise");
        // Null context pins the sample to world (0, 0, 0) — the deterministic
        // fallback constant (same convention as noise2w).
        wn.weights_at(pos, up, nullptr, a);
        CHECK(a[0] == 2.42036104f, "noise3w fallback golden value");
        wn.weights_at(posComp, up, nullptr, b);
        CHECK(a[0] == b[0], "null context pins world 3D noise to a constant");
    }

    // ---- const dedup + the 64-op cap hold for the new line forms ----
    {
        SurfaceProgram dp;
        CHECK(parse_tape("const 2\nnoise3 7 0.25 3 0.5 2\nconst 2\nadd r1 r2\n"
                         "material 2 r3\n",
                         dp, err),
              err.c_str());
        CHECK(dp.ops.size() == 3, "const dedup unchanged around a noise3 line");
        SurfaceRuntime rt{std::move(dp)};
        const float pos[3] = {3.7f, 1.5f, -2.2f}, up[3] = {0, 1, 0};
        float w[kMaxSurfaceMaterials];
        rt.weights_at(pos, up, nullptr, w);
        CHECK(w[0] == 1.79261982f, "remapped refs still evaluate the noise3 golden");
        std::string over;
        for (int i = 0; i < kMaxSurfaceOps; ++i)
            over += "const " + std::to_string(i) + "\n";
        over += "noise3 7 0.25 3 0.5 2\nmaterial 2 r" +
                std::to_string(kMaxSurfaceOps) + "\n";
        SurfaceProgram op_;
        CHECK(!parse_tape(over.c_str(), op_, err),
              "a noise3 op past the emitted-op cap still trips it");
    }

    // ================= P3 appearance lanes (spec section 5) ==============
    //
    // Canonical grammar, one directive of each kind at most, registers are
    // backward refs like a material's, values clamped at EVALUATION (not at
    // parse) to [0, 2] / [-0.5, 0.5] / [0, 1].
    {
        // ---- parse + register wiring ----
        const char* kApp =
            "const 0.5\n"        // r0 = weight
            "const 1.25\n"       // r1
            "const 0.8\n"        // r2
            "const 0.3\n"        // r3
            "const 0.75\n"       // r4
            "material 7 r0\n"
            "tint r1 r2 r1\n"
            "roughbias r3\n"
            "wetness r4\n"
            "metallic r2\n";
        SurfaceProgram ap;
        CHECK(parse_tape(kApp, ap, err), err.c_str());
        CHECK(ap.has_tint() && ap.has_rough_bias() && ap.has_wetness() &&
                  ap.has_metallic(),
              "appearance: all four directives parse");
        CHECK(ap.has_appearance(), "appearance: has_appearance() agrees");
        CHECK(ap.tint_reg[0] == 1 && ap.tint_reg[1] == 2 && ap.tint_reg[2] == 1,
              "tint: three independent register refs (repeat allowed)");
        CHECK(ap.rough_bias_reg == 3 && ap.wetness_reg == 4 &&
                  ap.metallic_reg == 2,
              "roughbias/wetness/metallic register refs");
        CHECK(ap.materials.size() == 1,
              "appearance directives do not become material columns");

        // ---- evaluation + defaults ----
        const float pos[3] = {1.0f, 2.0f, 3.0f}, up[3] = {0, 1, 0};
        SurfaceAppearance app;
        SurfaceRuntime art{ap};
        art.appearance_at(pos, up, nullptr, app);
        CHECK(app.tint[0] == 1.25f && app.tint[1] == 0.8f &&
                  app.tint[2] == 1.25f,
              "appearance: tint evaluates from its registers");
        CHECK(app.rough_bias == 0.3f && app.wetness == 0.75f,
              "appearance: roughbias/wetness evaluate from their registers");
        CHECK(app.metallic == 0.8f,
              "appearance: metallic evaluates from its register");
        SurfaceProgram plainp;
        CHECK(parse_tape("const 0.5\nmaterial 7 r0\n", plainp, err),
              err.c_str());
        CHECK(!plainp.has_appearance(),
              "a tape without directives declares no appearance");
        SurfaceRuntime plain_rt{plainp};
        SurfaceAppearance identity;
        plain_rt.appearance_at(pos, up, nullptr, identity);
        CHECK(identity.tint[0] == 1.0f && identity.tint[1] == 1.0f &&
                  identity.tint[2] == 1.0f && identity.rough_bias == 0.0f &&
                  identity.wetness == 0.0f && identity.metallic == 0.0f,
              "appearance: an undeclared lane evaluates to its identity");

        // ---- clamps (spec section 5 ranges) ----
        SurfaceProgram cp;
        CHECK(parse_tape("const 0.5\nconst 3\nconst -1\nconst 0.9\n"
                         "material 7 r0\ntint r1 r2 r1\nroughbias r3\n"
                         "wetness r2\n",
                         cp, err),
              err.c_str());
        SurfaceRuntime crt{cp};
        SurfaceAppearance capp;
        crt.appearance_at(pos, up, nullptr, capp);
        CHECK(capp.tint[0] == kSurfaceTintMax && capp.tint[1] == 0.0f,
              "appearance: tint clamps to [0, 2]");
        CHECK(capp.rough_bias == kSurfaceRoughBiasLimit,
              "appearance: roughbias clamps to +0.5");
        CHECK(capp.wetness == 0.0f, "appearance: wetness clamps to [0, 1]");
        SurfaceProgram cn;
        CHECK(parse_tape("const 0.5\nconst -0.9\nmaterial 7 r0\n"
                         "roughbias r1\n",
                         cn, err),
              err.c_str());
        SurfaceAppearance napp;
        SurfaceRuntime{cn}.appearance_at(pos, up, nullptr, napp);
        CHECK(napp.rough_bias == -kSurfaceRoughBiasLimit,
              "appearance: roughbias clamps to -0.5");

        // ---- a lane may be any expression, world inputs included ----
        SurfaceProgram wp2;
        CHECK(parse_tape("const 0.5\ninput wy\nsmoothstep 0 10 r1\n"
                         "material 7 r0\nwetness r2\n",
                         wp2, err),
              err.c_str());
        CHECK(wp2.uses_world_inputs(),
              "appearance: a lane driven by a world input keeps the tape "
              "world-dependent");
        const float l2w[16] = {1, 0, 0, 0, 0, 1, 0, 5, 0, 0, 1, 0, 0, 0, 0, 1};
        SurfaceWorldContext wctx;
        wctx.local_to_world = l2w;
        SurfaceRuntime wrt{wp2};
        SurfaceAppearance wapp, dapp;
        const float wpos[3] = {0.0f, 0.0f, 0.0f};
        wrt.appearance_at(wpos, up, &wctx, wapp);   // world y = 5 -> midway
        wrt.appearance_at(wpos, up, nullptr, dapp); // fallback y = 0 -> dry
        CHECK(wapp.wetness == 0.5f, "appearance: spatially varying wetness");
        CHECK(dapp.wetness == 0.0f,
              "appearance: world lane falls back with no world context");

        // ---- rejection paths ----
        SurfaceProgram bad;
        CHECK(!parse_tape("const 1\nmaterial 7 r0\ntint r0 r0 r0\n"
                          "tint r0 r0 r0\n",
                          bad, err),
              "duplicate tint directive is rejected");
        CHECK(!parse_tape("const 1\nmaterial 7 r0\nroughbias r0\n"
                          "roughbias r0\n",
                          bad, err),
              "duplicate roughbias directive is rejected");
        CHECK(!parse_tape("const 1\nmaterial 7 r0\nwetness r0\nwetness r0\n",
                          bad, err),
              "duplicate wetness directive is rejected");
        CHECK(!parse_tape("const 1\nmaterial 7 r0\nmetallic r0\n"
                          "metallic r0\n",
                          bad, err),
              "duplicate metallic directive is rejected");
        CHECK(!parse_tape("const 1\nmaterial 7 r0\nmetallic 0.5\n", bad, err),
              "metallic takes a register, not a literal");
        {
            // metallic evaluation + clamp: 1.6 clamps to 1, -0.2 clamps to 0.
            SurfaceProgram mp;
            CHECK(parse_tape("const 1\nconst 1.6\nmaterial 7 r0\n"
                             "metallic r1\n",
                             mp, err),
                  err.c_str());
            SurfaceRuntime mrt{mp};
            const float mpos[3] = {0, 0, 0}, mup[3] = {0, 1, 0};
            SurfaceAppearance mapp;
            mrt.appearance_at(mpos, mup, nullptr, mapp);
            CHECK(mapp.metallic == 1.0f, "appearance: metallic clamps to 1");
            SurfaceProgram mp2;
            // r1 = 0.2, r2 = 0.8, r3 = r1 - r2 = -0.6 -> clamps to 0.
            CHECK(parse_tape("const 1\nconst 0.2\noneminus r1\n"
                             "sub r1 r2\nmaterial 7 r0\nmetallic r3\n",
                             mp2, err),
                  err.c_str());
            SurfaceRuntime mrt2{mp2};
            SurfaceAppearance mapp2;
            mrt2.appearance_at(mpos, mup, nullptr, mapp2);
            CHECK(mapp2.metallic == 0.0f, "appearance: metallic clamps to 0");
        }
        CHECK(!parse_tape("const 1\nmaterial 7 r0\ntint r0\n", bad, err),
              "a 2-token tint is rejected");
        CHECK(!parse_tape("const 1\nmaterial 7 r0\ntint r0 r0\n", bad, err),
              "a 3-token tint is rejected");
        CHECK(!parse_tape("const 1\nmaterial 7 r0\ntint r0 r0 r0 r0\n", bad,
                          err),
              "a 5-token tint is rejected");
        CHECK(!parse_tape("const 1\nmaterial 7 r0\nwetness\n", bad, err),
              "a bare wetness is rejected");
        CHECK(!parse_tape("const 1\nmaterial 7 r0\nwetness 0.5\n", bad, err),
              "wetness takes a register, not a literal");
        CHECK(!parse_tape("const 1\nmaterial 7 r0\ntint r0 r9 r0\n", bad, err),
              "tint rejects a forward/out-of-range register ref");
        CHECK(!parse_tape("const 1\nwetness r0\n", bad, err),
              "appearance directives do not satisfy the material requirement");

        // ---- the hash (page-invalidation key) folds the directives ----
        SurfaceProgram h0, h1, h2, h3;
        CHECK(parse_tape("const 1\nconst 0.4\nmaterial 7 r0\n", h0, err),
              err.c_str());
        CHECK(parse_tape("const 1\nconst 0.4\nmaterial 7 r0\nwetness r1\n", h1,
                         err),
              err.c_str());
        CHECK(parse_tape("const 1\nconst 0.4\nmaterial 7 r0\nroughbias r1\n",
                         h2, err),
              err.c_str());
        CHECK(parse_tape("const 1\nconst 0.4\nmaterial 7 r0\nwetness r1\n", h3,
                         err),
              err.c_str());
        CHECK(h0.hash() != h1.hash(),
              "adding an appearance directive changes the tape hash");
        CHECK(h1.hash() != h2.hash(),
              "different appearance directives hash differently");
        CHECK(h1.hash() == h3.hash(),
              "the same directive text hashes identically");
    }

    // =======================================================================
    // HABITAT TAPES (docs/habitat-tape-sketch-2026-08-08.md)
    //
    // Same op set, same register machine, same parser -- only the OUTPUT
    // directives differ. channels_at is the third reader of eval_regs, after
    // weights_at and appearance_at.
    // =======================================================================
    using terrain_field::TapeMode;
    auto parse_habitat = [](const std::string& text, SurfaceProgram& out,
                            std::string& e) {
        return SurfaceProgram::parse(text, out, e, TapeMode::Habitat);
    };

    // --- the two modes are genuinely separate, both fail closed ------------
    {
        SurfaceProgram p; std::string e;
        CHECK(!parse_habitat("const 0.5\n", p, e),
              "a habitat tape declaring no channel is rejected");
        CHECK(e.find("no channels") != std::string::npos, e.c_str());
        CHECK(!parse_habitat("const 0.5\nmaterial 3 r0\n", p, e),
              "a habitat tape cannot declare materials");
        CHECK(!parse_tape("const 0.5\nchannel 0 r0\n", p, e),
              "a surfaces tape cannot declare channels");
        CHECK(parse_tape("const 0.5\nmaterial 3 r0\n", p, e), e.c_str());
        CHECK(parse_habitat("const 0.5\nchannel 0 r0\n", p, e), e.c_str());
    }

    // --- channel directives: bounds, duplicates, backward refs -------------
    {
        SurfaceProgram p; std::string e;
        CHECK(!parse_habitat("const 0.5\nchannel 99 r0\n", p, e),
              "channel index past the cap is rejected");
        CHECK(!parse_habitat("const 0.5\nchannel -1 r0\n", p, e),
              "negative channel index is rejected");
        CHECK(!parse_habitat("const 0.5\nchannel 0 r0\nchannel 0 r0\n", p, e),
              "the same channel declared twice is rejected");
        CHECK(!parse_habitat("channel 0 r0\nconst 0.5\n", p, e),
              "a forward register ref is rejected, as for a material");
        CHECK(parse_habitat("const 0.25\nconst 0.75\n"
                            "channel 0 r0\nchannel 3 r1\n", p, e), e.c_str());
        CHECK(p.channel_count == 2, "two channels declared");
    }

    // --- channels_at reads the declared registers, undeclared read 0 -------
    {
        SurfaceProgram p; std::string e;
        CHECK(parse_habitat("const 0.25\nconst 0.75\n"
                            "channel 0 r0\nchannel 3 r1\n", p, e), e.c_str());
        SurfaceRuntime rt(p);
        float ch[terrain_field::kMaxHabitatChannels] = {};
        rt.channels_at(123.0f, -456.0f, nullptr, ch);
        CHECK(std::fabs(ch[0] - 0.25f) < 1e-6f, "channel 0 reads its register");
        CHECK(std::fabs(ch[3] - 0.75f) < 1e-6f, "channel 3 reads its register");
        CHECK(ch[1] == 0.0f && ch[2] == 0.0f,
              "an undeclared channel reads 0 rather than garbage");
        CHECK(rt.channel_count() == 2, "runtime reports the declared count");
    }

    // --- a habitat sample IS a world position ------------------------------
    // channels_at feeds an identity local_to_world, so worldX/worldZ inputs and
    // world noise must see the coordinates the caller passed. If that wiring
    // were wrong every ecology would silently sample the world origin -- which
    // is exactly the failure mode the non-anchored surfaces fallback has, so it
    // would look plausible.
    {
        SurfaceProgram p; std::string e;
        CHECK(parse_habitat("input wx\ninput wz\nchannel 0 r0\nchannel 1 r1\n",
                            p, e), e.c_str());
        SurfaceRuntime rt(p);
        float ch[terrain_field::kMaxHabitatChannels] = {};
        rt.channels_at(1234.5f, -678.25f, nullptr, ch);
        CHECK(std::fabs(ch[0] - 1234.5f) < 1e-3f,
              "worldX input sees the sampled x");
        CHECK(std::fabs(ch[1] + 678.25f) < 1e-3f,
              "worldZ input sees the sampled z");
    }

    // --- world noise varies with position, and is deterministic ------------
    {
        SurfaceProgram p; std::string e;
        CHECK(parse_habitat("noise2w 11 0.00333 3 0.5 2\nchannel 0 r0\n", p, e),
              e.c_str());
        SurfaceRuntime rt(p);
        float a[terrain_field::kMaxHabitatChannels] = {};
        float b[terrain_field::kMaxHabitatChannels] = {};
        float again[terrain_field::kMaxHabitatChannels] = {};
        rt.channels_at(0.0f, 0.0f, nullptr, a);
        rt.channels_at(900.0f, 400.0f, nullptr, b);
        rt.channels_at(0.0f, 0.0f, nullptr, again);
        CHECK(std::fabs(a[0] - b[0]) > 1e-4f,
              "world noise varies across the world -- it is not pinned to the "
              "origin the way a non-anchored surfaces tape is");
        CHECK(a[0] == again[0], "the same sample is bit-identical on re-read");
    }

    // --- terrain inputs come from the bound field ---------------------------
    // kSurfInHeight (8) and kSurfInFieldSlope (12) are the two the scatter
    // planner queries per candidate today; folding them into the tape is what
    // removes those crossings.
    {
        FieldProgram fp; std::string fe;
        CHECK(FieldProgram::parse(
                  "noise2 42 0.005 4 0.5 2.0\nconst 60\nmul r0 r1\n"
                  "const 0.6\nconst 0.3\nheight r2\nmoisture r3\nrelief r4\n"
                  "seaLevel -80\nbiome 0.65 0.35\n", fp, fe), fe.c_str());
        FieldRuntime field(std::move(fp));

        SurfaceProgram p; std::string e;
        CHECK(parse_habitat("input height\ninput fslope\n"
                            "channel 0 r0\nchannel 1 r1\n", p, e), e.c_str());
        SurfaceRuntime rt(p);
        float ch[terrain_field::kMaxHabitatChannels] = {};
        rt.channels_at(310.0f, -220.0f, &field, ch);
        CHECK(std::fabs(ch[0] - field.height_at(310.0f, -220.0f)) < 1e-3f,
              "the height input matches the field's own height_at -- the same "
              "call the tree planner makes per candidate");
        CHECK(std::fabs(ch[1] - field.slope_at(310.0f, -220.0f)) < 1e-3f,
              "the field-slope input matches slope_at");
    }

    // --- the arithmetic an ecology needs is all already there ---------------
    // A miniature of the real habitat maths: saturate(a + w*noise), a
    // smoothstep band, and an inverted edge via sub/abs/oneminus. If the
    // vocabulary were short of anything, it shows up here rather than three
    // work packages later.
    {
        SurfaceProgram p; std::string e;
        const char* text =
            "noise2w 31 0.0019 4 0.5 2\n"        // r0: forest signal
            "const 0.72\n"                        // r1
            "mul r0 r1\n"                         // r2
            "smoothstep 0.40 0.61 r2\n"           // r3: forest
            "const 0.505\n"                       // r4
            "sub r2 r4\n"                         // r5
            "abs r5\n"                            // r6
            "smoothstep 0.045 0.145 r6\n"         // r7
            "oneminus r7\n"                       // r8: forestEdge
            "channel 0 r3\nchannel 1 r8\n";
        CHECK(parse_habitat(text, p, e), e.c_str());
        SurfaceRuntime rt(p);
        float ch[terrain_field::kMaxHabitatChannels] = {};
        bool in_range = true, saw_variation = false;
        float first = -1.0f;
        for (int i = 0; i < 64; ++i) {
            rt.channels_at(float(i) * 37.0f, float(i) * -19.0f, nullptr, ch);
            for (int c = 0; c < 2; ++c)
                if (!(ch[c] >= 0.0f && ch[c] <= 1.0f)) in_range = false;
            if (first < 0) first = ch[0];
            else if (std::fabs(ch[0] - first) > 1e-4f) saw_variation = true;
        }
        CHECK(in_range, "smoothstep/oneminus channels stay in [0,1]");
        CHECK(saw_variation, "the forest channel actually varies across space");
    }

    return check_summary();
}
