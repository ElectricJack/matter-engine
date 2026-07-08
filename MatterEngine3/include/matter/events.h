#pragma once
#include <string>

namespace matter {

enum class EventType { BakeStarted, BakePartDone, BakeFinished, BakeError };

struct Event {
    EventType type = EventType::BakeStarted;
    std::string module;        // BakePartDone: part module name
    int done = 0, total = 0;   // BakePartDone progress counters
    std::string message;       // BakeError: error detail
};

} // namespace matter
