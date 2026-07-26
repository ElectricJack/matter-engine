// event_property_tests.cpp — headless tests for evt::Property<T> +
// evt::PropertyScheduler (E5a of the event system; see
// MatterEngine3/docs/event-system.md S I.9, S II.1 E5). Plain CHECK + printf
// style, matching event_hub_tests.cpp; no GL, no window, no GALLIUM env.
//
// Coverage (S II.1 E5's property items):
//   - coalescing: N set()s between flushes -> exactly ONE delivery per
//     observer, with the LATEST value;
//   - prime-on-bind: bind() fires immediately with the current value; a bind
//     after some sets sees the latest;
//   - equality suppression: set(sameValue) does not dirty -> no delivery;
//   - affinity: set/flush off the claimed thread routes to the all-build
//     fail-fast seam (verified via a recording handler; no death test);
//   - edit-loop termination: a set() from inside a delivery is applied but
//     deferred to the NEXT flush, and equality suppression stops the ping-pong
//     (panel-edits -> gizmo-updates -> panel-hears-its-own-change converges);
//   - lifetime: a destroyed Property deregisters (a later flush doesn't touch
//     it — no UAF); a destroyed bind() Subscription stops delivery;
//   - set_now delivers immediately (not coalesced);
//   - the decoupled trace sink fires per flush delivery / per set_now.
#include "matter/event/property.h"
#include "matter/event/registration_error.h"
#include "check.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using matter::evt::Property;
using matter::evt::PropertyScheduler;
using matter::evt::Subscription;

// ---------------------------------------------------------------------------
// 1. Coalescing: a burst of set()s between flushes yields exactly ONE delivery
//    per observer at the next flush, carrying the LATEST value.
// ---------------------------------------------------------------------------
static void test_coalescing_latest_wins() {
    printf("[test_coalescing_latest_wins]\n");
    PropertyScheduler sched;
    sched.claim();
    Property<int> p(sched, "sel.count", 0);

    std::vector<int> seen;
    Subscription sub = p.bind("obs", [&](const int& v) { seen.push_back(v); });
    // Prime-on-bind delivered the initial value already.
    CHECK(seen.size() == 1 && seen[0] == 0, "bind primed with the current value");

    for (int i = 1; i <= 100; ++i) p.set(i);
    CHECK(seen.size() == 1, "no delivery inline on set() — coalesced to the flush point");

    sched.flush_dirty();
    CHECK(seen.size() == 2, "exactly ONE delivery for the whole burst");
    CHECK(seen.back() == 100, "coalesced delivery carries the LATEST value");

    // A second flush with no intervening set delivers nothing.
    sched.flush_dirty();
    CHECK(seen.size() == 2, "flush with a clean property delivers nothing");
    printf("ok coalescing_latest_wins\n");
}

// ---------------------------------------------------------------------------
// 2. Prime-on-bind: a bind AFTER some sets sees the latest value immediately;
//    multiple observers each get primed.
// ---------------------------------------------------------------------------
static void test_prime_on_bind_sees_latest() {
    printf("[test_prime_on_bind_sees_latest]\n");
    PropertyScheduler sched;
    sched.claim();
    Property<int> p(sched, "sel.count", 7);

    p.set(42);
    // Not yet flushed, but the stored value is already the latest.
    int primed = -1;
    Subscription sub = p.bind("late", [&](const int& v) { primed = v; });
    CHECK(primed == 42, "bind after set() primes with the latest stored value");

    int primed2 = -1;
    Subscription sub2 = p.bind("late2", [&](const int& v) { primed2 = v; });
    CHECK(primed2 == 42, "a second bind is independently primed");

    // The pending flush delivers to both (they subscribed before the flush).
    int a = -1, b = -1;
    // Rebind fresh observers to count flush deliveries distinctly.
    std::vector<int> ca, cb;
    Subscription s3 = p.bind("c3", [&](const int& v) { ca.push_back(v); });
    Subscription s4 = p.bind("c4", [&](const int& v) { cb.push_back(v); });
    ca.clear();
    cb.clear();
    sched.flush_dirty();
    CHECK(ca.size() == 1 && ca[0] == 42, "flush delivers latest to observer c3 once");
    CHECK(cb.size() == 1 && cb[0] == 42, "flush delivers latest to observer c4 once");
    (void)a;
    (void)b;
    printf("ok prime_on_bind_sees_latest\n");
}

// ---------------------------------------------------------------------------
// 3. Equality suppression: set() with an unchanged value (per operator==) does
//    not dirty, so the next flush delivers nothing.
// ---------------------------------------------------------------------------
static void test_equality_suppression() {
    printf("[test_equality_suppression]\n");
    PropertyScheduler sched;
    sched.claim();
    Property<int> p(sched, "sel.count", 5);

    std::vector<int> seen;
    Subscription sub = p.bind("obs", [&](const int& v) { seen.push_back(v); });
    seen.clear();  // drop the prime

    p.set(5);  // same value — must NOT dirty
    sched.flush_dirty();
    CHECK(seen.empty(), "set(sameValue) does not dirty -> no delivery");

    p.set(6);  // real change
    sched.flush_dirty();
    CHECK(seen.size() == 1 && seen[0] == 6, "a real change does deliver");

    p.set(6);  // same again
    sched.flush_dirty();
    CHECK(seen.size() == 1, "repeated same value still suppressed");
    printf("ok equality_suppression\n");
}

// ---------------------------------------------------------------------------
// 4. Affinity: set()/flush_dirty() off the claimed owner thread route to the
//    all-build fail-fast seam. We install a recording handler (no abort) and
//    assert the contract structurally (S II.1 E5: a death test is not required
//    — assert the single-thread contract).
// ---------------------------------------------------------------------------
static void test_affinity_off_thread_fails_fast() {
    printf("[test_affinity_off_thread_fails_fast]\n");
    PropertyScheduler sched;
    sched.claim();  // owner = this (main) thread
    Property<int> p(sched, "sel.count", 0);

    std::atomic<int> fail_fast_calls{0};
    matter::evt::set_fail_fast_handler([&](const char*) { fail_fast_calls.fetch_add(1); });

    // On-thread calls must NOT trip the affinity assert.
    p.set(1);
    sched.flush_dirty();
    CHECK(fail_fast_calls.load() == 0, "on-owner-thread set/flush do not fail-fast");

    // Off-thread set() trips it. The producer thread runs alone (main is joined-
    // waiting), so there is no concurrent access to the scheduler's vectors.
    std::thread([&] { p.set(2); }).join();
    CHECK(fail_fast_calls.load() >= 1, "off-owner-thread set() routes to fail-fast (affinity)");

    int before = fail_fast_calls.load();
    std::thread([&] { sched.flush_dirty(); }).join();
    CHECK(fail_fast_calls.load() > before, "off-owner-thread flush_dirty() routes to fail-fast");

    matter::evt::set_fail_fast_handler(nullptr);
    printf("ok affinity_off_thread_fails_fast\n");
}

// ---------------------------------------------------------------------------
// 5. Edit-loop termination: an observer that set()s the SAME property from
//    inside its own flush delivery is applied but deferred to the NEXT flush,
//    and equality suppression terminates the ping-pong. Model the canonical
//    panel-edits -> gizmo-updates -> panel-hears-its-own-change loop and prove
//    it converges (no infinite loop, finite deliveries).
// ---------------------------------------------------------------------------
static void test_edit_loop_terminates() {
    printf("[test_edit_loop_terminates]\n");
    PropertyScheduler sched;
    sched.claim();
    Property<int> transform(sched, "sel.transform", 0);

    int gizmo_deliveries = 0;
    int panel_deliveries = 0;

    // The "gizmo" observer: on hearing a change, it writes the value back
    // (rounding/normalizing to the same value here — the converging case).
    Subscription gizmo = transform.bind("gizmo", [&](const int& v) {
        ++gizmo_deliveries;
        transform.set(v);  // set-during-own-flush -> deferred to next flush
    });
    // The "panel" observer just records what it hears (its own change included).
    Subscription panel = transform.bind("panel", [&](const int& v) {
        ++panel_deliveries;
    });
    gizmo_deliveries = 0;
    panel_deliveries = 0;

    // Panel edits the transform.
    transform.set(10);

    // Drive flushes to quiescence. Because the gizmo writes back the SAME value,
    // equality suppression means the deferred set() does not dirty -> the loop
    // dies after the first real delivery. Bound the loop so a bug shows as a
    // failure, not a hang.
    int flushes = 0;
    for (; flushes < 1000; ++flushes) {
        sched.flush_dirty();
        // Second flush onward should be clean (nothing re-dirtied).
        if (flushes >= 1 && gizmo_deliveries == 1) break;
    }
    CHECK(flushes < 1000, "edit loop converged (did not run to the 1000-flush cap)");
    CHECK(gizmo_deliveries == 1, "gizmo delivered exactly once — writeback of same value suppressed");
    CHECK(panel_deliveries == 1, "panel delivered exactly once — no ping-pong");

    // Now prove the DEFER (not drop) half: a gizmo that writes a DIFFERENT value
    // once should produce exactly one extra deferred delivery, then converge.
    int deliveries = 0;
    Property<int> t2(sched, "sel.t2", 0);
    bool bumped = false;
    Subscription o = t2.bind("o", [&](const int& v) {
        ++deliveries;
        if (!bumped && v == 1) {
            bumped = true;
            t2.set(2);  // a real change from inside delivery -> deferred
        }
    });
    deliveries = 0;
    t2.set(1);
    sched.flush_dirty();  // delivers v=1; observer defers set(2)
    CHECK(deliveries == 1, "first flush delivers the edit; the writeback is deferred (not applied inline)");
    CHECK(t2.get() == 2, "the deferred set() DID apply its value immediately (defer is of delivery, not of the write)");
    sched.flush_dirty();  // delivers the deferred v=2
    CHECK(deliveries == 2, "second flush delivers the deferred change exactly once");
    sched.flush_dirty();  // v==2 unchanged -> nothing
    CHECK(deliveries == 2, "third flush is clean — converged");
    printf("ok edit_loop_terminates\n");
}

// ---------------------------------------------------------------------------
// 6a. Lifetime: a destroyed Subscription stops delivery (RAII unsubscribe).
// ---------------------------------------------------------------------------
static void test_subscription_destruction_stops_delivery() {
    printf("[test_subscription_destruction_stops_delivery]\n");
    PropertyScheduler sched;
    sched.claim();
    Property<int> p(sched, "sel.count", 0);

    std::vector<int> seen;
    {
        Subscription sub = p.bind("obs", [&](const int& v) { seen.push_back(v); });
        seen.clear();
        p.set(1);
        sched.flush_dirty();
        CHECK(seen.size() == 1 && seen[0] == 1, "observer delivered while subscribed");
    }  // Subscription destroyed -> logical unsubscribe
    p.set(2);
    sched.flush_dirty();
    CHECK(seen.size() == 1, "no delivery after the Subscription went out of scope");
    printf("ok subscription_destruction_stops_delivery\n");
}

// ---------------------------------------------------------------------------
// 6b. Lifetime: a destroyed Property deregisters from the scheduler — a later
//     flush does not touch it (no UAF). Also: a Subscription outliving its
//     Property is a safe no-op on destruction.
// ---------------------------------------------------------------------------
static void test_property_destruction_deregisters() {
    printf("[test_property_destruction_deregisters]\n");
    PropertyScheduler sched;
    sched.claim();

    Subscription outliving;  // will hold a bind from a soon-destroyed property
    {
        Property<int> p(sched, "sel.temp", 0);
        std::vector<int> seen;
        outliving = p.bind("obs", [&](const int& v) { /* captures only sched-external state */ });
        p.set(1);  // dirties the property
        // Destroy the property WHILE it is dirty — unregister must purge it from
        // the dirty set so the next flush cannot dereference freed memory.
    }
    // If deregistration failed, this flush would call flush_deliver() on a
    // destroyed Property -> UAF/crash. Reaching the CHECK means it was purged.
    sched.flush_dirty();
    CHECK(true, "flush after destroying a dirty property did not touch it (no UAF)");

    // Destroying the outliving Subscription now (its Property is already gone)
    // must be a safe no-op — the shared control block outlived the Property.
    outliving.reset();
    CHECK(true, "Subscription destroyed after its Property is a safe no-op");
    printf("ok property_destruction_deregisters\n");
}

// ---------------------------------------------------------------------------
// 7. set_now delivers immediately (not coalesced) and supersedes a pending
//    coalesced delivery.
// ---------------------------------------------------------------------------
static void test_set_now_immediate() {
    printf("[test_set_now_immediate]\n");
    PropertyScheduler sched;
    sched.claim();
    Property<int> p(sched, "sel.count", 0);

    std::vector<int> seen;
    Subscription sub = p.bind("obs", [&](const int& v) { seen.push_back(v); });
    seen.clear();

    p.set_now(1);
    CHECK(seen.size() == 1 && seen[0] == 1, "set_now delivers immediately, no flush needed");

    p.set_now(2);
    p.set_now(3);
    CHECK(seen.size() == 3 && seen[2] == 3, "each set_now delivers — not coalesced");

    // set_now supersedes a pending coalesced delivery: after a set() then a
    // set_now, the following flush must not re-deliver.
    seen.clear();
    p.set(10);       // dirties (coalesced)
    p.set_now(11);   // immediate + supersedes the pending dirty
    CHECK(seen.size() == 1 && seen[0] == 11, "set_now delivered the latest immediately");
    sched.flush_dirty();
    CHECK(seen.size() == 1, "flush after set_now does not re-deliver (pending superseded)");
    printf("ok set_now_immediate\n");
}

// ---------------------------------------------------------------------------
// 8. Decoupled trace sink: fires once per flush delivery (immediate=false) and
//    once per set_now (immediate=true), naming the property — the seam a hub
//    owner installs to record `prop.<name>` without coupling Property to Hub.
// ---------------------------------------------------------------------------
static void test_trace_sink_records_flushes() {
    printf("[test_trace_sink_records_flushes]\n");
    PropertyScheduler sched;
    sched.claim();

    std::vector<std::string> flush_records;
    std::vector<std::string> now_records;
    sched.set_trace_sink([&](const char* name, bool immediate) {
        if (immediate)
            now_records.push_back(name ? name : "");
        else
            flush_records.push_back(name ? name : "");
    });

    Property<int> p(sched, "sel.transform", 0);
    Subscription sub = p.bind("obs", [&](const int&) {});

    for (int i = 1; i <= 50; ++i) p.set(i);  // one coalesced flush record
    sched.flush_dirty();
    CHECK(flush_records.size() == 1 && flush_records[0] == "sel.transform",
          "one prop.<name> flush trace record for the coalesced burst");

    p.set_now(99);
    CHECK(now_records.size() == 1 && now_records[0] == "sel.transform",
          "set_now emits an immediate-flagged trace record");
    printf("ok trace_sink_records_flushes\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    printf("=== event_property_tests ===\n");
    test_coalescing_latest_wins();
    test_prime_on_bind_sees_latest();
    test_equality_suppression();
    test_affinity_off_thread_fails_fast();
    test_edit_loop_terminates();
    test_subscription_destruction_stops_delivery();
    test_property_destruction_deregisters();
    test_set_now_immediate();
    test_trace_sink_records_flushes();
    if (g_failures) {
        printf("\n%d FAILURES\n", g_failures);
        return 1;
    }
    printf("\nALL PASS\n");
    return 0;
}
