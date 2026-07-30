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
        Add, Sub, Mul, Min, Max, Clamp,
        Blend, Smoothstep, Abs, OneMinus, Pow,
        // ---- surfaces() tape only (FieldProgram::parse never emits these) ----
        Input,           // read a per-sample input (oct = code)
        Noise2World,     // fbm over WORLD (x, z) — world-anchored variants
        Ridge2World,
        FieldCurv,       // field curvature probe at world (x, z), radius = f0
        Noise3,          // 3D fbm over part-local (x, y, z), optional warp tail
        Ridge3,          // 3D ridge variant (same per-octave shape as Ridge2)
        Noise3World,     // 3D fbm over WORLD (x, y, z) — world-anchored variants
        Ridge3World,
        Fract            // x - floor(x); unary
    } kind;
    int a = -1, b = -1, c = -1;        // register operands (-1 = unused)
    float f0 = 0, f1 = 0, f2 = 0, f3 = 0; // literals: value/freq/gain/lac/edges
    uint32_t seed = 0;
    int oct = 0;
    // Optional [wseed wfreq wamp] domain-warp tail on the 3D noise ops. The
    // struct layout is internal (canonical TEXT is the compatibility surface);
    // wf2 is the spare the GPU op packing reserves.
    bool warp = false;                 // tail present on this op
    uint32_t wseed = 0;                // explicit warp seed token
    float wf0 = 0, wf1 = 0, wf2 = 0;   // warp freq / warp amp / spare
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

    // Height deficit vs the 4-neighbour ring average at probe distance
    // `radius` (metres, clamped to >= 0.25): positive = the point sits BELOW
    // its surroundings (concave — collection zones, gully floors), negative =
    // above them (convex — crests, ridge lines). Metres, so tape thresholds
    // read as "N metres deep". Rung-independent (pure field query).
    float curvature_at(float x, float z, float radius) const;
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

    static constexpr int kMaxOps = 96;

    // Evaluate register [0..target] into regs[], using (x, z) as world coords.
    void eval_regs(float regs[], int count, float x, float z) const;

    float eval_reg(int target, float x, float z) const;
};

// ---------------------------------------------------------------------------
// surfaces() classifier tape (chart-VT spec Phase 4 / plan contract C4).
//
// A world instance method `surfaces(s)` records an op tape (world_base.js.h,
// `globalThis.__surface_ops` + `__surface_mats`) that the host reads back as
// canonical text; SurfaceProgram::parse compiles it here, next to the terrain
// field whose op set it reuses. SurfaceRuntime evaluates the tape per surface
// sample (part-local position + normal always; world-frame queries only when
// the caller supplies a SurfaceWorldContext — the world-anchored rule) into
// weights over the declared materials. The page compositor consumes the
// weights as per-vertex u8 columns (classify_vertices) and keeps the top-2
// per texel on the GPU.
// ---------------------------------------------------------------------------

// Per-sample inputs an `input <name>` op can read. Codes are the parse order
// below; kSurfaceInputWorldFirst and up require a world context.
enum SurfaceInput : int {
    kSurfInLocalX = 0,   // part-local position, metres
    kSurfInLocalY = 1,
    kSurfInLocalZ = 2,
    kSurfInNormalY = 3,  // part-local unit normal y
    kSurfInSlope = 4,    // clamp(1 - normal.y, 0, 1): 0 flat, 1 vertical+
    // ---- world inputs (world-anchored variants only) ----
    kSurfInWorldX = 5,   // world-space position of the sample
    kSurfInAltitude = 6, // world-space y
    kSurfInWorldZ = 7,
    kSurfInHeight = 8,   // terrain field height at (worldX, worldZ)
    kSurfInMoisture = 9,
    kSurfInRelief = 10,
    kSurfInBiome = 11,   // FieldRuntime::Biome as float (0..3)
    kSurfInFieldSlope = 12, // FieldRuntime::slope_at(worldX, worldZ): |grad h|,
                            // rise-over-run (1.0 = 45 deg). Unlike kSurfInSlope
                            // (mesh-normal derived, so LOD-rung dependent) this
                            // is stable across the whole LOD ladder.
    kSurfInCount = 13,
};
constexpr int kSurfaceInputWorldFirst = kSurfInWorldX;

// Hard cap on declared tape materials; mirrored by the compositor's per-vertex
// weight packing (vt_compositor.cpp / vt_composite.comp pack 8 u8 weights per
// mesh vertex — keep the two in sync).
constexpr int kMaxSurfaceMaterials = 8;

// Hard cap on emitted (deduplicated) tape ops — mirrors FieldRuntime::kMaxOps
// and the shader's VT_TAPE_MAX_OPS register file. Raised 64 -> 96 when the
// StreamMountain P4 pass proved 64 binding (strata + speckle + seep terms were
// cut for budget); the GPU cost is the register file and the arena slot, both
// measured harmless at 64 with headroom.
constexpr int kMaxSurfaceOps = 96;

// ---------------------------------------------------------------------------
// P3 appearance lanes (texel-tape spec §5). Three optional output directives
// recorded after the `material` lines; each names tape register(s) whose value
// modulates the COMPOSITED texel (after the top-2 height blend, before BC
// encode) in the fixed order tint -> roughbias -> wetness -> metallic.
//
// The clamp ranges and the wetness response below are the single source of
// truth for three consumers that must never drift apart:
//   * SurfaceRuntime::appearance_at (CPU evaluation),
//   * shaders_vk/vt_surface_tape.glsl's VT_APP_* defines (GPU mode 3),
//   * the goldens in tests/surface_field_tests.cpp / vt_compositor_tests.cpp.
// ---------------------------------------------------------------------------
constexpr float kSurfaceTintMax        = 2.0f;   // tint clamps to [0, 2]
constexpr float kSurfaceRoughBiasLimit = 0.5f;   // roughbias clamps to [-.5,.5]
constexpr float kSurfaceWetAlbedoScale = 0.55f;  // albedo *= mix(1, .55, w)
constexpr float kSurfaceWetRoughness   = 0.08f;  // orm.g = mix(orm.g, .08, w)
// metallic writes orm.b directly (clamped [0, 1]). Deliberately a raw write,
// not a bias: base materials bake metalness 0, and PBR metalness is only
// meaningful near its endpoints — the lane exists for sparse mineral flecks /
// ore veins, not broad half-metal surfaces.

// Evaluated appearance for one sample; the defaults ARE the identity (a tape
// with no directives leaves the composited texel untouched).
struct SurfaceAppearance {
    float tint[3] = {1.0f, 1.0f, 1.0f};
    float rough_bias = 0.0f;
    float wetness = 0.0f;
    float metallic = 0.0f;
};

// The world-anchored rule (contract C4): only a variant referenced by exactly
// one instance may read world inputs (terrain sectors qualify by design).
inline bool surface_variant_world_anchored(uint32_t instance_count) {
    return instance_count == 1;
}

struct SurfaceProgram {
    // Parse canonical text: op lines (const/noise2/ridge2/noise2w/ridge2w/
    // noise3/ridge3/noise3w/ridge3w/curv/add/sub/mul/min/max/clamp/blend/
    // smoothstep/abs/oneminus/pow/fract/input) followed by
    // `material <handle> r<reg>` directives and the optional P3 appearance
    // directives `tint rR rG rB`, `roughbias rN`, `wetness rN`, `metallic rN`
    // (at most one of each; every register is a backward ref like a
    // material's). warp2 is NOT
    // part of the surface op set — the 3D noise ops carry an optional
    // [wseed wfreq wamp] tail that domain-warps their own sample point
    // instead. Returns false and sets err on any violation (including 0 or
    // > kMaxSurfaceMaterials declared materials, a warp tail that is not
    // exactly 0 or 3 tokens, or a duplicate/malformed appearance directive).
    //
    // Register refs in the text are SOURCE ordinals (the line order the JS
    // recorder emitted). Identical `const` lines are deduplicated at parse
    // time — refs are remapped, so duplicates cost no register budget and the
    // kMaxOps cap applies to the DEDUPLICATED op count.
    static bool parse(const std::string& text, SurfaceProgram& out, std::string& err);

    // FNV-1a 64-bit hash over the canonical program text bytes. Folds into the
    // VT page/tail content key so an edited tape invalidates resident pages.
    uint64_t hash() const;

    const std::string& text() const { return text_; }
    bool uses_world_inputs() const { return uses_world_inputs_; }

    // Bitmask over SurfaceInput codes the program actually reads (directly via
    // `input`, or implied — noise2w/ridge2w/curv imply worldX/worldZ;
    // noise3w/ridge3w imply worldX/altitude/worldZ). Lets
    // the runtime skip field queries (height/moisture/relief/biome/fslope are
    // full program evaluations each) for inputs no op consumes.
    uint32_t input_mask() const { return input_mask_; }

    struct MaterialSlot {
        int handle = -1;   // material registry index
        int reg = -1;      // weight register
    };

    std::vector<Op> ops;
    std::vector<MaterialSlot> materials;

    // P3 appearance lanes: the register each directive reads, -1 when the
    // directive is absent (identity). tint_reg[0] < 0 means "no tint" — the
    // three components are set together or not at all.
    int tint_reg[3] = {-1, -1, -1};
    int rough_bias_reg = -1;
    int wetness_reg = -1;
    int metallic_reg = -1;

    bool has_tint() const { return tint_reg[0] >= 0; }
    bool has_rough_bias() const { return rough_bias_reg >= 0; }
    bool has_wetness() const { return wetness_reg >= 0; }
    bool has_metallic() const { return metallic_reg >= 0; }
    bool has_appearance() const {
        return has_tint() || has_rough_bias() || has_wetness() ||
               has_metallic();
    }

private:
    bool uses_world_inputs_ = false;
    uint32_t input_mask_ = 0;
    std::string text_;
};

// P2 (texel-rate tape) accessors: evaluate one noise-family surface op at an
// explicit sample point — EXACTLY the arithmetic weights_at uses (fbm2 /
// fbm3_op including the optional domain-warp tail). Two consumers:
//   * the GPU tape packer pre-resolves world noise ops to constants for
//     non-world-anchored parts (the fallback pins them to world origin, so
//     the op's value IS a constant — computed here with the CPU float path);
//   * tests cross-check the GLSL noise twin against the CPU reference.
// Pure accessors over the existing internals; no semantic changes.
float surface_op_fbm2(const Op& op, float x, float z, bool ridged);
float surface_op_fbm3(const Op& op, float x, float y, float z, bool ridged);

// World-frame evaluation context. `field` supplies height/moisture/relief/
// biome; `local_to_world` is a row-major float[16] (null = identity). Passing
// no context at all (null SurfaceWorldContext*) makes every world input
// evaluate to its deterministic fallback constant.
struct SurfaceWorldContext {
    const FieldRuntime* field = nullptr;
    const float* local_to_world = nullptr;
};

class SurfaceRuntime {
public:
    explicit SurfaceRuntime(SurfaceProgram p);

    const SurfaceProgram& program() const { return prog_; }
    uint64_t hash() const { return prog_.hash(); }
    bool uses_world_inputs() const { return prog_.uses_world_inputs(); }
    uint32_t material_count() const { return (uint32_t)prog_.materials.size(); }
    int material_handle(uint32_t i) const { return prog_.materials[i].handle; }

    // Weights (clamped >= 0, NOT normalized) for one sample. out_weights has
    // material_count() entries. `world` null => world inputs read fallback
    // constants (worldX/altitude/worldZ/height = 0, moisture/relief = 0.5,
    // biome = 1 — deterministic, never instance-dependent).
    void weights_at(const float pos[3], const float nrm[3],
                    const SurfaceWorldContext* world, float* out_weights) const;

    // P3 appearance lanes for one sample, CLAMPED to their documented ranges
    // (tint [0, kSurfaceTintMax], rough_bias +-kSurfaceRoughBiasLimit, wetness
    // [0, 1]). A tape without a given directive yields that lane's identity,
    // so the result is always safe to apply unconditionally.
    //
    // This is the CPU twin of the mode-3 shader's application (spec §5). The
    // per-vertex u8 pipeline (classify_vertices) has no channels to carry
    // appearance, so mode-2 parts and the legacy fallback path apply NONE of
    // it — appearance requires weight-seam mode 3, exactly as the spec's
    // "requires mode 3 in practice" anticipates. This entry point exists for
    // tests, tooling, and any future CPU-side consumer.
    void appearance_at(const float pos[3], const float nrm[3],
                       const SurfaceWorldContext* world,
                       SurfaceAppearance& out) const;

    // Quantized per-vertex evaluation over an indexed mesh stream (positions
    // 3*n; normals 3*n, may be null => +Y). Per vertex the weights are
    // normalized to sum 1 then quantized to u8 (all-zero => material 0 gets
    // 255). `out` receives vertex_count * material_count() bytes, weight
    // column-major per vertex.
    void classify_vertices(const float* positions, const float* normals,
                           uint32_t vertex_count,
                           const SurfaceWorldContext* world,
                           uint8_t* out) const;

    // Warn-once latch for the world-input misuse diagnostic (world inputs on
    // a variant that is not world-anchored). Returns true exactly once per
    // runtime instance; the caller owns the actual message.
    bool note_world_input_misuse() const;

private:
    // Evaluate every register into `regs` (capacity kMaxSurfaceOps, enforced
    // at parse). The single evaluation path behind weights_at/appearance_at,
    // so the two can never disagree about what a register holds.
    void eval_regs(const float pos[3], const float nrm[3],
                   const SurfaceWorldContext* world, float* regs) const;

    SurfaceProgram prog_;
    mutable bool misuse_noted_ = false;
};

} // namespace terrain_field
