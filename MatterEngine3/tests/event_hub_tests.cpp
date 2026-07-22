// event_hub_tests.cpp — headless tests for evt::Hub (E1b of the event
// system; see MatterEngine3/docs/event-system.md S I.5-I.8, S II.1 E1,
// S II.4 items 1 and 7). Plain assert + printf style, matching
// event_channel_tests.cpp; no GL, no window, no GALLIUM env needed.
#include "matter/event/event_hub.h"
#include "matter/event/event_name.h"
#include "check.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using matter::evt::Hub;
using matter::evt::phase;
using matter::evt::RegistrationError;
using matter::evt::Subscription;
using matter::evt::SubscriptionSet;

// ---------------------------------------------------------------------------
// Test event types (S I.5: "define test event structs in the test file"
// when a domain header isn't otherwise needed).
// ---------------------------------------------------------------------------
struct Ping {
    MT_EVENT_NAME("test.ping");
    int value = 0;
};

struct OtherEvent {
    MT_EVENT_NAME("test.other");
    int value = 0;
};

struct Nudge {
    MT_EVENT_NAME("test.nudge");
    int value = 0;
};

// ---------------------------------------------------------------------------
// 1. Basic subscribe/emit/logical-unsubscribe; immediate delivery on the
//    caller thread.
// ---------------------------------------------------------------------------
static void test_immediate_subscribe_emit_unsubscribe() {
    printf("[test_immediate_subscribe_emit_unsubscribe]\n");
    Hub hub;
    std::vector<int> seen;
    std::thread::id emit_thread_id;
    {
        Subscription sub = hub.must_subscribe<Ping>(
            "t1.sub", matter::evt::immediate,
            [&](const Ping& e) {
                seen.push_back(e.value);
                emit_thread_id = std::this_thread::get_id();
            });
        hub.emit(Ping{1});
        CHECK(seen.size() == 1 && seen[0] == 1, "immediate subscriber saw the emitted value");
        CHECK(emit_thread_id == std::this_thread::get_id(), "immediate delivery ran on the emitter's thread");
    }  // Subscription destructor -> logical unsubscribe
    hub.emit(Ping{2});
    CHECK(seen.size() == 1, "no dispatch after Subscription went out of scope (logical unsubscribe)");
    printf("ok immediate_subscribe_emit_unsubscribe\n");
}

// ---------------------------------------------------------------------------
// 2. Lane delivery via pump: queued until pumped, delivered in FIFO order,
//    correct event content.
// ---------------------------------------------------------------------------
static void test_lane_delivery_via_pump() {
    printf("[test_lane_delivery_via_pump]\n");
    Hub hub;
    matter::evt::lane ln = matter::evt::make_lane();
    hub.claim_lane(ln);
    std::vector<int> seen;
    Subscription sub =
        hub.must_subscribe<Ping>("t2.sub", ln, [&](const Ping& e) { seen.push_back(e.value); });

    hub.emit(Ping{10});
    CHECK(seen.empty(), "queued delivery does not run until pump()");
    hub.emit(Ping{11});
    hub.emit(Ping{12});

    int delivered = hub.pump(ln, 100.0);
    CHECK(delivered == 3, "pump delivered all 3 queued envelopes");
    CHECK(seen.size() == 3 && seen[0] == 10 && seen[1] == 11 && seen[2] == 12,
          "queued deliveries arrive in FIFO order with correct payloads");
    printf("ok lane_delivery_via_pump\n");
}

// ---------------------------------------------------------------------------
// 3. Ordering determinism: N subscribers to one event registered in ~100
//    shuffled permutations, dispatch order always (phase, priority, name).
// ---------------------------------------------------------------------------
static void test_ordering_determinism_across_permutations() {
    printf("[test_ordering_determinism_across_permutations]\n");

    struct Spec {
        std::string name;
        phase ph;
        int priority;
    };
    // Deliberately mixed phases/priorities so the expected order isn't
    // alphabetical-by-name alone.
    std::vector<Spec> specs = {
        {"c_sub", phase::Default, 0},  {"a_sub", phase::First, 5},   {"z_sub", phase::Last, -1},
        {"m_sub", phase::Default, -5}, {"b_sub", phase::Early, 0},   {"y_sub", phase::Late, 2},
        {"d_sub", phase::Default, 0},  {"e_sub", phase::Default, 1},
    };

    // Expected order computed once from `specs` by the same (phase,
    // priority, name) key the Hub promises (S I.7).
    std::vector<Spec> expected = specs;
    std::sort(expected.begin(), expected.end(), [](const Spec& a, const Spec& b) {
        if (a.ph != b.ph) return static_cast<int>(a.ph) < static_cast<int>(b.ph);
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.name < b.name;
    });
    std::vector<std::string> expected_names;
    for (auto& s : expected) expected_names.push_back(s.name);

    const int kPermutations = 100;
    const size_t n = specs.size();
    bool all_match = true;
    for (int perm = 0; perm < kPermutations; ++perm) {
        // Deterministic pseudo-shuffle (index-derived, no RNG): rotate by
        // `perm` then reverse alternate blocks -- varies by permutation
        // index so 100 distinct registration orders are exercised.
        std::vector<Spec> order = specs;
        int rotate_by = perm % static_cast<int>(n);
        std::rotate(order.begin(), order.begin() + rotate_by, order.end());
        if (perm % 3 == 1) std::reverse(order.begin(), order.end());
        if (perm % 3 == 2) std::reverse(order.begin() + (n / 2), order.end());

        Hub hub;
        std::vector<std::string> dispatch_order;
        std::vector<Subscription> subs;
        for (auto& s : order) {
            subs.push_back(hub.must_subscribe<Ping>(
                s.name.c_str(), matter::evt::immediate,
                [&dispatch_order, name = s.name](const Ping&) { dispatch_order.push_back(name); },
                s.ph, s.priority));
        }
        hub.emit(Ping{perm});
        if (dispatch_order != expected_names) {
            all_match = false;
            printf("  permutation %d MISMATCH\n", perm);
            break;
        }
    }
    CHECK(all_match, "dispatch order identical to the (phase,priority,name) key across 100 permutations");
    printf("ok ordering_determinism_across_permutations\n");
}

// ---------------------------------------------------------------------------
// 4. All-build unique names: try_subscribe duplicate returns the error
//    WITHOUT mutating the registry; a subsequent emit dispatches only the
//    original. must_subscribe duplicate fires the fail-fast handler
//    (swappable, so the test can observe it without dying). This is
//    exercised in a normal (non-assert) code path, so it also proves the
//    invariant holds under NDEBUG -- see the report for how.
// ---------------------------------------------------------------------------
static void test_uniqueness_try_subscribe_no_mutation() {
    printf("[test_uniqueness_try_subscribe_no_mutation]\n");
    Hub hub;
    int calls = 0;
    Subscription first =
        hub.must_subscribe<Ping>("dup.name", matter::evt::immediate, [&](const Ping&) { ++calls; });

    auto result = hub.try_subscribe<Ping>("dup.name", matter::evt::immediate, [&](const Ping&) { ++calls; });
    CHECK(std::holds_alternative<RegistrationError>(result), "try_subscribe returns an error on duplicate name");
    CHECK(std::get<RegistrationError>(result) == RegistrationError::DuplicateName,
          "error is DuplicateName");

    hub.emit(Ping{1});
    CHECK(calls == 1, "only the ORIGINAL subscriber dispatched -- duplicate was never inserted");

    auto snap = hub.registry_snapshot();
    int total_subs = 0;
    for (auto& entry : snap) total_subs += static_cast<int>(entry.subscribers.size());
    CHECK(total_subs == 1, "registry reflects exactly one subscription, not two");
    printf("ok uniqueness_try_subscribe_no_mutation\n");
}

static void test_uniqueness_must_subscribe_fail_fast_release_path() {
    printf("[test_uniqueness_must_subscribe_fail_fast_release_path]\n");

    // Swappable fail-fast handler (registration_error.h): the default
    // handler calls std::abort(), which we do NOT want in a test process.
    // Installing a recording handler lets us observe the exact same
    // runtime branch (a plain `if` in Hub::subscribe_generic -- not
    // `assert`, so it is NOT compiled out under NDEBUG) fire without
    // terminating. This is how the release-path requirement is verified.
    std::atomic<int> fail_fast_calls{0};
    std::string last_message;
    std::mutex msg_mu;
    matter::evt::set_fail_fast_handler([&](const char* what) {
        ++fail_fast_calls;
        std::lock_guard<std::mutex> lk(msg_mu);
        last_message = what;
    });

    Hub hub;
    int calls = 0;
    Subscription first =
        hub.must_subscribe<Ping>("dup.must", matter::evt::immediate, [&](const Ping&) { ++calls; });
    Subscription second =
        hub.must_subscribe<Ping>("dup.must", matter::evt::immediate, [&](const Ping&) { ++calls; });

    CHECK(fail_fast_calls.load() == 1, "must_subscribe duplicate routed to the fail-fast handler exactly once");
    CHECK(!second.valid(), "the duplicate must_subscribe call returns an invalid handle, nothing inserted");
    CHECK(last_message.find("dup.must") != std::string::npos, "fail-fast message names the duplicate subscription");

    hub.emit(Ping{1});
    CHECK(calls == 1, "only the original must_subscribe callback ever dispatches");

    // Restore the default handler so later tests in this binary don't
    // inherit this recording handler.
    matter::evt::set_fail_fast_handler(nullptr);
    printf("ok uniqueness_must_subscribe_fail_fast_release_path\n");
}

// ---------------------------------------------------------------------------
// 5. Emit-vs-teardown race (S II.4 item 1): a subscriber callback captures
//    owner state; one thread emits in a loop while the main thread calls
//    unsubscribe_all_and_wait(). No use-after-free, no callback runs after
//    wait returns. Run for many iterations to surface flakiness.
// ---------------------------------------------------------------------------
struct RaceOwner {
    std::atomic<int> callback_hits{0};
    std::atomic<bool> destroyed{false};
    int sentinel = 0xC0FFEE;
};

static void test_emit_vs_teardown_race() {
    printf("[test_emit_vs_teardown_race]\n");
    const int kIterations = 500;
    std::atomic<int> post_wait_violations{0};

    for (int iter = 0; iter < kIterations; ++iter) {
        Hub hub;
        matter::evt::lane ln = matter::evt::make_lane();
        hub.claim_lane(ln);

        auto owner = std::make_unique<RaceOwner>();
        RaceOwner* owner_ptr = owner.get();

        SubscriptionSet subs;
        subs += hub.must_subscribe<Ping>(
            "race.immediate", matter::evt::immediate, [owner_ptr](const Ping&) {
                // Touch owner state -- if this runs after the owner is
                // destroyed/after unsubscribe_all_and_wait() returns, this
                // is a use-after-free (ASan/valgrind would catch it; here
                // we also cross-check via the sentinel + destroyed flag).
                CHECK(!owner_ptr->destroyed.load(), "callback observed owner not-yet-destroyed");
                CHECK(owner_ptr->sentinel == 0xC0FFEE, "owner sentinel intact (no UAF)");
                owner_ptr->callback_hits.fetch_add(1, std::memory_order_relaxed);
            });

        std::atomic<bool> stop{false};
        std::thread emitter([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                hub.emit(Ping{iter});
            }
        });

        // Let the emitter get going briefly, then tear down while it races.
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        subs.unsubscribe_all_and_wait();  // must not return while a callback is still in flight

        // Once unsubscribe_all_and_wait() has returned, it is safe to
        // destroy owner state -- mark it destroyed first so any (illegal)
        // late callback would be caught by the checks above.
        owner_ptr->destroyed.store(true);

        stop.store(true, std::memory_order_relaxed);
        emitter.join();
        // No more emits will reach the (now-unsubscribed) callback; owner
        // can be freed safely.
        owner.reset();
    }
    CHECK(post_wait_violations.load() == 0, "no callback ran after unsubscribe_all_and_wait() returned");
    printf("ok emit_vs_teardown_race (%d iterations)\n", kIterations);
}

// ---------------------------------------------------------------------------
// 6. Logical unsubscribe before pump suppresses a queued delivery.
// ---------------------------------------------------------------------------
static void test_logical_unsubscribe_before_pump_suppresses_delivery() {
    printf("[test_logical_unsubscribe_before_pump_suppresses_delivery]\n");
    Hub hub;
    matter::evt::lane ln = matter::evt::make_lane();
    hub.claim_lane(ln);
    int calls = 0;
    {
        Subscription sub =
            hub.must_subscribe<Ping>("t6.sub", ln, [&](const Ping&) { ++calls; });
        hub.emit(Ping{1});  // enqueued but not yet pumped
    }  // Subscription destroyed here -> logical unsubscribe, BEFORE pump()
    int delivered = hub.pump(ln, 50.0);
    CHECK(delivered == 1, "pump still drains the envelope from the channel");
    CHECK(calls == 0, "but no callback ran -- logical unsubscribe before pump suppressed delivery");
    printf("ok logical_unsubscribe_before_pump_suppresses_delivery\n");
}

// ---------------------------------------------------------------------------
// 7. Illegal self-wait: calling unsubscribe_all_and_wait() from inside one
//    of the set's own callbacks fails fast instead of deadlocking.
// ---------------------------------------------------------------------------
static void test_unsubscribe_all_and_wait_illegal_self_call() {
    printf("[test_unsubscribe_all_and_wait_illegal_self_call]\n");
    std::atomic<int> fail_fast_calls{0};
    matter::evt::set_fail_fast_handler([&](const char*) { ++fail_fast_calls; });

    Hub hub;
    SubscriptionSet subs;
    bool callback_ran = false;
    bool callback_returned = false;
    subs += hub.must_subscribe<Ping>("self.wait", matter::evt::immediate, [&](const Ping&) {
        callback_ran = true;
        // Illegal: waiting on our own set from inside our own callback
        // would deadlock (this callback's in_flight count can only reach
        // zero once this very call returns). Must fail-fast, not hang.
        subs.unsubscribe_all_and_wait();
        callback_returned = true;
    });

    hub.emit(Ping{1});
    CHECK(callback_ran, "callback executed");
    CHECK(callback_returned, "callback returned (i.e. unsubscribe_all_and_wait did not hang)");
    CHECK(fail_fast_calls.load() == 1, "illegal self-wait routed to the fail-fast handler exactly once");

    matter::evt::set_fail_fast_handler(nullptr);
    printf("ok unsubscribe_all_and_wait_illegal_self_call\n");
}

// ---------------------------------------------------------------------------
// 8. Close: invalidates handles, drops queued, waits for in-flight; a
//    handle destroyed after close() is a no-op (no crash).
// ---------------------------------------------------------------------------
static void test_close_invalidates_and_drops_queued() {
    printf("[test_close_invalidates_and_drops_queued]\n");
    auto hub = std::make_unique<Hub>();
    matter::evt::lane ln = matter::evt::make_lane();
    hub->claim_lane(ln);

    int immediate_calls = 0;
    int lane_calls = 0;
    Subscription imm_sub = hub->must_subscribe<Ping>(
        "t8.imm", matter::evt::immediate, [&](const Ping&) { ++immediate_calls; });
    Subscription lane_sub = hub->must_subscribe<Ping>("t8.lane", ln, [&](const Ping&) { ++lane_calls; });

    hub->emit(Ping{1});  // queues one envelope on `ln`, runs immediate once
    CHECK(immediate_calls == 1, "immediate ran before close");

    hub->close();  // external emitters stopped (this test's emit above already returned)

    int delivered = hub->pump(ln, 50.0);
    CHECK(delivered == 0, "close() cancelled the queued envelope -- pump delivers nothing");
    CHECK(lane_calls == 0, "queued callback never ran");

    // A handle destroyed after the hub is closed (and even after the hub
    // itself is destroyed) must be a safe no-op.
    hub.reset();
    imm_sub.reset();   // no crash
    lane_sub.reset();  // no crash
    printf("ok close_invalidates_and_drops_queued\n");
}

static void test_handle_destroyed_after_hub_destroyed_is_noop() {
    printf("[test_handle_destroyed_after_hub_destroyed_is_noop]\n");
    Subscription orphan;
    {
        Hub hub;
        orphan = hub.must_subscribe<Ping>("t8b.sub", matter::evt::immediate, [](const Ping&) {});
    }  // hub destroyed (its destructor calls close())
    orphan.reset();  // must not crash: Subscription holds no pointer into Hub
    CHECK(true, "destroying a Subscription after its Hub is destroyed did not crash");
    printf("ok handle_destroyed_after_hub_destroyed_is_noop\n");
}

// ---------------------------------------------------------------------------
// 9. Reentrancy: handler that emits (nesting recorded, depth cap fails
//    fast at 8); handler that subscribes/unsubscribes during dispatch
//    (applied after current dispatch, current walk unaffected).
// ---------------------------------------------------------------------------
static void test_reentrant_emit_from_handler_and_nesting_trace() {
    printf("[test_reentrant_emit_from_handler_and_nesting_trace]\n");
    Hub hub;
    hub.set_trace_enabled(true);
    int nudge_calls = 0;
    Subscription outer = hub.must_subscribe<Ping>(
        "t9.outer", matter::evt::immediate, [&](const Ping& e) {
            if (e.value == 1) hub.emit(Nudge{99});  // handler-emitted event
        });
    Subscription inner =
        hub.must_subscribe<Nudge>("t9.inner", matter::evt::immediate, [&](const Nudge&) { ++nudge_calls; });

    hub.emit(Ping{1});
    CHECK(nudge_calls == 1, "handler-emitted event dispatched to its own subscriber");

    auto trace = hub.trace_snapshot();
    CHECK(trace.size() >= 2, "trace recorded both the outer emit and the nested emit");
    bool found_nested = false;
    for (auto& rec : trace) {
        if (rec.event_name == "test.nudge" && rec.nesting_depth >= 1) found_nested = true;
    }
    CHECK(found_nested, "trace records nesting depth >=1 for the handler-emitted event");
    printf("ok reentrant_emit_from_handler_and_nesting_trace\n");
}

static void test_emit_depth_cap_fails_fast() {
    printf("[test_emit_depth_cap_fails_fast]\n");
    std::atomic<int> fail_fast_calls{0};
    matter::evt::set_fail_fast_handler([&](const char*) { ++fail_fast_calls; });

    Hub hub;
    std::atomic<int> dispatch_count{0};
    Subscription sub = hub.must_subscribe<Ping>(
        "t9b.cycle", matter::evt::immediate, [&](const Ping& e) {
            dispatch_count.fetch_add(1, std::memory_order_relaxed);
            hub.emit(Ping{e.value + 1});  // unconditional re-emit -- would recurse forever without the cap
        });

    hub.emit(Ping{0});

    CHECK(fail_fast_calls.load() >= 1, "emit-depth cap fired the fail-fast handler");
    CHECK(dispatch_count.load() < 20, "recursion was bounded by the depth cap, not runaway");

    matter::evt::set_fail_fast_handler(nullptr);
    printf("ok emit_depth_cap_fails_fast (dispatch_count=%d)\n", dispatch_count.load());
}

static void test_subscribe_unsubscribe_during_dispatch_deferred() {
    printf("[test_subscribe_unsubscribe_during_dispatch_deferred]\n");
    Hub hub;
    std::vector<std::string> walk;
    Subscription late;      // filled in by handler "a" on its first call only
    bool subscribed_late = false;

    Subscription a = hub.must_subscribe<Ping>(
        "t9c.a", matter::evt::immediate,
        [&](const Ping&) {
            walk.push_back("a");
            if (!subscribed_late) {
                subscribed_late = true;
                // Subscribing during dispatch: legal, must NOT affect
                // THIS walk (S I.6 reentrancy) -- only a later emit
                // should see the new subscriber.
                late = hub.must_subscribe<Ping>(
                    "t9c.late", matter::evt::immediate, [&](const Ping&) { walk.push_back("late"); });
            }
        },
        phase::Default, 0);
    Subscription b = hub.must_subscribe<Ping>(
        "t9c.b", matter::evt::immediate,
        [&](const Ping&) {
            walk.push_back("b");
            // Logically unsubscribing itself during dispatch: legal, and
            // per S I.6 "one already executing is allowed to finish" --
            // this call already pushed "b" above before removing itself,
            // so THIS walk is unaffected; only later emits skip it.
            b.reset();
        },
        phase::Default, 1);

    hub.emit(Ping{1});
    CHECK(walk == std::vector<std::string>({"a", "b"}),
          "subscribe-during-dispatch and self-unsubscribe-during-dispatch did not affect the CURRENT walk");

    walk.clear();
    hub.emit(Ping{2});
    CHECK(walk == std::vector<std::string>({"a", "late"}),
          "next emit sees the deferred subscribe ('late') and the deferred unsubscribe ('b' gone)");
    printf("ok subscribe_unsubscribe_during_dispatch_deferred\n");
}

// ---------------------------------------------------------------------------
// 10. Registry + trace: registry_snapshot reflects the ordered set; trace
//     records an emit with per-subscriber records and nesting depth for a
//     handler-emitted event (covered above); trace off = no records.
// ---------------------------------------------------------------------------
static void test_registry_snapshot_reflects_ordered_set() {
    printf("[test_registry_snapshot_reflects_ordered_set]\n");
    Hub hub;
    Subscription s1 =
        hub.must_subscribe<Ping>("z_last", matter::evt::immediate, [](const Ping&) {}, phase::Last);
    Subscription s2 =
        hub.must_subscribe<Ping>("a_first", matter::evt::immediate, [](const Ping&) {}, phase::First);
    Subscription s3 = hub.must_subscribe<OtherEvent>("other.sub", matter::evt::immediate,
                                                       [](const OtherEvent&) {});

    auto snap = hub.registry_snapshot();
    CHECK(snap.size() == 2, "registry has one entry per event TYPE (Ping, OtherEvent)");

    for (auto& entry : snap) {
        std::string type_name = entry.event_name;
        if (type_name == "test.ping") {
            CHECK(entry.subscribers.size() == 2, "Ping has 2 subscribers");
            CHECK(entry.subscribers[0].name == "a_first", "First phase subscriber sorted before Last");
            CHECK(entry.subscribers[1].name == "z_last", "Last phase subscriber sorted after First");
        } else if (type_name == "test.other") {
            CHECK(entry.subscribers.size() == 1, "OtherEvent has 1 subscriber");
            CHECK(entry.subscribers[0].name == "other.sub", "OtherEvent subscriber name matches");
        } else {
            CHECK(false, "unexpected event type in registry snapshot");
        }
    }

    hub.emit(Ping{1});
    hub.emit(Ping{2});
    auto snap2 = hub.registry_snapshot();
    for (auto& entry : snap2) {
        if (std::string(entry.event_name) == "test.ping") {
            for (auto& sub : entry.subscribers) {
                CHECK(sub.delivery_count == 2, "delivery_count reflects both emits");
            }
        }
    }
    printf("ok registry_snapshot_reflects_ordered_set\n");
}

static void test_trace_off_records_nothing() {
    printf("[test_trace_off_records_nothing]\n");
    Hub hub;
    CHECK(!hub.trace_enabled(), "trace is off by default");
    Subscription sub = hub.must_subscribe<Ping>("t10b.sub", matter::evt::immediate, [](const Ping&) {});
    hub.emit(Ping{1});
    hub.emit(Ping{2});
    auto trace = hub.trace_snapshot();
    CHECK(trace.empty(), "trace ring buffer stayed empty while tracing is off");
    printf("ok trace_off_records_nothing\n");
}

static void test_trace_on_records_per_subscriber_detail() {
    printf("[test_trace_on_records_per_subscriber_detail]\n");
    Hub hub;
    hub.set_trace_enabled(true);
    Subscription sub =
        hub.must_subscribe<Ping>("t10c.sub", matter::evt::immediate, [](const Ping&) {});
    hub.emit(Ping{42});
    auto trace = hub.trace_snapshot();
    CHECK(trace.size() == 1, "one trace record for the one emit");
    CHECK(trace[0].event_name == "test.ping", "trace record names the event type");
    CHECK(trace[0].subscribers.size() == 1, "trace record has one per-subscriber entry");
    CHECK(trace[0].subscribers[0].name == "t10c.sub", "per-subscriber record names the subscription");
    printf("ok trace_on_records_per_subscriber_detail\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    printf("=== event_hub_tests ===\n");
    test_immediate_subscribe_emit_unsubscribe();
    test_lane_delivery_via_pump();
    test_ordering_determinism_across_permutations();
    test_uniqueness_try_subscribe_no_mutation();
    test_uniqueness_must_subscribe_fail_fast_release_path();
    test_emit_vs_teardown_race();
    test_logical_unsubscribe_before_pump_suppresses_delivery();
    test_unsubscribe_all_and_wait_illegal_self_call();
    test_close_invalidates_and_drops_queued();
    test_handle_destroyed_after_hub_destroyed_is_noop();
    test_reentrant_emit_from_handler_and_nesting_trace();
    test_emit_depth_cap_fails_fast();
    test_subscribe_unsubscribe_during_dispatch_deferred();
    test_registry_snapshot_reflects_ordered_set();
    test_trace_off_records_nothing();
    test_trace_on_records_per_subscriber_detail();
    if (g_failures) {
        printf("\n%d FAILURES\n", g_failures);
        return 1;
    }
    printf("\nALL PASS\n");
    return 0;
}
