// ConsoleLog implementation: pure ring-buffer logic, no ImGui dependency.
// Kept in its own translation unit (separate from console_panel.cpp's
// draw_console_contents) so the headless unit test
// (tests/test_console_log.cpp) can link it without pulling in ImGui.
//
// Threading: every public method takes `mutex_`, which is what makes push()
// callable from a worker thread while the render thread is formatting rows.
// The lock is held for the whole of filtered(), including the copies.
//
// Timestamps are seconds on a steady_clock (monotonic, immune to wall-clock
// changes), measured from the epoch captured when this ConsoleLog was
// constructed, so they read as "seconds since app start" and never go
// backwards.
#include "console_panel.h"

#include <chrono>

namespace viewer {
namespace {

// Seconds since the first call to this function anywhere in the process. The
// function-local static is the zero point; ConsoleLog's constructor
// subtracts its own reading from every later one, so the value stored on an
// entry is relative to that ConsoleLog's construction.
double steady_seconds() {
    using namespace std::chrono;
    static const steady_clock::time_point epoch = steady_clock::now();
    return duration<double>(steady_clock::now() - epoch).count();
}

} // namespace

ConsoleLog::ConsoleLog(uint32_t capacity)
    : capacity_(capacity > 0 ? capacity : 1), start_time_(steady_seconds()) {
    ring_.resize(capacity_);
}

// Overwrites the OLDEST entry once the ring is full, so the log is a bounded
// tail and not a transcript — nothing warns when messages start falling off
// the back. Timestamped at push time, not at the time the panel draws it.
void ConsoleLog::push(LogSeverity severity, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    LogEntry& slot = ring_[head_];
    slot.severity = severity;
    slot.timestamp = steady_seconds() - start_time_;
    slot.message = message;
    head_ = (head_ + 1) % capacity_;
    if (count_ < capacity_) ++count_;
}

// Copy out the retained entries that pass the three severity checkboxes and
// (optionally) a message substring, OLDEST FIRST. Returns owned copies so the
// caller can iterate without holding the lock — which also means the whole
// scan-and-copy runs under it, briefly blocking worker-thread pushes.
// `text_filter` may be null or empty, both meaning "no text filter"; the match
// is a plain case-SENSITIVE substring test.
ConsoleLog::Snapshot ConsoleLog::filtered(bool show_info, bool show_warning,
                                          bool show_error,
                                          const char* text_filter) const {
    Snapshot snapshot;
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.entries.reserve(count_);
    // Oldest entry is at (head_ - count_) mod capacity_ when the buffer has
    // wrapped; when it hasn't wrapped, entries simply start at index 0.
    const uint32_t start = (head_ + capacity_ - count_) % capacity_;
    const bool has_text_filter = text_filter != nullptr && text_filter[0] != '\0';
    for (uint32_t i = 0; i < count_; ++i) {
        const uint32_t idx = (start + i) % capacity_;
        const LogEntry& entry = ring_[idx];
        switch (entry.severity) {
            case LogSeverity::Info:    if (!show_info) continue; break;
            case LogSeverity::Warning: if (!show_warning) continue; break;
            case LogSeverity::Error:   if (!show_error) continue; break;
        }
        if (has_text_filter &&
            entry.message.find(text_filter) == std::string::npos) {
            continue;
        }
        snapshot.entries.push_back(entry);
    }
    return snapshot;
}

// Resets the read window only. `ring_` keeps its allocation (nothing is freed
// or shrunk) and `start_time_` is untouched, so timestamps after a clear keep
// counting from the same zero.
void ConsoleLog::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    count_ = 0;
}

uint32_t ConsoleLog::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

} // namespace viewer
