#pragma once

#include "vt_chart_gpu.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace vt {

// Immutable suffix of the geometry buffer, after the original triangles.
// Keep triangle order: a left-first, stackless traversal returns the same
// first accepted triangle as the original bounded linear seed search.
struct VtSeedNode {
    float lower[4]{};
    float upper[4]{};
    uint32_t range[4]{}; // first triangle, leaf count (0 = branch), escape, reserved
};
static_assert(sizeof(VtSeedNode) == 48, "vt_surface_walk.glsl seed-node ABI");
constexpr uint32_t kVtSeedTriangleLimit = 2048;
constexpr uint32_t kVtSeedLeafTriangles = 8;
constexpr uint32_t kVtSeedMinTriangles = 64;

inline bool vt_build_seed_bvh(std::vector<GpuChart>& charts,
                              const std::vector<GpuTriGeometry>& triangles,
                              std::vector<VtSeedNode>& nodes) {
    nodes.clear();
    // Reserve once so preparation's accounting also bounds vector capacity.
    // Complete binary trees with eight-triangle leaves need at most half as
    // many nodes as the searched triangles (including non-power-of-two sizes).
    size_t capacity = 0;
    for (auto& chart : charts) {
        chart.tri_range[2] = chart.tri_range[3] = 0;
        const size_t first = chart.tri_range[0], count = chart.tri_range[1];
        if (first > triangles.size() || count > triangles.size() - first) return false;
        if (count >= kVtSeedMinTriangles) {
            uint32_t leaves = 1;
            while (leaves * kVtSeedLeafTriangles < std::min<size_t>(count, kVtSeedTriangleLimit)) leaves *= 2;
            capacity += leaves * 2 - 1;
        }
    }
    if (capacity > std::numeric_limits<uint32_t>::max()) return false;
    nodes.reserve(capacity);
    const auto build = [&](auto&& self, uint32_t first, uint32_t count) -> uint32_t {
        const uint32_t index = static_cast<uint32_t>(nodes.size());
        nodes.emplace_back();
        if (count <= kVtSeedLeafTriangles) {
            auto& node = nodes[index];
            node.range[0] = first; node.range[1] = count;
            for (int axis = 0; axis < 3; ++axis) {
                float lo = std::numeric_limits<float>::infinity(), hi = -lo;
                for (uint32_t i = first; i < first + count; ++i) {
                    const auto& tri = triangles[i];
                    for (const float* p : {tri.p0, tri.p1, tri.p2}) {
                        // Invalid source geometry cannot safely authorize a
                        // rejection. Unbounded nodes retain its linear behavior.
                        if (!std::isfinite(p[axis])) { lo = -INFINITY; hi = INFINITY; break; }
                        lo = std::min(lo, p[axis]); hi = std::max(hi, p[axis]);
                    }
                }
                // The seed predicate accepts barycentrics down to -2e-6.
                // Include that extension and float roundoff; the shader adds
                // the independently varying proxy-plane tolerance in metres.
                const float margin = 4e-6f * (hi - lo) +
                    8 * std::numeric_limits<float>::epsilon() * std::max({1.f, std::abs(lo), std::abs(hi)});
                node.lower[axis] = lo - margin;
                node.upper[axis] = hi + margin;
            }
        } else {
            const uint32_t left_count = count / 2;
            const uint32_t left = self(self, first, left_count);
            const uint32_t right = self(self, first + left_count, count - left_count);
            for (int axis = 0; axis < 3; ++axis) {
                nodes[index].lower[axis] = std::min(nodes[left].lower[axis], nodes[right].lower[axis]);
                nodes[index].upper[axis] = std::max(nodes[left].upper[axis], nodes[right].upper[axis]);
            }
        }
        nodes[index].range[2] = static_cast<uint32_t>(nodes.size());
        return index;
    };
    for (auto& chart : charts) {
        const uint32_t count = std::min(chart.tri_range[1], kVtSeedTriangleLimit);
        if (count < kVtSeedMinTriangles) continue;
        chart.tri_range[2] = build(build, chart.tri_range[0], count);
        chart.tri_range[3] = static_cast<uint32_t>(nodes.size()) - chart.tri_range[2];
    }
    return true;
}

} // namespace vt
