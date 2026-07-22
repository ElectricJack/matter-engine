// matter/event/channel.h — evt::Channel<T>, the consolidated mutex+deque
// transport primitive for the engine event system.
//
// Design authority: MatterEngine3/docs/event-system.md
//   S I.5  — the API sketch this header implements verbatim.
//   S I.6  — threading contract for queued delivery (bounded, drop/reject
//            counters, single-owner-thread pump).
//   S II.4 items 4 and 6 — transport-contract corrections (non-blocking /
//            blocking / timed consumer ops; caller-visible push outcomes).
//
// Channel<T> generalizes matter_async::GpuJobQueue and
// matter_async::CommandQueue (src/async_bake.h) into one hardened,
// policy-driven primitive. E2 reimplements those two on top of this type;
// E1a (this file) is transport only — no Hub, no typed events, no
// consumers. It has no dependency on anything under matter/event/ or
// matter/events/ beyond the standard library.
//
// Threading: multi-producer (push side, any thread), single-consumer (pop
// side — try_pop/wait_pop/wait_pop_for/pump/pump_one — exactly one logical
// owner thread). One mutex + two condition variables guard the whole
// object, matching the proven GpuJobQueue/CommandQueue shape (no lock-free
// primitives; S I.2 non-goals). A debug-only guard (compiled out under
// NDEBUG) aborts if a second thread calls a pop-side method — there is no
// thread-registry in Channel<T> itself (unlike
// matter_async::register_gl_thread/assert_gl_thread); that discipline is
// layered on by the Hub/lane wiring in later milestones.
#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace matter {
namespace evt {

enum class PushResult { Queued, Coalesced, RejectedFull, ShutDown };
enum class WaitResult { Item, Timeout, ShutDown };
enum class OnFull { RejectNewest, DropOldest, CoalesceNewest, BlockProducer };

template <class T>
class Channel {
public:
    struct Policy {
        size_t capacity;            // 0 = unbounded
        OnFull default_on_full;
    };
    struct PushOptions {
        OnFull on_full;                          // explicit per push
        std::optional<uint64_t> coalesce_key;    // required for CoalesceNewest
    };

    // Policy is mandatory: lossiness under load is a choice made explicitly
    // at construction, never a silently-inherited default (event-system.md
    // S I.5). There is no default constructor.
    explicit Channel(Policy policy) : policy_(policy) {}

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&&) = delete;
    Channel& operator=(Channel&&) = delete;

    // Defensive safety net: if the owner destroys the channel without
    // calling shut_down() first, fail any latches still parked in the queue
    // (run_blocking callers) rather than hanging them forever. Not a
    // substitute for an explicit shut_down() in the owning subsystem's
    // teardown order.
    ~Channel() {
        std::unique_lock<std::mutex> lock(m_);
        if (!shut_down_) drain_and_fail_locked();
    }

    // ------------------------------------------------------------------
    // Push side — any thread.
    // ------------------------------------------------------------------

    // Uses the channel's mandatory default_on_full policy.
    PushResult push(T item) {
        std::unique_lock<std::mutex> lock(m_);
        return push_locked(lock, std::move(item), PushOptions{policy_.default_on_full, std::nullopt},
                            nullptr);
    }

    // Per-push policy override.
    PushResult push(T item, PushOptions opts) {
        std::unique_lock<std::mutex> lock(m_);
        return push_locked(lock, std::move(item), opts, nullptr);
    }

    // Push + block until the item is handed to the consumer: delivered
    // synchronously (try_pop/wait_pop/wait_pop_for hand it directly to the
    // caller) or, when the owner consumes via pump/pump_one, after the
    // delivery callback returns for that item. Mirrors
    // GpuJobQueue::run_blocking's "post + wait" shape (src/async_bake.h)
    // adapted for a generic T: Channel<T> has no notion of "the job
    // succeeded" (that's GpuJobQueue-specific), so the bool here means
    // "delivered" (true) vs. "never delivered" (false) — rejected at push,
    // shut down, or evicted from the queue before delivery (DropOldest,
    // a CoalesceNewest replacement, or shut_down() draining it).
    bool run_blocking(T item) {
        return run_blocking(std::move(item), PushOptions{policy_.default_on_full, std::nullopt});
    }

    bool run_blocking(T item, PushOptions opts) {
        auto latch = std::make_shared<Latch>();
        PushResult pr;
        {
            std::unique_lock<std::mutex> lock(m_);
            pr = push_locked(lock, std::move(item), opts, latch);
        }
        if (pr == PushResult::RejectedFull || pr == PushResult::ShutDown) {
            return false;
        }
        std::unique_lock<std::mutex> lk(latch->m);
        latch->cv.wait(lk, [&] { return latch->done; });
        return latch->ok;
    }

    // ------------------------------------------------------------------
    // Pop side — single consumer thread.
    // ------------------------------------------------------------------

    bool try_pop(T& out) {
        std::unique_lock<std::mutex> lock(m_);
        check_consumer_thread_locked();
        if (q_.empty()) return false;
        Entry e = std::move(q_.front());
        q_.pop_front();
        cv_not_full_.notify_one();
        lock.unlock();
        if (e.latch) complete_latch(e.latch, true);
        out = std::move(e.item);
        return true;
    }

    WaitResult wait_pop(T& out) {
        std::unique_lock<std::mutex> lock(m_);
        check_consumer_thread_locked();
        cv_not_empty_.wait(lock, [&] { return !q_.empty() || shut_down_; });
        if (q_.empty()) return WaitResult::ShutDown;
        Entry e = std::move(q_.front());
        q_.pop_front();
        cv_not_full_.notify_one();
        lock.unlock();
        if (e.latch) complete_latch(e.latch, true);
        out = std::move(e.item);
        return WaitResult::Item;
    }

    WaitResult wait_pop_for(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_);
        check_consumer_thread_locked();
        bool signaled =
            cv_not_empty_.wait_for(lock, timeout, [&] { return !q_.empty() || shut_down_; });
        if (!signaled) return WaitResult::Timeout;
        if (q_.empty()) return WaitResult::ShutDown;
        Entry e = std::move(q_.front());
        q_.pop_front();
        cv_not_full_.notify_one();
        lock.unlock();
        if (e.latch) complete_latch(e.latch, true);
        out = std::move(e.item);
        return WaitResult::Item;
    }

    // Owner thread: run whole deliveries until ms_budget elapses or the
    // channel empties. Always delivers at least one item when work is
    // pending (progress guarantee) — mirrors GpuJobQueue::pump
    // (src/async_bake.cpp).
    int pump(double ms_budget, const std::function<void(T&)>& fn) {
        using Clock = std::chrono::steady_clock;
        auto start = Clock::now();
        int ran = 0;
        while (true) {
            std::optional<Entry> e;
            {
                std::unique_lock<std::mutex> lock(m_);
                check_consumer_thread_locked();
                if (q_.empty()) break;
                e.emplace(std::move(q_.front()));
                q_.pop_front();
                cv_not_full_.notify_one();
            }
            fn(e->item);
            ++ran;
            if (e->latch) complete_latch(e->latch, true);
            auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            if (elapsed >= ms_budget) break;
        }
        return ran;
    }

    // Owner thread: deliver at most one item. Returns 1 if delivered, 0 if
    // the channel was empty.
    int pump_one(const std::function<void(T&)>& fn) {
        std::optional<Entry> e;
        {
            std::unique_lock<std::mutex> lock(m_);
            check_consumer_thread_locked();
            if (q_.empty()) return 0;
            e.emplace(std::move(q_.front()));
            q_.pop_front();
            cv_not_full_.notify_one();
        }
        fn(e->item);
        if (e->latch) complete_latch(e->latch, true);
        return 1;
    }

    // Idempotent. Rejects future pushes, fails everything currently queued
    // (including parked run_blocking latches), and wakes every waiter
    // (blocked poppers and BlockProducer pushers alike).
    void shut_down() {
        std::unique_lock<std::mutex> lock(m_);
        if (shut_down_) return;
        shut_down_ = true;
        drain_and_fail_locked();
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

    // Best-effort emptiness probe, callable from any thread (locks m_). True
    // when no items are currently queued. Added for transport wrappers that
    // preserve an emptiness query in their public API (matter_async::
    // GpuJobQueue::idle, src/async_bake.h) — a wrapper-side pending counter
    // cannot track it without racing run_blocking()'s internal enqueue, so the
    // probe belongs on the channel that owns the queue. Purely additive; no
    // existing behavior changes.
    bool empty() const {
        std::lock_guard<std::mutex> lock(m_);
        return q_.empty();
    }

    // Monotonic intentional-loss counter: DropOldest evictions and
    // CoalesceNewest evicted-replacements.
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    // Monotonic caller-visible rejection counter: RejectNewest-at-capacity
    // and CoalesceNewest-with-no-matching-key-at-capacity.
    uint64_t rejected() const { return rejected_.load(std::memory_order_relaxed); }

private:
    struct Latch {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        bool ok = false;
    };
    // key is only ever compared for CoalesceNewest; latch is null for
    // fire-and-forget push()/push(item, opts) and non-null only when the
    // entry originated from run_blocking().
    struct Entry {
        T item;
        std::optional<uint64_t> key;
        std::shared_ptr<Latch> latch;
    };

    static void complete_latch(const std::shared_ptr<Latch>& latch, bool ok) {
        std::lock_guard<std::mutex> lk(latch->m);
        latch->ok = ok;
        latch->done = true;
        latch->cv.notify_all();
    }

    // Caller holds m_. Empties q_ and fails every latch found (shutdown /
    // destructor path) so no run_blocking() caller can hang forever.
    void drain_and_fail_locked() {
        std::deque<Entry> local = std::move(q_);
        q_.clear();
        for (auto& e : local) {
            if (e.latch) complete_latch(e.latch, false);
        }
    }

    // Caller holds `lock` (already locked on entry; may unlock/relock
    // internally for OnFull::BlockProducer). `latch` is null for
    // fire-and-forget pushes, non-null only from run_blocking().
    PushResult push_locked(std::unique_lock<std::mutex>& lock, T item, PushOptions opts,
                            std::shared_ptr<Latch> latch) {
        if (shut_down_) {
            if (latch) complete_latch(latch, false);
            return PushResult::ShutDown;
        }

        const bool has_room = (policy_.capacity == 0) || (q_.size() < policy_.capacity);
        if (has_room) {
            q_.push_back(Entry{std::move(item), opts.coalesce_key, std::move(latch)});
            cv_not_empty_.notify_one();
            return PushResult::Queued;
        }

        // At capacity: OnFull governs what happens to the newest item.
        switch (opts.on_full) {
            case OnFull::RejectNewest: {
                ++rejected_;
                if (latch) complete_latch(latch, false);
                return PushResult::RejectedFull;
            }
            case OnFull::DropOldest: {
                Entry evicted = std::move(q_.front());
                q_.pop_front();
                ++dropped_;
                if (evicted.latch) complete_latch(evicted.latch, false);
                q_.push_back(Entry{std::move(item), opts.coalesce_key, std::move(latch)});
                cv_not_empty_.notify_one();
                return PushResult::Queued;
            }
            case OnFull::CoalesceNewest: {
                assert(opts.coalesce_key.has_value() &&
                       "Channel<T>::push: OnFull::CoalesceNewest requires PushOptions::coalesce_key");
                if (opts.coalesce_key.has_value()) {
                    for (auto& e : q_) {
                        if (e.key.has_value() && *e.key == *opts.coalesce_key) {
                            // Replace in place: same queue slot/position, so
                            // FIFO order among distinct keys is preserved.
                            if (e.latch) complete_latch(e.latch, false);
                            e.item = std::move(item);
                            e.latch = std::move(latch);
                            ++dropped_;
                            return PushResult::Coalesced;
                        }
                    }
                }
                // No matching key queued: never clobber an unrelated entry.
                ++rejected_;
                if (latch) complete_latch(latch, false);
                return PushResult::RejectedFull;
            }
            case OnFull::BlockProducer: {
                // Genuine block-until-space-or-shutdown. Channel<T> has no
                // thread registry, so it cannot itself detect "called from
                // the owner/consumer thread" (which would deadlock here);
                // that guard is the consumer wiring's responsibility once a
                // Hub/lane owns the consumer thread (E2+). Tested in E1a
                // with a dedicated producer thread.
                cv_not_full_.wait(lock, [&] {
                    return shut_down_ || policy_.capacity == 0 || q_.size() < policy_.capacity;
                });
                if (shut_down_) {
                    if (latch) complete_latch(latch, false);
                    return PushResult::ShutDown;
                }
                q_.push_back(Entry{std::move(item), opts.coalesce_key, std::move(latch)});
                cv_not_empty_.notify_one();
                return PushResult::Queued;
            }
        }
        // Unreachable (all OnFull enumerators handled above); kept so
        // -Wreturn-type is satisfied without relying on switch exhaustiveness
        // analysis across compilers.
        if (latch) complete_latch(latch, false);
        return PushResult::RejectedFull;
    }

#ifndef NDEBUG
    // Debug-only single-consumer guard: records the first thread to call any
    // pop-side method and aborts if a different thread calls one later.
    void check_consumer_thread_locked() {
        auto id = std::this_thread::get_id();
        if (!consumer_thread_.has_value()) {
            consumer_thread_ = id;
            return;
        }
        assert(*consumer_thread_ == id &&
               "Channel<T>: pop-side method called from a second thread — "
               "Channel<T> is single-consumer (try_pop/wait_pop/wait_pop_for/pump/pump_one)");
    }
    std::optional<std::thread::id> consumer_thread_;
#else
    void check_consumer_thread_locked() {}
#endif

    Policy policy_;
    mutable std::mutex m_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    std::deque<Entry> q_;
    bool shut_down_ = false;
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> rejected_{0};
};

}  // namespace evt
}  // namespace matter
