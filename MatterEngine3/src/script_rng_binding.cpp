// MatterEngine3/src/script_rng_binding.cpp
//
// The engine-side half of the deterministic script RNG; script_rng_binding.h
// carries the contract. Three small pieces: SplitMix32 seed expansion, the
// xoshiro128** step, and a scanner that lifts an integer seed out of a params
// JSON blob.
//
// WHY IT IS SPELLED OUT rather than taken from <random>. The identical
// generator exists in JavaScript in shared-lib/rng.js, and a part baked by the
// C++ host must draw the SAME numbers as the same part reasoned about in
// script -- so every operation below is explicit 32-bit unsigned arithmetic
// that both languages reproduce exactly. Do not "improve" a constant, a shift,
// or the order of the state updates: any change silently rebakes every seeded
// part in the repo differently, and the artifacts are content-addressed, so
// the divergence shows up as a cache full of parts nobody can reproduce.
//
// No QuickJS include here on purpose -- installing `random()` as the
// Math.random thunk is the script host's job, and keeping this file
// dependency-free is what lets the generator be unit-tested and, in principle,
// re-derived in any language.
#include "script_rng_binding.h"
#include <cctype>
#include <cstdlib>

namespace script_rng {

static uint32_t rotl(uint32_t x, int k){ return (x << k) | (x >> (32 - k)); }

// SplitMix32: expand one 32-bit seed into the four state words xoshiro128**
// needs. Run unconditionally, seed 0 included -- the golden-ratio increment is
// added BEFORE the first mix, so even an all-zero seed produces a non-zero
// state, which xoshiro requires (an all-zero state is a fixed point that emits
// zeros forever).
ScriptRng::ScriptRng(uint32_t seed) {
    uint32_t z = seed;
    for (int i = 0; i < 4; ++i) {
        z += 0x9e3779b9u;
        uint32_t w = z;
        w = (w ^ (w >> 16)) * 0x21f0aaadu;
        w = (w ^ (w >> 15)) * 0x735a2d97u;
        s[i] = w ^ (w >> 15);
    }
}

// One xoshiro128** step. The scrambled output is computed from the state
// BEFORE it advances; that ordering is part of the algorithm, not an
// optimisation, and part of the shared-lib/rng.js parity contract.
uint32_t ScriptRng::next_u32() {
    uint32_t result = rotl(s[1] * 5u, 7) * 9u;
    uint32_t t = s[1] << 9;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t;
    s[3] = rotl(s[3], 11);
    return result;
}

// [0, 1), with 2^-32 granularity. The divisor is 2^32 and not 2^32 - 1, which
// is exactly what keeps 1.0 out of the range; JS does the same division, so
// the two agree bit-for-bit on the resulting double.
double ScriptRng::random() { return next_u32() / 4294967296.0; }

uint32_t seed_from_params_json(const std::string& j, const std::string& key) {
    // Minimal: find "key" : <digits>. Good enough for flat params blobs; SP-2's
    // structured route bypasses this and constructs ScriptRng(seed) directly.
    std::string needle = "\"" + key + "\"";
    size_t k = j.find(needle);
    if (k == std::string::npos) return 0u;
    size_t c = j.find(':', k + needle.size());
    if (c == std::string::npos) return 0u;
    size_t i = c + 1;
    while (i < j.size() && std::isspace((unsigned char)j[i])) ++i;
    bool neg = (i < j.size() && j[i] == '-'); if (neg) ++i;
    uint64_t v = 0; bool any = false;
    while (i < j.size() && std::isdigit((unsigned char)j[i])) { v = v * 10 + (j[i]-'0'); ++i; any = true; }
    if (!any) return 0u;
    return (uint32_t)(neg ? (uint32_t)(-(int64_t)v) : v);
}

} // namespace script_rng
