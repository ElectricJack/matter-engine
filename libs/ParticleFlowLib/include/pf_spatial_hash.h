#pragma once
// Included by particle_flow.h after V3 is defined; do not include directly.
// V3 must be defined in pf namespace before this is included.
#include <cmath>
#include <unordered_map>
#include <utility>

// Forward declare V3 for use in this header
namespace pf { struct V3; }

namespace pf {

// Uniform-grid point hash. Correctness does not depend on the key function
// (all candidates are exact-distance filtered); key collisions only cost time.
// Deterministic: cells are visited in fixed (z,y,x) loop order and points in
// insertion order, so query callbacks always fire in the same sequence.
class SpatialHash {
public:
    explicit SpatialHash(float cell) : cell_(cell > 1e-6f ? cell : 1e-6f) {}
    void clear() { cells_.clear(); count_ = 0; }
    size_t size() const { return count_; }
    float cell_size() const { return cell_; }

    void insert(V3 p, uint32_t idx) {
        cells_[key(cx(p.x), cx(p.y), cx(p.z))].push_back({p, idx});
        ++count_;
    }

    // Visit stored points within r of p: fn(idx, point, distance_squared).
    template <class F>
    void query(V3 p, float r, F&& fn) const {
        const float r2 = r * r;
        const int x0 = cx(p.x - r), x1 = cx(p.x + r);
        const int y0 = cx(p.y - r), y1 = cx(p.y + r);
        const int z0 = cx(p.z - r), z1 = cx(p.z + r);
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    auto it = cells_.find(key(x, y, z));
                    if (it == cells_.end()) continue;
                    for (const auto& e : it->second) {
                        V3 d = e.first - p;
                        float d2 = dot(d, d);
                        if (d2 <= r2) fn(e.second, e.first, d2);
                    }
                }
    }

private:
    int cx(float v) const { return (int)std::floor(v / cell_); }
    // Injective within +/- 2^20 cells per axis (far beyond any part bake):
    // 21 low bits of each coordinate packed into one 64-bit key. Distinct
    // cells always get distinct keys, so buckets never merge and queries
    // can never report a point twice.
    static uint64_t key(int32_t x, int32_t y, int32_t z) {
        return (uint64_t(uint32_t(x) & 0x1FFFFF) << 42) |
               (uint64_t(uint32_t(y) & 0x1FFFFF) << 21) |
               (uint64_t(uint32_t(z) & 0x1FFFFF));
    }
    float cell_;
    size_t count_ = 0;
    std::unordered_map<uint64_t, std::vector<std::pair<V3, uint32_t>>> cells_;
};

} // namespace pf
