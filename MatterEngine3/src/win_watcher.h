#pragma once
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
