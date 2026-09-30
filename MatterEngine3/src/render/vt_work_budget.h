#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "vt_types.h"

namespace vt {
// Retired GPU samples price a 32-texel tile of work. Start conservatively, react to an
// expensive slice immediately, and recover slowly. One tile guarantees progress;
// the estimate is a scheduling target, not GPU preemption or a hard deadline.
class VtGpuWorkBudget {
public:
    void set_ms(float ms) { ms_ = std::isfinite(ms) ? std::clamp(ms, 0.0f, 32.0f) : 4.0f; }
    bool enabled() const { return ms_ > 0.0f; }
    uint32_t tiles() const {
        // A new page can become costly before its retired sample arrives.
        // Keep that prediction delay to two tiles even after cheap prior pages.
        return enabled() ? uint32_t(std::clamp(ms_ / tile_ms_, 1.0f, 2.0f)) : kVtPageTiles;
    }
    bool pair_exceeds_budget(const VtGpuWorkBudget& other) const {
        return enabled() && other.enabled() &&
            tile_ms_*float(tiles()) + other.tile_ms_*float(other.tiles()) > ms_ + other.ms_;
    }
    void observe(float ms, uint32_t tiles) {
        if (!tiles || !std::isfinite(ms) || ms <= 0.0f) return;
        tile_ms_ = std::max(std::clamp(ms / float(tiles), 0.01f, 1000.0f), tile_ms_ * 0.98f);
    }
private:
    float ms_ = 4.0f, tile_ms_ = 1.0f;
};
} // namespace vt
