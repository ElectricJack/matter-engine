#pragma once
// ParticleFlowLib: generic agent-particle kernel + path recording.
// No engine dependencies. All state is instance-contained (no globals/statics):
// N sims run concurrently on bake worker threads with zero coordination.
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace pf {

struct V3 { float x = 0, y = 0, z = 0; };

inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(V3 a);
V3 normalize(V3 a);   // zero-safe: returns {0,0,0} for near-zero input

// xoshiro256++ seeded via splitmix64. Deterministic, instance-owned.
struct Rng {
    uint64_t s[4];
    explicit Rng(uint64_t seed);
    uint64_t next_u64();
    float next_unit();                  // [0, 1)
    float range(float a, float b);
    V3 unit_sphere();                   // uniform direction on the unit sphere
};

// Append-only polylines with per-vertex attribute channels.
// Monotonic accretion is enforced structurally: vertices are only appended,
// paths are only appended, existing data is never mutated.
struct PathSet {
    struct Path {
        uint32_t particle_id = 0;
        std::vector<float> xyz;                     // 3 floats per vertex
        std::vector<std::vector<float>> channels;   // [channel][vertex]
        bool closed = false;                        // particle died / finalized
        size_t vertex_count() const { return xyz.size() / 3; }
    };
    std::vector<std::string> channel_names;         // fixed at construction
    std::vector<Path> paths;
};

} // namespace pf
