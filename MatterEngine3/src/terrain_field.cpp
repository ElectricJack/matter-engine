// terrain_field.cpp — native field program interpreter for infinite-world terrain.
// Pure CPU module: no JS, no GL, no engine subsystem dependencies.

#include <cassert>
#include <atomic>
#include <chrono>
#include "terrain_field.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <unordered_map>

// ---------------------------------------------------------------------------
// File-scope constants shared between parse (static fn) and eval.
// ---------------------------------------------------------------------------
// The surfaces/field op cap. NOT a second 96: the number lives once, in
// terrain_field.h, because the GPU mirrors it and a silent divergence between
// the parser's bound and the shader's register file is the kind of bug that
// shows up as garbage texels rather than a compile error.
static constexpr int kMaxOps = terrain_field::kMaxSurfaceOps;
// The register file every tape evaluator puts on the stack. Sized for the
// LARGER of the two caps: a surfaces tape is bounded at kMaxOps because the
// GPU mirrors it, a habitat tape at kMaxHabitatOps because nothing does.
//
// It lives on the STACK, one array per eval call, and is deliberately NOT
// zero-initialised. `= {}` on this array made every caller memset 4 KB to run a
// program that typically uses ~31 registers -- pure waste, and it is provably
// unnecessary: SurfaceProgram::parse resolves every operand and every
// `channel` target through resolve_reg(), which rejects any reference that is
// not already emitted (`r >= src_map.size()` -> parse error). So a program can
// only ever reference registers written by EARLIER ops, and the forward
// evaluation loop writes regs[i] at step i. Every register that is read has
// been written in the same pass.
//
// It must stay a stack local rather than a static or a member: a
// SurfaceRuntime is shared by every bake worker, so shared mutable scratch
// would be a data race (see kMaxHabitatOps' note in terrain_field.h).
//
// If a forward-referencing op kind is ever added, this becomes a read of
// uninitialised stack -- the assert in eval_regs is the tripwire for that.
static constexpr int kMaxTapeRegs = terrain_field::kMaxHabitatOps;

// ---------------------------------------------------------------------------
// Internal noise core (file-scope anonymous namespace).
// ---------------------------------------------------------------------------
namespace {

inline uint32_t hash2i(int32_t ix, int32_t iz, uint32_t seed) {
    uint32_t h = (uint32_t)ix * 374761393u + (uint32_t)iz * 668265263u
               + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

inline float rand01(int32_t ix, int32_t iz, uint32_t seed) {
    return (float)(hash2i(ix, iz, seed) & 0xffffff) / (float)0x1000000;
}

inline float smooth5(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }

float value_noise(float x, float z, uint32_t seed) {
    int32_t ix = (int32_t)std::floor(x), iz = (int32_t)std::floor(z);
    float fx = x - ix, fz = z - iz;
    float a = rand01(ix,     iz,     seed);
    float b = rand01(ix + 1, iz,     seed);
    float c = rand01(ix,     iz + 1, seed);
    float d = rand01(ix + 1, iz + 1, seed);
    float u = smooth5(fx), v = smooth5(fz);
    return (a + (b - a) * u) * (1 - v) + (c + (d - c) * u) * v;   // 0..1
}

float fbm2(float x, float z, uint32_t seed, int oct, float gain, float lac,
           float freq, bool ridged) {
    float amp = 1.0f, sum = 0.0f, norm = 0.0f;
    for (int i = 0; i < oct; ++i) {
        float n = value_noise(x * freq, z * freq, seed + (uint32_t)i * 131u);
        n = n * 2.0f - 1.0f;                        // -1..1
        if (ridged) n = 1.0f - std::fabs(n) * 2.0f; // ridge: peaks at lattice
        sum += n * amp; norm += amp;
        amp *= gain; freq *= lac;
    }
    return sum / norm;   // ~-1..1
}

// 3D lattice hash: hash2i's avalanche mix with a third multiply-fold for y.
// hash2i's fold constants are the odd xxHash32 primes (374761393 = PRIME32_5,
// 668265263 = PRIME32_4, 2246822519 = PRIME32_2); y takes the remaining one,
// 3266489917 = PRIME32_3, keeping the whole lattice hash one constant family.
// Bit-exact uint arithmetic — the determinism anchor for the GPU twin.
inline uint32_t hash3i(int32_t ix, int32_t iy, int32_t iz, uint32_t seed) {
    uint32_t h = (uint32_t)ix * 374761393u + (uint32_t)iy * 3266489917u
               + (uint32_t)iz * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

inline float rand01_3(int32_t ix, int32_t iy, int32_t iz, uint32_t seed) {
    return (float)(hash3i(ix, iy, iz, seed) & 0xffffff) / (float)0x1000000;
}

float value_noise3(float x, float y, float z, uint32_t seed) {
    int32_t ix = (int32_t)std::floor(x), iy = (int32_t)std::floor(y),
            iz = (int32_t)std::floor(z);
    float fx = x - ix, fy = y - iy, fz = z - iz;
    // Trilinear over the 8 cell corners, same smoothstep fade as value_noise.
    float c000 = rand01_3(ix,     iy,     iz,     seed);
    float c100 = rand01_3(ix + 1, iy,     iz,     seed);
    float c010 = rand01_3(ix,     iy + 1, iz,     seed);
    float c110 = rand01_3(ix + 1, iy + 1, iz,     seed);
    float c001 = rand01_3(ix,     iy,     iz + 1, seed);
    float c101 = rand01_3(ix + 1, iy,     iz + 1, seed);
    float c011 = rand01_3(ix,     iy + 1, iz + 1, seed);
    float c111 = rand01_3(ix + 1, iy + 1, iz + 1, seed);
    float u = smooth5(fx), v = smooth5(fy), w = smooth5(fz);
    float x00 = c000 + (c100 - c000) * u;
    float x10 = c010 + (c110 - c010) * u;
    float x01 = c001 + (c101 - c001) * u;
    float x11 = c011 + (c111 - c011) * u;
    float y0 = x00 + (x10 - x00) * v;
    float y1 = x01 + (x11 - x01) * v;
    return y0 + (y1 - y0) * w;   // 0..1
}

float fbm3(float x, float y, float z, uint32_t seed, int oct, float gain,
           float lac, float freq, bool ridged) {
    float amp = 1.0f, sum = 0.0f, norm = 0.0f;
    for (int i = 0; i < oct; ++i) {
        float n = value_noise3(x * freq, y * freq, z * freq,
                               seed + (uint32_t)i * 131u);
        n = n * 2.0f - 1.0f;                        // -1..1
        if (ridged) n = 1.0f - std::fabs(n) * 2.0f; // ridge: peaks at lattice
        sum += n * amp; norm += amp;
        amp *= gain; freq *= lac;
    }
    return sum / norm;   // ~-1..1
}

// Tokenize a line by whitespace. Returns tokens.
std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> toks;
    std::istringstream ss(line);
    std::string tok;
    while (ss >> tok) toks.push_back(tok);
    return toks;
}

// Parse "r<N>" -> register index, or return -1 if not a register ref.
int parse_reg(const std::string& s) {
    if (s.size() >= 2 && s[0] == 'r') {
        try { return std::stoi(s.substr(1)); }
        catch (...) {}
    }
    return -1;
}

// Parse float literal (throws on failure).
float parse_float(const std::string& s) {
    return std::stof(s);
}

} // namespace

// ---------------------------------------------------------------------------
// FieldProgram::parse
// ---------------------------------------------------------------------------
namespace terrain_field {

bool FieldProgram::parse(const std::string& text, FieldProgram& out, std::string& err) {
    out = FieldProgram();
    out.text_ = text;

    // Split into lines.
    std::vector<std::string> lines;
    {
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            // Strip trailing \r
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) lines.push_back(line);
        }
    }

    bool have_height   = false;
    bool have_moisture = false;
    bool have_relief   = false;
    bool have_seaLevel = false;
    bool have_biome    = false;

    for (const auto& line : lines) {
        auto toks = tokenize(line);
        if (toks.empty()) continue;

        const std::string& op = toks[0];

        // ---- Directives (order-independent, after ops by convention) ----

        if (op == "height") {
            if (toks.size() < 2) { err = "height: missing register"; return false; }
            int r = parse_reg(toks[1]);
            if (r < 0) { err = "height: expected register ref"; return false; }
            if (r >= (int)out.ops.size()) { err = "height: forward register ref"; return false; }
            out.height_reg = r;
            have_height = true;
            continue;
        }
        if (op == "moisture") {
            if (toks.size() < 2) { err = "moisture: missing register"; return false; }
            int r = parse_reg(toks[1]);
            if (r < 0) { err = "moisture: expected register ref"; return false; }
            if (r >= (int)out.ops.size()) { err = "moisture: forward register ref"; return false; }
            out.moisture_reg = r;
            have_moisture = true;
            continue;
        }
        if (op == "relief") {
            if (toks.size() < 2) { err = "relief: missing register"; return false; }
            int r = parse_reg(toks[1]);
            if (r < 0) { err = "relief: expected register ref"; return false; }
            if (r >= (int)out.ops.size()) { err = "relief: forward register ref"; return false; }
            out.relief_reg = r;
            have_relief = true;
            continue;
        }
        if (op == "seaLevel") {
            if (toks.size() < 2) { err = "seaLevel: missing value"; return false; }
            try { out.sea_level = parse_float(toks[1]); }
            catch (...) { err = "seaLevel: invalid float"; return false; }
            have_seaLevel = true;
            continue;
        }
        if (op == "biome") {
            if (toks.size() < 3) { err = "biome: missing thresholds"; return false; }
            try {
                out.mount_relief_thresh = parse_float(toks[1]);
                out.rocky_moist_thresh  = parse_float(toks[2]);
            } catch (...) { err = "biome: invalid float"; return false; }
            have_biome = true;
            continue;
        }

        // ---- Op lines ----

        int op_idx = (int)out.ops.size();
        if (op_idx >= kMaxOps) {
            err = "too many ops (max 64)";
            return false;
        }

        Op o{};

        auto require_reg = [&](int tok_idx, int& reg_out) -> bool {
            if (tok_idx >= (int)toks.size()) {
                err = std::string(op) + ": missing operand";
                return false;
            }
            int r = parse_reg(toks[tok_idx]);
            if (r < 0) { err = std::string(op) + ": expected register ref at token " + std::to_string(tok_idx); return false; }
            if (r >= op_idx) { err = std::string(op) + ": forward register ref r" + std::to_string(r); return false; }
            reg_out = r;
            return true;
        };

        auto require_float = [&](int tok_idx, float& f_out) -> bool {
            if (tok_idx >= (int)toks.size()) {
                err = std::string(op) + ": missing float literal";
                return false;
            }
            try { f_out = parse_float(toks[tok_idx]); return true; }
            catch (...) { err = std::string(op) + ": invalid float at token " + std::to_string(tok_idx); return false; }
        };

        auto require_uint = [&](int tok_idx, uint32_t& u_out) -> bool {
            if (tok_idx >= (int)toks.size()) {
                err = std::string(op) + ": missing uint literal";
                return false;
            }
            try { u_out = (uint32_t)std::stoul(toks[tok_idx]); return true; }
            catch (...) { err = std::string(op) + ": invalid uint at token " + std::to_string(tok_idx); return false; }
        };

        auto require_int = [&](int tok_idx, int& i_out) -> bool {
            if (tok_idx >= (int)toks.size()) {
                err = std::string(op) + ": missing int literal";
                return false;
            }
            try { i_out = std::stoi(toks[tok_idx]); return true; }
            catch (...) { err = std::string(op) + ": invalid int at token " + std::to_string(tok_idx); return false; }
        };

        // reg-or-float helper: if token starts with 'r' try reg, else float literal
        // For ops that accept a register or float for the x/last operand:
        auto reg_or_float_as_reg = [&](int tok_idx, int& reg_out, float& imm_out, bool& is_imm) -> bool {
            if (tok_idx >= (int)toks.size()) {
                err = std::string(op) + ": missing operand";
                return false;
            }
            int r = parse_reg(toks[tok_idx]);
            if (r >= 0) {
                if (r >= op_idx) { err = std::string(op) + ": forward register ref"; return false; }
                reg_out = r; is_imm = false; return true;
            }
            // float literal: emit implicit const op first
            if (op_idx + 1 >= kMaxOps) { err = "too many ops (implicit const would exceed the op cap)"; return false; }
            try { imm_out = parse_float(toks[tok_idx]); }
            catch (...) { err = std::string(op) + ": invalid float"; return false; }
            is_imm = true;
            return true;
        };

        if (op == "input") {
            // World-coordinate access. The field is a pure function of (x, z),
            // so those are the only two inputs that exist here — `input wx` and
            // `input wz` and nothing else. The surfaces() tape's other input
            // names (ly/ny/slope/height/...) are either undefined in this
            // context or would make the field self-referential, so they are
            // rejected by name rather than silently reading 0.
            //
            // Why the field needs coordinates at all: every other op is
            // translation-covariant noise, which can only ever produce
            // statistically-uniform terrain. An AUTHORED shape at an authored
            // place — a dome at (cx, cz), a road, a crater — is not expressible
            // without them. Op::Input already existed for the surfaces() tape;
            // this only opens two of its codes to FieldProgram, and eval_regs
            // already has x and z in hand.
            if (toks.size() < 2) { err = "input: missing name"; return false; }
            if      (toks[1] == "wx") o.oct = kSurfInWorldX;
            else if (toks[1] == "wz") o.oct = kSurfInWorldZ;
            else {
                err = "input: the field program accepts only 'wx' and 'wz', got '"
                    + toks[1] + "'";
                return false;
            }
            o.kind = Op::Input;
        }
        else if (op == "const") {
            o.kind = Op::Const;
            if (!require_float(1, o.f0)) return false;
        }
        else if (op == "noise2") {
            // noise2 seed freq oct gain lac
            o.kind = Op::Noise2;
            if (!require_uint(1, o.seed))   return false;
            if (!require_float(2, o.f0))    return false; // freq
            if (!require_int(3, o.oct))     return false;
            if (!require_float(4, o.f1))    return false; // gain
            if (!require_float(5, o.f2))    return false; // lac
        }
        else if (op == "ridge2") {
            // ridge2 seed freq oct gain lac
            o.kind = Op::Ridge2;
            if (!require_uint(1, o.seed))   return false;
            if (!require_float(2, o.f0))    return false; // freq
            if (!require_int(3, o.oct))     return false;
            if (!require_float(4, o.f1))    return false; // gain
            if (!require_float(5, o.f2))    return false; // lac
        }
        else if (op == "warp2") {
            // warp2 rSrc seed freq strength
            o.kind = Op::Warp2;
            if (!require_reg(1, o.a))       return false; // rSrc
            if (!require_uint(2, o.seed))   return false;
            if (!require_float(3, o.f0))    return false; // freq
            if (!require_float(4, o.f1))    return false; // strength
        }
        else if (op == "add") {
            o.kind = Op::Add;
            if (!require_reg(1, o.a)) return false;
            if (!require_reg(2, o.b)) return false;
        }
        else if (op == "sub") {
            o.kind = Op::Sub;
            if (!require_reg(1, o.a)) return false;
            if (!require_reg(2, o.b)) return false;
        }
        else if (op == "abs") {
            o.kind = Op::Abs;
            if (!require_reg(1, o.a)) return false;
        }
        else if (op == "oneminus") {
            o.kind = Op::OneMinus;
            if (!require_reg(1, o.a)) return false;
        }
        else if (op == "pow") {
            // pow a e — e is a float-literal exponent; base is clamped to >= 0
            // (GLSL pow is undefined there; a clamp keeps the tape total).
            o.kind = Op::Pow;
            if (!require_reg(1, o.a))    return false;
            if (!require_float(2, o.f0)) return false;
        }
        else if (op == "mul") {
            o.kind = Op::Mul;
            if (!require_reg(1, o.a)) return false;
            if (!require_reg(2, o.b)) return false;
        }
        else if (op == "min") {
            o.kind = Op::Min;
            if (!require_reg(1, o.a)) return false;
            if (!require_reg(2, o.b)) return false;
        }
        else if (op == "max") {
            o.kind = Op::Max;
            if (!require_reg(1, o.a)) return false;
            if (!require_reg(2, o.b)) return false;
        }
        else if (op == "clamp") {
            // clamp a lo hi  — lo/hi are float literals
            o.kind = Op::Clamp;
            if (!require_reg(1, o.a))   return false;
            if (!require_float(2, o.f0)) return false; // lo
            if (!require_float(3, o.f1)) return false; // hi
        }
        else if (op == "blend") {
            // blend a b t  — a, b, t are register refs
            o.kind = Op::Blend;
            if (!require_reg(1, o.a)) return false;
            if (!require_reg(2, o.b)) return false;
            if (!require_reg(3, o.c)) return false;
        }
        else if (op == "smoothstep") {
            // smoothstep e0 e1 x  — e0/e1 float literals, x reg or literal
            o.kind = Op::Smoothstep;
            if (!require_float(1, o.f0)) return false; // e0
            if (!require_float(2, o.f1)) return false; // e1
            // x: reg or literal
            {
                bool is_imm = false;
                float imm_val = 0.0f;
                int reg_idx = -1;
                if (!reg_or_float_as_reg(3, reg_idx, imm_val, is_imm)) return false;
                if (is_imm) {
                    // emit an implicit const op
                    Op cst{};
                    cst.kind = Op::Const;
                    cst.f0 = imm_val;
                    out.ops.push_back(cst);
                    op_idx = (int)out.ops.size();  // now referencing the const just added
                    o.a = op_idx - 1;              // x register = the implicit const
                } else {
                    o.a = reg_idx;
                }
            }
        }
        else {
            err = std::string("unknown op: ") + op;
            return false;
        }

        out.ops.push_back(o);
    }

    // Validate that all required directives are present.
    if (!have_height)   { err = "missing 'height' directive";   return false; }
    if (!have_moisture) { err = "missing 'moisture' directive"; return false; }
    if (!have_relief)   { err = "missing 'relief' directive";   return false; }
    if (!have_seaLevel) { err = "missing 'seaLevel' directive"; return false; }
    if (!have_biome)    { err = "missing 'biome' directive";    return false; }

    return true;
}

// ---------------------------------------------------------------------------
// FieldProgram::hash — FNV-1a 64 over the canonical text bytes.
// ---------------------------------------------------------------------------
uint64_t FieldProgram::hash() const {
    constexpr uint64_t offset = 14695981039346656037ULL;
    constexpr uint64_t prime  = 1099511628211ULL;
    uint64_t h = offset;
    for (unsigned char c : text_) {
        h ^= (uint64_t)c;
        h *= prime;
    }
    return h;
}

// ---------------------------------------------------------------------------
// FieldRuntime
// ---------------------------------------------------------------------------
FieldRuntime::FieldRuntime(FieldProgram p)
    : prog_(std::move(p))
{}

// Evaluate all registers 0..(count-1) into regs[] for world position (x, z).
void FieldRuntime::eval_regs(float regs[], int count, float x, float z) const {
    const auto& ops = prog_.ops;
    for (int i = 0; i < count && i < (int)ops.size(); ++i) {
        const Op& o = ops[i];
        switch (o.kind) {
        case Op::Const:
            regs[i] = o.f0;
            break;
        case Op::Noise2:
            // noise2 seed freq oct gain lac
            regs[i] = fbm2(x, z, o.seed, o.oct, o.f1, o.f2, o.f0, false);
            break;
        case Op::Ridge2:
            regs[i] = fbm2(x, z, o.seed, o.oct, o.f1, o.f2, o.f0, true);
            break;
        case Op::Warp2: {
            // dx = (value_noise(x*freq, z*freq, seed) * 2 - 1) * strength
            // dz = (value_noise(x*freq, z*freq, seed^0x9e37) * 2 - 1) * strength
            float freq = o.f0, strength = o.f1;
            float dx = (value_noise(x * freq, z * freq, o.seed)              * 2.0f - 1.0f) * strength;
            float dz = (value_noise(x * freq, z * freq, o.seed ^ 0x9e37u)   * 2.0f - 1.0f) * strength;
            // Evaluate rSrc at displaced coords using a scratch register file.
            // Uninitialised on purpose, same argument as kMaxTapeRegs: the
            // parser admits only backward register refs, and eval_regs writes
            // regs[i] for every i < count before anything reads it, so no
            // uninitialised slot is ever consumed. Zeroing all kMaxOps floats
            // here ran once per warp2 op per field evaluation (StreamMountain
            // has two warp2 ops, and the habitat tape triggers five field
            // evaluations per scatter candidate).
            float scratch[kMaxOps];
            eval_regs(scratch, o.a + 1, x + dx, z + dz);
            regs[i] = scratch[o.a];
            break;
        }
        case Op::Add:
            regs[i] = regs[o.a] + regs[o.b];
            break;
        case Op::Sub:
            regs[i] = regs[o.a] - regs[o.b];
            break;
        case Op::Mul:
            regs[i] = regs[o.a] * regs[o.b];
            break;
        case Op::Min:
            regs[i] = std::min(regs[o.a], regs[o.b]);
            break;
        case Op::Max:
            regs[i] = std::max(regs[o.a], regs[o.b]);
            break;
        case Op::Clamp:
            regs[i] = std::max(o.f0, std::min(o.f1, regs[o.a]));
            break;
        case Op::Blend: {
            float t = regs[o.c];
            regs[i] = regs[o.a] * (1.0f - t) + regs[o.b] * t;
            break;
        }
        case Op::Smoothstep: {
            float e0 = o.f0, e1 = o.f1;
            float xv = regs[o.a];
            float t = std::max(0.0f, std::min(1.0f, (xv - e0) / (e1 - e0)));
            regs[i] = t * t * (3.0f - 2.0f * t);
            break;
        }
        case Op::Abs:
            regs[i] = std::fabs(regs[o.a]);
            break;
        case Op::OneMinus:
            regs[i] = 1.0f - regs[o.a];
            break;
        case Op::Pow:
            regs[i] = std::pow(std::max(regs[o.a], 0.0f), o.f0);
            break;
        case Op::Input:
            // FieldProgram::parse admits only wx and wz here, so the else IS
            // wz and there is no unreachable third case to guard.
            regs[i] = (o.oct == kSurfInWorldX) ? x : z;
            break;
        case Op::Noise2World:
        case Op::Ridge2World:
        case Op::FieldCurv:
        case Op::Noise3:
        case Op::Ridge3:
        case Op::Noise3World:
        case Op::Ridge3World:
        case Op::Fract:
            // surfaces()-tape-only ops; FieldProgram::parse never emits them.
            regs[i] = 0.0f;
            break;
        }
    }
}

float FieldRuntime::eval_reg(int target, float x, float z) const {
    float regs[kMaxTapeRegs];   // uninitialised on purpose -- see kMaxTapeRegs
    eval_regs(regs, target + 1, x, z);
    return regs[target];
}

// Diagnostic counters for height_at, the field-program evaluation that the
// habitat tape triggers FIVE times per scatter candidate (once for `height`,
// four more inside slope_at's finite differences). Measured at ~93% of the
// scatter cost, so this is the term any fieldSlope change has to beat.
// Always-on counting is a single relaxed increment; the timer is the caller's
// business (field_probe_ns is only accumulated when the caller opts in).
std::atomic<unsigned long long> g_field_height_calls{0};
std::atomic<unsigned long long> g_field_height_ns{0};
bool g_field_probe_timing = false;

float FieldRuntime::height_at(float x, float z) const {
    if (!g_field_probe_timing) {
        g_field_height_calls.fetch_add(1, std::memory_order_relaxed);
        return eval_reg(prog_.height_reg, x, z);
    }
    const auto t0 = std::chrono::steady_clock::now();
    const float v = eval_reg(prog_.height_reg, x, z);
    g_field_height_calls.fetch_add(1, std::memory_order_relaxed);
    g_field_height_ns.fetch_add((unsigned long long)std::chrono::duration_cast<
        std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
        std::memory_order_relaxed);
    return v;
}

float FieldRuntime::density_at(float x, float y, float z) const {
    return height_at(x, z) - y;
}

float FieldRuntime::slope_at(float x, float z) const {
    constexpr float eps = 0.5f;
    float hx0 = height_at(x - eps, z);
    float hx1 = height_at(x + eps, z);
    float hz0 = height_at(x, z - eps);
    float hz1 = height_at(x, z + eps);
    float gx = (hx1 - hx0) / (2.0f * eps);
    float gz = (hz1 - hz0) / (2.0f * eps);
    return std::sqrt(gx * gx + gz * gz);
}

// ---------------------------------------------------------------------------
// HeightLattice — see terrain_field.h for the determinism/approximation
// contract. Reconstruction is deliberately boring: bilinear height, bilinear-
// blended central-difference gradient for slope.
// ---------------------------------------------------------------------------
float HeightLattice::height(float x, float z) const {
    const float u = (x - x0) / spacing, v = (z - z0) / spacing;
    const int i = (int)u, k = (int)v;              // contains() => in range
    const float fu = u - float(i), fv = v - float(k);
    const float* row0 = &h[size_t(k) * size_t(nx)];
    const float* row1 = row0 + nx;
    const float a = row0[i] + (row0[i + 1] - row0[i]) * fu;
    const float b = row1[i] + (row1[i + 1] - row1[i]) * fu;
    return a + (b - a) * fv;
}

float HeightLattice::slope(float x, float z) const {
    const float u = (x - x0) / spacing, v = (z - z0) / spacing;
    const int i = (int)u, k = (int)v;
    const float fu = u - float(i), fv = v - float(k);
    const float inv2s = 1.0f / (2.0f * spacing);
    // Central-difference gradient at the four surrounding nodes, then the same
    // bilinear blend height() uses. contains() guarantees the +-1 reach.
    auto node = [&](int ii, int kk) -> float {
        return h[size_t(kk) * size_t(nx) + size_t(ii)];
    };
    auto gxn = [&](int ii, int kk) -> float {
        return (node(ii + 1, kk) - node(ii - 1, kk)) * inv2s;
    };
    auto gzn = [&](int ii, int kk) -> float {
        return (node(ii, kk + 1) - node(ii, kk - 1)) * inv2s;
    };
    const float gx0 = gxn(i, k)     + (gxn(i + 1, k)     - gxn(i, k))     * fu;
    const float gx1 = gxn(i, k + 1) + (gxn(i + 1, k + 1) - gxn(i, k + 1)) * fu;
    const float gx  = gx0 + (gx1 - gx0) * fv;
    const float gz0 = gzn(i, k)     + (gzn(i + 1, k)     - gzn(i, k))     * fu;
    const float gz1 = gzn(i, k + 1) + (gzn(i + 1, k + 1) - gzn(i, k + 1)) * fu;
    const float gz  = gz0 + (gz1 - gz0) * fv;
    return std::sqrt(gx * gx + gz * gz);
}

HeightLattice build_height_lattice(const FieldRuntime& field,
                                   float x0, float z0, float x1, float z1,
                                   float spacing) {
    HeightLattice lat;
    lat.spacing = spacing;
    // Snap OUTWARD to world-aligned node indices, +-2 nodes of margin: one so
    // bilinear cells cover the rect edge, one more for slope's node-gradient
    // reach. Node index space keeps the world-alignment exact.
    const int gi0 = (int)std::floor(x0 / spacing) - 2;
    const int gi1 = (int)std::ceil (x1 / spacing) + 2;
    const int gk0 = (int)std::floor(z0 / spacing) - 2;
    const int gk1 = (int)std::ceil (z1 / spacing) + 2;
    lat.x0 = float(gi0) * spacing;
    lat.z0 = float(gk0) * spacing;
    lat.nx = gi1 - gi0 + 1;
    lat.nz = gk1 - gk0 + 1;
    lat.h.resize(size_t(lat.nx) * size_t(lat.nz));
    for (int k = 0; k < lat.nz; ++k)
        for (int i = 0; i < lat.nx; ++i)
            lat.h[size_t(k) * size_t(lat.nx) + size_t(i)] =
                field.height_at(float(gi0 + i) * spacing,
                                float(gk0 + k) * spacing);
    return lat;
}

float FieldRuntime::curvature_at(float x, float z, float radius) const {
    const float e = std::max(radius, 0.25f);
    const float ring = (height_at(x + e, z) + height_at(x - e, z) +
                        height_at(x, z + e) + height_at(x, z - e)) * 0.25f;
    return ring - height_at(x, z);   // + concave (below ring), - convex
}

float FieldRuntime::moisture_at(float x, float z) const {
    return eval_reg(prog_.moisture_reg, x, z);
}

float FieldRuntime::relief_at(float x, float z) const {
    return eval_reg(prog_.relief_reg, x, z);
}

FieldRuntime::Biome FieldRuntime::biome_at(float x, float z) const {
    if (height_at(x, z) < prog_.sea_level)             return Ocean;
    if (relief_at(x, z) >= prog_.mount_relief_thresh)  return Mountains;
    if (moisture_at(x, z) < prog_.rocky_moist_thresh)  return Foothills;
    return Meadow;
}

FieldRuntime::Material FieldRuntime::material_at(float x, float z) const {
    if (slope_at(x, z) > 1.0f) return MatRock;
    Biome b = biome_at(x, z);
    if (b == Mountains) return height_at(x, z) > 100.0f ? MatSnow : MatRock;
    if (b == Foothills) return MatDirt;
    if (b == Ocean)     return MatDirt;   // sea floor
    return MatGrass;
}

// ---------------------------------------------------------------------------
// SurfaceProgram — surfaces() classifier tape (chart-VT Phase 4, contract C4).
// ---------------------------------------------------------------------------

// (kMaxOps IS kMaxSurfaceOps now — see the alias at the top of this file. The
// assert that used to guard the two literals against drift is gone with the
// second literal. The one mirror that still cannot be checked by the compiler
// is VT_TAPE_MAX_OPS in shaders_vk/vt_surface_tape.glsl; raising the cap means
// raising it there too, and rebuilding SPIR-V — `make -C MatterEngine3` alone
// does not.)

namespace {

// input-op name <-> code table (must stay in sync with SurfaceInput).
const char* const kSurfaceInputNames[kSurfInCount] = {
    "lx", "ly", "lz", "ny", "slope",
    "wx", "wy", "wz", "height", "moisture", "relief", "biome", "fslope",
};

int surface_input_code(const std::string& name) {
    for (int i = 0; i < kSurfInCount; ++i)
        if (name == kSurfaceInputNames[i]) return i;
    return -1;
}

// 3D fbm for one tape op, applying its optional domain warp first: the sample
// point is displaced per axis before the fbm — p' = p + amp * (n(p·wfreq) · 2
// − 1) — where n is single-octave 3D value noise and the three axis seeds
// derive from the explicit wseed token (wseed, wseed^0x9e37, wseed^0x7f4a —
// the same axis-decorrelation xors warp2 uses).
float fbm3_op(const Op& o, float x, float y, float z, bool ridged) {
    if (o.warp) {
        const float sx = x * o.wf0, sy = y * o.wf0, sz = z * o.wf0;
        x += o.wf1 * (value_noise3(sx, sy, sz, o.wseed)           * 2.0f - 1.0f);
        y += o.wf1 * (value_noise3(sx, sy, sz, o.wseed ^ 0x9e37u) * 2.0f - 1.0f);
        z += o.wf1 * (value_noise3(sx, sy, sz, o.wseed ^ 0x7f4au) * 2.0f - 1.0f);
    }
    return fbm3(x, y, z, o.seed, o.oct, o.f1, o.f2, o.f0, ridged);
}

} // namespace

// P2 accessors (see terrain_field.h): thin wrappers over the file-static
// noise core so the GPU tape packer / tests can evaluate a single op at an
// explicit point with the exact CPU float path.
float surface_op_fbm2(const Op& op, float x, float z, bool ridged) {
    return fbm2(x, z, op.seed, op.oct, op.f1, op.f2, op.f0, ridged);
}
float surface_op_fbm3(const Op& op, float x, float y, float z, bool ridged) {
    return fbm3_op(op, x, y, z, ridged);
}

bool SurfaceProgram::parse(const std::string& text, SurfaceProgram& out,
                           std::string& err, TapeMode mode) {
    out = SurfaceProgram();
    out.text_ = text;

    std::vector<std::string> lines;
    {
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) lines.push_back(line);
        }
    }

    // Register refs in the text are SOURCE ordinals (one per op line, in
    // order). Deduplicating identical `const` lines means source ordinal and
    // emitted-op index diverge, so every ref goes through src_map.
    std::vector<int> src_map;                       // source ordinal -> op index
    std::unordered_map<uint32_t, int> const_pool;   // float bits -> op index
    auto resolve_reg = [&](const std::string& tok) -> int {
        const int r = parse_reg(tok);
        if (r < 0 || r >= (int)src_map.size()) return -1;
        return src_map[r];
    };

    for (const auto& line : lines) {
        auto toks = tokenize(line);
        if (toks.empty()) continue;
        const std::string& op = toks[0];

        // ---- `channel <index> r<reg>` directive (habitat tapes) ----
        // The habitat analogue of `material`: same backward-ref rule, same
        // source-ordinal resolution, and like a material it is an OUTPUT
        // directive so it never perturbs register numbering.
        if (op == "channel") {
            if (mode != TapeMode::Habitat) {
                err = "channel: only a habitat tape declares channels";
                return false;
            }
            if (toks.size() < 3) { err = "channel: expected <index> r<reg>"; return false; }
            int index = -1;
            try { index = std::stoi(toks[1]); }
            catch (...) { err = "channel: invalid index"; return false; }
            if (index < 0 || index >= kMaxHabitatChannels) {
                err = "channel: index out of 0.." +
                      std::to_string(kMaxHabitatChannels - 1);
                return false;
            }
            const int reg = resolve_reg(toks[2]);
            if (reg < 0) {
                err = "channel: expected backward register ref"; return false;
            }
            if (out.channel_regs.empty())
                out.channel_regs.assign(kMaxHabitatChannels, -1);
            if (out.channel_regs[index] >= 0) {
                err = "channel: index " + toks[1] + " declared twice";
                return false;
            }
            out.channel_regs[index] = reg;
            ++out.channel_count;
            continue;
        }

        // ---- `material <handle> r<reg>` directive ----
        if (op == "material") {
            if (mode != TapeMode::Surfaces) {
                err = "material: a habitat tape declares channels, not materials";
                return false;
            }
            if (toks.size() < 3) { err = "material: expected <handle> r<reg>"; return false; }
            MaterialSlot slot;
            try { slot.handle = std::stoi(toks[1]); }
            catch (...) { err = "material: invalid handle"; return false; }
            if (slot.handle < 0) { err = "material: negative handle"; return false; }
            slot.reg = resolve_reg(toks[2]);
            if (slot.reg < 0) {
                err = "material: expected backward register ref"; return false;
            }
            for (const MaterialSlot& existing : out.materials) {
                if (existing.handle == slot.handle) {
                    err = "material: handle " + toks[1] + " declared twice";
                    return false;
                }
            }
            if ((int)out.materials.size() >= kMaxSurfaceMaterials) {
                err = "material: more than " +
                      std::to_string(kMaxSurfaceMaterials) +
                      " declared materials";
                return false;
            }
            out.materials.push_back(slot);
            continue;
        }

        // ---- P3 appearance directives (spec §5) ----
        // `tint rR rG rB`, `roughbias rN`, `wetness rN`: register refs exactly
        // like a material's (backward, source ordinals), at most one directive
        // of each kind. Token counts are EXACT — a 2-token `tint` is a typo,
        // not a shorthand. Clamping happens at evaluation, not here: the tape
        // may compute any value and the lane's documented range is the
        // contract at the point of use (SurfaceRuntime::appearance_at and the
        // mode-3 shader apply the identical clamps).
        if (op == "tint" || op == "roughbias" || op == "wetness" ||
            op == "metallic") {
            const bool is_tint = (op == "tint");
            const size_t want = is_tint ? 4u : 2u;
            if (toks.size() != want) {
                err = is_tint ? std::string("tint: expected 'tint rR rG rB'")
                              : op + ": expected '" + op + " rN'";
                return false;
            }
            const bool dup = is_tint ? out.has_tint()
                           : (op == "roughbias") ? out.has_rough_bias()
                           : (op == "wetness")   ? out.has_wetness()
                                                 : out.has_metallic();
            if (dup) {
                err = op + ": declared twice (at most one per tape)";
                return false;
            }
            int regs[3] = {-1, -1, -1};
            for (size_t t = 1; t < want; ++t) {
                regs[t - 1] = resolve_reg(toks[t]);
                if (regs[t - 1] < 0) {
                    err = op + ": expected backward register ref";
                    return false;
                }
            }
            if (is_tint) {
                out.tint_reg[0] = regs[0];
                out.tint_reg[1] = regs[1];
                out.tint_reg[2] = regs[2];
            } else if (op == "roughbias") {
                out.rough_bias_reg = regs[0];
            } else if (op == "wetness") {
                out.wetness_reg = regs[0];
            } else {
                out.metallic_reg = regs[0];
            }
            continue;
        }

        // ---- op lines (same discipline as FieldProgram::parse) ----
        Op o{};

        auto require_reg = [&](int tok_idx, int& reg_out) -> bool {
            if (tok_idx >= (int)toks.size()) {
                err = std::string(op) + ": missing operand"; return false;
            }
            int r = resolve_reg(toks[tok_idx]);
            if (r < 0) {
                err = std::string(op) + ": expected backward register ref";
                return false;
            }
            reg_out = r;
            return true;
        };
        auto require_float = [&](int tok_idx, float& f_out) -> bool {
            if (tok_idx >= (int)toks.size()) {
                err = std::string(op) + ": missing float literal"; return false;
            }
            try { f_out = parse_float(toks[tok_idx]); return true; }
            catch (...) { err = std::string(op) + ": invalid float"; return false; }
        };
        auto require_uint = [&](int tok_idx, uint32_t& u_out) -> bool {
            if (tok_idx >= (int)toks.size()) {
                err = std::string(op) + ": missing uint literal"; return false;
            }
            try { u_out = (uint32_t)std::stoul(toks[tok_idx]); return true; }
            catch (...) { err = std::string(op) + ": invalid uint"; return false; }
        };
        auto require_int = [&](int tok_idx, int& i_out) -> bool {
            if (tok_idx >= (int)toks.size()) {
                err = std::string(op) + ": missing int literal"; return false;
            }
            try { i_out = std::stoi(toks[tok_idx]); return true; }
            catch (...) { err = std::string(op) + ": invalid int"; return false; }
        };

        if (op == "input") {
            if (toks.size() < 2) { err = "input: missing name"; return false; }
            const int code = surface_input_code(toks[1]);
            if (code < 0) { err = "input: unknown name '" + toks[1] + "'"; return false; }
            o.kind = Op::Input;
            o.oct = code;
            out.input_mask_ |= 1u << code;
            if (code >= kSurfaceInputWorldFirst) out.uses_world_inputs_ = true;
        }
        else if (op == "const") {
            o.kind = Op::Const;
            if (!require_float(1, o.f0)) return false;
            // Dedup: an identical earlier const owns this source ordinal.
            uint32_t bits = 0;
            std::memcpy(&bits, &o.f0, sizeof(bits));
            auto it = const_pool.find(bits);
            if (it != const_pool.end()) {
                src_map.push_back(it->second);
                continue;
            }
            const_pool.emplace(bits, (int)out.ops.size());
        }
        else if (op == "noise2" || op == "ridge2" ||
                 op == "noise2w" || op == "ridge2w") {
            // noise2|ridge2 seed freq oct gain lac — over part-local (x, z).
            // noise2w|ridge2w — same params over WORLD (x, z); world-anchored
            // variants only (fallback context pins them to world (0, 0)).
            const bool is_world = (op == "noise2w" || op == "ridge2w");
            const bool is_ridge = (op == "ridge2" || op == "ridge2w");
            o.kind = is_world ? (is_ridge ? Op::Ridge2World : Op::Noise2World)
                              : (is_ridge ? Op::Ridge2 : Op::Noise2);
            if (!require_uint(1, o.seed)) return false;
            if (!require_float(2, o.f0))  return false; // freq
            if (!require_int(3, o.oct))   return false;
            if (!require_float(4, o.f1))  return false; // gain
            if (!require_float(5, o.f2))  return false; // lac
            if (is_world) {
                out.uses_world_inputs_ = true;
                out.input_mask_ |= (1u << kSurfInWorldX) | (1u << kSurfInWorldZ);
            }
        }
        else if (op == "noise3" || op == "ridge3" ||
                 op == "noise3w" || op == "ridge3w") {
            // noise3|ridge3 seed freq oct gain lac [wseed wfreq wamp] — 3D fbm
            // over part-local (x, y, z), so it varies along vertical surfaces
            // where the 2D pair smears. noise3w|ridge3w — same params over
            // WORLD (x, y, z); world-anchored variants only (fallback context
            // pins them to world (0, 0, 0)). The optional 3-token tail domain-
            // warps the op's own sample point before the fbm (see fbm3_op) —
            // the tape's substitute for the field program's stateful warp2.
            const bool is_world = (op == "noise3w" || op == "ridge3w");
            const bool is_ridge = (op == "ridge3" || op == "ridge3w");
            o.kind = is_world ? (is_ridge ? Op::Ridge3World : Op::Noise3World)
                              : (is_ridge ? Op::Ridge3 : Op::Noise3);
            if (!require_uint(1, o.seed)) return false;
            if (!require_float(2, o.f0))  return false; // freq
            if (!require_int(3, o.oct))   return false;
            if (!require_float(4, o.f1))  return false; // gain
            if (!require_float(5, o.f2))  return false; // lac
            if (toks.size() != 6) {
                // Warp tail is all-or-nothing: exactly [wseed wfreq wamp].
                if (toks.size() != 9) {
                    err = std::string(op) +
                          ": warp tail must be exactly 'wseed wfreq wamp'";
                    return false;
                }
                o.warp = true;
                if (!require_uint(6, o.wseed)) return false;
                if (!require_float(7, o.wf0))  return false; // warp freq
                if (!require_float(8, o.wf1))  return false; // warp amp
            }
            if (is_world) {
                out.uses_world_inputs_ = true;
                out.input_mask_ |= (1u << kSurfInWorldX) |
                                   (1u << kSurfInAltitude) |
                                   (1u << kSurfInWorldZ);
            }
        }
        else if (op == "fract") {
            // fract a — x - floor(x); unary (strata banding).
            o.kind = Op::Fract;
            if (!require_reg(1, o.a)) return false;
        }
        else if (op == "curv") {
            // curv radius — field curvature probe at world (x, z), metres.
            o.kind = Op::FieldCurv;
            if (!require_float(1, o.f0)) return false;
            out.uses_world_inputs_ = true;
            out.input_mask_ |= (1u << kSurfInWorldX) | (1u << kSurfInWorldZ);
        }
        else if (op == "add" || op == "sub" || op == "mul" ||
                 op == "min" || op == "max") {
            o.kind = (op == "add") ? Op::Add
                   : (op == "sub") ? Op::Sub
                   : (op == "mul") ? Op::Mul
                   : (op == "min") ? Op::Min : Op::Max;
            if (!require_reg(1, o.a)) return false;
            if (!require_reg(2, o.b)) return false;
        }
        else if (op == "abs" || op == "oneminus") {
            o.kind = (op == "abs") ? Op::Abs : Op::OneMinus;
            if (!require_reg(1, o.a)) return false;
        }
        else if (op == "pow") {
            // pow a e — float-literal exponent; base clamped to >= 0 at eval.
            o.kind = Op::Pow;
            if (!require_reg(1, o.a))    return false;
            if (!require_float(2, o.f0)) return false;
        }
        else if (op == "clamp") {
            o.kind = Op::Clamp;
            if (!require_reg(1, o.a))    return false;
            if (!require_float(2, o.f0)) return false;
            if (!require_float(3, o.f1)) return false;
        }
        else if (op == "blend") {
            o.kind = Op::Blend;
            if (!require_reg(1, o.a)) return false;
            if (!require_reg(2, o.b)) return false;
            if (!require_reg(3, o.c)) return false;
        }
        else if (op == "smoothstep") {
            o.kind = Op::Smoothstep;
            if (!require_float(1, o.f0)) return false;
            if (!require_float(2, o.f1)) return false;
            if (!require_reg(3, o.a))    return false;
        }
        else {
            err = std::string("unknown surface op: ") + op;
            return false;
        }

        // Cap applies to EMITTED ops (dedup'd consts already continued above),
        // and is PER MODE: a surfaces tape is bounded by the shader's register
        // file, a habitat tape only by the evaluator's stack array.
        const int op_cap =
            (mode == TapeMode::Habitat) ? kMaxHabitatOps : kMaxOps;
        if ((int)out.ops.size() >= op_cap) {
            err = "too many ops (max " + std::to_string(op_cap) + ")";
            return false;
        }
        src_map.push_back((int)out.ops.size());
        out.ops.push_back(o);
    }

    // Each mode fails closed on declaring nothing, for the same reason: a tape
    // with no outputs computed a pile of noise and threw it away, which is
    // always a bug and never a intent.
    if (mode == TapeMode::Habitat) {
        if (out.channel_count == 0) {
            err = "habitat() declared no channels (missing 'channel' "
                  "directives)";
            return false;
        }
        return true;
    }
    if (out.materials.empty()) {
        err = "surfaces() declared no materials (missing 'material' directives)";
        return false;
    }
    return true;
}

uint64_t SurfaceProgram::hash() const {
    constexpr uint64_t offset = 14695981039346656037ULL;
    constexpr uint64_t prime  = 1099511628211ULL;
    uint64_t h = offset;
    for (unsigned char c : text_) {
        h ^= (uint64_t)c;
        h *= prime;
    }
    return h;
}

// ---------------------------------------------------------------------------
// SurfaceRuntime
// ---------------------------------------------------------------------------
SurfaceRuntime::SurfaceRuntime(SurfaceProgram p)
    : prog_(std::move(p))
{}

void SurfaceRuntime::eval_regs(const float pos[3], const float nrm[3],
                               const SurfaceWorldContext* world,
                               float* regs) const {
    // `regs` is deliberately uninitialised (see kMaxTapeRegs). That is only
    // sound while every operand is a BACKWARD reference, which the parser
    // enforces -- assert it here so a forward-referencing op added later trips
    // in debug rather than silently reading stack garbage in release.
#ifndef NDEBUG
    for (size_t i = 0; i < prog_.ops.size(); ++i) {
        const Op& dop = prog_.ops[i];
        for (int r : {dop.a, dop.b, dop.c})
            assert(r < (int)i && "tape operand must reference an earlier op");
    }
#endif
    // ---- per-sample input vector ----
    float in[kSurfInCount];
    in[kSurfInLocalX] = pos[0];
    in[kSurfInLocalY] = pos[1];
    in[kSurfInLocalZ] = pos[2];
    // Defensive normalization: chart meshes carry unit normals, but the tape
    // must stay deterministic on any input.
    float ny = nrm ? nrm[1] : 1.0f;
    {
        const float nx = nrm ? nrm[0] : 0.0f;
        const float nz = nrm ? nrm[2] : 0.0f;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        ny = (len > 1e-8f) ? ny / len : 1.0f;
    }
    in[kSurfInNormalY] = ny;
    in[kSurfInSlope] = std::max(0.0f, std::min(1.0f, 1.0f - ny));
    // Fallback constants: deterministic, never instance-dependent (the misuse
    // contract for world inputs on multi-instance variants). Field-query
    // inputs the tape never reads (input_mask) keep these values too — each
    // one is a full field-program evaluation (fslope is 4, biome is 3), so
    // only pay for what an op consumes.
    in[kSurfInWorldX]     = 0.0f;
    in[kSurfInAltitude]   = 0.0f;
    in[kSurfInWorldZ]     = 0.0f;
    in[kSurfInHeight]     = 0.0f;
    in[kSurfInMoisture]   = 0.5f;
    in[kSurfInRelief]     = 0.5f;
    in[kSurfInBiome]      = 1.0f;
    in[kSurfInFieldSlope] = 0.0f;
    if (world) {
        float wx = pos[0], wy = pos[1], wz = pos[2];
        if (world->local_to_world) {
            const float* m = world->local_to_world;   // row-major
            wx = m[0] * pos[0] + m[1] * pos[1] + m[2]  * pos[2] + m[3];
            wy = m[4] * pos[0] + m[5] * pos[1] + m[6]  * pos[2] + m[7];
            wz = m[8] * pos[0] + m[9] * pos[1] + m[10] * pos[2] + m[11];
        }
        in[kSurfInWorldX]   = wx;
        in[kSurfInAltitude] = wy;
        in[kSurfInWorldZ]   = wz;
        if (world->field) {
            const uint32_t mask = prog_.input_mask();
            // height / fieldSlope: the pre-sampled lattice, when present and
            // covering, stands in for the field program (5 evaluations per
            // sample otherwise -- see HeightLattice). Out-of-bounds samples
            // take the exact path so a lattice is never a correctness gate.
            const HeightLattice* lat = world->height_lattice;
            const bool lat_hit = lat && lat->contains(wx, wz);
            if (mask & (1u << kSurfInHeight))
                in[kSurfInHeight]   = lat_hit ? lat->height(wx, wz)
                                              : world->field->height_at(wx, wz);
            if (mask & (1u << kSurfInMoisture))
                in[kSurfInMoisture] = world->field->moisture_at(wx, wz);
            if (mask & (1u << kSurfInRelief))
                in[kSurfInRelief]   = world->field->relief_at(wx, wz);
            if (mask & (1u << kSurfInBiome))
                in[kSurfInBiome]    = (float)world->field->biome_at(wx, wz);
            if (mask & (1u << kSurfInFieldSlope))
                in[kSurfInFieldSlope] = lat_hit ? lat->slope(wx, wz)
                                                : world->field->slope_at(wx, wz);
        }
    }

    // ---- evaluate every register (bounded by kMaxOps, enforced at parse) ----
    const auto& ops = prog_.ops;
    for (int i = 0; i < (int)ops.size(); ++i) {
        const Op& o = ops[i];
        switch (o.kind) {
        case Op::Input:
            regs[i] = in[o.oct];
            break;
        case Op::Const:
            regs[i] = o.f0;
            break;
        case Op::Noise2:
            regs[i] = fbm2(in[kSurfInLocalX], in[kSurfInLocalZ], o.seed, o.oct,
                           o.f1, o.f2, o.f0, false);
            break;
        case Op::Ridge2:
            regs[i] = fbm2(in[kSurfInLocalX], in[kSurfInLocalZ], o.seed, o.oct,
                           o.f1, o.f2, o.f0, true);
            break;
        case Op::Noise2World:
            // World-frame fbm: continuous across sector variants. Without a
            // world context wx/wz hold fallback 0 — a constant, per contract.
            regs[i] = fbm2(in[kSurfInWorldX], in[kSurfInWorldZ], o.seed, o.oct,
                           o.f1, o.f2, o.f0, false);
            break;
        case Op::Ridge2World:
            regs[i] = fbm2(in[kSurfInWorldX], in[kSurfInWorldZ], o.seed, o.oct,
                           o.f1, o.f2, o.f0, true);
            break;
        case Op::FieldCurv:
            regs[i] = (world && world->field)
                ? world->field->curvature_at(in[kSurfInWorldX],
                                             in[kSurfInWorldZ], o.f0)
                : 0.0f;
            break;
        case Op::Noise3:
            regs[i] = fbm3_op(o, in[kSurfInLocalX], in[kSurfInLocalY],
                              in[kSurfInLocalZ], false);
            break;
        case Op::Ridge3:
            regs[i] = fbm3_op(o, in[kSurfInLocalX], in[kSurfInLocalY],
                              in[kSurfInLocalZ], true);
            break;
        case Op::Noise3World:
            // World-frame 3D fbm. Without a world context wx/wy/wz hold
            // fallback 0 — a constant, per contract (same rule as noise2w).
            regs[i] = fbm3_op(o, in[kSurfInWorldX], in[kSurfInAltitude],
                              in[kSurfInWorldZ], false);
            break;
        case Op::Ridge3World:
            regs[i] = fbm3_op(o, in[kSurfInWorldX], in[kSurfInAltitude],
                              in[kSurfInWorldZ], true);
            break;
        case Op::Fract:
            regs[i] = regs[o.a] - std::floor(regs[o.a]);
            break;
        case Op::Add:
            regs[i] = regs[o.a] + regs[o.b];
            break;
        case Op::Sub:
            regs[i] = regs[o.a] - regs[o.b];
            break;
        case Op::Mul:
            regs[i] = regs[o.a] * regs[o.b];
            break;
        case Op::Min:
            regs[i] = std::min(regs[o.a], regs[o.b]);
            break;
        case Op::Max:
            regs[i] = std::max(regs[o.a], regs[o.b]);
            break;
        case Op::Clamp:
            regs[i] = std::max(o.f0, std::min(o.f1, regs[o.a]));
            break;
        case Op::Blend: {
            const float t = regs[o.c];
            regs[i] = regs[o.a] * (1.0f - t) + regs[o.b] * t;
            break;
        }
        case Op::Smoothstep: {
            const float t = std::max(
                0.0f, std::min(1.0f, (regs[o.a] - o.f0) / (o.f1 - o.f0)));
            regs[i] = t * t * (3.0f - 2.0f * t);
            break;
        }
        case Op::Abs:
            regs[i] = std::fabs(regs[o.a]);
            break;
        case Op::OneMinus:
            regs[i] = 1.0f - regs[o.a];
            break;
        case Op::Pow:
            regs[i] = std::pow(std::max(regs[o.a], 0.0f), o.f0);
            break;
        case Op::Warp2:
            // Not part of the surface op set (parse rejects it); keep the
            // switch exhaustive.
            regs[i] = 0.0f;
            break;
        }
    }
}

void SurfaceRuntime::weights_at(const float pos[3], const float nrm[3],
                                const SurfaceWorldContext* world,
                                float* out_weights) const {
    float regs[kMaxTapeRegs];   // uninitialised on purpose -- see kMaxTapeRegs
    eval_regs(pos, nrm, world, regs);
    for (size_t k = 0; k < prog_.materials.size(); ++k)
        out_weights[k] = std::max(0.0f, regs[prog_.materials[k].reg]);
}

void SurfaceRuntime::channels_at(float world_x, float world_z,
                                 const FieldRuntime* field,
                                 float* out_channels) const {
    channels_at(world_x, world_z, field, nullptr, out_channels);
}

void SurfaceRuntime::channels_at(float world_x, float world_z,
                                 const FieldRuntime* field,
                                 const HeightLattice* lattice,
                                 float* out_channels) const {
    for (int i = 0; i < kMaxHabitatChannels; ++i) out_channels[i] = 0.0f;
    if (prog_.channel_regs.empty()) return;

    // A habitat sample IS a world position, so the part-local frame is the
    // identity and the "normal" is up. eval_regs derives worldX/worldZ by
    // transforming pos through local_to_world; an identity transform makes the
    // two the same, which is exactly what we want and avoids a second code
    // path through the register machine.
    static const float kIdentity[16] = {
        1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
    const float pos[3] = {world_x, 0.0f, world_z};
    const float nrm[3] = {0.0f, 1.0f, 0.0f};
    SurfaceWorldContext ctx;
    ctx.field = field;
    ctx.local_to_world = kIdentity;
    ctx.height_lattice = lattice;

    float regs[kMaxTapeRegs];   // uninitialised on purpose -- see kMaxTapeRegs
    eval_regs(pos, nrm, &ctx, regs);
    for (int i = 0; i < kMaxHabitatChannels; ++i) {
        const int reg = prog_.channel_regs[i];
        if (reg >= 0) out_channels[i] = regs[reg];
    }
}

void SurfaceRuntime::appearance_at(const float pos[3], const float nrm[3],
                                   const SurfaceWorldContext* world,
                                   SurfaceAppearance& out) const {
    out = SurfaceAppearance{};   // identity for every absent directive
    if (!prog_.has_appearance()) return;
    float regs[kMaxTapeRegs];   // uninitialised on purpose -- see kMaxTapeRegs
    eval_regs(pos, nrm, world, regs);
    auto clamp_to = [](float v, float lo, float hi) {
        return std::max(lo, std::min(hi, v));
    };
    if (prog_.has_tint()) {
        for (int c = 0; c < 3; ++c)
            out.tint[c] =
                clamp_to(regs[prog_.tint_reg[c]], 0.0f, kSurfaceTintMax);
    }
    if (prog_.has_rough_bias())
        out.rough_bias = clamp_to(regs[prog_.rough_bias_reg],
                                  -kSurfaceRoughBiasLimit,
                                  kSurfaceRoughBiasLimit);
    if (prog_.has_wetness())
        out.wetness = clamp_to(regs[prog_.wetness_reg], 0.0f, 1.0f);
    if (prog_.has_metallic())
        out.metallic = clamp_to(regs[prog_.metallic_reg], 0.0f, 1.0f);
}

void SurfaceRuntime::classify_vertices(const float* positions,
                                       const float* normals,
                                       uint32_t vertex_count,
                                       const SurfaceWorldContext* world,
                                       uint8_t* out) const {
    const uint32_t count = material_count();
    float w[kMaxSurfaceMaterials];
    static const float kUp[3] = {0.0f, 1.0f, 0.0f};
    for (uint32_t v = 0; v < vertex_count; ++v) {
        const float* pos = positions + (size_t)v * 3;
        const float* nrm = normals ? normals + (size_t)v * 3 : kUp;
        weights_at(pos, nrm, world, w);
        float sum = 0.0f;
        for (uint32_t k = 0; k < count; ++k) sum += w[k];
        uint8_t* dst = out + (size_t)v * count;
        if (sum <= 1e-8f) {
            // All-zero weights: the first declared material owns the sample
            // (deterministic fail-closed choice, documented in the header).
            for (uint32_t k = 0; k < count; ++k) dst[k] = (k == 0) ? 255 : 0;
            continue;
        }
        for (uint32_t k = 0; k < count; ++k) {
            const float q = w[k] / sum * 255.0f;
            dst[k] = (uint8_t)std::lround(std::max(0.0f, std::min(255.0f, q)));
        }
    }
}

bool SurfaceRuntime::note_world_input_misuse() const {
    if (misuse_noted_) return false;
    misuse_noted_ = true;
    return true;
}

} // namespace terrain_field
