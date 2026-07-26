// async_bake.cpp — Phase B async-bake primitives implementation.
// GpuJobQueue: thread-safe GL-work FIFO; pump() drains on the app/GL thread.
// CommandQueue: superseding bake command queue for the worker thread.
// GL-thread guard: register/assert for debug-mode off-thread detection.
//
// E2 (event-system.md §II.1): both queues are now thin wrappers over
// evt::Channel<T>. The Channel owns the mutex+deque+cv transport, the
// progress-guaranteeing budgeted pump, the run_blocking completion latch, and
// shutdown draining; this file only layers each queue's specific policy on top.
#include "async_bake.h"

#include <cstdio>
#include <exception>
#include <thread>

namespace matter_async {

namespace {

#ifndef NDEBUG
// Recorded by register_gl_thread(); read by both assert_gl_thread and the
// off-thread guard below. Empty until the owner registers.
std::atomic<std::thread::id> s_gl_thread_id{std::thread::id{}};
#endif

// Owner-thread guard for the blocking-latch path (event-system.md §I.5):
// GpuJobQueue::run_blocking posts a job and waits for the app/GL thread to pump
// it. Calling it FROM the GL thread would deadlock (the waiter is the pumper).
// Channel<T> deliberately does not own this guard — it has no thread registry —
// so the wrapper layer enforces it, reusing the existing GL-thread registration.
// Debug-only and a no-op until a GL thread is registered, so it can only convert
// an already-deadlocking misuse into a fast abort; correct code never trips it.
void assert_off_gl_thread(const char* where) {
#ifndef NDEBUG
    auto gl_id = s_gl_thread_id.load(std::memory_order_relaxed);
    if (gl_id == std::thread::id{}) return;  // not registered yet, skip
    if (std::this_thread::get_id() == gl_id) {
        std::fprintf(stderr,
                     "assert_off_gl_thread FAILED at %s: blocking call from the "
                     "GL/pump thread would deadlock\n",
                     where ? where : "?");
        std::abort();
    }
#else
    (void)where;
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// GpuJobQueue — evt::Channel<JobEnvelope> wrapper
// ---------------------------------------------------------------------------

void GpuJobQueue::post(GpuJob job) {
    // Fire-and-forget: no result slot, no waiter. push() returns ShutDown after
    // shut_down() and the item is silently discarded — matching the original
    // "post after shutdown: silently discard" behavior.
    ch_.push(JobEnvelope{std::move(job), nullptr});
}

bool GpuJobQueue::run_blocking(GpuJob job, std::string& err) {
    assert_off_gl_thread("GpuJobQueue::run_blocking");

    auto result = std::make_shared<JobResult>();
    JobEnvelope env{std::move(job), result};

    // Channel::run_blocking returns "was it delivered?" — true once the pump
    // callback has run for this item, false if it was never delivered (rejected
    // or shut down). The unbounded RejectNewest policy never rejects, so a false
    // here means shutdown, exactly the original's "shutdown" failure.
    const bool delivered = ch_.run_blocking(std::move(env));
    if (!delivered) {
        err = "shutdown";
        return false;
    }
    // The pump callback filled `result` before Channel completed the latch, and
    // the latch's mutex establishes happens-before, so these reads are safe.
    err = result->err;
    return result->ok;
}

int GpuJobQueue::pump(double ms_budget) {
    // Channel::pump preserves the progress guarantee (always delivers >= 1 item
    // when work is pending) and the time budget. The delivery callback runs the
    // job exactly as the original inline pump did: cancelled-token jobs are
    // skipped as "cancelled", exceptions are contained, and the (ok, err) result
    // is stashed for any run_blocking waiter. Returns the count delivered
    // (skipped-cancelled jobs included).
    return ch_.pump(ms_budget, [](JobEnvelope& env) {
        bool ok = false;
        std::string job_err;
        if (env.job.token && env.job.token->is_cancelled()) {
            job_err = "cancelled";
            ok = false;
        } else {
            try {
                ok = env.job.fn(job_err);
            } catch (const std::exception& exception) {
                job_err = exception.what();
                ok = false;
            } catch (...) {
                job_err = "unknown GPU job failure";
                ok = false;
            }
        }
        if (env.result) {
            env.result->ok = ok;
            env.result->err = std::move(job_err);
        }
    });
}

void GpuJobQueue::shut_down() {
    // Fails every parked run_blocking latch (delivered=false -> run_blocking
    // returns "shutdown", false), drops fire-and-forget pending jobs, rejects
    // future pushes, and wakes all waiters.
    ch_.shut_down();
}

bool GpuJobQueue::idle() const {
    return ch_.empty();
}

// ---------------------------------------------------------------------------
// CommandQueue — evt::Channel<Command> wrapper + supersession hook
// ---------------------------------------------------------------------------

std::shared_ptr<CancelToken> CommandQueue::push(Command c) {
    auto tok = std::make_shared<CancelToken>();
    c.token = tok;

    std::lock_guard<std::mutex> lk(m_);

    // Guard: if already shut down, cancel the token and return without enqueuing.
    if (shut_down_) {
        tok->cancel();
        return tok;
    }

    if (c.kind == CommandKind::Shutdown) {
        // Cancel in-flight and every queued token, then shut the channel down so
        // the consumer's wait_pop / wait_pop_for wakes with ShutDown and pop()
        // returns false (drained). Observationally identical to the original
        // "clear queue, enqueue Shutdown, notify_all, pop returns false" path.
        shut_down_ = true;
        if (in_flight_) in_flight_->cancel();
        for (auto& t : pending_) {
            if (t) t->cancel();
        }
        pending_.clear();
        ch_.shut_down();
        return tok;
    }

    if (c.kind == CommandKind::BakeAll || c.kind == CommandKind::Reload) {
        // Supersession: cancel the in-flight command's token and every queued
        // command's token. The superseded commands stay physically in the
        // channel (now cancelled) and are skipped when popped, so the consumer's
        // next real command is this one — exactly the original clear-the-queue
        // result. pending_ keeps those cancelled tokens in order so the mirror
        // stays in lockstep with the channel; each drops as its command is
        // popped-and-skipped.
        if (in_flight_) in_flight_->cancel();
        for (auto& t : pending_) {
            if (t) t->cancel();
        }
    }
    // RebakeCone (and the superseding BakeAll/Reload) enqueue FIFO.
    ch_.push(std::move(c));
    pending_.push_back(tok);
    return tok;
}

bool CommandQueue::pop(Command& out) {
    for (;;) {
        Command tmp;
        const matter::evt::WaitResult r = ch_.wait_pop(tmp);
        if (r != matter::evt::WaitResult::Item) {
            // ShutDown + drained (wait_pop never times out).
            return false;
        }
        std::lock_guard<std::mutex> lk(m_);
        if (!pending_.empty()) pending_.pop_front();
        if (tmp.token && tmp.token->is_cancelled()) {
            // Superseded/drained before delivery — skip and keep waiting. A
            // cancelled entry is always followed by its non-cancelled
            // replacement (supersession enqueues it last), so this loop
            // terminates at the replacement or at ShutDown.
            continue;
        }
        in_flight_ = tmp.token;
        out = std::move(tmp);
        return true;
    }
}

bool CommandQueue::pop_wait(Command& out, int ms, bool& out_timed_out) {
    out_timed_out = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() < 0) remaining = std::chrono::milliseconds(0);

        Command tmp;
        const matter::evt::WaitResult r = ch_.wait_pop_for(tmp, remaining);
        if (r == matter::evt::WaitResult::Timeout) {
            out_timed_out = true;
            return false;
        }
        if (r == matter::evt::WaitResult::ShutDown) {
            return false;  // out_timed_out stays false: shutdown/drained
        }
        // WaitResult::Item
        std::lock_guard<std::mutex> lk(m_);
        if (!pending_.empty()) pending_.pop_front();
        if (tmp.token && tmp.token->is_cancelled()) {
            // Superseded — skip within the remaining time budget.
            continue;
        }
        in_flight_ = tmp.token;
        out = std::move(tmp);
        return true;
    }
}

void CommandQueue::shut_down() {
    std::lock_guard<std::mutex> lk(m_);
    shut_down_ = true;
    if (in_flight_) in_flight_->cancel();
    for (auto& t : pending_) {
        if (t) t->cancel();
    }
    pending_.clear();
    ch_.shut_down();
}

// ---------------------------------------------------------------------------
// GL-thread guard
// ---------------------------------------------------------------------------

void register_gl_thread() {
#ifndef NDEBUG
    s_gl_thread_id.store(std::this_thread::get_id(), std::memory_order_relaxed);
#endif
}

void assert_gl_thread(const char* where) {
#ifndef NDEBUG
    auto gl_id = s_gl_thread_id.load(std::memory_order_relaxed);
    if (gl_id == std::thread::id{}) return; // not registered yet, skip
    if (std::this_thread::get_id() != gl_id) {
        std::fprintf(stderr, "assert_gl_thread FAILED at %s: called from wrong thread\n",
                     where ? where : "?");
        std::abort();
    }
#else
    (void)where;
#endif
}

} // namespace matter_async
