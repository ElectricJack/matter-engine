// async_bake.cpp — Phase B async-bake primitives implementation.
// GpuJobQueue: thread-safe GL-work FIFO; pump() drains on the app/GL thread.
// CommandQueue: superseding bake command queue for the worker thread.
// GL-thread guard: register/assert for debug-mode off-thread detection.
#include "async_bake.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <exception>
#include <thread>

namespace matter_async {

// ---------------------------------------------------------------------------
// GpuJobQueue internals
// ---------------------------------------------------------------------------

// Completion latch held by run_blocking.
struct GpuJobQueue::Pending {
    GpuJob job;
    // Latch — null when fire-and-forget (no waiter).
    struct Latch {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        bool ok   = false;
        std::string err;
    };
    std::shared_ptr<Latch> latch; // null for fire-and-forget
};

void GpuJobQueue::post(GpuJob job) {
    auto p = std::make_shared<Pending>();
    p->job = std::move(job);
    // No latch — caller doesn't wait.
    {
        std::lock_guard<std::mutex> lk(m_);
        if (shut_down_) {
            // Post after shutdown: silently discard (fire-and-forget; no waiter).
            return;
        }
        q_.push_back(std::move(p));
    }
    cv_.notify_one();
}

bool GpuJobQueue::run_blocking(GpuJob job, std::string& err) {
    auto latch = std::make_shared<Pending::Latch>();
    auto p = std::make_shared<Pending>();
    p->job   = std::move(job);
    p->latch = latch;

    {
        std::lock_guard<std::mutex> lk(m_);
        if (shut_down_) {
            // Post after shutdown: resolve immediately as failed.
            err = "shutdown";
            return false;
        }
        q_.push_back(p);
    }
    cv_.notify_one();

    // Wait for pump to complete the job.
    std::unique_lock<std::mutex> lk(latch->m);
    latch->cv.wait(lk, [&] { return latch->done; });
    err = latch->err;
    return latch->ok;
}

int GpuJobQueue::pump(double ms_budget) {
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    int ran = 0;

    while (true) {
        std::shared_ptr<Pending> p;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (q_.empty()) break;
            p = q_.front();
            q_.pop_front();
        }

        // Execute or skip.
        bool ok = false;
        std::string job_err;
        if (p->job.token && p->job.token->is_cancelled()) {
            // Cancelled: skip fn, mark failed as "cancelled".
            job_err = "cancelled";
            ok = false;
        } else {
            try {
                ok = p->job.fn(job_err);
            } catch (const std::exception& exception) {
                job_err = exception.what();
                ok = false;
            } catch (...) {
                job_err = "unknown GPU job failure";
                ok = false;
            }
        }
        ++ran;

        // Notify waiter if any.
        if (p->latch) {
            std::lock_guard<std::mutex> lk(p->latch->m);
            p->latch->ok  = ok;
            p->latch->err = std::move(job_err);
            p->latch->done = true;
            p->latch->cv.notify_all();
        }

        // Check budget: always run at least one, then stop if elapsed.
        auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (ran > 0 && elapsed >= ms_budget) break;
    }

    return ran;
}

void GpuJobQueue::shut_down() {
    std::deque<std::shared_ptr<Pending>> local;
    {
        std::lock_guard<std::mutex> lk(m_);
        shut_down_ = true;
        local = std::move(q_);
        q_.clear();
    }
    // Fail all pending jobs.
    for (auto& p : local) {
        if (p->latch) {
            std::lock_guard<std::mutex> lk(p->latch->m);
            p->latch->ok   = false;
            p->latch->err  = "shutdown";
            p->latch->done = true;
            p->latch->cv.notify_all();
        }
    }
    cv_.notify_all();
}

bool GpuJobQueue::idle() const {
    std::lock_guard<std::mutex> lk(m_);
    return q_.empty();
}

// ---------------------------------------------------------------------------
// CommandQueue
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
        // Cancel in-flight and all queued tokens, then clear queue and wake consumer.
        shut_down_ = true;
        if (in_flight_) in_flight_->cancel();
        for (auto& cmd : q_) {
            if (cmd.token) cmd.token->cancel();
        }
        q_.clear();
        q_.push_back(std::move(c));
        cv_.notify_all();
        return tok;
    }

    if (c.kind == CommandKind::BakeAll || c.kind == CommandKind::Reload) {
        // Supersession: cancel in-flight + all queued, then enqueue this one.
        if (in_flight_) in_flight_->cancel();
        for (auto& cmd : q_) {
            if (cmd.token) cmd.token->cancel();
        }
        q_.clear();
        q_.push_back(std::move(c));
    } else {
        // RebakeCone: simple FIFO enqueue.
        q_.push_back(std::move(c));
    }

    cv_.notify_one();
    return tok;
}

bool CommandQueue::pop(Command& out) {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [this] { return !q_.empty() || shut_down_; });

    if (q_.empty()) {
        // Shut down and drained.
        return false;
    }

    out = std::move(q_.front());
    q_.pop_front();
    in_flight_ = out.token;

    // If this is a Shutdown command, clear in_flight_ and return false to signal the consumer.
    if (out.kind == CommandKind::Shutdown) {
        in_flight_ = nullptr;
        return false;
    }

    return true;
}

bool CommandQueue::pop_wait(Command& out, int ms, bool& out_timed_out) {
    out_timed_out = false;
    std::unique_lock<std::mutex> lk(m_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    bool signaled = cv_.wait_until(lk, deadline,
        [this] { return !q_.empty() || shut_down_; });

    if (!signaled) {
        // Timed out — no command arrived.
        out_timed_out = true;
        return false;
    }
    if (q_.empty()) {
        // Shut down and drained.
        return false;
    }

    out = std::move(q_.front());
    q_.pop_front();
    in_flight_ = out.token;

    if (out.kind == CommandKind::Shutdown) {
        in_flight_ = nullptr;
        return false;
    }

    return true;
}

void CommandQueue::shut_down() {
    std::lock_guard<std::mutex> lk(m_);
    shut_down_ = true;
    if (in_flight_) in_flight_->cancel();
    for (auto& cmd : q_) {
        if (cmd.token) cmd.token->cancel();
    }
    q_.clear();
    cv_.notify_all();
}

// ---------------------------------------------------------------------------
// GL-thread guard
// ---------------------------------------------------------------------------

#ifndef NDEBUG
static std::atomic<std::thread::id> s_gl_thread_id{std::thread::id{}};
#endif

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
