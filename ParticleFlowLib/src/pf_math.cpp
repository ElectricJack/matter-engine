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

uint64_t Rng::next_u64() {
    const uint64_t r = rotl64(s[0] + s[3], 23) + s[0];
    const uint64_t t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl64(s[3], 45);
    return r;
}

float Rng::next_unit() { return (float)((next_u64() >> 40) * (1.0 / 16777216.0)); }

float Rng::range(float a, float b) { return a + (b - a) * next_unit(); }

V3 Rng::unit_sphere() {
    float z = range(-1.0f, 1.0f);
    float a = range(0.0f, 6.28318530718f);
    float r = std::sqrt(std::fmax(0.0f, 1.0f - z * z));
    return {r * std::cos(a), r * std::sin(a), z};
}

} // namespace pf
