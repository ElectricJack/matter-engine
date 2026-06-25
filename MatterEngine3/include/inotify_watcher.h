#pragma once
#include "file_watcher.h"
#ifdef __linux__
namespace live_edit {
// Linux inotify-backed FileWatcher. Implemented in Task 8.
class InotifyWatcher : public FileWatcher {
public:
    InotifyWatcher();
    ~InotifyWatcher() override;
    void add_watch(const std::string& dir) override;
    int poll(std::vector<FileEvent>& out) override;
    long long now_ms() override;
private:
    int fd_ = -1;
    void* dirs_ = nullptr;
};
} // namespace live_edit
#endif // __linux__
