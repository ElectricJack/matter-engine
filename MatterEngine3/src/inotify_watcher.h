#pragma once
// MatterEngine3/src/inotify_watcher.h
//
// Linux-only FileWatcher backend for the dev live-edit loop. Both this
// declaration and its implementation live inside `#ifdef __linux__`, so the
// type does not exist at all on other platforms -- code that constructs one
// must be guarded the same way. See file_watcher.h for the interface contract
// and inotify_watcher.cpp for the backend's limits (not recursive, silent on
// failure, timestamps taken at drain time).
#include "file_watcher.h"
#ifdef __linux__
namespace live_edit {
// Owns one inotify file descriptor plus the watch-descriptor -> directory map
// needed to rebuild absolute paths. Non-copyable in practice (it holds a raw
// fd closed in the destructor); create one per session and keep it alive for
// as long as the LiveEditSession that references it.
// Linux inotify-backed FileWatcher. Implemented in Task 8.
class InotifyWatcher : public FileWatcher {
public:
    InotifyWatcher();
    ~InotifyWatcher() override;
    void add_watch(const std::string& dir) override;
    int poll(std::vector<FileEvent>& out) override;
    long long now_ms() override;
private:
    // fd_ < 0 means inotify_init1 failed; every method then no-ops.
    // dirs_ is really an `std::unordered_map<int, std::string>*` (watch
    // descriptor -> watched directory), kept opaque so this header does not
    // have to include <unordered_map>. Allocated lazily, deleted in the dtor.
    int fd_ = -1;
    void* dirs_ = nullptr;
};
} // namespace live_edit
#endif // __linux__
