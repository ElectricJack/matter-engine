#pragma once

#include "matter/vt_budgets.h"
#include "matter/world_session.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace editor {

// Diagnostic only: bounded RAM capture, one row per successful presentation,
// written after the frame loop. No per-frame file I/O or growing profiler ring.
// Keep names and expressions in one table so schema columns cannot drift from
// their values when new residency counters are added.
#define MATTER_VT_TRACE_FIELDS(X) \
    X(active, stats.vt_active) \
    X(variants, stats.vt_variants) \
    X(pool_used, stats.vt_pool_used) \
    X(pool_capacity, stats.vt_pool_capacity) \
    X(replacement_reserve_pages, stats.vt_replacement_reserve_pages) \
    X(dirty_pages, stats.vt_dirty_pages) \
    X(fills_stale_total, stats.vt_fills_stale_total) \
    X(pinned, stats.vt_pool_pinned) \
    X(queue, stats.vt_queue_depth) \
    X(mandatory_queue, stats.vt_mandatory_queue_depth) \
    X(detail_queue, stats.vt_detail_queue_depth) \
    X(oldest_mandatory_age_frames, stats.vt_oldest_mandatory_age_frames) \
    X(oldest_detail_age_frames, stats.vt_oldest_detail_age_frames) \
    X(fills_total, stats.vt_fills_total) \
    X(evictions_total, stats.vt_evictions_total) \
    X(invalidations_total, stats.vt_invalidations_total) \
    X(pages_dropped_total, stats.vt_pages_dropped_total) \
    X(fills_failed_total, stats.vt_fills_failed_total) \
    X(requests_dropped_total, stats.vt_requests_dropped_total) \
    X(enrich_total, stats.vt_enrich_total) \
    X(enrich_queue, stats.vt_enrich_queue_depth) \
    X(shared_refs_total, stats.vt_shared_refs_total) \
    X(finer_rebuilds_total, stats.vt_finer_rebuilds_total) \
    X(mesh_bytes, stats.vt_mesh_bytes) \
    X(pool_bytes, stats.vt_pool_bytes) \
    X(resident_sectors, stats.resident_sectors) \
    X(vertex_uploads, stats.vk_vertex_uploads) \
    X(cluster_uploads, stats.vk_cluster_uploads) \
    X(dlss_resets, stats.dlss_reset_count) \
    X(output_width, stats.dlss_output_width) \
    X(output_height, stats.dlss_output_height) \
    X(internal_width, stats.dlss_internal_width) \
    X(internal_height, stats.dlss_internal_height) \
    X(cpu_demand_ms, valid_cpu ? stats.vt_cpu_demand_ms : missing) \
    X(cpu_begin_ms, valid_cpu ? stats.vt_cpu_begin_ms : missing) \
    X(cpu_pre_pass_ms, valid_cpu ? stats.vt_cpu_pre_pass_ms : missing) \
    X(cpu_post_pass_ms, valid_cpu ? stats.vt_cpu_post_pass_ms : missing) \
    X(cpu_registration_ms, valid_cpu ? stats.draw_vt_requests_ms : missing) \
    X(gpu_readback_sequence, stats.gpu_timing_sample.sequence) \
    X(gpu_total_ms, (stats.gpu_timing_sample.valid_mask & 1u) ? stats.gpu_timing_sample.milliseconds[0] : missing) \
    X(gpu_vt_page_ms, (stats.gpu_timing_sample.valid_mask & (1u << matter::kGpuTimingVt)) ? stats.gpu_timing_sample.milliseconds[matter::kGpuTimingVt] : missing) \
    X(gpu_vt_feedback_ms, (stats.gpu_timing_sample.valid_mask & (1u << matter::kGpuTimingVtFeedbackReadback)) ? stats.gpu_timing_sample.milliseconds[matter::kGpuTimingVtFeedbackReadback] : missing) \
    X(gpu_vt_ms, valid_vt_gpu ? stats.gpu_timing_sample.milliseconds[matter::kGpuTimingVt] + stats.gpu_timing_sample.milliseconds[matter::kGpuTimingVtFeedbackReadback] : missing) \
    X(fills_per_frame, budgets.fills_per_frame) \
    X(tail_fills_per_frame, budgets.tail_fills_per_frame) \
    X(enrich_per_frame, budgets.enrich_per_frame) \
    X(queue_cap, budgets.queue_cap) \
    X(request_budget_ms, budgets.request_budget_ms) \
    X(linger_frames, budgets.linger_frames) \
    X(evict_protect_frames, budgets.evict_protect_frames)

class VtTrace {
public:
    static constexpr size_t kMaxRows = 32768;
#define MATTER_VT_TRACE_NAME(name, expression) #name,
    inline static constexpr const char* kColumns[] = {
        MATTER_VT_TRACE_FIELDS(MATTER_VT_TRACE_NAME)};
#undef MATTER_VT_TRACE_NAME
    struct Row {
        uint64_t serial = 0;
        uint64_t vt_serial = 0;
        double elapsed_ms = 0;
        std::string marker;
        std::array<double, std::size(kColumns)> values{};
    };

    explicit VtTrace(const char* path) : path_(path ? path : "") {
        if (!path_.empty()) rows_.reserve(kMaxRows);
    }

    bool enabled() const { return !path_.empty(); }

    void record(uint64_t serial, double elapsed_ms,
                const matter::FrameStats& stats, const std::string& marker) {
        if (!enabled()) return;
        if (rows_.size() == kMaxRows) { ++dropped_rows_; return; }
        const bool valid_cpu = stats.vt_active && stats.vt_cpu_frame_serial != 0;
        const double missing = std::numeric_limits<double>::quiet_NaN();
        constexpr uint32_t vt_gpu_mask = (1u << matter::kGpuTimingVt) |
            (1u << matter::kGpuTimingVtFeedbackReadback);
        const bool valid_vt_gpu =
            (stats.gpu_timing_sample.valid_mask & vt_gpu_mask) == vt_gpu_mask;
        const auto& budgets = matter::vt_residency_budgets();
        Row row;
        row.serial = serial;
        row.vt_serial = stats.vt_cpu_frame_serial;
        row.elapsed_ms = elapsed_ms;
        row.marker = marker;
#define MATTER_VT_TRACE_VALUE(name, expression) static_cast<double>(expression),
        row.values = {MATTER_VT_TRACE_FIELDS(MATTER_VT_TRACE_VALUE)};
#undef MATTER_VT_TRACE_VALUE
        rows_.push_back(std::move(row));
    }

    bool write() const {
        if (!enabled()) return true;
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << std::setprecision(17)
            << "{\"schema\":1,\"max_rows\":" << kMaxRows
            << ",\"gpu_time_basis\":\"latest_retired_readback_not_current_frame\","
               "\"cpu_scope\":\"VT demand, hooks and registration; initial runtime creation and source loading separate\","
               "\"gpu_scope\":\"VT pre-pass plus feedback readback; shader feedback writes and sampling remain inside gbuffer\","
               "\"columns\":[";
        for (size_t i = 0; i < std::size(kColumns); ++i) {
            if (i) out << ',';
            quote(out, kColumns[i]);
        }
        out << "]}\n";
        for (const Row& row : rows_) {
            out << "{\"serial\":" << row.serial << ",\"vt_serial\":" << row.vt_serial
                << ",\"elapsed_ms\":" << row.elapsed_ms << ",\"marker\":";
            quote(out, row.marker);
            out << ",\"values\":[";
            for (size_t i = 0; i < row.values.size(); ++i) {
                if (i) out << ',';
                if (std::isfinite(row.values[i])) out << row.values[i];
                else out << "null";
            }
            out << "]}\n";
        }
        out << "{\"end\":true,\"rows\":" << rows_.size()
            << ",\"dropped_rows\":" << dropped_rows_ << "}\n";
        out.flush();
        return out.good();
    }

private:
    static void quote(std::ostream& out, const std::string& value) {
        constexpr char hex[] = "0123456789abcdef";
        out << '"';
        for (const unsigned char c : value) {
            if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
            else if (c < 32u) out << "\\u00" << hex[c >> 4] << hex[c & 15];
            else out << static_cast<char>(c);
        }
        out << '"';
    }
    std::string path_;
    std::vector<Row> rows_;
    size_t dropped_rows_ = 0;
};

#undef MATTER_VT_TRACE_FIELDS
} // namespace editor
