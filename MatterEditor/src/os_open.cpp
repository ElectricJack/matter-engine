// MatterEditor/src/os_open.cpp
//
// Implementation of the one function declared in os_open.h. Deliberately tiny
// and dependency-free: it exists so the rest of the editor never has to carry
// a `#include <windows.h>` (or its POSIX process headers) just to open a file
// in the user's editor.
//
// Both branches are best-effort — see the contract in os_open.h. Neither
// reports failure, and neither validates `path`.
#include "os_open.h"

#ifdef _WIN32
// This TU alone calls a USER32 shell-display API and does not include raylib.
// The rest of the editor keeps NOUSER defined to avoid raylib/Win32 names.
#ifdef NOUSER
#undef NOUSER
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

namespace viewer {

void os_open_file(const std::string& path) {
#ifdef _WIN32
    ShellExecuteA(NULL, "open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
#else
    // Double fork, so the viewer is never left with a zombie and xdg-open's
    // own child is reparented to init rather than to us: the first child
    // immediately forks again and `_exit(0)`s, the grandchild exec's, and the
    // parent's waitpid() therefore reaps the (already exiting) first child
    // rather than blocking for the lifetime of the opened application.
    // `_exit(127)` mirrors the shell's "command not found" when execlp fails;
    // nothing observes it, since the parent only waits on the first child.
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() != 0) _exit(0);
        execlp("xdg-open", "xdg-open", path.c_str(), (char*)nullptr);
        _exit(127);
    } else if (pid > 0) {
        waitpid(pid, nullptr, 0);
    }
#endif
}

} // namespace viewer
