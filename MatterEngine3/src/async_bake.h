#pragma once
// Phase B async-bake primitives. Kernel-internal — NOT part of the matter/ API.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace matter_async {

struct CancelToken {
    std::atomic<bool> cancelled{false};
    void cancel() { cancelled.store(true, std::memory_order_relaxed); }
    bool is_cancelled() const { return cancelled.load(std::memory_order_relaxed); }
};

// One unit of GL-thread work. fn returns false + fills err on failure.
struct GpuJob {
    std::string name;
    std::function<bool(std::string& err)> fn;
    std::shared_ptr<CancelToken> token;  // if set and cancelled, job is skipped (fails "cancelled")
};

// Thread-safe FIFO of GL jobs. Worker posts; the app thread pumps.
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
    struct Pending;        // job + optional completion latch (mutex/cv/done/ok/err)
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Pending>> q_;
    bool shut_down_ = false;
};

enum class CommandKind { BakeAll, Reload, RebakeCone, Shutdown };
struct Command {
    CommandKind kind = CommandKind::BakeAll;
    std::vector<std::string> changed_files;   // RebakeCone only
    std::shared_ptr<CancelToken> token;       // filled by CommandQueue::push
};

// Single-consumer queue with supersession: BakeAll/Reload cancels the
// in-flight command's token and clears ALL pending commands. RebakeCone
// queues FIFO. Shutdown cancels everything and wakes the consumer.
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
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Command> q_;
    std::shared_ptr<CancelToken> in_flight_;  // token of the command last popped
    bool shut_down_ = false;
};

// GL-thread guard. register_gl_thread() is called once by EngineContext::create;
// assert_gl_thread aborts with `where` in debug builds when called off-thread.
// Both are no-ops in NDEBUG builds.
void register_gl_thread();
void assert_gl_thread(const char* where);

} // namespace matter_async
