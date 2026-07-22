// matter/event/detail/dispatch_context.h — INTERNAL. Not part of the
// public evt:: API; shared only between event_hub.h/.cpp (which push/pop
// while invoking callbacks) and subscription.cpp (which reads it to
// detect the illegal self-wait case in
// SubscriptionSet::unsubscribe_all_and_wait). Lives under include/ rather
// than src/ solely so it is reachable from the public, template-heavy
// event_hub.h without requiring consumers of that header to add -Isrc to
// their include path.
//
// Design authority: MatterEngine3/docs/event-system.md S I.6 (reentrancy:
// active-check-before-entry, emit-depth cap) and S I.5/I.6 (the illegal
// self-wait case in unsubscribe_all_and_wait).
#pragma once
#include <vector>

namespace matter::evt {
struct SubscriptionBlock;
}

namespace matter::evt::detail {

// Control blocks whose callback is currently executing on THIS thread's
// call stack (immediate dispatch or lane-pump dispatch, possibly nested
// via handler-emitted events). unsubscribe_all_and_wait() fail-fasts if
// any of its own blocks appear here on the calling thread — waiting for
// in_flight to reach zero in that case would deadlock waiting on its own
// stack frame.
std::vector<const SubscriptionBlock*>& dispatch_stack();

// RAII push/pop around one callback invocation.
struct ScopedDispatch {
    explicit ScopedDispatch(const SubscriptionBlock* b) { dispatch_stack().push_back(b); }
    ~ScopedDispatch() { dispatch_stack().pop_back(); }
    ScopedDispatch(const ScopedDispatch&) = delete;
    ScopedDispatch& operator=(const ScopedDispatch&) = delete;
};

bool is_dispatching_on_this_thread(const SubscriptionBlock* b);

// Thread-local handler-emitted-event nesting depth (S I.6: all-build
// fail-fast at depth 8, cf. registration_error.h's fail_fast). Exposed as
// a mutable reference so Hub::emit/pump can increment/decrement it with a
// small RAII guard at each call site.
int& emit_depth();

}  // namespace matter::evt::detail
