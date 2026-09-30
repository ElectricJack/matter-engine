#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace viewer {

// FIFO simulator: a cache hit does not refresh insertion time. This estimates
// reuse, not a hardware performance guarantee; validate with GPU invocations.
inline size_t vertex_cache_misses(const std::vector<uint32_t>& indices,
                                  size_t vertex_count, uint32_t cache_size = 32) {
    std::vector<uint64_t> stamps(vertex_count, 0);
    uint64_t clock = uint64_t(cache_size) + 1;
    size_t misses = 0;
    for (uint32_t vertex : indices) {
        if (vertex >= vertex_count) return std::numeric_limits<size_t>::max();
        if (clock - stamps[vertex] > cache_size) {
            stamps[vertex] = clock++;
            ++misses;
        }
    }
    return misses;
}

// Reorder whole triangles within ONE draw range; never change a corner's index,
// winding or provoking vertex. Vertex/attribute arrays and draw spans stay put.
// Linear adjacency-fan traversal (Tipsify, Sander/Nehab/Barczak 2007, section 3):
// https://gfx.cs.princeton.edu/gfx/pubs/Sander_2007_%3ETR/tipsy.pdf
// Includes disconnected components, duplicate/degenerate triangles and unused
// vertices. Invalid input is untouched. Use off the render thread at prebuild.
inline bool optimize_vertex_cache_order(std::vector<uint32_t>& indices,
                                       size_t vertex_count) {
    constexpr uint32_t cache_size = 32;
    constexpr uint32_t missing = UINT32_MAX;
    if (indices.size() < 96 || indices.size() % 3 != 0 ||
        indices.size() >= UINT32_MAX || vertex_count >= UINT32_MAX)
        return false;
    std::vector<uint32_t> live(vertex_count, 0);
    for (uint32_t vertex : indices) {
        if (vertex >= vertex_count) return false;
        ++live[vertex];
    }
    std::vector<uint32_t> offsets(vertex_count + 1, 0);
    for (size_t v = 0; v < vertex_count; ++v)
        offsets[v + 1] = offsets[v] + live[v];
    std::vector<uint32_t> cursor(offsets.begin(), offsets.end() - 1);
    std::vector<uint32_t> adjacency(indices.size());
    for (size_t i = 0; i < indices.size(); ++i)
        adjacency[cursor[indices[i]]++] = static_cast<uint32_t>(i / 3);
    std::vector<uint64_t> stamps(vertex_count, 0);
    std::vector<uint8_t> emitted(indices.size() / 3, 0);
    std::vector<uint32_t> output, dead_ends, candidates;
    output.reserve(indices.size());
    dead_ends.reserve(indices.size());
    uint64_t clock = cache_size + 1;
    size_t scan = 0;
    uint32_t fan = indices.front();
    while (fan != missing) {
        candidates.clear();
        for (uint32_t a = offsets[fan]; a < offsets[fan + 1]; ++a) {
            const uint32_t triangle = adjacency[a];
            if (emitted[triangle]) continue;
            emitted[triangle] = 1;
            for (size_t corner = 0; corner < 3; ++corner) {
                const uint32_t vertex = indices[size_t(triangle) * 3 + corner];
                output.push_back(vertex);
                dead_ends.push_back(vertex);
                candidates.push_back(vertex);
                --live[vertex];
                if (clock - stamps[vertex] > cache_size)
                    stamps[vertex] = clock++;
            }
        }
        fan = missing;
        uint64_t best = 0;
        for (uint32_t vertex : candidates) {
            if (!live[vertex]) continue;
            const uint64_t age = clock - stamps[vertex];
            const uint64_t priority = age + 2ull * live[vertex] <= cache_size
                ? age : 0;
            if (fan == missing || priority > best) {
                fan = vertex;
                best = priority;
            }
        }
        while (fan == missing && !dead_ends.empty()) {
            const uint32_t vertex = dead_ends.back();
            dead_ends.pop_back();
            if (live[vertex]) fan = vertex;
        }
        while (fan == missing && scan < vertex_count) {
            if (live[scan]) fan = static_cast<uint32_t>(scan);
            ++scan;
        }
    }
    // Already-local meshes should keep their original order. The simulator
    // also avoids accepting a traversal that makes this locality model worse.
    if (output.size() != indices.size() ||
        vertex_cache_misses(output, vertex_count, cache_size) >=
            vertex_cache_misses(indices, vertex_count, cache_size))
        return false;
    indices.swap(output);
    return true;
}

} // namespace viewer
