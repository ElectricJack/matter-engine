#pragma once
// terrain_field.h — native field program interpreter for infinite-world terrain.
// Pure CPU module: no JS, no GL, no engine subsystem dependencies.
// Used by Tasks 4, 5, 7, 9 (world evaluator, mesher, etc.).

#include <string>
#include <cstdint>
#include <vector>

namespace terrain_field {

// Hard cap on emitted (deduplicated) tape ops. THE authority for the surfaces
// tape: terrain_field.cpp's kMaxOps and vt_compositor.cpp's kTapeSlotOps are
// both aliases of this, so the only uncheckable mirror left is the shader's
// VT_TAPE_MAX_OPS register file. Raised 64 -> 96 when the
// StreamMountain P4 pass proved 64 binding (strata + speckle + seep terms were
// cut for budget); the GPU cost is the register file and the arena slot, both
// measured harmless at 64 with headroom.
//
// Declared here rather than down in the surfaces() section where it was born:
// FieldRuntime::ColumnCache sizes a register file with it, and that class comes
// first in this header.
constexpr int kMaxSurfaceOps = 96;

// ---------------------------------------------------------------------------
// Op — single instruction in the field program.
// ---------------------------------------------------------------------------
struct Op {
    enum Kind {
        Const, Noise2, Ridge2, Warp2,
        Add, Sub, Mul, Min, Max, Clamp,
        Blend, Smoothstep, Abs, OneMinus, Pow,
        // Read a per-sample input (oct = a SurfaceInput code). Shared by both
        // tapes, but with different admissible codes: the surfaces() tape takes
        // any of them, while FieldProgram::parse takes ONLY kSurfInWorldX,
        // kSurfInAltitude and kSurfInWorldZ — a terrain field is a function of
        // (x, y, z) and nothing else is defined there.
        Input,
        // ---- surfaces() tape only (FieldProgram::parse never emits these) ----
        Noise2World,     // fbm over WORLD (x, z) — world-anchored variants
        Ridge2World,
        FieldCurv,       // field curvature probe at world (x, z), radius = f0
        // Noise3/Ridge3 are shared with FieldProgram (a 3D density's whole
        // point), where the coordinates are always world coordinates and the
        // warp tail is never emitted. Noise3World/Ridge3World stay tape-only:
        // the tape samples PART-LOCAL space by default and has to opt into the
        // world frame, a distinction the field program has never had.
        Noise3,          // 3D fbm over (x, y, z), optional warp tail
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
    // The 3D DENSITY register: positive is solid, the surface is the zero
    // crossing. -1 means the `density` directive was absent, which is the
    // heightfield case and is reconstructed as `height_reg - y` — see
    // is_heightfield below.
    int density_reg = -1;
    // True when the density is exactly `height(x, z) - y`.
    //
    // This is not a second representation; the runtime is 3D either way. It is
    // a RECOGNISER, and it exists because two specialisations downstream are
    // provable equivalences rather than approximations:
    //   * terrain_mesher narrows its Y slab to a few voxels around the sampled
    //     surface, which is only sound when nothing below the surface can be
    //     air and nothing above it can be solid;
    //   * that same mesher takes the density gradient's y component as the
    //     constant -2*eps, because the height term cancels exactly.
    // A general 3D density supports neither, and a heightfield world must not
    // pay for the loss.
    bool is_heightfield = true;
    // Per-op: does this register's value depend on world y? Computed in parse
    // by propagating from `input wy` and the 3D noise ops along the operand
    // edges. Drives the height-register contract check and the column cache.
    std::vector<uint8_t> y_dependent;
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

    // The SURFACE height. Every heightfield consumer in the engine goes through
    // here — scatter placement, biome_at, material_at, slope_at, curvature_at,
    // the VT tape's `s.height`, the mesher's slab bounds — and a general 3D
    // density cannot supply one without a ray march, so a field program is
    // required to declare a y-INDEPENDENT height register (enforced in parse).
    float height_at(float x, float z) const;
    // The 3D density: positive solid, negative air, surface at zero. Equal to
    // height_at(x, z) - y for a heightfield world, and a full evaluation of the
    // density register otherwise.
    float density_at(float x, float y, float z) const;
    bool is_heightfield() const { return prog_.is_heightfield; }
    float slope_at(float x, float z) const;              // |grad h|, central diff eps=0.5

    // ---- Column cache: one (x, z), many y -------------------------------------
    //
    // The voxel mesher walks a whole Y column at one (x, z), and for a cave
    // world that column is hundreds of samples deep. Re-running the entire
    // program per sample would re-evaluate the multi-octave SURFACE fbm — by far
    // the most expensive part of a typical program, and entirely y-independent —
    // once per voxel of depth.
    //
    // So: eval_column() runs every y-independent op once and holds the results;
    // density_at(cache, y) then runs only the y-dependent tail. For a heightfield
    // world the tail is a single subtraction, which is why the general path costs
    // those worlds nothing even before the mesher's is_heightfield shortcut.
    //
    // The cache is caller-owned so it can be hoisted out of the column loop;
    // it holds no pointer back to the runtime, so reusing one against a
    // different FieldRuntime is a caller error, not a dangling reference.
    struct ColumnCache {
        // Sized by the op cap, same uninitialised-on-purpose argument as the
        // register files in terrain_field.cpp: parse admits only backward
        // register refs, so every slot is written before it is read.
        float regs[kMaxSurfaceOps];
        float x = 0.0f, z = 0.0f;
    };
    void eval_column(ColumnCache& cache, float x, float z) const;
    float density_at(ColumnCache& cache, float y) const;

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

    // (No private kMaxOps here. There was a third `= 96` at this spot that
    // nothing referenced — the parser and evaluator both use the file-scope
    // alias of kMaxSurfaceOps in terrain_field.cpp. An unused copy of a
    // mirrored constant is worse than no copy: it reads like the authority.)

    // Evaluate register [0..target] into regs[], using (x, y, z) as world
    // coords. `y` is read only by `input wy` and the 3D noise ops; for a
    // y-independent target (height/moisture/relief) any value gives the same
    // answer, and those callers pass 0.
    void eval_regs(float regs[], int count, float x, float y, float z) const;

    float eval_reg(int target, float x, float y, float z) const;
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

// (kMaxSurfaceOps is declared at the top of this header — FieldRuntime's column
// cache needs it to size a register file, and that class comes first. Its
// authority note lives with the declaration.)

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

// Hard cap on declared habitat channels. Habitat tapes are read per scatter
// CANDIDATE at bake time, so the cost that matters is the crossing, not the
// channel count; this exists to bound the fixed-size output buffer callers put
// on the stack rather than because more would be expensive.
//
// Raised 16 -> 32 for WP3 (docs/streammountain-refactor-implementation-
// 2026-08-09.md §4.2): alpine_ecology.js's formSuitabilities measured as
// plan.select's dominant half (~2x plan.identity, every band and run), so
// its per-form suitability products move into anonymous tape channels
// (12 base + up to 19 form scores). Still a pure stack-budget constant with
// no GPU mirror -- confirmed by re-reading the comment above before this
// edit -- and the only stack cost that scales with it is the fixed-size
// `float ch[kMaxHabitatChannels]` buffers in dsl_bindings.cpp (j_habitatAt,
// j_planCandidates): 64 bytes -> 128 bytes, immaterial next to
// kMaxHabitatOps' 4 KB register-file budget below.
constexpr int kMaxHabitatChannels = 32;

// Op cap for a HABITAT tape, distinct from the surfaces cap.
//
// The 96 that bounds a surfaces tape is a GPU constraint: it mirrors the
// shader's VT_TAPE_MAX_OPS register file and the compositor's arena slot, and
// raising it means touching both. A habitat tape never reaches the GPU -- it is
// evaluated on the CPU at bake time -- so none of that applies.
//
// WHAT ACTUALLY BOUNDS IT, and why it is a compile-time constant at all: the
// evaluator puts the register file on the STACK, one array per channels_at
// call. It has to. A SurfaceRuntime is shared by every bake worker (a dozen
// threads on this machine), so a mutable scratch member would be a data race,
// and heap-allocating per call would cost more than the evaluation it serves.
//
// So the cap is a stack budget, and stack is cheap: 1024 floats is 4 KB per
// call against worker stacks measured in megabytes. It is set here well above
// any ecology anyone has written -- StreamMountain's alpine tape, the largest
// in the tree, is ~100 ops -- so that an author never trims an ecology to fit a
// number, which is exactly what the 96 was forcing before the caps were split.
//
// Raising it further is a one-line change with no downstream mirror to keep in
// sync; the only question to ask is whether 4 KB x (recursion depth of 1) still
// looks small next to the thread's stack.
constexpr int kMaxHabitatOps = 1024;

// Which OUTPUT directives a tape may declare. The op set, the register machine
// and every arithmetic rule are identical either way -- this only decides what
// the program is allowed to emit at the end, and therefore what "a valid tape"
// means.
//
//   Surfaces  material / tint / roughbias / wetness / metallic. Must declare
//             at least one material: a classifier that classifies nothing is a
//             bug, and failing closed on it is a deliberate diagnostic.
//   Habitat   `channel <index> r<reg>` only. Must declare at least one
//             channel, for the same reason.
//
// Two modes rather than one permissive parse, so neither mode's fail-closed
// diagnostic is weakened by the other's existence.
enum class TapeMode { Surfaces, Habitat };

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
    static bool parse(const std::string& text, SurfaceProgram& out,
                      std::string& err, TapeMode mode = TapeMode::Surfaces);

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

    // Habitat mode: the register each declared channel reads, indexed by the
    // channel INDEX the author used. -1 = never declared, which the runtime
    // reports as 0 rather than as an error -- an ecology that does not model
    // (say) flowerPatch should not have to declare it.
    std::vector<int> channel_regs;
    int channel_count = 0;   // declared channels, not the vector's size

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

// ---------------------------------------------------------------------------
// HeightLattice — a rectangle of pre-sampled field heights on a WORLD-ALIGNED
// grid, with bilinear height and central-difference slope reconstruction.
//
// PROTOTYPE (2026-08-10 scatter-cost investigation). Why this exists: a
// habitat tape that reads `height` + `fieldSlope` costs FIVE full field-
// program evaluations per sample (one direct, four inside slope_at's finite
// differences) — measured at 63% of the tape's cost and 93%-of-scatter
// territory. Sampling the field once per lattice node and reconstructing
// amortizes those five evaluations across every sample that lands in the
// rectangle.
//
// Determinism contract: nodes sit at integer multiples of `spacing` in world
// space (the build SNAPS the requested rect outward), so two overlapping
// rectangles built at the same spacing sample identical world positions and
// reconstruct identical values — the result is a pure function of (field,
// spacing, query point), never of the rect that happened to trigger the
// build. Keep spacing FIXED per consumer (not rung-derived): kSurfInFieldSlope
// documents rung-invariance as the reason it exists, and a rung-dependent
// lattice would silently take that away.
//
// What changes vs the exact path: height is the bilinear interpolant (exact
// at nodes), and slope is the |grad| of central differences over 2*spacing
// instead of slope_at's 2*eps = 1 m baseline — a slightly wider low-pass.
// Values move; consumers gate on this being acceptable, which is why the
// scatter binding keeps it behind an opt-in.
// ---------------------------------------------------------------------------
struct HeightLattice {
    float x0 = 0.0f, z0 = 0.0f;   // world position of node (0, 0); multiples of spacing
    float spacing = 1.0f;
    int nx = 0, nz = 0;
    std::vector<float> h;         // nz rows of nx, row-major [k * nx + i]

    // True when (x, z) can be reconstructed with the full stencil: bilinear
    // needs nodes (i..i+1, k..k+1), slope's node gradients reach one further.
    bool contains(float x, float z) const {
        const float u = (x - x0) / spacing, v = (z - z0) / spacing;
        return u >= 1.0f && v >= 1.0f && u < float(nx - 2) && v < float(nz - 2);
    }
    float height(float x, float z) const;   // bilinear; caller checked contains()
    float slope(float x, float z) const;    // |grad h|; caller checked contains()
};

// Sample `field` over the rect [x0, x1] x [z0, z1] snapped outward to the
// world-aligned lattice, with enough margin that contains() holds on the
// whole requested rect.
HeightLattice build_height_lattice(const FieldRuntime& field,
                                   float x0, float z0, float x1, float z1,
                                   float spacing);

// World-frame evaluation context. `field` supplies height/moisture/relief/
// biome; `local_to_world` is a row-major float[16] (null = identity). Passing
// no context at all (null SurfaceWorldContext*) makes every world input
// evaluate to its deterministic fallback constant.
//
// `height_lattice` (optional) short-circuits the `height` and `fieldSlope`
// inputs to lattice reconstruction when the sample is inside it; out-of-bounds
// samples fall back to the exact field-program path. See HeightLattice for the
// approximation contract.
struct SurfaceWorldContext {
    const FieldRuntime* field = nullptr;
    const float* local_to_world = nullptr;
    const HeightLattice* height_lattice = nullptr;
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

    // Habitat channels for one WORLD (x, z) sample. The third reader of the
    // same registers weights_at and appearance_at read -- eval_regs does the
    // work, this only decides which registers come back out.
    //
    // World-space entry point on purpose: a habitat tape is evaluated at a
    // scatter CANDIDATE's world position, not at a mesh vertex, so there is no
    // part-local frame to transform through. `field` supplies height / slope /
    // moisture / relief / biome inputs; null makes every world input read its
    // deterministic fallback, exactly as weights_at does with a null context.
    //
    // out_channels must have room for kMaxHabitatChannels. Undeclared channels
    // read 0.
    void channels_at(float world_x, float world_z, const FieldRuntime* field,
                     float* out_channels) const;
    // Lattice-accelerated variant: `lattice` (may be null = exact path)
    // short-circuits the height / fieldSlope inputs per SurfaceWorldContext's
    // contract. Same registers, same channels out.
    void channels_at(float world_x, float world_z, const FieldRuntime* field,
                     const HeightLattice* lattice, float* out_channels) const;
    int channel_count() const { return prog_.channel_count; }
    // Real op count -- the thing that actually predicts eval cost. The scatter
    // profiler used to print channel_count twice under an "ops" label, which
    // sent a tape-cost investigation chasing a 31-op program when the real one
    // is far larger.
    int op_count() const { return (int)prog_.ops.size(); }

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
