#pragma once

#include <cstdint>

// THE ONE HASH the visible-id bitmask is addressed by (M4, identity-buffer
// occlusion). Its GLSL twin is in shaders_vk/visible_ids.comp and the two must
// agree bit for bit, so this header exists for the same reason
// render/lod_distance.h does: a rule with a CPU mirror and a GPU mirror needs a
// single written-down definition, or the two drift and the symptom is a
// visibility signal that is subtly wrong in a way nothing reports.
//
// WHY A HASH AND NOT AN EXACT SET. The GPU side has to answer "was this token
// on screen" with nothing but atomics over two million pixels, and the CPU side
// only ever ASKS about tokens it already holds (the ~10^3 live entries of
// visible_by_token). So the GPU never has to enumerate -- only to record
// membership -- and a bitmask is the cheapest structure that supports that.
//
// COLLISIONS ARE THE SAFE FAILURE, and that is what makes the choice sound
// rather than merely cheap. Two tokens sharing a bit means a sector reads
// visible when it was not: the streamer keeps it one level finer than it needed
// to, which costs DETAIL. The opposite error -- a visible sector reading
// occluded -- would also only cost detail, because the cap never gates
// coverage. Every failure mode of this structure therefore degrades toward
// "more resident detail", which is the doctrine the whole feature is built on.
// An exact structure (a dense per-sector index, or an append buffer) would buy
// back a percent of that and cost either a new attachment ABI or an overflow
// path whose failure direction is the wrong one.
namespace viewer {

// 2^19 bits = 64 KiB. At the ~5x10^3 distinct tokens a busy frame draws, that
// is a ~1% load factor, so ~1% of occluded sectors are spuriously kept fine.
inline constexpr uint32_t kVisibleIdBits = 1u << 19;
inline constexpr uint32_t kVisibleIdShift = 32u - 19u;
inline constexpr uint32_t kVisibleIdWords = kVisibleIdBits / 32u;

// Knuth multiplicative, taking the HIGH bits. The high bits are where a
// multiplicative hash actually mixes; masking the low ones instead would leave
// the token's own low-bit structure intact, and these tokens are themselves a
// fold (vulkan_history_token) rather than a counter, so that structure is not
// something to rely on being random. uint32 multiplication wraps identically in
// C++ and GLSL, which is what lets the two implementations be textually the
// same expression.
inline constexpr uint32_t visible_id_hash(uint32_t token) {
    return (token * 2654435761u) >> kVisibleIdShift;
}

// The identity attachment's CLEAR value, and therefore "no token here".
// record_raster clears colour attachment 4 to {UINT32_MAX, UINT32_MAX}, not to
// zero -- so a reduction that skipped only 0 would set a bit for every pixel of
// sky. A real token can in principle equal this (vulkan_history_token only
// avoids 0), in which case that one sector reads as never-drawn and is held one
// level coarser: a 1-in-2^32 detail cost, not a correctness one.
inline constexpr uint32_t kVisibleIdEmpty = 0xFFFFFFFFu;

} // namespace viewer
