// event_channel_tests.cpp — headless tests for evt::Channel<T> (E1a of the
// event system; see MatterEngine3/docs/event-system.md S I.5/I.6, S II.1 E1).
// Plain assert + printf style, matching async_queue_tests.cpp; no GL, no
// window, no GALLIUM env needed.
#include "matter/event/channel.h"
#include "check.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <thread>
#include <vector>

using matter::evt::Channel;
using matter::evt::OnFull;
using matter::evt::PushResult;
using matter::evt::WaitResult;

using IntPolicy = Channel<int>::Policy;
using IntOpts = Channel<int>::PushOptions;

// ---------------------------------------------------------------------------
// 1. push/try_pop FIFO.
// ---------------------------------------------------------------------------
static void test_push_try_pop_fifo() {
    printf("[test_push_try_pop_fifo]\n");
    Channel<int> ch(IntPolicy{0, OnFull::RejectNewest});
    CHECK(ch.push(1) == PushResult::Queued, "push 1 queued");
    CHECK(ch.push(2) == PushResult::Queued, "push 2 queued");
    CHECK(ch.push(3) == PushResult::Queued, "push 3 queued");
    int out = 0;
    CHECK(ch.try_pop(out) && out == 1, "try_pop returns 1 first (FIFO)");
    CHECK(ch.try_pop(out) && out == 2, "try_pop returns 2 second (FIFO)");
    CHECK(ch.try_pop(out) && out == 3, "try_pop returns 3 third (FIFO)");
    CHECK(!ch.try_pop(out), "try_pop false on empty channel");
    printf("ok push_try_pop_fifo\n");
}

// ---------------------------------------------------------------------------
// 2. wait_pop blocks then returns on push (producer thread).
// ---------------------------------------------------------------------------
static void test_wait_pop_blocks_then_returns_on_push() {
    printf("[test_wait_pop_blocks_then_returns_on_push]\n");
    Channel<int> ch(IntPolicy{0, OnFull::RejectNewest});
    int got = 0;
    WaitResult wr = WaitResult::Timeout;
    std::thread consumer([&] { wr = ch.wait_pop(got); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ch.push(42);
    consumer.join();
    CHECK(wr == WaitResult::Item, "wait_pop returned Item");
    CHECK(got == 42, "wait_pop delivered the pushed value");
    printf("ok wait_pop_blocks_then_returns_on_push\n");
}

// ---------------------------------------------------------------------------
// 3. wait_pop_for times out (Timeout) and returns Item once one arrives.
// ---------------------------------------------------------------------------
static void test_wait_pop_for_timeout_then_item() {
    printf("[test_wait_pop_for_timeout_then_item]\n");
    Channel<int> ch(IntPolicy{0, OnFull::RejectNewest});
    int out = 0;
    WaitResult wr1 = ch.wait_pop_for(out, std::chrono::milliseconds(20));
    CHECK(wr1 == WaitResult::Timeout, "wait_pop_for times out on empty channel");

    ch.push(7);
    WaitResult wr2 = ch.wait_pop_for(out, std::chrono::milliseconds(1000));
    CHECK(wr2 == WaitResult::Item && out == 7, "wait_pop_for returns Item once pushed");
    printf("ok wait_pop_for_timeout_then_item\n");
}

// ---------------------------------------------------------------------------
// 4. pump budget + progress guarantee (always >=1 delivered when pending).
// ---------------------------------------------------------------------------
static void test_pump_budget_progress_guarantee() {
    printf("[test_pump_budget_progress_guarantee]\n");
    Channel<int> ch(IntPolicy{0, OnFull::RejectNewest});
    for (int i = 0; i < 3; ++i) ch.push(i);
    std::vector<int> delivered;
    auto fn = [&](int& v) {
        delivered.push_back(v);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    int ran1 = ch.pump(0.1, fn);  // budget 0.1ms; delivery takes 5ms -> min-one rule fires
    CHECK(ran1 == 1, "pump(0.1ms) delivers exactly 1 (min-one guarantee)");
    int ran2 = ch.pump(1000.0, fn);
    CHECK(ran2 == 2, "second pump drains the remaining 2 within a large budget");
    CHECK(delivered.size() == 3 && delivered[0] == 0 && delivered[1] == 1 && delivered[2] == 2,
          "pump delivers in FIFO order across calls");
    printf("ok pump_budget_progress_guarantee\n");
}

// ---------------------------------------------------------------------------
// 5. pump_one delivers at most one.
// ---------------------------------------------------------------------------
static void test_pump_one_delivers_at_most_one() {
    printf("[test_pump_one_delivers_at_most_one]\n");
    Channel<int> ch(IntPolicy{0, OnFull::RejectNewest});
    ch.push(1);
    ch.push(2);
    std::vector<int> delivered;
    auto fn = [&](int& v) { delivered.push_back(v); };
    int r1 = ch.pump_one(fn);
    CHECK(r1 == 1 && delivered.size() == 1 && delivered[0] == 1, "pump_one delivers exactly one item");
    int r2 = ch.pump_one(fn);
    CHECK(r2 == 1 && delivered.size() == 2 && delivered[1] == 2, "pump_one delivers the next item");
    int r3 = ch.pump_one(fn);
    CHECK(r3 == 0 && delivered.size() == 2, "pump_one returns 0 on empty channel");
    printf("ok pump_one_delivers_at_most_one\n");
}

// ---------------------------------------------------------------------------
// 6. RejectNewest rejects at capacity: rejected() increments, item not lost
//    silently (caller sees RejectedFull, channel contents unchanged).
// ---------------------------------------------------------------------------
static void test_reject_newest_at_capacity() {
    printf("[test_reject_newest_at_capacity]\n");
    Channel<int> ch(IntPolicy{2, OnFull::RejectNewest});
    CHECK(ch.push(1) == PushResult::Queued, "push 1 queued");
    CHECK(ch.push(2) == PushResult::Queued, "push 2 queued (channel now full)");
    uint64_t rejected_before = ch.rejected();
    CHECK(ch.push(3) == PushResult::RejectedFull, "push 3 rejected at capacity");
    CHECK(ch.rejected() == rejected_before + 1, "rejected() incremented");
    int out = 0;
    CHECK(ch.try_pop(out) && out == 1, "1 still there");
    CHECK(ch.try_pop(out) && out == 2, "2 still there (3 was never enqueued)");
    CHECK(!ch.try_pop(out), "nothing else queued");
    printf("ok reject_newest_at_capacity\n");
}

// ---------------------------------------------------------------------------
// 7. DropOldest evicts oldest: dropped() increments, newest retained.
// ---------------------------------------------------------------------------
static void test_drop_oldest_evicts_oldest() {
    printf("[test_drop_oldest_evicts_oldest]\n");
    Channel<int> ch(IntPolicy{2, OnFull::DropOldest});
    ch.push(1);
    ch.push(2);
    uint64_t dropped_before = ch.dropped();
    PushResult pr = ch.push(3);
    CHECK(pr == PushResult::Queued, "push at capacity with DropOldest returns Queued");
    CHECK(ch.dropped() == dropped_before + 1, "dropped() incremented for evicted oldest");
    int out = 0;
    CHECK(ch.try_pop(out) && out == 2, "oldest (1) was evicted; 2 now first");
    CHECK(ch.try_pop(out) && out == 3, "newest (3) retained");
    printf("ok drop_oldest_evicts_oldest\n");
}

// ---------------------------------------------------------------------------
// 8. CoalesceNewest with a matching key replaces in place (Coalesced),
//    §II.4.4 correctness point.
// ---------------------------------------------------------------------------
static void test_coalesce_newest_matching_key_replaces() {
    printf("[test_coalesce_newest_matching_key_replaces]\n");
    Channel<int> ch(IntPolicy{2, OnFull::RejectNewest});
    IntOpts no_key{OnFull::RejectNewest, std::nullopt};
    IntOpts opt_key1{OnFull::CoalesceNewest, uint64_t(1)};

    CHECK(ch.push(100, no_key) == PushResult::Queued, "first push (no key) queues under capacity");
    CHECK(ch.push(10, opt_key1) == PushResult::Queued, "second push (key=1) queues under capacity");
    // Channel now full: [100 (no key), 10 (key=1)].
    uint64_t dropped_before = ch.dropped();
    PushResult pr = ch.push(11, opt_key1);  // matches the key=1 entry.
    CHECK(pr == PushResult::Coalesced, "matching key coalesces at capacity");
    CHECK(ch.dropped() == dropped_before + 1, "dropped() incremented for the coalesced-away value");
    int out = 0;
    CHECK(ch.try_pop(out) && out == 100, "unrelated first entry (no key) is untouched, still first");
    CHECK(ch.try_pop(out) && out == 11, "coalesced entry now holds the newest value, same slot");
    printf("ok coalesce_newest_matching_key_replaces\n");
}

// ---------------------------------------------------------------------------
// 9. CoalesceNewest with a NON-matching key at capacity rejects, and does
//    NOT clobber the other key's entry (§II.4.4 correctness point).
// ---------------------------------------------------------------------------
static void test_coalesce_newest_nonmatching_key_rejects_without_clobbering() {
    printf("[test_coalesce_newest_nonmatching_key_rejects_without_clobbering]\n");
    Channel<int> ch(IntPolicy{1, OnFull::RejectNewest});
    IntOpts opt_key1{OnFull::CoalesceNewest, uint64_t(1)};
    IntOpts opt_key2{OnFull::CoalesceNewest, uint64_t(2)};

    CHECK(ch.push(10, opt_key1) == PushResult::Queued, "first push (key=1) queues under capacity");
    uint64_t rejected_before = ch.rejected();
    PushResult pr = ch.push(20, opt_key2);  // different key, at capacity.
    CHECK(pr == PushResult::RejectedFull, "non-matching key at capacity is rejected, not clobbered");
    CHECK(ch.rejected() == rejected_before + 1, "rejected() incremented");
    int out = 0;
    CHECK(ch.try_pop(out) && out == 10, "original key=1 entry is untouched (value 10, not 20)");
    printf("ok coalesce_newest_nonmatching_key_rejects_without_clobbering\n");
}

// ---------------------------------------------------------------------------
// 10. run_blocking completes/latches once the consumer delivers the item.
// ---------------------------------------------------------------------------
static void test_run_blocking_completes() {
    printf("[test_run_blocking_completes]\n");
    Channel<int> ch(IntPolicy{0, OnFull::RejectNewest});
    bool result = false;
    std::thread producer([&] { result = ch.run_blocking(99); });

    std::vector<int> got;
    // Deterministically wait for the item to appear and be delivered via pump.
    while (true) {
        int ran = ch.pump(5.0, [&](int& v) { got.push_back(v); });
        if (ran > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    producer.join();
    CHECK(result == true, "run_blocking returned true once delivered via pump");
    CHECK(got.size() == 1 && got[0] == 99, "delivered value matches the pushed item");
    printf("ok run_blocking_completes\n");
}

// ---------------------------------------------------------------------------
// 11. shut_down wakes wait_pop waiters and fails a run_blocking caller that
//     is never consumed; push() returns ShutDown afterward.
// ---------------------------------------------------------------------------
static void test_shutdown_wakes_waiters_and_fails_run_blocking() {
    printf("[test_shutdown_wakes_waiters_and_fails_run_blocking]\n");

    // Channel A: a wait_pop waiter with nothing ever pushed on it.
    Channel<int> chA(IntPolicy{0, OnFull::RejectNewest});
    WaitResult wr = WaitResult::Item;
    std::thread waiter([&] {
        int out = 0;
        wr = chA.wait_pop(out);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    chA.shut_down();
    waiter.join();
    CHECK(wr == WaitResult::ShutDown, "wait_pop returned ShutDown after shut_down with nothing pushed");
    CHECK(chA.push(1) == PushResult::ShutDown, "push after shut_down returns ShutDown");

    // Channel B: a run_blocking caller whose item is never pumped/consumed.
    Channel<int> chB(IntPolicy{0, OnFull::RejectNewest});
    bool rb_result = true;
    std::thread rb_thread([&] { rb_result = chB.run_blocking(5); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    chB.shut_down();
    rb_thread.join();
    CHECK(rb_result == false, "run_blocking returned false after shut_down without being consumed");

    printf("ok shutdown_wakes_waiters_and_fails_run_blocking\n");
}

// ---------------------------------------------------------------------------
// 12. BlockProducer blocks a producer thread until the consumer pops, then
//     unblocks; also unblocks (with ShutDown) on shutdown.
// ---------------------------------------------------------------------------
static void test_block_producer_blocks_until_pop_then_unblocks() {
    printf("[test_block_producer_blocks_until_pop_then_unblocks]\n");
    Channel<int> ch(IntPolicy{1, OnFull::RejectNewest});
    ch.push(1);  // fill capacity

    IntOpts block_opts{OnFull::BlockProducer, std::nullopt};
    PushResult pr2 = PushResult::RejectedFull;
    bool producer_done = false;
    std::thread producer([&] {
        pr2 = ch.push(2, block_opts);
        producer_done = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(!producer_done, "BlockProducer push is still blocked while the channel stays full");

    int out = 0;
    CHECK(ch.try_pop(out) && out == 1, "consumer pops the only slot, making room");
    producer.join();
    CHECK(producer_done, "blocked producer unblocked after room freed");
    CHECK(pr2 == PushResult::Queued, "blocked push eventually queues");
    CHECK(ch.try_pop(out) && out == 2, "blocked item now in the channel");
    printf("ok block_producer_blocks_until_pop_then_unblocks\n");
}

static void test_block_producer_unblocks_on_shutdown() {
    printf("[test_block_producer_unblocks_on_shutdown]\n");
    Channel<int> ch(IntPolicy{1, OnFull::RejectNewest});
    ch.push(1);  // fill capacity

    IntOpts block_opts{OnFull::BlockProducer, std::nullopt};
    PushResult pr = PushResult::Queued;
    std::thread producer([&] { pr = ch.push(2, block_opts); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ch.shut_down();
    producer.join();
    CHECK(pr == PushResult::ShutDown, "BlockProducer push unblocks with ShutDown on shut_down()");
    printf("ok block_producer_unblocks_on_shutdown\n");
}

// ---------------------------------------------------------------------------
// 13. Counters (dropped/rejected) are accurate and monotonic.
// ---------------------------------------------------------------------------
static void test_counters_accurate_and_monotonic() {
    printf("[test_counters_accurate_and_monotonic]\n");
    Channel<int> ch(IntPolicy{1, OnFull::DropOldest});
    CHECK(ch.dropped() == 0 && ch.rejected() == 0, "counters start at 0");
    ch.push(1);
    ch.push(2);  // drops 1
    ch.push(3);  // drops 2
    CHECK(ch.dropped() == 2, "dropped() reflects two evictions");
    CHECK(ch.rejected() == 0, "rejected() untouched by DropOldest policy");

    Channel<int> ch2(IntPolicy{1, OnFull::RejectNewest});
    ch2.push(1);
    ch2.push(2);  // rejected
    ch2.push(3);  // rejected
    CHECK(ch2.rejected() == 2, "rejected() reflects two RejectNewest rejections");
    CHECK(ch2.dropped() == 0, "dropped() untouched by RejectNewest policy");
    printf("ok counters_accurate_and_monotonic\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    printf("=== event_channel_tests ===\n");
    test_push_try_pop_fifo();
    test_wait_pop_blocks_then_returns_on_push();
    test_wait_pop_for_timeout_then_item();
    test_pump_budget_progress_guarantee();
    test_pump_one_delivers_at_most_one();
    test_reject_newest_at_capacity();
    test_drop_oldest_evicts_oldest();
    test_coalesce_newest_matching_key_replaces();
    test_coalesce_newest_nonmatching_key_rejects_without_clobbering();
    test_run_blocking_completes();
    test_shutdown_wakes_waiters_and_fails_run_blocking();
    test_block_producer_blocks_until_pop_then_unblocks();
    test_block_producer_unblocks_on_shutdown();
    test_counters_accurate_and_monotonic();
    if (g_failures) {
        printf("\n%d FAILURES\n", g_failures);
        return 1;
    }
    printf("\nALL PASS\n");
    return 0;
}
