// terrain_field.cpp — native field program interpreter for infinite-world terrain.
// Pure CPU module: no JS, no GL, no engine subsystem dependencies.

#include "terrain_field.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <algorithm>

// ---------------------------------------------------------------------------
// File-scope constants shared between parse (static fn) and eval.
// ---------------------------------------------------------------------------
static constexpr int kMaxOps = 64;

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
            if (op_idx + 1 >= kMaxOps) { err = "too many ops (implicit const would exceed 64)"; return false; }
            try { imm_out = parse_float(toks[tok_idx]); }
            catch (...) { err = std::string(op) + ": invalid float"; return false; }
            is_imm = true;
            return true;
        };

        if (op == "const") {
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
            float scratch[kMaxOps] = {};
            eval_regs(scratch, o.a + 1, x + dx, z + dz);
            regs[i] = scratch[o.a];
            break;
        }
        case Op::Add:
            regs[i] = regs[o.a] + regs[o.b];
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
        }
    }
}

float FieldRuntime::eval_reg(int target, float x, float z) const {
    float regs[kMaxOps] = {};
    eval_regs(regs, target + 1, x, z);
    return regs[target];
}

float FieldRuntime::height_at(float x, float z) const {
    return eval_reg(prog_.height_reg, x, z);
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

} // namespace terrain_field
