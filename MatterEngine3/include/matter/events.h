#pragma once
#include <string>

namespace matter {

enum class EventType { BakeStarted, BakePartDone, BakeFinished, BakeError };

// Structured bake-error classification (Phase B). None on non-error events.
enum class BakeErrorCode { None, Cancelled, OutOfMemory, ScriptError, GpuError, IoError, Internal };

struct Event {
    EventType type = EventType::BakeStarted;
    std::string module;        // BakePartDone/BakeError: part module name (may be empty)
    int done = 0, total = 0;   // BakePartDone counters (total 0 = indeterminate phase)
    std::string message;       // BakeError: error detail
    // --- Phase B additions (struct is append-only) ---
    std::string phase;         // "install" | "compose" | "parts" | "gl" | "cone" | ""
    BakeErrorCode code = BakeErrorCode::None;   // BakeError classification
    int errors = 0;            // BakeFinished: failed-part count (skip-and-continue)
};

} // namespace matter
