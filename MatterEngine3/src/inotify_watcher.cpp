// MatterEngine3/src/inotify_watcher.cpp
//
// The Linux FileWatcher backend for the dev live-edit loop (file_watcher.h).
// The WHOLE translation unit is wrapped in `#ifdef __linux__`, so on Windows
// it compiles to an empty object file and the editor falls back to the other
// backend named in file_watcher.h.
//
// Mechanics and limits
//   - The inotify fd is opened NON-BLOCKING in the constructor; poll() drains
//     it in a loop until read() returns <= 0 (EAGAIN), so a single poll()
//     empties whatever the kernel has queued and never blocks the host tick.
//   - Subscribed mask: IN_CLOSE_WRITE, IN_CREATE, IN_DELETE, IN_MOVED_TO,
//     IN_MOVED_FROM. Editors that save by rename are covered by the MOVED
//     pair; IN_MODIFY is deliberately not used, so a half-written file does
//     not trigger a bake.
//   - NOT RECURSIVE. inotify watches one directory per descriptor, so the
//     caller must add_watch() every directory it cares about; new
//     subdirectories created later are not picked up on their own.
//   - Construction and add_watch failures are silent: a failed
//     inotify_init1() leaves fd_ < 0 and every later call becomes a no-op
//     returning zero events, which reads exactly like "nothing changed".
//   - Event timestamps are stamped with now_ms() at DRAIN time, not at the
//     time the kernel recorded the event.
//
#include "inotify_watcher.h"
#ifdef __linux__
#include <sys/inotify.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <climits>
#include <unordered_map>
#include <cstring>

namespace live_edit {

// wd->dir lets us rebuild absolute paths from per-event file names. Stored as a
// member (declared `void* dirs_` in the header to keep <unordered_map> out of
// it); accessed via this helper.
static std::unordered_map<int, std::string>& wd_map(void*& opaque) {
    if (!opaque) opaque = new std::unordered_map<int, std::string>();
    return *static_cast<std::unordered_map<int, std::string>*>(opaque);
}

InotifyWatcher::InotifyWatcher() {
    fd_ = inotify_init1(IN_NONBLOCK);
}
InotifyWatcher::~InotifyWatcher() {
    if (fd_ >= 0) ::close(fd_);
    delete static_cast<std::unordered_map<int, std::string>*>(dirs_);
}

// Watches `dir` itself only -- not its subdirectories. Silently does nothing
// if the watcher failed to initialize or the path cannot be watched; the
// descriptor is remembered so poll() can turn a per-event file name back into
// an absolute path.
void InotifyWatcher::add_watch(const std::string& dir) {
    if (fd_ < 0) return;
    int wd = inotify_add_watch(fd_, dir.c_str(),
                               IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
    if (wd >= 0) wd_map(dirs_)[wd] = dir;
}

// Drains the kernel queue into `out` and returns how many events were
// appended. Events with no file name (ev->len == 0 -- watch-level
// notifications such as IN_IGNORED) are skipped, and no coalescing is done
// here: one save can surface as several events, which is exactly what
// LiveEditSession's debounce window exists to absorb.
int InotifyWatcher::poll(std::vector<FileEvent>& out) {
    if (fd_ < 0) return 0;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    int added = 0;
    for (;;) {
        ssize_t len = ::read(fd_, buf, sizeof(buf));
        if (len <= 0) break;  // EAGAIN (NONBLOCK) or EOF -> done draining
        for (char* p = buf; p < buf + len; ) {
            auto* ev = reinterpret_cast<struct inotify_event*>(p);
            if (ev->len > 0) {
                auto it = wd_map(dirs_).find(ev->wd);
                std::string dir = (it == wd_map(dirs_).end()) ? std::string() : it->second;
                std::string path = dir.empty() ? std::string(ev->name) : dir + "/" + ev->name;
                FileEventKind k = FileEventKind::Modified;
                if (ev->mask & (IN_CREATE | IN_MOVED_TO))      k = FileEventKind::Created;
                else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) k = FileEventKind::Deleted;
                out.push_back(FileEvent{path, k, now_ms()});
                ++added;
            }
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    return added;
}

long long InotifyWatcher::now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

} // namespace live_edit
#endif // __linux__
