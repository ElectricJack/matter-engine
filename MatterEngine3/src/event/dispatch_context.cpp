// src/event/dispatch_context.cpp — thread-local storage backing
// matter/event/detail/dispatch_context.h. See that header for the
// contract; this file only owns the actual thread_local objects (kept in
// one TU so there is exactly one definition per thread regardless of how
// many other TUs include the header).
#include "matter/event/detail/dispatch_context.h"

#include <algorithm>

namespace matter::evt::detail {

std::vector<const SubscriptionBlock*>& dispatch_stack() {
    static thread_local std::vector<const SubscriptionBlock*> stack;
    return stack;
}

bool is_dispatching_on_this_thread(const SubscriptionBlock* b) {
    auto& s = dispatch_stack();
    return std::find(s.begin(), s.end(), b) != s.end();
}

int& emit_depth() {
    static thread_local int depth = 0;
    return depth;
}

}  // namespace matter::evt::detail
