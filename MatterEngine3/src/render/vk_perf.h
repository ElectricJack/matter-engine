#pragma once

// MatterEngine3/src/render/vk_perf.h
//
// Header-only, no Vulkan dependency; included by vk_scene_renderer.cpp and
// vk_temporal.cpp. Everything lives in `viewer::vk_perf`.
//
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

// The MATTER_VK_VECTOR_GROWTH kill switch, read ONCE into a function-local
// static: changing the variable after the first call has no effect for the
// rest of the process. Anything other than a leading '0' (including unset and
// empty) means enabled.
inline bool geometric_growth_enabled() {
    static const bool value = [] {
        const char* env = std::getenv("MATTER_VK_VECTOR_GROWTH");
        return env == nullptr || env[0] == '\0' || env[0] != '0';
    }();
    return value;
}

// reserve() that grows to at least twice the current capacity. Never shrinks
// and never reallocates when `count` already fits, so it is safe to call every
// frame. It does trade memory for reallocations: a vector that spikes once
// keeps the doubled capacity until it is destroyed or shrunk elsewhere.
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
