// async_queue_tests.cpp — headless tests for Phase B async bake primitives.
// Tests: GpuJobQueue, CommandQueue, CancelToken, GL-thread guard.
// Plain assert + printf style; no GL, no window, no GALLIUM env needed.
#include "async_bake.h"
#include "matter/events.h"
#include "check.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include <string>

using namespace matter_async;

// ---------------------------------------------------------------------------
// 1. pump_runs_posted_jobs_in_order
//    Post 3 jobs appending to a vector; pump(1000) runs all 3, returns 3.
// ---------------------------------------------------------------------------
static void test_pump_runs_posted_jobs_in_order() {
    std::printf("[test_pump_runs_posted_jobs_in_order]\n");
    GpuJobQueue q;
    std::vector<int> order;
    for (int i = 0; i < 3; ++i) {
        int idx = i;
        q.post({std::string("job") + std::to_string(i),
                [&order, idx](std::string&) { order.push_back(idx); return true; },
                nullptr});
    }
    int ran = q.pump(1000.0);
    CHECK(ran == 3, "pump returns 3 for 3 posted jobs");
    CHECK(order.size() == 3, "all 3 jobs ran");
    CHECK(order[0] == 0 && order[1] == 1 && order[2] == 2, "jobs ran in FIFO order");
    printf("ok pump_runs_posted_jobs_in_order\n");
}

// ---------------------------------------------------------------------------
// 2. pump_respects_budget_but_always_runs_one
//    Post 2 jobs that each sleep 5 ms; pump(0.1) returns 1 (min-one guarantee),
//    second pump(0.1) returns 1.
// ---------------------------------------------------------------------------
static void test_pump_respects_budget_but_always_runs_one() {
    std::printf("[test_pump_respects_budget_but_always_runs_one]\n");
    GpuJobQueue q;
    for (int i = 0; i < 2; ++i) {
        q.post({"slow", [](std::string&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return true;
        }, nullptr});
    }
    int r1 = q.pump(0.1);   // budget = 0.1 ms; job takes 5 ms → min-one rule fires
    CHECK(r1 == 1, "first pump(0.1ms) returns 1 (min-one guarantee)");
    CHECK(!q.idle(), "queue still has one job pending");
    int r2 = q.pump(0.1);
    CHECK(r2 == 1, "second pump(0.1ms) returns 1");
    CHECK(q.idle(), "queue now empty");
    printf("ok pump_respects_budget_but_always_runs_one\n");
}

// ---------------------------------------------------------------------------
// 3. run_blocking_returns_result
//    Worker thread calls run_blocking with a job returning false + err "boom";
//    main thread pumps; join; assert waiter saw false/"boom".
// ---------------------------------------------------------------------------
static void test_run_blocking_returns_result() {
    std::printf("[test_run_blocking_returns_result]\n");
    GpuJobQueue q;
    bool got_ok = true;
    std::string got_err;
    std::thread worker([&]() {
        std::string err;
        bool ok = q.run_blocking(
            {"failing", [](std::string& e) { e = "boom"; return false; }, nullptr},
            err);
        got_ok = ok;
        got_err = err;
    });
    // Wait deterministically until the job is posted (queue non-idle), then pump.
    while (q.idle()) std::this_thread::yield();
    q.pump(1000.0);
    worker.join();
    CHECK(got_ok == false, "run_blocking returned false for failing job");
    CHECK(got_err == "boom", "run_blocking propagated error string");
    printf("ok run_blocking_returns_result\n");
}

// ---------------------------------------------------------------------------
// 4. cancelled_token_skips_job
//    Post job with cancelled token; pump; assert fn never ran and pump returned 1.
// ---------------------------------------------------------------------------
static void test_cancelled_token_skips_job() {
    std::printf("[test_cancelled_token_skips_job]\n");
    GpuJobQueue q;
    auto tok = std::make_shared<CancelToken>();
    tok->cancel();
    bool fn_ran = false;
    q.post({"skipped", [&fn_ran](std::string&) { fn_ran = true; return true; }, tok});
    int ran = q.pump(1000.0);
    CHECK(ran == 1, "pump counts cancelled job as executed (returns 1)");
    CHECK(!fn_ran, "fn did not run for cancelled token job");
    printf("ok cancelled_token_skips_job\n");
}

// ---------------------------------------------------------------------------
// 5. shutdown_unblocks_waiter
//    Worker blocks in run_blocking; main calls shut_down() without pumping;
//    join succeeds, waiter got false.
// ---------------------------------------------------------------------------
static void test_shutdown_unblocks_waiter() {
    std::printf("[test_shutdown_unblocks_waiter]\n");
    GpuJobQueue q;
    bool got_ok = true;
    std::thread worker([&]() {
        std::string err;
        bool ok = q.run_blocking(
            {"never-pumped", [](std::string&) { return true; }, nullptr},
            err);
        got_ok = ok;
    });
    // Wait deterministically until the job is posted (queue non-idle), then shut down.
    while (q.idle()) std::this_thread::yield();
    q.shut_down();
    worker.join();
    CHECK(got_ok == false, "run_blocking returned false after shut_down");
    printf("ok shutdown_unblocks_waiter\n");
}

// ---------------------------------------------------------------------------
// 6. bakeall_supersedes_pending_and_cancels_inflight
//    Push RebakeCone x2, pop one (in-flight), push BakeAll; assert:
//    in-flight token cancelled, next pop returns BakeAll, queue then empty.
// ---------------------------------------------------------------------------
static void test_bakeall_supersedes_pending_and_cancels_inflight() {
    std::printf("[test_bakeall_supersedes_pending_and_cancels_inflight]\n");
    CommandQueue cq;

    // Push two RebakeCone commands
    auto tok1 = cq.push({CommandKind::RebakeCone, {"file1.js"}, nullptr});
    auto tok2 = cq.push({CommandKind::RebakeCone, {"file2.js"}, nullptr});

    // Pop the first (it becomes in-flight)
    Command inflight;
    bool popped1 = cq.pop(inflight);
    CHECK(popped1, "first pop succeeded");
    CHECK(inflight.kind == CommandKind::RebakeCone, "first popped is RebakeCone");
    std::shared_ptr<CancelToken> inflight_tok = inflight.token;

    // Now push BakeAll — should cancel in-flight, drop remaining queued commands
    auto bake_tok = cq.push({CommandKind::BakeAll, {}, nullptr});

    CHECK(inflight_tok && inflight_tok->is_cancelled(),
          "in-flight RebakeCone token was cancelled by BakeAll push");

    // Next pop should return BakeAll
    Command next;
    bool popped2 = cq.pop(next);
    CHECK(popped2, "second pop succeeded");
    CHECK(next.kind == CommandKind::BakeAll, "BakeAll superseded the pending RebakeCone");

    // Queue should now be empty; shut down so pop returns false
    cq.shut_down();
    Command drain;
    bool popped3 = cq.pop(drain);
    CHECK(!popped3, "queue empty after BakeAll consumed + shutdown");

    printf("ok bakeall_supersedes_pending_and_cancels_inflight\n");
}

// ---------------------------------------------------------------------------
// 7. command_shutdown_wakes_pop
//    Consumer thread in pop; shut_down(); join; pop returned false.
// ---------------------------------------------------------------------------
static void test_command_shutdown_wakes_pop() {
    std::printf("[test_command_shutdown_wakes_pop]\n");
    CommandQueue cq;
    bool pop_returned_false = false;
    std::thread consumer([&]() {
        Command c;
        bool ok = cq.pop(c);
        pop_returned_false = !ok;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cq.shut_down();
    consumer.join();
    CHECK(pop_returned_false, "pop returned false after shut_down");
    printf("ok command_shutdown_wakes_pop\n");
}

// ---------------------------------------------------------------------------
// 8. push_after_shutdown_is_cancelled_and_not_queued
//    shut_down() a CommandQueue, then push(BakeAll) — returned token must be
//    already cancelled and a subsequent pop must return false (nothing dequeued).
// ---------------------------------------------------------------------------
static void test_push_after_shutdown_is_cancelled_and_not_queued() {
    std::printf("[test_push_after_shutdown_is_cancelled_and_not_queued]\n");
    CommandQueue cq;
    cq.shut_down();

    auto tok = cq.push({CommandKind::BakeAll, {}, nullptr});
    CHECK(tok != nullptr, "push returns a non-null token even after shutdown");
    CHECK(tok->is_cancelled(), "token returned by push-after-shutdown is already cancelled");

    Command c;
    bool ok = cq.pop(c);
    CHECK(!ok, "pop returns false after shutdown (nothing dequeued)");

    printf("ok push_after_shutdown_is_cancelled_and_not_queued\n");
}

// ---------------------------------------------------------------------------
// 9. event_struct_shape_test
//    Constructs a matter::Event, sets phase/code/errors, asserts defaults.
// ---------------------------------------------------------------------------
static void test_event_struct_shape() {
    std::printf("[test_event_struct_shape]\n");
    matter::Event ev;
    CHECK(ev.code == matter::BakeErrorCode::None, "default code is None");
    CHECK(ev.errors == 0, "default errors is 0");
    CHECK(ev.phase.empty(), "default phase is empty");

    ev.phase = "compose";
    ev.code = matter::BakeErrorCode::GpuError;
    ev.errors = 5;
    CHECK(ev.phase == "compose", "phase set correctly");
    CHECK(ev.code == matter::BakeErrorCode::GpuError, "code set correctly");
    CHECK(ev.errors == 5, "errors set correctly");
    printf("ok event_struct_shape_test\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    std::printf("=== async_queue_tests ===\n");
    test_pump_runs_posted_jobs_in_order();
    test_pump_respects_budget_but_always_runs_one();
    test_run_blocking_returns_result();
    test_cancelled_token_skips_job();
    test_shutdown_unblocks_waiter();
    test_bakeall_supersedes_pending_and_cancels_inflight();
    test_command_shutdown_wakes_pop();
    test_push_after_shutdown_is_cancelled_and_not_queued();
    test_event_struct_shape();
    if (g_failures) {
        std::printf("\n%d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("\nALL PASS\n");
    return 0;
}
