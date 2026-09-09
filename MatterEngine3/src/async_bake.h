#pragma once
// MatterEngine3/src/async_bake.h
//
// The two queues that connect the engine's app/GL thread to its bake worker
// thread, running in opposite directions:
//
//   * CommandQueue -- requester (app/editor) to worker. Bake commands, with
//     supersession: a new full bake cancels the one in flight and everything
//     queued behind it.
//   * GpuJobQueue  -- worker to app/GL thread. Work that must run on the thread
//     owning the graphics context, either fire-and-forget (post) or waited on
//     (run_blocking).
//
// Both are torn down through shut_down(). Nothing is ever silently abandoned:
// a dropped, superseded or shut-down item completes as cancelled through its
// CancelToken, and every parked run_blocking waiter is released, so no producer
// can be left blocked on a queue that no longer has a consumer.
//
// Nothing here touches GL/Vulkan itself -- it only decides which thread runs
// what, and when.
// Phase B async-bake primitives. Kernel-internal — NOT part of the matter/ API.
//
// E2 (event-system.md §II.1): GpuJobQueue and CommandQueue are now thin
// wrappers over evt::Channel<T> (include/matter/event/channel.h). Their public
// method signatures and semantics are preserved EXACTLY — no consumer changes.
// The mutex+deque+cv machinery that used to live here is the Channel's now;
// what remains here is the queue-specific policy each wrapper layers on top:
//   * GpuJobQueue  — job execution + cancel-token/exception handling in the
//                    pump callback, plus the run_blocking result (ok/err) that
//                    Channel's "delivered?" latch does not itself carry.
//   * CommandQueue — cancel-token supersession (BakeAll/Reload/Shutdown) and
//                    the single-consumer pop contract, layered on an unbounded
//                    Channel via wait_pop / wait_pop_for (§I.5).
#include "matter/event/channel.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace matter_async {

// One-shot, monotonic cancel flag shared between whoever requested a piece of
// work and whoever runs it. Only ever goes false -> true; there is no reset, so
// a cancelled token stays cancelled for the life of the request. Always held
// through shared_ptr: the requester keeps one to cancel with, the queued
// Command/GpuJob keeps one to poll. Relaxed atomics are deliberate -- the flag
// is a polled hint, and the queue's own mutex supplies the ordering that matters
// for the payload it guards.
struct CancelToken {
    std::atomic<bool> cancelled{false};
    void cancel() { cancelled.store(true, std::memory_order_relaxed); }
    bool is_cancelled() const { return cancelled.load(std::memory_order_relaxed); }
};

// One unit of GL-thread work. fn returns false + fills err on failure.
struct GpuJob {
    std::string name;  // shown by the [gpu-job] slow-job log; "(unnamed)" if empty
    std::function<bool(std::string& err)> fn;
    std::shared_ptr<CancelToken> token;  // if set and cancelled, job is skipped (fails "cancelled")
};

// Thread-safe FIFO of GL jobs. Worker posts; the app thread pumps.
// E2: reimplemented on evt::Channel<JobEnvelope> (unbounded — commands/jobs are
// non-dropping). post→push; run_blocking→Channel::run_blocking + a result slot;
// pump→Channel::pump (progress guarantee + time budget preserved);
// shut_down→Channel::shut_down; idle→Channel::empty.
// Threading: post() is safe from any thread. run_blocking() is safe from any
// thread EXCEPT the pump thread -- calling it there waits on yourself and
// deadlocks, which debug builds turn into an immediate abort
// (assert_off_gl_thread in async_bake.cpp). pump() is single-consumer: the
// app/GL thread only.
class GpuJobQueue {
public:
    void post(GpuJob job);                              // fire-and-forget
    bool run_blocking(GpuJob job, std::string& err);    // post + wait; false on fail/cancel/shutdown
    // App/GL thread: run whole jobs until ms_budget elapsed or queue empty.
    // Always runs at least one job when work is pending (progress guarantee).
    // Returns the number of jobs executed (skipped-cancelled jobs count).
    int pump(double ms_budget);
    void shut_down();      // fail all pending + future jobs; unblock all waiters
    bool idle() const;     // nothing pending
private:
    // Channel<T> delivers items but has no notion of "the job succeeded" — that
    // is GpuJobQueue-specific. run_blocking therefore carries its own result
    // slot, filled by the pump callback before Channel completes the delivery
    // latch (null for fire-and-forget post()).
    struct JobResult {
        bool ok = false;
        std::string err;
    };
    struct JobEnvelope {
        GpuJob job;
        std::shared_ptr<JobResult> result;  // null for fire-and-forget
    };
    using Chan = matter::evt::Channel<JobEnvelope>;
    Chan ch_{Chan::Policy{/*capacity=*/0, matter::evt::OnFull::RejectNewest}};
};

// What the bake worker is being asked to do. The kind also selects the queue
// policy applied by CommandQueue::push:
//   BakeAll    - full bake; supersedes everything queued and in flight.
//   Reload     - reload then bake; supersedes exactly like BakeAll.
//   RebakeCone - incremental rebake driven by `changed_files`; queues FIFO and
//                supersedes nothing.
//   Shutdown   - cancel everything and end the consumer loop (pop returns
//                false). Never delivered as a command; it is handled inside
//                push().
enum class CommandKind { BakeAll, Reload, RebakeCone, Shutdown };
// One request handed to the bake worker. Built by the producer and consumed by
// the single worker thread. Callers leave `token` null -- CommandQueue::push
// creates it, stores it in the command, and returns it, so the producer's copy
// and the worker's copy are the same token.
struct Command {
    CommandKind kind = CommandKind::BakeAll;
    std::vector<std::string> changed_files;   // RebakeCone only
    std::shared_ptr<CancelToken> token;       // filled by CommandQueue::push
};

// Single-consumer queue with supersession: BakeAll/Reload cancels the
// in-flight command's token and clears ALL pending commands. RebakeCone
// queues FIFO. Shutdown cancels everything and wakes the consumer.
//
// E2: reimplemented on an UNBOUNDED evt::Channel<Command> (capacity = 0;
// commands are non-dropping — never DropOldest/CoalesceNewest). The worker's
// blocking pop uses Channel::wait_pop / wait_pop_for. Supersession is an
// explicit hook layered here, not in the Channel: a mutex-guarded token mirror
// (pending_) lets push() cancel the in-flight token plus every queued command's
// token in one critical section; superseded commands remain physically in the
// channel (now cancelled) and are skipped when popped, so the consumer sees
// exactly the same command stream as the original clear-the-queue design. Every
// displaced command completes as cancelled via its CancelToken.
// Threading: push() and shut_down() are safe from any thread; pop() and
// pop_wait() are single-consumer, the bake worker thread only.
class CommandQueue {
public:
    std::shared_ptr<CancelToken> push(Command c);
    bool pop(Command& out);             // blocks; false once shut down and drained
    // Phase C Task 6: timed pop for the refine loop.
    // Returns true + fills out if a command is available within ms milliseconds.
    // Returns false (and does NOT fill out) on timeout, shutdown, or empty+drained.
    // Caller must check the return value; false on shutdown signals termination.
    // out_timed_out is set to true on timeout (vs false on shutdown/drained).
    bool pop_wait(Command& out, int ms, bool& out_timed_out);
    void shut_down();
private:
    using Chan = matter::evt::Channel<Command>;
    Chan ch_{Chan::Policy{/*capacity=*/0, matter::evt::OnFull::RejectNewest}};

    // Guards the supersession bookkeeping (NOT the channel — the channel owns
    // its own lock). pending_ mirrors, in FIFO order, the CancelTokens of the
    // commands currently queued in ch_; each is popped in lockstep as its
    // command is delivered. Lock order is always m_ -> ch_ internal mutex
    // (push/shut_down hold m_ then touch ch_); pop touches ch_ first (blocking
    // wait_pop, no m_ held) then acquires m_, so the two never nest the other
    // way and cannot deadlock.
    std::mutex m_;
    std::deque<std::shared_ptr<CancelToken>> pending_;
    std::shared_ptr<CancelToken> in_flight_;  // token of the command last delivered
    bool shut_down_ = false;
};

// GL-thread guard. register_gl_thread() is called once by EngineContext::create;
// assert_gl_thread aborts with `where` in debug builds when called off-thread.
// Both are no-ops in NDEBUG builds.
void register_gl_thread();
void assert_gl_thread(const char* where);

} // namespace matter_async
