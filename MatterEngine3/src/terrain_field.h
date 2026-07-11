#pragma once
// terrain_field.h — native field program interpreter for infinite-world terrain.
// Pure CPU module: no JS, no GL, no engine subsystem dependencies.
// Used by Tasks 4, 5, 7, 9 (world evaluator, mesher, etc.).

#include <string>
#include <cstdint>
#include <vector>

namespace terrain_field {

// ---------------------------------------------------------------------------
// Op — single instruction in the field program.
// ---------------------------------------------------------------------------
struct Op {
    enum Kind {
        Const, Noise2, Ridge2, Warp2,
        Add, Mul, Min, Max, Clamp,
        Blend, Smoothstep
    } kind;
    int a = -1, b = -1, c = -1;        // register operands (-1 = unused)
    float f0 = 0, f1 = 0, f2 = 0, f3 = 0; // literals: value/freq/gain/lac/edges
    uint32_t seed = 0;
    int oct = 0;
};

// ---------------------------------------------------------------------------
// FieldProgram — parsed, immutable field program.
// ---------------------------------------------------------------------------
struct FieldProgram {
    // Parse a canonical text program (one op per line, directives at end).
    // Returns false and sets err on any violation.
    static bool parse(const std::string& text, FieldProgram& out, std::string& err);

    // FNV-1a 64-bit hash over the canonical program text bytes.
    uint64_t hash() const;

    const std::string& text() const { return text_; }

    // Internal fields — accessed by FieldRuntime.
    std::vector<Op> ops;
    int height_reg  = -1;
    int moisture_reg = -1;
    int relief_reg  = -1;
    float sea_level = 0.0f;
    float mount_relief_thresh = 0.65f;
    float rocky_moist_thresh  = 0.35f;

private:
    std::string text_;
};

// ---------------------------------------------------------------------------
// FieldRuntime — evaluator bound to a compiled FieldProgram.
// ---------------------------------------------------------------------------
class FieldRuntime {
public:
    explicit FieldRuntime(FieldProgram p);

    float height_at(float x, float z) const;
    float density_at(float x, float y, float z) const;  // height_at(x,z) - y
    float slope_at(float x, float z) const;              // |grad h|, central diff eps=0.5
    float moisture_at(float x, float z) const;           // 0..1
    float relief_at(float x, float z) const;             // 0..1

    enum Biome { Ocean = 0, Meadow = 1, Foothills = 2, Mountains = 3 };
    Biome biome_at(float x, float z) const;

    enum Material { MatGrass = 0, MatDirt = 1, MatRock = 2, MatSnow = 3 };
    Material material_at(float x, float z) const;       // slope/height/biome rules

    float sea_level() const { return prog_.sea_level; }
    uint64_t hash() const   { return prog_.hash(); }

private:
    FieldProgram prog_;

    static constexpr int kMaxOps = 64;

    // Evaluate register [0..target] into regs[], using (x, z) as world coords.
    void eval_regs(float regs[], int count, float x, float z) const;

    float eval_reg(int target, float x, float z) const;
};

} // namespace terrain_field
