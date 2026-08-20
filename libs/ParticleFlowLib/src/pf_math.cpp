// libs/ParticleFlowLib/src/pf_math.cpp
//
// The non-inline math for `particle_flow.h`: vector length/normalize and the
// full `Rng` implementation (splitmix64 seeding + xoshiro256++ generation).
//
// This file is the determinism root of ParticleFlowLib. Every emitter position,
// jitter direction and script-visible random draw comes out of `Rng`, so the
// constants below are effectively part of the on-disk format: change one and
// every cached bake that ran a particle sim produces different geometry. Treat
// them as frozen.
//
// `normalize` is zero-safe by design (returns {0,0,0} below a 1e-8 length)
// because the field kernels lean on it — "no direction" is a normal result
// there, not an error, and callers test the returned vector rather than
// pre-checking the input.
#include "particle_flow.h"
#include <cmath>

namespace pf {

float length(V3 a) { return std::sqrt(dot(a, a)); }

V3 normalize(V3 a) {
    float l = length(a);
    if (l < 1e-8f) return {0, 0, 0};
    return a * (1.0f / l);
}

static inline uint64_t rotl64(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

// Expand a single user seed into xoshiro's four 64-bit lanes with splitmix64.
// Seeding all lanes from one value directly would leave low-entropy states that
// take many draws to decorrelate; splitmix64 is the reference fix. The final
// all-zero guard matters because xoshiro's all-zero state is a fixed point that
// only ever emits zeros.
Rng::Rng(uint64_t seed) {
    // splitmix64 expansion of the seed into 4 non-zero lanes.
    uint64_t z = seed;
    for (int i = 0; i < 4; ++i) {
        z += 0x9E3779B97F4A7C15ull;
        uint64_t t = z;
        t = (t ^ (t >> 30)) * 0xBF58476D1CE4E5B9ull;
        t = (t ^ (t >> 27)) * 0x94D049BB133111EBull;
        s[i] = t ^ (t >> 31);
    }
    if (!(s[0] | s[1] | s[2] | s[3])) s[0] = 1;
}

// xoshiro256++ (Blackman/Vigna). Mutates the four-lane state in place, so this
// is not const and not thread-safe — but each `Sim` owns its own `Rng`, which
// is exactly what lets sims run concurrently without coordination.
uint64_t Rng::next_u64() {
    const uint64_t r = rotl64(s[0] + s[3], 23) + s[0];
    const uint64_t t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl64(s[3], 45);
    return r;
}

// [0, 1), built from the top 24 bits so every representable value is exactly a
// multiple of 2^-24 and lands on a float without rounding. 1.0 is never
// returned, which is what makes `range` a half-open interval too.
float Rng::next_unit() { return (float)((next_u64() >> 40) * (1.0 / 16777216.0)); }

float Rng::range(float a, float b) { return a + (b - a) * next_unit(); }

// Uniform direction on the unit sphere via Archimedes' theorem: sample z
// uniformly in [-1,1], then the azimuth uniformly, and the ring radius follows.
// Genuinely area-uniform (no polar clustering) and costs exactly two draws, so
// it does not perturb the stream by a data-dependent amount.
V3 Rng::unit_sphere() {
    float z = range(-1.0f, 1.0f);
    float a = range(0.0f, 6.28318530718f);
    float r = std::sqrt(std::fmax(0.0f, 1.0f - z * z));
    return {r * std::cos(a), r * std::sin(a), z};
}

} // namespace pf
