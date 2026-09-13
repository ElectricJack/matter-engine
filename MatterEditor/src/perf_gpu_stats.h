#pragma once
#include "matter/gpu_timing_sample.h"
#include <algorithm>
#include <cmath>
#include <ostream>
#include <vector>

namespace viewer {
// Collect retired RAW timestamps, once per readback. Percentiles describe
// executions with available timestamp pairs, not absent passes or EMA values.
class PerfGpuStats {
public:
    void reset(uint64_t current_sequence = 0) {
        last_sequence_ = current_sequence;
        for (auto& samples : samples_) samples.clear();
    }
    void add(const matter::GpuTimingSample& sample) {
        if (!sample.sequence || sample.sequence <= last_sequence_) return;
        last_sequence_ = sample.sequence;
        for (size_t i = 0; i < samples_.size(); ++i) {
            const float ms = sample.milliseconds[i];
            if ((sample.valid_mask & (1u << i)) && std::isfinite(ms) && ms >= 0)
                samples_[i].push_back(ms);
        }
    }
    void append_json(std::ostream& out) const {
        out << ",\"gpu_pass_statistics\":{\"metric\":\"raw_available_executions\","
               "\"window\":\"readbacks_observed_during_sampling\","
               "\"median_method\":\"midpoint\",\"p95_method\":\"nearest_rank\",\"passes\":{";
        for (size_t i = 0; i < samples_.size(); ++i) {
            if (i) out << ',';
            out << '"' << matter::kGpuTimingNames[i] << "\":{\"samples\":"
                << samples_[i].size() << ",\"median_ms\":";
            if (samples_[i].empty()) {
                out << "null,\"p95_ms\":null}";
                continue;
            }
            auto sorted = samples_[i];
            std::sort(sorted.begin(), sorted.end());
            const size_t n = sorted.size();
            const double median = n % 2 ? sorted[n / 2]
                : (sorted[n / 2 - 1] + sorted[n / 2]) * 0.5;
            const size_t p95 = static_cast<size_t>(std::ceil(n * 0.95)) - 1;
            out << median << ",\"p95_ms\":" << sorted[p95] << '}';
        }
        out << "}}";
    }
private:
    uint64_t last_sequence_ = 0;
    std::array<std::vector<double>, matter::kGpuTimingNames.size()> samples_{};
};
} // namespace viewer
