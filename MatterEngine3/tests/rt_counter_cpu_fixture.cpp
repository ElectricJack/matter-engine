// Included by test_rt_counter_cpu.py after injecting unmodified production
// functions. Intentionally no Vulkan device or fake GPU synchronization.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

constexpr int VK_NULL_HANDLE = 0;
static unsigned warnings = 0, maps = 0, invalidations = 0;
static bool map_ok = true, invalidate_ok = true;
bool pipeline_profile_enabled() { return false; }
template <class... Args> void log_info(Args...) {}
#define MATTER_LOGI(...) log_info(__VA_ARGS__)
#define MATTER_LOGW(...) (++warnings)
struct Buffer { int buffer = 1; void* mapped = nullptr; };
namespace matter {
bool map_buffer(Buffer& b, std::string&) {
    ++maps;
    return map_ok && b.mapped;
}
bool invalidate_buffer(Buffer&, size_t, size_t, std::string&) {
    ++invalidations;
    return invalidate_ok;
}
}
enum class LocalDirectOwner { Raster };
struct FrameResources {
    Buffer rt_error_counter, rt_test_output;
    bool primary_light_audit_pending = false, rt_adaptive_diagnostics_pending = false;
    uint64_t rt_counter_candidate_serial = 0, rt_counter_sample_serial = 0;
    uint64_t geometry_feedback_pending_serial = 0, geometry_feedback_serial = 0;
    bool geometry_feedback_valid = false;
    uint64_t local_direct_candidate_serial = 0, local_direct_lane_revision = 0;
    LocalDirectOwner local_direct_owner = LocalDirectOwner::Raster;
    uint64_t rt_tlas_pending_serial = 0, rt_tlas_geometry_epoch = 0, rt_tlas_pending_epoch = 0;
    bool rt_tlas_valid = false;
};
struct RtLodRecord {
    uint64_t candidate_serial = 0;
    std::shared_ptr<int> candidate, blas;
    bool built = false, geometry_opaque = false, candidate_opaque = false;
    std::vector<uint32_t> material_ids;
};
struct Part {
    std::vector<RtLodRecord> rt_lods;
    bool rt_geometry_classification_dirty = false;
};
bool rt_material_ids_are_opaque(const std::vector<int>&,
                                const std::vector<uint32_t>&) { return true; }
struct BlasCache {
    uint64_t hits = 0, misses = 0, captures = 0, rejected = 0;
    void finish(uint64_t, bool) {}
};
struct DirectHistory {
    uint64_t candidate_serial = 0, presented_index = 0, candidate_index = 0;
    uint64_t presented_token = 0, candidate_token = 0;
    uint64_t presented_scene_key = 0, candidate_scene_key = 0;
    bool reset_pending = false, filtered_valid = false;
};
struct VkSceneRenderer {
    struct { void finish(uint64_t, bool) {} } frame_image_layouts_;
    std::array<FrameResources, 2> frames_{};
    bool primary_light_audit_ = false, rt_adaptive_diagnostics_ = false;
    uint64_t primary_light_audit_frames_ = 0, rt_adaptive_diagnostic_frames_ = 0;
    uint64_t rt_rejected_surface_lookups_total_ = 0;
    uint64_t rt_rejected_surface_lookup_samples_ = 0;
    uint32_t rt_rejected_surface_lookups_last_logged_ = 0;
    std::unique_ptr<BlasCache> blas_cache_;
    uint64_t sparse_candidate_serial_ = 0;
    std::shared_ptr<int> sparse_presented_, sparse_candidate_;
    unsigned sparse_presented_index_ = 0, sparse_candidate_index_ = 0;
    DirectHistory local_direct_history_state_;
    uint64_t gi_candidate_frame_serial_ = 0;
    uint64_t gi_presented_history_index_ = 0, gi_candidate_history_index_ = 0;
    uint64_t gi_presented_attempt_token_ = 0, gi_candidate_attempt_token_ = 0;
    uint64_t gi_presented_scene_key_ = 0, gi_candidate_scene_key_ = 0;
    uint64_t gi_history_reset_count_ = 0;
    bool gi_candidate_was_reset_ = false, gi_history_reset_pending_ = false;
    bool gi_candidate_used_diffuse_reset_ = false, gi_diffuse_history_reset_pending_ = false;
    bool gi_candidate_used_reflection_reset_ = false, gi_reflection_history_reset_pending_ = false;
    std::vector<Part> parts_;
    std::vector<int> material_staging_;
    LocalDirectOwner local_direct_owner_ = LocalDirectOwner::Raster;
    uint64_t local_direct_lane_revision_ = 0;
    void report_rt_trace_counters(uint32_t);
    void finish_ray_tracing_frame(uint64_t, bool);
    std::array<uint64_t, 4> blas_cache_stats() const;
};

// PRODUCTION_FUNCTIONS

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string name = argv[1];
    VkSceneRenderer r;
    std::array<GpuRtCounters, 2> memory{};
    for (unsigned s = 0; s < 2; ++s)
        r.frames_[s].rt_error_counter.mapped = &memory[s];
    // Represents a recorded writer only. Actual promotion is production code;
    // the native fixture is responsible for proving that recording sets it.
    const auto candidate = [&](unsigned slot, uint64_t serial, uint32_t count) {
        assert(r.frames_[slot].rt_counter_sample_serial == 0);
        memory[slot].invalid_part_records = count;
        r.frames_[slot].rt_counter_candidate_serial = serial;
    };
    const auto consume = [&](unsigned slot) { r.report_rt_trace_counters(slot); };
    const auto expect = [&](uint64_t total, uint64_t samples, unsigned logs,
                            unsigned reads) {
        assert(r.rt_rejected_surface_lookups_total_ == total);
        assert(r.rt_rejected_surface_lookup_samples_ == samples);
        assert(warnings == logs);
        assert(maps == reads && invalidations == reads);
    };
    if (name == "poison") {
        memory[0].invalid_part_records = 4234134786u;
        memory[1].invalid_part_records = 1082262592u;
        for (unsigned i = 0; i < 12; ++i) consume(i % 2);
        consume(2); // invalid slot is also a no-op
        expect(0, 0, 0, 0);
    } else if (name == "equal") {
        candidate(0, 1, 7);
        candidate(1, 2, 7);
        consume(0); consume(1); // candidates are not submitted samples
        expect(0, 0, 0, 0);
        r.finish_ray_tracing_frame(1, true);
        r.finish_ray_tracing_frame(2, true);
        consume(0); consume(1);
        expect(14, 2, 1, 2);
    } else if (name == "stale") {
        candidate(0, 1, 2);
        candidate(1, 2, 3);
        r.finish_ray_tracing_frame(1, true);
        r.finish_ray_tracing_frame(2, true);
        // No more producers: retired samples remain consumable, and cycles
        // after that cannot manufacture new samples from unchanged bytes.
        for (unsigned i = 0; i < 12; ++i) consume(i % 2);
        expect(5, 2, 2, 2);
    } else if (name == "submission") {
        // Abandoned recording with no candidate, even with poisoned bytes.
        memory[0].invalid_part_records = 99;
        r.finish_ray_tracing_frame(1, false);
        consume(0);
        candidate(0, 2, 7);
        candidate(1, 3, 8);
        r.finish_ray_tracing_frame(0, true); // serial zero never promotes
        r.finish_ray_tracing_frame(9, true); // other serial never promotes
        consume(0); consume(1);
        expect(0, 0, 0, 0);
        assert(r.frames_[0].rt_counter_candidate_serial == 2);
        assert(r.frames_[1].rt_counter_candidate_serial == 3);
        r.finish_ray_tracing_frame(2, false);
        assert(r.frames_[0].rt_counter_candidate_serial == 0);
        r.finish_ray_tracing_frame(2, true); // cannot revive failed candidate
        r.finish_ray_tracing_frame(3, true);
        consume(0); consume(1);
        expect(8, 1, 1, 1);
        assert(r.frames_[1].rt_counter_candidate_serial == 0);
    } else if (name == "duplicate") {
        candidate(0, 1, 6);
        r.finish_ray_tracing_frame(1, true);
        consume(0); consume(0);
        r.finish_ray_tracing_frame(1, true); // duplicate commit cannot revive
        consume(0);
        expect(6, 1, 1, 1);
    } else if (name == "zero") {
        for (uint64_t serial = 1; serial <= 3; ++serial) {
            candidate(0, serial, serial == 2 ? 0 : 7);
            r.finish_ray_tracing_frame(serial, true);
            consume(0);
        }
        expect(14, 3, 2, 3); // zero sample also resets changed-value logging
    } else if (name == "map-failure" || name == "invalidate-failure") {
        candidate(0, 1, 7);
        r.finish_ray_tracing_frame(1, true);
        map_ok = name != "map-failure";
        invalidate_ok = name != "invalidate-failure";
        consume(0);
        map_ok = invalidate_ok = true;
        consume(0); // failed read retires sample; never retry poisoned bytes
        assert(r.frames_[0].rt_counter_sample_serial == 0);
        assert(r.rt_rejected_surface_lookups_total_ == 0);
        assert(r.rt_rejected_surface_lookup_samples_ == 0);
        assert(warnings == 1 && maps == 1);
        assert(invalidations == (name == "map-failure" ? 0u : 1u));
    } else if (name == "missing-buffer") {
        candidate(0, 1, 7);
        r.finish_ray_tracing_frame(1, true);
        r.frames_[0].rt_error_counter.buffer = VK_NULL_HANDLE;
        consume(0);
        r.frames_[0].rt_error_counter.buffer = 1;
        consume(0); // recreating a handle cannot resurrect its old sample
        expect(0, 0, 0, 0);
        assert(r.frames_[0].rt_counter_sample_serial == 0);
    } else {
        assert(false && "unknown case");
    }
    std::printf("PASS %s\n", name.c_str());
}
