#pragma once
// MatterEngine3/src/win_watcher.h — Windows FileWatcher backend (stubbed).
// The entire body is inside #ifdef _WIN32, so including it on another
// platform is a no-op. See file_watcher.h for the interface and the live-edit
// contract (LiveEditSession holds a watcher by reference and polls it once per
// host tick). Nothing in the engine or the editor constructs this class today;
// its only reference is dev_live_edit_tests.cpp, which asserts that
// constructing it throws — so the deferral cannot quietly turn into a watcher
// that reports no changes.
#include "file_watcher.h"
#ifdef _WIN32
#include <stdexcept>
namespace live_edit {
// DEFERRED / STUBBED (SP-5): Windows ReadDirectoryChangesW backend is not yet
// implemented. The Linux inotify backend is the fully-implemented v1 path. This
// stub makes the unimplemented state explicit (throws) instead of silently
// no-op'ing. Implement with an overlapped ReadDirectoryChangesW loop mapping
// FILE_NOTIFY_INFORMATION -> FileEvent, mirroring InotifyWatcher.
class WinDirWatcher : public FileWatcher {
public:
    WinDirWatcher() { throw std::runtime_error("WinDirWatcher: ReadDirectoryChangesW backend not implemented (SP-5 deferred)"); }
    void add_watch(const std::string&) override {}
    int poll(std::vector<FileEvent>&) override { return 0; }
    long long now_ms() override { return 0; }
};
} // namespace live_edit
#endif // _WIN32
