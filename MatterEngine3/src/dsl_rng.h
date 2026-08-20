#pragma once
// ---------------------------------------------------------------------------
// MatterEngine3/src/dsl_rng.h
//
// The bake's random number source. Header-only, no dependencies.
//
// Who holds one: `DslState` owns the bake's generator (`DslState::rng()`), and
// the `Math.random` override in `dsl_bindings.cpp` draws from it. The tileset
// path creates its own short-lived instances per placement domain, seeded from
// `tileset::placement_seed(...)`, so each domain's attribute stream is
// independent and reproducible on its own.
//
// Not thread-safe and not meant to be shared: one `Rng` belongs to one bake or
// one placement domain, and every draw mutates `state`.
//
// THE VALUE STREAM IS FROZEN. Every scattered instance in every world is
// derived from these draws, so changing the constants, the shift amounts, or
// even the NUMBER of draws a caller makes moves content. A zero seed is
// remapped to the golden-ratio constant so an unseeded generator still
// produces a usable stream rather than a degenerate one.
// ---------------------------------------------------------------------------
#include <cstdint>
namespace dsl {
// SplitMix64-backed [0,1) generator. Deterministic and seedable so a bake's
// Math.random() depends only on the seed (derived from the part's params),
// never on wall-clock or process entropy. This keeps the bake reproducible
// and the resolved-hash <-> bytes contract intact.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next_u64() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double next_unit() { return (next_u64() >> 11) * (1.0 / 9007199254740992.0); } // [0,1)
};
} // namespace dsl
