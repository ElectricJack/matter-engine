// MatterEngine3/tests/terrain_field_tests.cpp
#include "check.h"
#include "../src/terrain_field.h"
#include <cmath>
#include <string>

using namespace terrain_field;

static FieldRuntime make(const std::string& text) {
    FieldProgram p; std::string err;
    if (!FieldProgram::parse(text, p, err)) { printf("parse err: %s\n", err.c_str()); }
    return FieldRuntime(std::move(p));
}

int main() {
    // --- constant program: height 5 everywhere -----------------------------
    {
        FieldRuntime f = make(
            "const 5\nconst 0.5\nconst 0.5\n"
            "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n");
        CHECK(std::fabs(f.height_at(3, 7) - 5.0f) < 1e-5f, "const height");
        CHECK(std::fabs(f.density_at(3, 5, 7)) < 1e-5f, "density zero at surface");
        CHECK(f.density_at(3, 0, 7) > 0 && f.density_at(3, 10, 7) < 0,
              "density sign: solid below, air above");
        CHECK(f.slope_at(3, 7) < 1e-4f, "flat slope");
        CHECK(f.biome_at(3, 7) == FieldRuntime::Meadow, "mid moisture/relief = meadow");
    }
    // --- biome classification via crafted control fields --------------------
    {
        FieldRuntime f = make("const 5\nconst 0.5\nconst 0.9\n"
            "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n");
        CHECK(f.biome_at(0, 0) == FieldRuntime::Mountains, "high relief = mountains");
    }
    {
        FieldRuntime f = make("const 5\nconst 0.1\nconst 0.2\n"
            "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n");
        CHECK(f.biome_at(0, 0) == FieldRuntime::Foothills, "low moisture = foothills");
    }
    {
        FieldRuntime f = make("const -3\nconst 0.5\nconst 0.2\n"
            "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n");
        CHECK(f.biome_at(0, 0) == FieldRuntime::Ocean, "below sea level = ocean");
    }
    // --- noise: deterministic, seed-sensitive, continuous, bounded ---------
    {
        const char* prog =
            "noise2 1234 0.01 4 0.5 2.0\nconst 0.5\nconst 0.5\n"
            "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n";
        FieldRuntime a = make(prog), b = make(prog);
        CHECK(a.height_at(10, 20) == b.height_at(10, 20), "noise deterministic");
        FieldRuntime c = make(
            "noise2 9999 0.01 4 0.5 2.0\nconst 0.5\nconst 0.5\n"
            "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n");
        CHECK(a.height_at(10, 20) != c.height_at(10, 20), "seed changes noise");
        float h0 = a.height_at(10, 20), h1 = a.height_at(10.01f, 20);
        CHECK(std::fabs(h1 - h0) < 0.05f, "noise continuous");
        bool bounded = true;
        for (int i = 0; i < 1000; ++i) {
            float v = a.height_at(i * 3.7f, i * -1.9f);
            if (v < -1.5f || v > 1.5f) bounded = false;
        }
        CHECK(bounded, "fbm output roughly in [-1,1]");
    }
    // --- hash: stable for same text, differs for different text ------------
    {
        // Programs use valid backward-only register refs (ops precede directives).
        const char* prog_a = "const 1\nconst 0.5\nconst 0.5\nheight r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n";
        const char* prog_b = "const 1\nconst 0.5\nconst 0.5\nheight r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n";
        const char* prog_c = "const 2\nconst 0.5\nconst 0.5\nheight r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n";
        FieldProgram p1, p2, p3; std::string err;
        CHECK(FieldProgram::parse(prog_a, p1, err), "hash prog_a parses ok");
        CHECK(FieldProgram::parse(prog_b, p2, err), "hash prog_b parses ok");
        CHECK(FieldProgram::parse(prog_c, p3, err), "hash prog_c parses ok");
        CHECK(p1.hash() == p2.hash(), "hash stable");
        CHECK(p1.hash() != p3.hash(), "hash differs on text change");
    }
    // --- forward register reference is rejected ----------------------------
    {
        // r5 does not exist when op r2 references it (only r0,r1,r2 defined so far).
        FieldProgram p; std::string err;
        CHECK(!FieldProgram::parse(
            "const 1\nconst 0.5\nadd r0 r5\n"
            "const 0.5\nheight r0\nmoisture r1\nrelief r3\nseaLevel 0\nbiome 0.65 0.35\n",
            p, err), "forward register ref rejected");
        CHECK(!err.empty(), "forward ref error message set");
    }
    // --- programs beyond the op cap are rejected ---------------------------
    {
        // Generate 100 'const 1' lines — above the kMaxOps=96 limit.
        std::string big;
        for (int i = 0; i < 100; ++i) big += "const 1\n";
        big += "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n";
        FieldProgram p; std::string err;
        CHECK(!FieldProgram::parse(big, p, err),
              "program past the op cap rejected");
        CHECK(!err.empty(), "oversized program error message set");
    }
    // --- materials ----------------------------------------------------------
    {
        // steep ridge program: mul big amplitude -> slope high somewhere
        FieldRuntime f = make(
            "ridge2 7 0.02 4 0.5 2.0\nconst 110\nmul r0 r1\nconst 0.5\nconst 0.9\n"
            "height r2\nmoisture r3\nrelief r4\nseaLevel 0\nbiome 0.65 0.35\n");
        // find a steep sample; material must be rock there
        bool found_rock = false;
        for (int i = 0; i < 4000 && !found_rock; ++i) {
            float x = i * 1.3f, z = i * -2.1f;
            if (f.slope_at(x, z) > 1.2f)
                found_rock = f.material_at(x, z) == FieldRuntime::MatRock;
        }
        CHECK(found_rock, "steep slope classifies as rock");
    }
    // --- parse errors fail loudly ------------------------------------------
    {
        FieldProgram p; std::string err;
        CHECK(!FieldProgram::parse("bogusop 1 2\nheight r0\n", p, err), "unknown op rejected");
        CHECK(!err.empty(), "error message set");
        CHECK(!FieldProgram::parse("const 1\n", p, err), "missing height directive rejected");
    }
    // --- sub / abs / oneminus / pow -----------------------------------------
    {
        FieldRuntime f = make(
            "const 5\n"        // r0
            "const 2\n"        // r1
            "sub r1 r0\n"      // r2 = -3
            "abs r2\n"         // r3 = 3
            "pow r3 2\n"       // r4 = 9
            "oneminus r4\n"    // r5 = -8
            "abs r5\n"         // r6 = 8
            "const 0.5\n"      // r7
            "height r6\nmoisture r7\nrelief r7\nseaLevel 0\nbiome 0.65 0.35\n");
        CHECK(std::fabs(f.height_at(0, 0) - 8.0f) < 1e-5f,
              "sub/abs/pow/oneminus chain evaluates");
        // pow clamps its base to >= 0 (fractional exponents stay total).
        FieldRuntime g = make(
            "const -4\nconst 0.5\npow r0 0.5\n"
            "height r2\nmoisture r1\nrelief r1\nseaLevel -1\nbiome 0.65 0.35\n");
        CHECK(g.height_at(0, 0) == 0.0f, "pow clamps negative bases to 0");
    }
    // --- curvature_at: zero on constants, sign matches the ring average -----
    {
        FieldRuntime flat = make(
            "const 5\nconst 0.5\nconst 0.5\n"
            "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n");
        CHECK(flat.curvature_at(3, 7, 4.0f) == 0.0f, "constant field: zero curvature");
        FieldRuntime n = make(
            "noise2 1234 0.01 4 0.5 2.0\nconst 60\nmul r0 r1\nconst 0.5\n"
            "height r2\nmoisture r3\nrelief r3\nseaLevel 0\nbiome 0.65 0.35\n");
        const float x = 37.0f, z = -12.0f, e = 8.0f;
        const float manual =
            (n.height_at(x + e, z) + n.height_at(x - e, z) +
             n.height_at(x, z + e) + n.height_at(x, z - e)) * 0.25f -
            n.height_at(x, z);
        CHECK(std::fabs(n.curvature_at(x, z, e) - manual) < 1e-6f,
              "curvature_at is the ring-average height deficit");
    }
    // --- input wx / wz: world coordinates in the FIELD tape -----------------
    // The field op set is otherwise translation-covariant noise, which can only
    // make statistically uniform terrain. These two inputs are what let a world
    // author a shape AT A PLACE, and PomProof's dome() is built from them.
    {
        FieldRuntime f = make(
            "input wx\ninput wz\nadd r0 r1\nconst 0.5\n"
            "height r2\nmoisture r3\nrelief r3\nseaLevel 0\nbiome 0.65 0.35\n");
        CHECK(std::fabs(f.height_at(3, 7) - 10.0f) < 1e-5f, "input wx + wz");
        CHECK(std::fabs(f.height_at(-2.5f, 0.5f) - (-2.0f)) < 1e-5f,
              "input wx/wz carry sign and fraction");
        // Not translation-invariant — the whole point.
        CHECK(f.height_at(0, 0) != f.height_at(100, 0), "field varies with world x");
    }
    {
        // The exact hemisphere PomProof's dome() emits, at R = 10 about (4, -6):
        //   y = R * sqrt(clamp(1 - d^2/R^2, 0, 1))
        FieldRuntime f = make(
            "input wx\nconst 4\nsub r0 r1\n"
            "input wz\nconst -6\nsub r3 r4\n"
            "mul r2 r2\nmul r5 r5\nadd r6 r7\n"
            "const 0.01\nmul r8 r9\noneminus r10\nclamp r11 0 1\npow r12 0.5\n"
            "const 10\nmul r13 r14\nconst 0.5\n"
            "height r15\nmoisture r16\nrelief r16\nseaLevel 0\nbiome 0.65 0.35\n");
        CHECK(std::fabs(f.height_at(4, -6) - 10.0f) < 1e-4f, "dome crown = R");
        CHECK(std::fabs(f.height_at(4 + 10.0f / 1.41421356f, -6) - 7.0710678f) < 1e-3f,
              "dome at d = R/sqrt2 is R/sqrt2 tall (45 deg point)");
        CHECK(f.height_at(4 + 20, -6) == 0.0f, "dome is exactly zero outside its footprint");
        CHECK(f.height_at(4, -6 - 30) == 0.0f, "dome footprint is radial, not axis-aligned");
        // Steepness is the property the test world needs, and it has to be
        // measured at the scale it exists at. slope_at's central difference is
        // fixed at eps = 0.5 m, so at the rim one of its probes lands OUTSIDE
        // the footprint (height 0) and it reports ~3 (72 deg) for a face that
        // is analytically 87 deg. Measure the rise over 1 cm instead, which is
        // both the true property and the resolution the surface exists at.
        {
            const float a = f.height_at(4 + 9.990f, -6);
            const float b = f.height_at(4 + 9.999f, -6);
            CHECK((a - b) / 0.009f > 10.0f,
                  "dome flank exceeds 84 deg within 1 cm of the rim");
            CHECK(f.slope_at(4 + 9.99f, -6) > 3.0f,
                  "slope_at still reads the rim as steep, just blunted by its 0.5 m probe");
        }
    }
    // --- the field tape rejects every other input name ----------------------
    {
        FieldProgram p; std::string err;
        CHECK(!FieldProgram::parse(
                  "input slope\nconst 0.5\n"
                  "height r0\nmoisture r1\nrelief r1\nseaLevel 0\nbiome 0.65 0.35\n",
                  p, err),
              "field program rejects surfaces()-only input names");
        CHECK(err.find("wx") != std::string::npos,
              "the rejection names the inputs that ARE allowed");
    }

    // =======================================================================
    // 3D DENSITY. The field program is a function of (x, y, z); a heightfield
    // is the density `h(x, z) - y` and is RECOGNISED, not a separate mode.
    // =======================================================================

    // --- the heightfield is the default, and it is recognised ---------------
    {
        FieldRuntime f = make(
            "const 5\nconst 0.5\nconst 0.5\n"
            "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n");
        CHECK(f.is_heightfield(), "no density directive = heightfield");
        CHECK(std::fabs(f.density_at(3, -2, 7) - 7.0f) < 1e-5f,
              "heightfield density reconstructs as height - y");
    }
    // Spelled out longhand, the same field must still be recognised: a
    // round-tripped or hand-written program should not lose the mesher's
    // specialisations over a notation difference.
    {
        FieldRuntime f = make(
            "const 5\nconst 0.5\nconst 0.5\ninput wy\nsub r0 r3\n"
            "height r0\ndensity r4\nmoisture r1\nrelief r2\n"
            "seaLevel 0\nbiome 0.65 0.35\n");
        CHECK(f.is_heightfield(), "explicit sub(height, wy) is recognised too");
        CHECK(std::fabs(f.density_at(3, -2, 7) - 7.0f) < 1e-5f,
              "and evaluates identically");
    }

    // --- input wy reads world altitude --------------------------------------
    // A density that is y and nothing else: solid below 0, air above, surface
    // exactly at 0 -- and a height register that stays independent of it.
    {
        FieldRuntime f = make(
            "const 0\nconst 0.5\ninput wy\nconst -1\nmul r2 r3\n"
            "height r0\ndensity r4\nmoisture r1\nrelief r1\n"
            "seaLevel -100\nbiome 0.65 0.35\n");
        CHECK(!f.is_heightfield(), "a density that is not height - y is volumetric");
        CHECK(f.density_at(1, -10, 2) > 0 && f.density_at(1, 10, 2) < 0,
              "input wy reads world altitude");
        CHECK(f.height_at(1, 2) == 0.0f, "height register unaffected by y");
    }

    // --- noise3 is deterministic, seed-sensitive, and varies in y -----------
    {
        const char* prog =
            "noise3 1234 0.02 3 0.5 2\nconst 0.5\n"
            "height r1\ndensity r0\nmoisture r1\nrelief r1\n"
            "seaLevel -100\nbiome 0.65 0.35\n";
        FieldRuntime a = make(prog), b = make(prog);
        CHECK(a.density_at(10, 20, 30) == b.density_at(10, 20, 30),
              "noise3 deterministic");
        CHECK(a.density_at(10, 20, 30) != a.density_at(10, 24, 30),
              "noise3 varies along y -- the whole point");
        FieldRuntime c = make(
            "noise3 999 0.02 3 0.5 2\nconst 0.5\n"
            "height r1\ndensity r0\nmoisture r1\nrelief r1\n"
            "seaLevel -100\nbiome 0.65 0.35\n");
        CHECK(a.density_at(10, 20, 30) != c.density_at(10, 20, 30),
              "seed changes noise3");
        for (int i = 0; i < 400; ++i) {
            const float v = a.density_at(i * 3.7f, i * 1.1f, i * -1.9f);
            CHECK(v >= -1.5f && v <= 1.5f, "noise3 stays in range");
        }
    }
    // ridge3 too, and its peaks must reach higher than plain noise does.
    {
        FieldRuntime f = make(
            "ridge3 77 0.05 2 0.5 2\nconst 0.5\n"
            "height r1\ndensity r0\nmoisture r1\nrelief r1\n"
            "seaLevel -100\nbiome 0.65 0.35\n");
        float hi = -2.0f;
        for (int i = 0; i < 4000; ++i)
            hi = std::fmax(hi, f.density_at(i * 0.7f, i * 0.31f, i * -0.53f));
        CHECK(hi > 0.6f, "ridge3 produces crests -- what a tunnel field needs");
    }

    // --- the 2D-projection contract -----------------------------------------
    // height/moisture/relief take no y, so a y-dependent register behind any of
    // them would silently read a slice at whatever y the evaluator passed.
    {
        FieldProgram p; std::string err;
        CHECK(!FieldProgram::parse(
                  "input wy\nconst 0.5\n"
                  "height r0\nmoisture r1\nrelief r1\nseaLevel 0\nbiome 0.65 0.35\n",
                  p, err),
              "a y-dependent height register is rejected");
        CHECK(err.find("world y") != std::string::npos,
              "and the rejection says why");
        CHECK(!FieldProgram::parse(
                  "noise3 5 0.1 2 0.5 2\nconst 1\nconst 0.5\n"
                  "height r1\nmoisture r0\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n",
                  p, err),
              "y-dependence is rejected transitively, through moisture too");
    }
    // The *World 3D ops stay tape-only: a field program has no part-local frame
    // to distinguish them from, so admitting both spellings would put two names
    // for one op into the canonical text.
    {
        FieldProgram p; std::string err;
        CHECK(!FieldProgram::parse(
                  "noise3w 5 0.1 2 0.5 2\nconst 1\nconst 0.5\n"
                  "height r1\nmoisture r2\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n",
                  p, err),
              "noise3w is not a field-program op");
    }

    // --- ColumnCache == the general evaluator, everywhere -------------------
    // The mesher runs the y-independent prefix once per column and only the
    // y-dependent tail per voxel. If those two ever disagree, terrain meshes
    // differently from what every other consumer of the field believes.
    {
        // Deliberately exercises the awkward cases: a 2D warp under a 3D term,
        // blend with a y-dependent selector, and min/max mixing the two.
        FieldRuntime f = make(
            "noise2 11 0.01 3 0.5 2\n"          // r0  surface, y-independent
            "const 40\nmul r0 r1\n"             // r2
            "ridge3 22 0.02 2 0.5 2\n"          // r3  y-dependent
            "input wy\n"                        // r4
            "sub r2 r4\n"                       // r5  height - y
            "const 0.3\nsub r3 r6\n"            // r7
            "const -1\nmul r7 r8\n"             // r9
            "min r5 r9\n"                       // r10
            "noise2 33 0.004 2 0.5 2\n"         // r11 y-independent
            "smoothstep 0 1 r3\n"               // r12 y-dependent
            "blend r11 r9 r12\n"                // r13 mixes both
            "max r10 r13\n"                     // r14
            "const 0.5\n"                       // r15
            "height r2\ndensity r14\nmoisture r15\nrelief r15\n"
            "seaLevel -100\nbiome 0.65 0.35\n");
        CHECK(!f.is_heightfield(), "the mixed program is volumetric");
        double worst = 0;
        FieldRuntime::ColumnCache cc;
        for (int i = 0; i < 25; ++i) {
            const float x = float(i * 37 - 400), z = float(i * -53 + 90);
            f.eval_column(cc, x, z);
            for (float y = -300; y <= 300; y += 7.0f)
                worst = std::fmax(worst,
                    std::fabs(double(f.density_at(cc, y)) -
                              double(f.density_at(x, y, z))));
        }
        CHECK(worst == 0.0, "column cache is bit-identical to the general path");
    }
    // ...and for a heightfield, where the tail is a single subtraction.
    {
        FieldRuntime f = make(
            "noise2 11 0.01 3 0.5 2\nconst 40\nmul r0 r1\nconst 0.5\n"
            "height r2\nmoisture r3\nrelief r3\nseaLevel 0\nbiome 0.65 0.35\n");
        FieldRuntime::ColumnCache cc;
        double worst = 0;
        for (int i = 0; i < 25; ++i) {
            const float x = float(i * 37), z = float(i * -53);
            f.eval_column(cc, x, z);
            for (float y = -100; y <= 100; y += 3.0f)
                worst = std::fmax(worst,
                    std::fabs(double(f.density_at(cc, y)) -
                              double(f.density_at(x, y, z))));
        }
        CHECK(worst == 0.0, "column cache matches on the heightfield path too");
    }
    return check_summary();
}
