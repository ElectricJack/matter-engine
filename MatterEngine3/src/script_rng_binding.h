#pragma once
#include <cstdint>
#include <string>

// MatterEngine3/src/script_rng_binding.h
//
// SP-7 seeded-PRNG contract that backs SP-2's Math.random replacement. Pure C++,
// no QuickJS dependency: SP-2's host installs ScriptRng::random as the Math.random
// thunk and seeds it from the part's params. Algorithm: xoshiro128** seeded via
// SplitMix32 (must match shared-lib/rng.js bit-for-bit).
namespace script_rng {

// A 128-bit xoshiro128** state, held by value. Trivially copyable, and copying
// FORKS the stream: the copy replays exactly what the original would have
// produced from that point. Not thread-safe in any sense -- every next_u32()
// mutates the state -- so one instance belongs to one bake.
//
// Determinism is the entire point. Two ScriptRng built from the same seed
// produce identical sequences on any platform, here and in shared-lib/rng.js,
// which is what makes a part's scattered geometry reproducible from its
// content hash alone.
struct ScriptRng {
    // xoshiro128** state; never all-zero, guaranteed by the constructor's
    // SplitMix32 expansion.
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
