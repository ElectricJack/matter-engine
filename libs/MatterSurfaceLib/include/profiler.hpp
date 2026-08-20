#pragma once


// libs/MatterSurfaceLib/include/profiler.hpp
//
// A small, header-only, wall-clock profiler local to MatterSurfaceLib.
//
// This is NOT the engine's profiler. MatterEngine3 and MatterEditor use
// `libs/ProfileLib` (frame records, Chrome-trace export via MATTER_PROFILE_TRACE,
// the in-editor Performance/Memory panels, compile-out via MATTER_PROFILE=0).
// This header predates that and survives only because `src/blas_manager.cpp`
// and `src/tlas_manager.cpp` still bracket their hot paths with
// `PROFILE_SECTION(...)`. Prefer ProfileLib for anything new.
//
// Usage:
//     PROFILE_FRAME_BEGIN();            // bracket a frame so section times
//     ...                               // can be expressed as a % of it
//     { PROFILE_SECTION("BLAS Build");  // RAII: times the enclosing scope
//       ... }
//     PROFILE_FRAME_END();
//     PROFILE_PRINT();                  // sorted table to stdout
//
// Considerations:
//  - All state lives in one process-wide singleton (`Profiler::instance()`)
//    behind a single mutex. Timestamps are deliberately taken BEFORE the lock
//    so contention is not charged to the measured code, but the lock itself
//    serialises every begin/end across all threads.
//  - `ScopedTimer` (and therefore PROFILE_SECTION / TLAS_PUSH_MATRIX) carries
//    its own start timestamp, so nesting the same name or timing it on two
//    threads at once is safe: each scope reports its own elapsed time.
//    The manual begin_section()/end_section() pair does NOT have that property
//    -- it keys the pending start by name, so a second start for a live name
//    replaces the first. Prefer the RAII form.
//  - Nothing is compiled out. There is no MATTER_PROFILE-style switch here, so
//    the clock reads, the string keys and the mutex are always paid for.
//  - All times are milliseconds. `percentage` is relative to the AVERAGE frame
//    time and only refreshes every 10th `end_frame()`.
//  - `reset_stats()` zeroes the accumulated stats but does not clear in-flight
//    section starts or live ScopedTimers, so a section straddling a reset still
//    records on its end.
//  - Output goes through `printf` to stdout, not through `matter/log.h`, so it
//    does not appear in the editor Console.

#include <algorithm>   // std::sort, std::min, std::max
#include <cstdio>      // printf
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

namespace Performance {

using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::nanoseconds;

// Statistics for a performance section
// One accumulator per named section (and one for the frame itself). All times
// are in milliseconds and are cumulative since construction or the last
// `reset()`. Two things to know:
//  - `min_time_ms` starts at the sentinel 1e9 meaning "no sample yet";
//    `print_stats()` prints 0.0 instead whenever it is still above 1e8.
//  - `percentage` is a cached, lazily-refreshed field (every 10th frame) and
//    is NOT recomputed on `update()`. Reading it directly can give a stale or
//    zero value; `print_stats()` sidesteps it and divides inline.
struct SectionStats {
    std::string name;
    double total_time_ms = 0.0;
    double average_time_ms = 0.0;
    double min_time_ms = 1e9;
    double max_time_ms = 0.0;
    double percentage = 0.0;
    int call_count = 0;
    
    void update(double time_ms) {
        total_time_ms += time_ms;
        call_count++;
        average_time_ms = total_time_ms / call_count;
        min_time_ms = std::min(min_time_ms, time_ms);
        max_time_ms = std::max(max_time_ms, time_ms);
    }
    
    void reset() {
        total_time_ms = 0.0;
        average_time_ms = 0.0;
        min_time_ms = 1e9;
        max_time_ms = 0.0;
        percentage = 0.0;
        call_count = 0;
    }
};

// The process-wide profiler singleton. Created on first `instance()` call
// (function-local static, so initialisation is thread-safe) and never
// destroyed before exit; it holds a `std::mutex`, so it is neither copyable
// nor movable, and the private constructor keeps callers on `instance()`.
//
// Every public method takes `mutex_`, so all of them are safe to call from any
// thread -- but see the name-keying caveat in the file header: concurrent
// sections sharing a name will corrupt each other's timings even though the
// container access itself is synchronised.
//
// Frame bracketing is optional: sections still accumulate without
// begin_frame/end_frame, but percentages are meaningless until a frame time
// exists.
//
// Main profiler class
class Profiler {
public:
    static Profiler& instance() {
        static Profiler prof;
        return prof;
    }
    
    void begin_frame() {
        // Capture timestamp BEFORE acquiring the lock so lock contention
        // does not pollute the frame-start measurement (consistent with
        // end_frame() and begin_section()).
        auto t = Clock::now();
        std::lock_guard<std::mutex> lk(mutex_);
        frame_start_ = t;
        frame_count_++;
    }

    void end_frame() {
        auto frame_end = Clock::now();
        std::lock_guard<std::mutex> lk(mutex_);
        frame_end_ = frame_end;
        auto frame_duration = std::chrono::duration_cast<Duration>(frame_end_ - frame_start_);
        double frame_time_ms = frame_duration.count() / 1e6;

        frame_stats_.update(frame_time_ms);

        // Update percentages every few frames for smoother display
        if (frame_count_ % 10 == 0) {
            update_percentages();
        }
    }
    
    // Manual (non-RAII) section start. Only ONE start is retained per name, so
    // a nested or concurrent section with the same name silently replaces the
    // pending one and the outer/earlier scope's measurement is lost. Allocates
    // a map node plus a copy of the string on a name's first use. `ScopedTimer`
    // does not use this pair -- it times itself via record_section() -- so this
    // caveat applies only to hand-written begin/end calls.
    void begin_section(const std::string& name) {
        // Take a timestamp BEFORE acquiring the lock to keep timing accurate.
        auto t = Clock::now();
        std::lock_guard<std::mutex> lk(mutex_);
        section_starts_[name] = t;
    }

    // Closes the pending section for `name` and folds the elapsed time into
    // its `SectionStats`. Silently does nothing if no start is pending --
    // an unmatched end, or a second end for the same name, is a no-op rather
    // than an error, so a mismatched pair shows up as missing data, not a
    // failure.
    void end_section(const std::string& name) {
        auto end_time = Clock::now();
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = section_starts_.find(name);
        if (it == section_starts_.end()) return;

        auto duration = std::chrono::duration_cast<Duration>(end_time - it->second);
        double time_ms = duration.count() / 1e6;

        sections_[name].name = name;
        sections_[name].update(time_ms);

        section_starts_.erase(it);
    }

    // Fold an already-measured duration into `name`'s stats. This is what
    // ScopedTimer uses: the caller owns its own start timestamp, so nothing is
    // keyed by name while the section is in flight and same-name sections can
    // nest or run concurrently without losing measurements.
    void record_section(const std::string& name, double time_ms) {
        std::lock_guard<std::mutex> lk(mutex_);
        sections_[name].name = name;
        sections_[name].update(time_ms);
    }

    
    // Dumps a table of every section, sorted by average time, to stdout via
    // `printf` (not `matter/log.h`). `const` only in the C++ sense: it does not
    // reset anything, so successive calls report cumulative averages since the
    // last `reset_stats()`. It holds the mutex for the whole dump, and
    // allocates + sorts a temporary index vector, so it is not something to
    // call per frame. Sections that were never entered, or that average under
    // 0.01 ms, are filtered out entirely.
    void print_stats() const {
        std::lock_guard<std::mutex> lk(mutex_);
        printf("\n=== Performance Statistics ===\n");
        printf("Frame: %.2f ms (%.1f FPS)\n",
               frame_stats_.average_time_ms, 1000.0 / frame_stats_.average_time_ms);
        printf("Frames: %d\n", frame_count_);
        printf("\nSection Breakdown:\n");
        printf("%-25s %8s %8s %8s %8s %6s %5s\n", 
               "Section", "Avg(ms)", "Min(ms)", "Max(ms)", "Total(ms)", "Calls", "%");
        printf("%-25s %8s %8s %8s %8s %6s %5s\n", 
               "-------", "-------", "-------", "-------", "--------", "-----", "--");
        
        // Sort sections by average time
        std::vector<const SectionStats*> sorted_sections;
        for (const auto& pair : sections_) {
            sorted_sections.push_back(&pair.second);
        }
        std::sort(sorted_sections.begin(), sorted_sections.end(),
            [](const SectionStats* a, const SectionStats* b) {
                return a->average_time_ms > b->average_time_ms;
            });
        
        // Compute percentages inline against the frame average rather than relying
        // on the lazily-updated percentage field (which only refreshes every 10
        // frames -- at a few fps a short report window never reaches that, leaving
        // every section at 0% and filtered out).
        const double frame_avg = frame_stats_.average_time_ms;
        for (const auto* stats : sorted_sections) {
            // Skip sections with very low activity or that are only initialization-related
            if (stats->call_count == 0 || stats->average_time_ms < 0.01) {
                continue;
            }
            double pct = (frame_avg > 0.0) ? (stats->average_time_ms / frame_avg) * 100.0 : 0.0;

            printf("%-25s %8.2f %8.2f %8.2f %8.2f %6d %4.1f%%\n",
                   stats->name.c_str(),
                   stats->average_time_ms,
                   stats->min_time_ms < 1e8 ? stats->min_time_ms : 0.0,  // Fix crazy min values
                   stats->max_time_ms,
                   stats->total_time_ms,
                   stats->call_count,
                   pct);
        }
        printf("\n");
    }
    
    void reset_stats() {
        std::lock_guard<std::mutex> lk(mutex_);
        frame_stats_.reset();
        frame_count_ = 0;
        for (auto& pair : sections_) {
            pair.second.reset();
        }
    }
    
    double get_frame_time_ms() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return frame_stats_.average_time_ms;
    }
    
    // Average (not most-recent) time for `name`, in milliseconds. Returns 0.0
    // for a name that was never timed, which is indistinguishable from a
    // section that genuinely averaged zero.
    double get_section_time_ms(const std::string& name) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = sections_.find(name);
        return (it != sections_.end()) ? it->second.average_time_ms : 0.0;
    }

private:
    Profiler() = default;
    
    void update_percentages() {
        if (frame_stats_.average_time_ms <= 0.0) return;
        
        for (auto& pair : sections_) {
            pair.second.percentage = (pair.second.average_time_ms / frame_stats_.average_time_ms) * 100.0;
        }
    }
    
    mutable std::mutex mutex_;
    TimePoint frame_start_;
    TimePoint frame_end_;
    SectionStats frame_stats_{"Frame"};
    int frame_count_ = 0;

    std::unordered_map<std::string, SectionStats> sections_;
    std::unordered_map<std::string, TimePoint> section_starts_;
};

// RAII timer class for automatic section timing.
//
// The start timestamp lives in the timer object, not in a name-keyed map, so
// two timers sharing a name -- nested, or on different threads -- each report
// their own elapsed time instead of clobbering one another. As elsewhere, the
// timestamps are taken outside the profiler's lock.
class ScopedTimer {
public:
    explicit ScopedTimer(const std::string& section_name)
        : section_name_(section_name), start_(Clock::now()) {}

    ~ScopedTimer() {
        auto end = Clock::now();
        double time_ms =
            std::chrono::duration_cast<Duration>(end - start_).count() / 1e6;
        Profiler::instance().record_section(section_name_, time_ms);
    }

private:
    std::string section_name_;
    TimePoint   start_;
};

} // namespace Performance

// Convenience macros
#define PROFILE_FRAME_BEGIN() Performance::Profiler::instance().begin_frame()
#define PROFILE_FRAME_END() Performance::Profiler::instance().end_frame()
// The timer variable is named after the line it is expanded on, so two
// PROFILE_SECTION uses in the same scope no longer collide (only two on the
// SAME line would).
#define PROFILE_CONCAT_INNER(a, b) a##b
#define PROFILE_CONCAT(a, b) PROFILE_CONCAT_INNER(a, b)
#define PROFILE_SECTION(name)     Performance::ScopedTimer PROFILE_CONCAT(_profile_timer_, __LINE__)(name)
#define PROFILE_PRINT() Performance::Profiler::instance().print_stats()
#define PROFILE_RESET() Performance::Profiler::instance().reset_stats()