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
        FieldProgram p1, p2, p3; std::string err;
        FieldProgram::parse("const 1\nheight r0\nconst 0.5\nmoisture r2\nconst 0.5\nrelief r4\nseaLevel 0\nbiome 0.65 0.35\n", p1, err);
        FieldProgram::parse("const 1\nheight r0\nconst 0.5\nmoisture r2\nconst 0.5\nrelief r4\nseaLevel 0\nbiome 0.65 0.35\n", p2, err);
        FieldProgram::parse("const 2\nheight r0\nconst 0.5\nmoisture r2\nconst 0.5\nrelief r4\nseaLevel 0\nbiome 0.65 0.35\n", p3, err);
        CHECK(p1.hash() == p2.hash(), "hash stable");
        CHECK(p1.hash() != p3.hash(), "hash differs on text change");
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
    return check_summary();
}
