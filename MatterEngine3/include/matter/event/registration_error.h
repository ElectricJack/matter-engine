// matter/event/registration_error.h — the all-build fail-fast hook shared
// by Hub::must_subscribe (duplicate name) and
// SubscriptionSet::unsubscribe_all_and_wait (illegal self-wait).
//
// Design authority: MatterEngine3/docs/event-system.md
//   S I.5 rule 2   — try_subscribe returns DuplicateName; must_subscribe
//                    routes the same condition to "the process
//                    registration-error handler" and never inserts the
//                    duplicate.
//   S I.5/I.6      — unsubscribe_all_and_wait() is illegal from inside an
//                    affected callback or the affected lane's pump, and
//                    that is fail-fast in every build.
//   S II.4 item 7  — "Debug-only uniqueness" correction: duplicate names
//                    (and, here, illegal self-wait) are rejected in EVERY
//                    build, not just debug. fail_fast() below is a plain
//                    runtime function call, never `assert`/NDEBUG-gated,
//                    so the check fires identically in a release build —
//                    that is the mechanism, not a policy note. Tests
//                    install a recording handler via
//                    set_fail_fast_handler() so the fail-fast branch can
//                    be observed without terminating the test process.
#pragma once
#include <functional>

namespace matter::evt {

// try_subscribe's runtime-extensible error channel (S I.5 rule 2). Only
// one enumerator exists in E1b — must_register_handler's
// DuplicateHandler analogue belongs to the command layer (E4), not here.
enum class RegistrationError { DuplicateName };

// Process-wide, swappable fail-fast handler. The default handler prints a
// diagnostic to stderr and calls std::abort() — the correct behavior in
// shipping engine/editor code, where a duplicate subscription name or a
// self-wait deadlock is a configuration bug, not a recoverable condition.
// Tests replace it with a handler that records the call instead of
// aborting, so the release (non-assert) fail-fast path can be exercised
// and asserted on directly.
using FailFastHandler = std::function<void(const char* what)>;

// Installs a new handler (thread-safe). Passing an empty std::function
// restores the default abort handler.
void set_fail_fast_handler(FailFastHandler handler);

// Invokes the currently-installed handler. Callers that call this must
// not assume it returns (the default handler does not); code after a
// fail_fast() call exists only for the benefit of a test-installed
// recording handler.
void fail_fast(const char* what);

}  // namespace matter::evt
