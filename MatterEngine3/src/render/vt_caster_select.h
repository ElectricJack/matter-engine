#pragma once

// M6.5 — choosing which impostored props cast into a terrain sector's pages,
// and identifying the set that was chosen.
//
// WHY THIS IS ITS OWN HEADER. The harvest runs inside VkSceneRenderer, which
// needs a Vulkan device to instantiate and cannot be exercised headlessly. But
// the two things that can be WRONG here are pure arithmetic:
//
//   - the overlap test (get the reach expansion wrong and shadows stop at
//     sector seams, or every sector harvests the whole world), and
//   - the set hash (get it unstable and the bake re-runs forever; get it
//     insensitive and a moved caster keeps a stale shadow).
//
// Both fail QUIETLY — a stale shadow and a correct one look equally plausible
// in a screenshot. Keeping them here, as free functions over plain data, is
// what lets vt_residency_tests assert them without a GPU.

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace vt {

// One candidate caster, in WORLD space. Deliberately not the renderer's
// RtInstance: this header must not know about Vulkan, and the fields below are
// everything the decision needs.
struct CasterCandidate {
    float    aabb_min[3] = {0, 0, 0};   // world-space cluster bounds
    float    aabb_max[3] = {0, 0, 0};
    uint64_t part_hash   = 0;           // already a content hash
    uint32_t rung        = 0;           // the coarse mesh rung the impostor depicts
    // Animated instances are excluded: their occlusion moves every frame, so a
    // baked shadow would be wrong the moment it landed, and the bake would
    // never converge because the caster-set hash would change continuously.
    bool     animated    = false;
    // The receiver casts no shadow onto itself here — self-occlusion is the
    // CONTACT tier's job (vt_enrich_ao.comp), which is tuned for it and which
    // this tier deliberately does not duplicate.
    bool     is_receiver = false;
};

// Does `c` reach into the receiver's bounds?
//
// `reach` EXPANDS THE RECEIVER, and that expansion is not a tolerance — it is
// the mechanism that carries shadows across sector seams. A prop just outside
// a sector still casts into it when the sun is low, so a test against the bare
// bounds would leave a visible discontinuity exactly on the sector grid, which
// reads as a terrain bug rather than a missing caster.
inline bool caster_overlaps_receiver(const float receiver_min[3],
                                     const float receiver_max[3],
                                     float reach,
                                     const CasterCandidate& c) {
    if (c.animated || c.is_receiver) return false;
    for (int i = 0; i < 3; ++i) {
        if (c.aabb_max[i] < receiver_min[i] - reach) return false;
        if (c.aabb_min[i] > receiver_max[i] + reach) return false;
    }
    return true;
}

// Quantum (metres) the relative transform is snapped to before hashing.
//
// Without it the hash is a float-equality test on a matrix and re-bakes on
// noise that moves nothing visible. Too coarse and a caster slides visibly
// before the shadow follows. 1 cm is far below one page texel at the coarse
// mips this tier runs on, so a change big enough to move the hash is a change
// big enough to see.
constexpr float kCasterHashQuantumMeters = 0.01f;

// The identity of a caster SET.
//
// This is what keeps the bake a pure function of its inputs while depending on
// what happens to be loaded. A different loaded set is a DIFFERENT KEY, not
// nondeterminism — the same discipline vt_page_content_salt applies to mode and
// version. Fold it into the page content identity and a page baked against two
// casters is never confused with one baked against three.
//
// ORDER MUST NOT MATTER. The caller harvests in whatever order the instance
// list happens to be in, and that order shifts as parts stream in and out. A
// hash sensitive to it would change without the world changing and re-bake
// forever, so this combines per-caster words with a commutative mix.
inline uint64_t caster_set_hash(const CasterCandidate* casters, size_t count,
                                const float receiver_origin[3]) {
    uint64_t acc = 0;
    for (size_t i = 0; i < count; ++i) {
        const CasterCandidate& c = casters[i];
        // Per-caster identity: content hash + which rung + where it sits
        // RELATIVE to the receiver (so translating a whole sector and its
        // props together does not invalidate anything).
        uint64_t h = 0xcbf29ce484222325ull;
        auto mix = [&h](uint64_t v) {
            h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        };
        mix(c.part_hash);
        mix(c.rung);
        for (int k = 0; k < 3; ++k) {
            const float centre = 0.5f * (c.aabb_min[k] + c.aabb_max[k]);
            const float rel = centre - receiver_origin[k];
            const int64_t q =
                static_cast<int64_t>(rel / kCasterHashQuantumMeters +
                                     (rel >= 0.0f ? 0.5f : -0.5f));
            mix(static_cast<uint64_t>(q));
            const float extent = c.aabb_max[k] - c.aabb_min[k];
            const int64_t qe =
                static_cast<int64_t>(extent / kCasterHashQuantumMeters + 0.5f);
            mix(static_cast<uint64_t>(qe));
        }
        // Commutative combine: addition, so harvest order cannot matter.
        // Multiplying would let a single zero annihilate the set, and XOR
        // would let a duplicated caster cancel itself out.
        acc += h;
    }
    // Fold the COUNT in too. Without it, a set and the same set plus a caster
    // whose own hash folded to zero would collide -- unlikely, but the whole
    // value of this number is that a collision means "do not re-bake".
    acc ^= static_cast<uint64_t>(count) * 0x9E3779B97F4A7C15ull;
    return acc;
}

}  // namespace vt
