#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vt {

// CPU collection of four-u16 requests extracted from both halves of the
// RGBA32_UINT visible feedback image. Sorting uses
// (transport owner, mip, y, x), equivalent to the residency queue's previous
// (owner index, mip, y, x) order. No request is sampled out or deferred here.
class VtFeedbackKeys {
public:
    void begin(size_t reserve_hint = 0) {
        keys_.clear();
        keys_.reserve(reserve_hint);
        recent_.fill(0);
        raw_hits_ = run_hits_ = 0;
    }

    // The test injection seam carries u32 fields; reject values the actual
    // feedback format cannot encode instead of aliasing another owner's key.
    void add_request(uint32_t owner_index, uint32_t mip, uint32_t x, uint32_t y) {
        if (owner_index >= 65535u || mip > 65535u || x > 65535u || y > 65535u) return;
        add(pack(owner_index + 1u, mip, x, y));
    }

    void append_texels(const uint16_t* rgba, size_t texel_count) {
        const uint16_t endian_probe = 1;
        const bool little_endian =
            *reinterpret_cast<const unsigned char*>(&endian_probe) == 1;
        uint64_t previous = 0; // every valid key has a nonzero owner
        uint64_t raw_hits = 0, run_hits = 0;
        for (size_t i = 0; i < texel_count; ++i) {
            const uint16_t* t = rgba + i * 4u;
            // The transport already occupies one 64-bit word. Compare that
            // word before reordering fields, and do not require u64 alignment
            // or violate aliasing by casting the uint16 input pointer.
            uint64_t texel;
            std::memcpy(&texel, t, sizeof(texel));
            if (little_endian ? (texel & 0xFFFFu) == 0 : (texel >> 48) == 0)
                continue;
            ++raw_hits;
            if (texel == previous) continue;
            previous = texel;
            ++run_hits;
            // Little-endian transport [owner,x,y,mip] becomes sort order
            // [x,y,mip,owner] by rotating one field. The probe folds at compile
            // time; retain the field-based path on big-endian hosts.
            const uint64_t key = little_endian
                ? (texel >> 16) | (texel << 48)
                : pack(t[0], t[3], t[1], t[2]);
            add(key);
        }
        raw_hits_ += raw_hits;
        run_hits_ += run_hits;
    }

    const std::vector<uint64_t>& finish() {
        std::sort(keys_.begin(), keys_.end());
        keys_.erase(std::unique(keys_.begin(), keys_.end()), keys_.end());
        return keys_;
    }

    uint64_t raw_hits() const { return raw_hits_; }
    uint64_t run_hits() const { return run_hits_; }
    size_t candidates() const { return keys_.size(); }

private:
    static uint64_t pack(uint32_t owner, uint32_t mip, uint32_t x, uint32_t y) {
        return (uint64_t(owner) << 48) | (uint64_t(mip) << 32) |
               (uint64_t(y) << 16) | uint64_t(x);
    }

    void add(uint64_t key) {
        // A bounded duplicate filter, not a capacity-limited request set.
        // Collisions only let a duplicate through; full-key equality is the
        // only rejection rule, and finish() removes remaining duplicates.
        // Thus arbitrarily interleaved owners/pages cannot lose a request or
        // change deterministic queue order. 32 KiB replaces repeated sorting
        // of the same pages from hundreds of separate screen rows.
        const uint64_t folded = key ^ (key >> 32);
        const size_t bucket = size_t((folded * 0x9E3779B97F4A7C15ull) >> 52);
        if (recent_[bucket] == key) return;
        recent_[bucket] = key;
        keys_.push_back(key);
    }

    std::array<uint64_t, 4096> recent_{};
    // Reused across frames; growth is bounded by the input feedback extent
    // plus explicit test injection, never by the duration of a session.
    std::vector<uint64_t> keys_;
    uint64_t raw_hits_ = 0;
    uint64_t run_hits_ = 0;
};

} // namespace vt
