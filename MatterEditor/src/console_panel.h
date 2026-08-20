#pragma once

// MatterEditor/src/console_panel.h
//
// The editor's Console panel: a thread-safe in-memory log ring buffer
// (`ConsoleLog`) plus the ImGui widget that renders it (`ConsolePanelState` +
// draw_console_contents).
//
// Layering. The two halves live in separate translation units on purpose:
// console_log.cpp implements ConsoleLog with no ImGui dependency, so the
// headless unit test (MatterEditor/tests/test_console_log.cpp) can link it,
// and console_panel.cpp implements draw_console_contents(), the only part that
// needs ImGui. issue_reporter.cpp uses the same split, and it is also the
// log's other reader: write_log_tail() dumps an unfiltered snapshot into a
// filed issue.
//
// This is NOT a sink for matter/log.h — no MATTER_LOG* output is forwarded
// here. Entries arrive only through explicit ConsoleLog::push() calls, nearly
// all of them in MatterEditor/src/main.cpp (world connect, bake and save/load
// failures), some of them from worker threads, which is why the buffer locks.
//
// Ownership and lifetime: main.cpp owns the single ConsoleLog for the life of
// the process and passes it (with the Ui-owned ConsolePanelState) into
// Ui::draw_console_panel every frame. Neither object is copied.
//
// Control surface: the three show_* flags and auto_scroll are ALSO bound into
// the property registry as the `console.filters` group at Scope::User
// (editor_props.cpp), so they persist to editor_settings.json and a headless
// QA run can toggle them over the FIFO command file with
// `set console.filters.show_info 0` (docs/agent/control-surface.md). The
// panel's checkboxes edit the very same struct fields directly.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace viewer {

// Severity of one console line. Selects the row colour in the panel and which
// of the three filter checkboxes must be on for the row to be shown; there is
// no ordering relation implied (the filters are three independent toggles, not
// a minimum level).
enum class LogSeverity : uint8_t { Info, Warning, Error };

// One captured log line. Stored by value in the ring and copied out wholesale
// by ConsoleLog::filtered(), so a reader never holds a pointer into the ring.
struct LogEntry {
    LogSeverity severity = LogSeverity::Info;
    double timestamp = 0.0;  // seconds since app start
    std::string message;
};

// Thread-safe ring buffer for log messages.
//
// Fixed capacity, decided at construction and never grown: once full, the
// oldest entry is overwritten, so this is a bounded tail rather than a
// transcript, and nothing signals that messages were dropped. All four public
// methods take `mutex_`, so push() is safe from any thread while the render
// thread reads.
//
// Cost note: filtered() is O(retained entries) and allocates a copy of every
// surviving message. The panel calls it once per frame, which is affordable at
// the default 4096 but is the first thing to look at if the capacity grows.
class ConsoleLog {
public:
    // `capacity` is the number of entries retained, not a byte budget; 0 is
    // clamped up to 1. The ring is allocated in full up front and never
    // resized, and the timestamp epoch is captured here.
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

    // Drop every retained entry (the panel's Clear button). Resets the read
    // window only: the storage is kept and the timestamp epoch is unchanged,
    // so later entries keep counting from the same app-start zero.
    void clear();
    // Entries currently retained — NOT the number ever pushed. Takes the lock.
    uint32_t size() const;

private:
    mutable std::mutex mutex_;
    std::vector<LogEntry> ring_;
    uint32_t capacity_ = 4096;
    uint32_t head_ = 0;     // next write position
    uint32_t count_ = 0;    // number of valid entries
    double start_time_ = 0.0;
};

// Per-panel view state: which severities are shown, the substring filter, and
// the auto-scroll bookkeeping. Ui owns one instance for the process.
//
// The three show_* flags and auto_scroll are also the bound fields of the
// `console.filters` property group (Scope::User), so they persist and are
// reachable from the FIFO `set` path. `text_filter` is deliberately not bound:
// it is a char[256] because ImGui::InputText writes into a fixed buffer, and
// the property schema has no type for that (its String means std::string).
struct ConsolePanelState {
    bool show_info = true;
    bool show_warning = true;
    bool show_error = true;
    char text_filter[256] = {};
    bool auto_scroll = true;
    // Whether the scroll region ended LAST frame pinned to the bottom.
    // Auto-scroll only snaps to the tail while this holds, so scrolling up to
    // read history pauses the follow until you scroll back down. Scroll
    // bookkeeping, not a setting — which is why it is not in the bound group.
    bool was_at_bottom = true;
};

// Draw the console panel contents (call inside Begin/End).
//
// ImGui/main thread only. Snapshots and re-formats the whole filtered log on
// every call, writes back into `state`, and can call log.clear() (the Clear
// button).
void draw_console_contents(ConsolePanelState& state, ConsoleLog& log);

} // namespace viewer
