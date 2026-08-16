#pragma once

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace viewer {

enum class LogSeverity : uint8_t { Info, Warning, Error };

struct LogEntry {
    LogSeverity severity = LogSeverity::Info;
    double timestamp = 0.0;  // seconds since app start
    std::string message;
};

// Thread-safe ring buffer for log messages.
class ConsoleLog {
public:
    explicit ConsoleLog(uint32_t capacity = 4096);

    // Thread-safe push (called from bake callbacks on worker threads).
    void push(LogSeverity severity, const std::string& message);

    // Access for rendering (call from main thread only).
    // Returns owned copies so the caller can iterate without holding the lock.
    struct Snapshot {
        std::vector<LogEntry> entries;
    };
    Snapshot filtered(bool show_info, bool show_warning, bool show_error,
                      const char* text_filter) const;

    void clear();
    uint32_t size() const;

private:
    mutable std::mutex mutex_;
    std::vector<LogEntry> ring_;
    uint32_t capacity_ = 4096;
    uint32_t head_ = 0;     // next write position
    uint32_t count_ = 0;    // number of valid entries
    double start_time_ = 0.0;
};

struct ConsolePanelState {
    bool show_info = true;
    bool show_warning = true;
    bool show_error = true;
    char text_filter[256] = {};
    bool auto_scroll = true;
    bool was_at_bottom = true;

    // Line selection (for copy-to-clipboard). Indices are into the most recent
    // filtered snapshot, oldest->newest. Transient UI state, not persisted:
    // cleared when the console is cleared or a copy consumes it is optional.
    std::set<int> selection;
    int sel_anchor = -1;  // last clicked row, for shift-click range selection
};

// Draw the console panel contents (call inside Begin/End).
void draw_console_contents(ConsolePanelState& state, ConsoleLog& log);

} // namespace viewer
