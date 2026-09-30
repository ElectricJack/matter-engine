#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vt {
// Retired GPU samples price a row of work. Start conservatively, react to an
// expensive slice immediately, and recover slowly. One row guarantees progress;
// the estimate is a scheduling target, not GPU preemption or a hard deadline.
class VtGpuWorkBudget {
public:
    void set_ms(float ms) { ms_ = std::isfinite(ms) ? std::clamp(ms, 0.0f, 32.0f) : 4.0f; }
    bool enabled() const { return ms_ > 0.0f; }
    uint32_t rows() const {
        // A new page can become costly before its retired sample arrives.
        // Keep that prediction delay to two rows even after cheap prior pages.
        return enabled() ? uint32_t(std::clamp(ms_ / row_ms_, 1.0f, 2.0f)) : 136u;
    }
    bool pair_exceeds_budget(const VtGpuWorkBudget& other) const {
        return enabled() && other.enabled() &&
            row_ms_*float(rows()) + other.row_ms_*float(other.rows()) > ms_ + other.ms_;
    }
    void observe(float ms, uint32_t rows) {
        if (!rows || !std::isfinite(ms) || ms <= 0.0f) return;
        row_ms_ = std::max(std::clamp(ms / float(rows), 0.01f, 1000.0f), row_ms_ * 0.98f);
    }
private:
    float ms_ = 4.0f, row_ms_ = 1.0f;
};
} // namespace vt
