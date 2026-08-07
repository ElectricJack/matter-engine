#pragma once

// Geometric pre-growth for the large vectors the renderer's build region refills
// every frame. Split out of the former vk_build_profile.h when the profiling
// half migrated to libs/ProfileLib (see
// docs/superpowers/specs/2026-08-07-engine-profiler-design.md). This half is
// pure allocation policy and unrelated to timing.
//
// std::vector's copy-assign, assign(), and reserve() all allocate EXACTLY the
// requested element count when they have to grow. A streaming fill adds
// instances every frame, so `v.reserve(n)` / `v = other` with an n that ticks
// upward reallocates on every single frame: capacity lands exactly on size, and
// the next frame is one element too large again. At ~90k instances those are
// 8-15 MB blocks, which on Windows go to the large-block allocator and get
// decommitted on free -- so each frame also eats a soft page fault per 4 KB of
// the fresh block. push_back's own doubling never kicks in because reserve()
// already sized the buffer to the exact final count.
//
// Rounding the request up to at least twice the current capacity restores the
// amortised-O(1) growth these vectors are supposed to have: O(log N)
// reallocations over a fill instead of O(N).
//
// This only ever changes a vector's CAPACITY. Sizes, contents, iteration order
// and every observable value are untouched, so it cannot move a pixel.
// MATTER_VK_VECTOR_GROWTH=0 falls back to the exact-size requests.

#include <cstdlib>
#include <vector>

namespace viewer {
namespace vk_perf {

inline bool geometric_growth_enabled() {
    static const bool value = [] {
        const char* env = std::getenv("MATTER_VK_VECTOR_GROWTH");
        return env == nullptr || env[0] == '\0' || env[0] != '0';
    }();
    return value;
}

template <typename T, typename A>
inline void reserve_geometric(std::vector<T, A>& target, std::size_t count) {
    if (count <= target.capacity()) return;
    if (!geometric_growth_enabled()) {
        target.reserve(count);
        return;
    }
    const std::size_t doubled = target.capacity() * 2;
    target.reserve(count > doubled ? count : doubled);
}

// resize() that grows geometrically. Same contract as reserve_geometric: only
// capacity behaviour differs from a plain resize().
template <typename T, typename A>
inline void resize_geometric(std::vector<T, A>& target, std::size_t count) {
    reserve_geometric(target, count);
    target.resize(count);
}

}  // namespace vk_perf
}  // namespace viewer
