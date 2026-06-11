#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

namespace Performance {

using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::nanoseconds;

// Statistics for a performance section
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

// Main profiler class
class Profiler {
public:
    static Profiler& instance() {
        static Profiler prof;
        return prof;
    }
    
    void begin_frame() {
        frame_start_ = Clock::now();
        frame_count_++;
    }
    
    void end_frame() {
        frame_end_ = Clock::now();
        auto frame_duration = std::chrono::duration_cast<Duration>(frame_end_ - frame_start_);
        double frame_time_ms = frame_duration.count() / 1e6;
        
        frame_stats_.update(frame_time_ms);
        
        // Update percentages every few frames for smoother display
        if (frame_count_ % 10 == 0) {
            update_percentages();
        }
    }
    
    void begin_section(const std::string& name) {
        section_starts_[name] = Clock::now();
    }
    
    void end_section(const std::string& name) {
        auto it = section_starts_.find(name);
        if (it == section_starts_.end()) return;
        
        auto end_time = Clock::now();
        auto duration = std::chrono::duration_cast<Duration>(end_time - it->second);
        double time_ms = duration.count() / 1e6;
        
        sections_[name].name = name;
        sections_[name].update(time_ms);
        
        section_starts_.erase(it);
    }
    
    void print_stats() const {
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
        
        for (const auto* stats : sorted_sections) {
            // Skip sections with very low activity or that are only initialization-related
            if (stats->call_count == 0 || 
                stats->average_time_ms < 0.01 || 
                stats->percentage < 0.1) {
                continue;
            }
            
            printf("%-25s %8.2f %8.2f %8.2f %8.2f %6d %4.1f%%\n",
                   stats->name.c_str(),
                   stats->average_time_ms,
                   stats->min_time_ms < 1e8 ? stats->min_time_ms : 0.0,  // Fix crazy min values
                   stats->max_time_ms,
                   stats->total_time_ms,
                   stats->call_count,
                   stats->percentage);
        }
        printf("\n");
    }
    
    void reset_stats() {
        frame_stats_.reset();
        frame_count_ = 0;
        for (auto& pair : sections_) {
            pair.second.reset();
        }
    }
    
    double get_frame_time_ms() const {
        return frame_stats_.average_time_ms;
    }
    
    double get_section_time_ms(const std::string& name) const {
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
    
    TimePoint frame_start_;
    TimePoint frame_end_;
    SectionStats frame_stats_{"Frame"};
    int frame_count_ = 0;
    
    std::unordered_map<std::string, SectionStats> sections_;
    std::unordered_map<std::string, TimePoint> section_starts_;
};

// RAII timer class for automatic section timing
class ScopedTimer {
public:
    explicit ScopedTimer(const std::string& section_name) 
        : section_name_(section_name) {
        Profiler::instance().begin_section(section_name_);
    }
    
    ~ScopedTimer() {
        Profiler::instance().end_section(section_name_);
    }

private:
    std::string section_name_;
};

} // namespace Performance

// Convenience macros
#define PROFILE_FRAME_BEGIN() Performance::Profiler::instance().begin_frame()
#define PROFILE_FRAME_END() Performance::Profiler::instance().end_frame()
#define PROFILE_SECTION(name) Performance::ScopedTimer _timer(name)
#define PROFILE_PRINT() Performance::Profiler::instance().print_stats()
#define PROFILE_RESET() Performance::Profiler::instance().reset_stats()