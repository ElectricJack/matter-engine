// portable_realpath.h — cross-platform abspath() helper for MatterEngine3
// headless test suites.
//
// UCRT64 (MSYS2's Windows toolchain) does not declare POSIX realpath() in
// <unistd.h>/<stdlib.h>, so any TU that calls it raw fails to COMPILE on
// Windows (tech-debt.md §8). Several test files each carried a byte-identical
// `static std::string abspath(...)` wrapper around realpath(); this header
// replaces all of those copies with one definition, platform-gated.
#pragma once

#include <cstdlib>
#include <limits.h>  // PATH_MAX (UCRT64's <limits.h> redefines this to 512)
#include <string>

// Resolves `rel` to an absolute path. Returns `rel` unchanged if resolution
// fails (mirrors the previous per-file realpath()-based helpers).
static inline std::string abspath(const std::string& rel) {
    char buf[PATH_MAX];
#ifdef _WIN32
    // _fullpath does not require the target to exist (unlike POSIX realpath),
    // so a bad path degrades to a nonexistent absolute path rather than a
    // fallback to `rel` — callers already treat a missing file at the
    // resolved path as failure downstream (e.g. "FAIL: WorldSector.js
    // readable"), so this is an acceptable behavioral difference.
    if (_fullpath(buf, rel.c_str(), sizeof(buf))) return std::string(buf);
#else
    if (realpath(rel.c_str(), buf)) return std::string(buf);
#endif
    return rel;
}
