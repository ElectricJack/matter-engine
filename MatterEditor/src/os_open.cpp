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
