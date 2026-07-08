#pragma once
#include <string>
#include <vector>
#include <functional>
#include <deque>

// OS-native file-change abstraction for SP-5 dev live-edit. Real backends:
// InotifyWatcher (Linux), WinDirWatcher (Windows, stubbed). FakeWatcher drives
// tests with synthetic events. See
// docs/superpowers/specs/2026-06-24-dev-live-edit-design.md
namespace live_edit {

enum class FileEventKind { Modified, Created, Deleted };

struct FileEvent {
    std::string path;        // absolute path of the changed script file
    FileEventKind kind = FileEventKind::Modified;
    long long t_ms = 0;      // monotonic event time in milliseconds
};

// A watcher emits FileEvents for files under the watched directories. poll()
// returns events that have arrived since the last call (already coalesced by
// the backend where cheap; LiveEditSession applies the debounce window).
class FileWatcher {
public:
    virtual ~FileWatcher() = default;
    virtual void add_watch(const std::string& dir) = 0;
    // Non-blocking: append newly observed events to `out`. Returns count added.
    virtual int poll(std::vector<FileEvent>& out) = 0;
    // Monotonic clock the session uses for debounce. Real backends use a steady
    // clock; the fake lets tests advance it deterministically.
    virtual long long now_ms() = 0;
};

// Test double: tests push events and control the clock; no OS involvement.
class FakeWatcher : public FileWatcher {
public:
    void add_watch(const std::string& dir) override { watched_.push_back(dir); }
    int poll(std::vector<FileEvent>& out) override {
        int n = 0;
        while (!pending_.empty()) { out.push_back(pending_.front()); pending_.pop_front(); ++n; }
        return n;
    }
    long long now_ms() override { return clock_ms_; }

    // --- test controls ---
    void push(const std::string& path, FileEventKind k = FileEventKind::Modified) {
        pending_.push_back(FileEvent{path, k, clock_ms_});
    }
    void advance_ms(long long d) { clock_ms_ += d; }
    void set_now_ms(long long t) { clock_ms_ = t; }
    const std::vector<std::string>& watched() const { return watched_; }
private:
    std::deque<FileEvent> pending_;
    std::vector<std::string> watched_;
    long long clock_ms_ = 0;
};

} // namespace live_edit
