// matter/log.h -- the engine's one lightweight logging facility.
//
// WHY THIS EXISTS. Before this header the codebase logged ad hoc through
// ~500 raw printf/fprintf(stderr, "[tag] ...") call sites, none of which the
// in-editor Console panel (viewer::ConsoleLog) could see. This routes every
// migrated log through a single sink so the editor can mirror engine output
// into its Console, while a compile-time tee keeps the same lines flowing to
// stderr for headless/QA runs that grep the terminal.
//
// WHY HEADER-ONLY. The engine is compiled three different ways -- the headless
// archive (ME3_CPP), the editor's viewer-lib (VIEWER_CPP), and a dozen
// standalone test exes (MatterEditor/Makefile) that each hand-pick a subset of
// engine .cpp on their own link line. A logging *.cpp would have to be added to
// every one of those link lines or they would fail to resolve log_write. An
// inline, function-local-static implementation makes any TU that includes this
// header self-sufficient: zero Makefile edits, one shared sink registry per
// linked image (all builds here are static archives into one exe).
//
// USAGE (printf-style; the tag replaces the old "[tag]" prefix, and a trailing
// '\n' is optional -- the sink strips it and the tee adds exactly one):
//   MATTER_LOGI("stream", "sector %d ready in %.1f ms", id, ms);
//   MATTER_LOGW("props",  "unknown key '%s', ignored", key);
//   MATTER_LOGE("vk",     "device lost during submit");
//
// COMPILE-TIME REMOVAL. Define MATTER_LOG_MIN_LEVEL to a MATTER_LOG_LEVEL_*
// value to strip every macro below that level to `((void)0)` -- args are not
// evaluated and nothing is emitted. e.g. -DMATTER_LOG_MIN_LEVEL=3 keeps only
// warnings and errors; -DMATTER_LOG_MIN_LEVEL=6 compiles logging out entirely.
// Define MATTER_LOG_TEE=0 to suppress the stderr tee (sinks still fire).

#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// ---- severity levels (numeric so MATTER_LOG_MIN_LEVEL can compare) ----------
#define MATTER_LOG_LEVEL_TRACE 0
#define MATTER_LOG_LEVEL_DEBUG 1
#define MATTER_LOG_LEVEL_INFO  2
#define MATTER_LOG_LEVEL_WARN  3
#define MATTER_LOG_LEVEL_ERROR 4
#define MATTER_LOG_LEVEL_OFF   6

// Everything compiled in by default; a build/CLI can raise the floor.
#ifndef MATTER_LOG_MIN_LEVEL
#define MATTER_LOG_MIN_LEVEL MATTER_LOG_LEVEL_TRACE
#endif

// Tee migrated logs to stderr as well as to registered sinks (default on).
#ifndef MATTER_LOG_TEE
#define MATTER_LOG_TEE 1
#endif

namespace matter {
namespace log {

enum class Level : int {
    Trace = MATTER_LOG_LEVEL_TRACE,
    Debug = MATTER_LOG_LEVEL_DEBUG,
    Info  = MATTER_LOG_LEVEL_INFO,
    Warn  = MATTER_LOG_LEVEL_WARN,
    Error = MATTER_LOG_LEVEL_ERROR,
};

// A sink receives every emitted log line: its level, the (possibly empty) tag,
// and the formatted message with any trailing newline already stripped. Called
// on whatever thread logged -- the editor's sink pushes into a thread-safe ring
// buffer, so sinks must be prepared for concurrent calls.
using Sink = void (*)(Level level, const char* tag, const char* message,
                      void* user);

namespace detail {

struct SinkEntry {
    Sink fn = nullptr;
    void* user = nullptr;
};

// One registry per linked image (Meyers singleton keeps this header-only and
// ODR-safe -- every TU that includes the header shares the same instance).
inline std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}
inline std::vector<SinkEntry>& registry() {
    static std::vector<SinkEntry> sinks;
    return sinks;
}

inline const char* level_name(Level level) {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
    }
    return "?";
}

} // namespace detail

// Register/unregister a sink. Registration is idempotent per (fn,user) pair;
// removal drops the matching pair. Both are safe to call at any time.
inline void add_sink(Sink fn, void* user = nullptr) {
    if (!fn) return;
    std::lock_guard<std::mutex> lock(detail::registry_mutex());
    auto& sinks = detail::registry();
    for (const auto& e : sinks) {
        if (e.fn == fn && e.user == user) return;
    }
    sinks.push_back({fn, user});
}

inline void remove_sink(Sink fn, void* user = nullptr) {
    std::lock_guard<std::mutex> lock(detail::registry_mutex());
    auto& sinks = detail::registry();
    for (size_t i = 0; i < sinks.size(); ++i) {
        if (sinks[i].fn == fn && sinks[i].user == user) {
            sinks.erase(sinks.begin() + static_cast<long>(i));
            return;
        }
    }
}

// Core emit. Formats the message, tees to stderr (unless compiled out), and
// fans out to every registered sink. Callers use the MATTER_LOG* macros below
// rather than calling this directly, so severity gating happens at compile time.
inline void write(Level level, const char* tag, const char* fmt, ...)
#if defined(__GNUC__)
    // gnu_printf, not printf: on MinGW the plain `printf` archetype maps to the
    // MSVCRT format checker, which rejects %zu/%llu/%lld and would fire -Wformat
    // (a hard error under the smoke build's -Werror) on migrated calls. The
    // codebase compiles with MinGW-ANSI stdio, whose own printf decls use
    // gnu_printf -- matching that keeps our checking identical to theirs.
    __attribute__((format(gnu_printf, 3, 4)))
#endif
    ;

inline void write(Level level, const char* tag, const char* fmt, ...) {
    char stackbuf[1024];
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(stackbuf, sizeof(stackbuf), fmt ? fmt : "", ap);
    va_end(ap);

    std::string heap;
    const char* msg = stackbuf;
    if (n < 0) {
        msg = "";
        n = 0;
    } else if (static_cast<size_t>(n) >= sizeof(stackbuf)) {
        heap.resize(static_cast<size_t>(n) + 1);
        std::vsnprintf(&heap[0], heap.size(), fmt ? fmt : "", ap2);
        heap.resize(static_cast<size_t>(n));  // drop the trailing '\0'
        msg = heap.c_str();
    }
    va_end(ap2);

    // Strip a single trailing newline so migrated `"...\n"` format strings and
    // newline-free ones both yield one clean console line. We work on a mutable
    // copy only when there is a newline to strip.
    std::string trimmed;
    size_t len = std::strlen(msg);
    if (len > 0 && msg[len - 1] == '\n') {
        trimmed.assign(msg, len - 1);
        if (!trimmed.empty() && trimmed.back() == '\r') trimmed.pop_back();
        msg = trimmed.c_str();
    }

    const char* safe_tag = tag ? tag : "";

#if MATTER_LOG_TEE
    if (safe_tag[0] != '\0') {
        std::fprintf(stderr, "[%s] %s\n", safe_tag, msg);
    } else {
        std::fprintf(stderr, "%s\n", msg);
    }
#endif

    std::lock_guard<std::mutex> lock(detail::registry_mutex());
    for (const auto& e : detail::registry()) {
        e.fn(level, safe_tag, msg, e.user);
    }
}

} // namespace log
} // namespace matter

// ---- call-site macros -------------------------------------------------------
// Each is compiled to `((void)0)` when its level is below MATTER_LOG_MIN_LEVEL,
// so raising the floor removes both the call and its argument evaluation.
#if MATTER_LOG_MIN_LEVEL <= MATTER_LOG_LEVEL_TRACE
#define MATTER_LOGT(tag, ...) ::matter::log::write(::matter::log::Level::Trace, (tag), __VA_ARGS__)
#else
#define MATTER_LOGT(tag, ...) ((void)0)
#endif

#if MATTER_LOG_MIN_LEVEL <= MATTER_LOG_LEVEL_DEBUG
#define MATTER_LOGD(tag, ...) ::matter::log::write(::matter::log::Level::Debug, (tag), __VA_ARGS__)
#else
#define MATTER_LOGD(tag, ...) ((void)0)
#endif

#if MATTER_LOG_MIN_LEVEL <= MATTER_LOG_LEVEL_INFO
#define MATTER_LOGI(tag, ...) ::matter::log::write(::matter::log::Level::Info, (tag), __VA_ARGS__)
#else
#define MATTER_LOGI(tag, ...) ((void)0)
#endif

#if MATTER_LOG_MIN_LEVEL <= MATTER_LOG_LEVEL_WARN
#define MATTER_LOGW(tag, ...) ::matter::log::write(::matter::log::Level::Warn, (tag), __VA_ARGS__)
#else
#define MATTER_LOGW(tag, ...) ((void)0)
#endif

#if MATTER_LOG_MIN_LEVEL <= MATTER_LOG_LEVEL_ERROR
#define MATTER_LOGE(tag, ...) ::matter::log::write(::matter::log::Level::Error, (tag), __VA_ARGS__)
#else
#define MATTER_LOGE(tag, ...) ((void)0)
#endif
