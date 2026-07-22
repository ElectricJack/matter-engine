// src/event/subscription.cpp — lane constants/registration, the process
// fail-fast handler, and SubscriptionSet::unsubscribe_all_and_wait.
//
// Design authority: MatterEngine3/docs/event-system.md S I.5/I.6, S II.4
// item 7. See matter/event/subscription.h and
// matter/event/registration_error.h for the contracts implemented here.
#include "matter/event/subscription.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "matter/event/detail/dispatch_context.h"

namespace matter::evt {

// Predefined engine-wide lanes. Ids 1-3 are reserved for these; make_lane()
// starts minting fresh ids at 4 so a domain lane can never collide with a
// predefined one.
const lane lane::app{1};
const lane lane::legacy_poll{2};
const lane lane::diagnostics{3};

namespace {

std::atomic<uint32_t> g_next_lane_id{4};

void default_fail_fast_handler(const char* what) {
    std::fprintf(stderr, "evt: FATAL registration/lifetime error: %s\n", what);
    std::fflush(stderr);
    std::abort();
}

std::mutex g_fail_fast_mu;
FailFastHandler g_fail_fast_handler = default_fail_fast_handler;

}  // namespace

lane make_lane() { return lane(g_next_lane_id.fetch_add(1, std::memory_order_relaxed)); }

void set_fail_fast_handler(FailFastHandler handler) {
    std::lock_guard<std::mutex> lk(g_fail_fast_mu);
    g_fail_fast_handler = handler ? std::move(handler) : FailFastHandler(default_fail_fast_handler);
}

void fail_fast(const char* what) {
    FailFastHandler h;
    {
        std::lock_guard<std::mutex> lk(g_fail_fast_mu);
        h = g_fail_fast_handler;
    }
    h(what);
}

void SubscriptionSet::unsubscribe_all_and_wait() {
    std::vector<std::shared_ptr<SubscriptionBlock>> blocks;
    {
        std::lock_guard<std::mutex> lk(m_);
        blocks.reserve(subs_.size());
        for (auto& s : subs_) {
            if (s.block()) blocks.push_back(s.block());
        }
    }

    // Illegal-self-wait guard (S I.5/I.6, S II.4 item 1): if any of these
    // blocks' callback is on THIS thread's call stack right now (this call
    // is happening from inside one of this set's own callbacks, or from
    // the affected lane's pump while it is mid-dispatch), waiting for
    // in_flight to hit zero would deadlock on our own stack frame. Fail
    // fast instead of hanging — this is a plain runtime check (not
    // assert), so it fires the same way in a release build.
    for (auto& b : blocks) {
        if (detail::is_dispatching_on_this_thread(b.get())) {
            fail_fast(
                "evt::SubscriptionSet::unsubscribe_all_and_wait: called from inside one of "
                "this set's own callbacks (or the affected lane's pump) -- this would "
                "deadlock waiting for the current dispatch to finish.");
            return;  // A recording test handler doesn't abort; don't spin-wait after this.
        }
    }

    // Phase 1: mark every block inactive so no NEW dispatch begins.
    for (auto& b : blocks) b->active.store(false, std::memory_order_release);

    // Phase 2: wait for every already-in-flight callback (started before
    // phase 1 ran, on another thread) to finish.
    for (auto& b : blocks) {
        while (b->in_flight.load(std::memory_order_acquire) != 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    reset_all();
}

}  // namespace matter::evt
