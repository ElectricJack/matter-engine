#pragma once
#include <cstdint>
#include <string>

// SP-7 seeded-PRNG contract that backs SP-2's Math.random replacement. Pure C++,
// no QuickJS dependency: SP-2's host installs ScriptRng::random as the Math.random
// thunk and seeds it from the part's params. Algorithm: xoshiro128** seeded via
// SplitMix32 (must match shared-lib/rng.js bit-for-bit).
namespace script_rng {

struct ScriptRng {
    uint32_t s[4];
    explicit ScriptRng(uint32_t seed);
    uint32_t next_u32();
    double   random();   // [0,1)
};

// Extract an unsigned 32-bit seed from a params JSON object by key. Returns 0 if
// the key is absent or non-integer. (Minimal scan; SP-2 may pass a structured
// params object instead — both routes must agree on the integer value.)
uint32_t seed_from_params_json(const std::string& params_json, const std::string& key);

} // namespace script_rng
