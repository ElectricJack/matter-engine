// matter/event/subscription.h — RAII subscription handles, the
// phase/lane vocabulary, and evt::Subscription/evt::SubscriptionSet (E1b).
//
// Design authority: MatterEngine3/docs/event-system.md
//   S I.5 rule 3   — Subscription/SubscriptionSet lifetime contract.
//   S I.6          — threading contract: active-bit check immediately
//                    before entry, in-flight counting, quiescent
//                    unsubscribe as the explicit barrier.
//   S I.7          — phase/priority/name total ordering key (the phase
//                    enum and its declared order live here).
//   S II.4 item 1  — cross-thread subscriber lifetime correction.
//   S II.4 item 7  — all-build (never debug-only) uniqueness/fail-fast.
//
// Subscription intentionally holds NO pointer back into Hub — only a
// shared_ptr<SubscriptionBlock>. That is what makes "a handle destroyed
// after the hub is a safe no-op" (S I.5/I.6) true without an actual
// std::weak_ptr<Hub>: SubscriptionBlock is a free-standing, independently
// refcounted object. Hub's per-event-type subscriber records hold their
// own shared_ptr copy of the exact same block; whichever side (Hub or
// Subscription) is destroyed first, the block simply outlives it via the
// other side's refcount, and touching an orphaned block (active.store,
// in_flight inspection) is always well-defined.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "matter/event/registration_error.h"

namespace matter::evt {

// Fixed, non-extensible ordering phases (S I.7). Declared with explicit
// values so the enum's numeric order IS the phase component of the
// (phase, priority, name) ordering key — deliberately not user-extensible
// (an open phase graph reintroduces emergent order, which this design
// exists to kill).
enum class phase : int { First = 0, Early = 1, Default = 2, Late = 3, Last = 4 };

// Tag selecting immediate (same-thread, inline, no-queue-cost) delivery
// at a subscribe call site: `hub.must_subscribe<E>("name", evt::immediate, cb)`.
struct immediate_t {};
inline constexpr immediate_t immediate{};

// A small typed lane id (S I.5 rule 4). A lane is a queued-delivery
// destination with exactly one owning/pumping thread (S I.6,
// Hub::claim_lane). A handful of engine-wide lanes are predefined below;
// domain code that needs its own (e.g. a Lab job lane) mints one with
// make_lane() — ids are process-unique and stable for the process
// lifetime, which is all the ordering/registry/trace machinery in this
// milestone needs (E1b does not track per-lane display names; the
// registry/trace report lane ids, and a future inspector milestone can
// layer a name table on top without changing this type).
struct lane {
    uint32_t id = 0;  // 0 == invalid/unset (default-constructed lane)

    constexpr lane() = default;
    explicit constexpr lane(uint32_t v) : id(v) {}

    constexpr bool valid() const { return id != 0; }
    friend constexpr bool operator==(lane a, lane b) { return a.id == b.id; }
    friend constexpr bool operator!=(lane a, lane b) { return a.id != b.id; }

    static const lane app;
    static const lane legacy_poll;
    static const lane diagnostics;
};

// Mints a fresh, process-unique lane id (thread-safe). Use for
// domain/Lab-specific lanes beyond the predefined constants above.
lane make_lane();

// ---------------------------------------------------------------------
// The shared callback control block (S I.6): one per live subscription,
// jointly owned by the Subscription handle and the Hub's live subscriber
// record for as long as either side needs it.
// ---------------------------------------------------------------------
struct SubscriptionBlock {
    // Cleared by Subscription::reset()/~Subscription() (non-blocking
    // logical unsubscribe) and by Hub::close(). Dispatch checks this bit
    // immediately before invoking the callback — not merely at snapshot
    // time — so a subscription removed after the dispatch snapshot was
    // taken but before its turn is still correctly skipped (S I.6).
    std::atomic<bool> active{true};
    // Incremented immediately before invoking the callback, decremented
    // immediately after it returns. unsubscribe_all_and_wait()/Hub::close()
    // mark `active` false first, then wait for this to reach zero — the
    // finite condition S I.6 promises for quiescent teardown.
    std::atomic<int> in_flight{0};
    // Successful (active-at-invoke) delivery count. Lives on the block
    // (not the registry record) so registry_snapshot() sees a live count
    // even from a copy of the subscriber record taken during dispatch.
    std::atomic<uint64_t> delivery_count{0};
};

// RAII subscription handle. Move-only. Destructor performs a non-blocking
// logical unsubscribe (S I.5 rule 3): after it returns, no NEW dispatch
// to this callback can begin, though one already executing on another
// thread may finish.
class Subscription {
public:
    Subscription() = default;
    explicit Subscription(std::shared_ptr<SubscriptionBlock> block) : block_(std::move(block)) {}

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&&) noexcept = default;
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            reset();
            block_ = std::move(other.block_);
        }
        return *this;
    }

    ~Subscription() { reset(); }

    // Non-blocking logical unsubscribe. Safe to call from anywhere,
    // including from this subscription's own callback (S I.6 reentrancy:
    // "logically unsubscribing during dispatch is legal").
    void reset() {
        if (block_) block_->active.store(false, std::memory_order_release);
        block_.reset();
    }

    bool valid() const { return block_ != nullptr; }
    uint64_t delivery_count() const {
        return block_ ? block_->delivery_count.load(std::memory_order_relaxed) : 0;
    }

private:
    friend class SubscriptionSet;
    const std::shared_ptr<SubscriptionBlock>& block() const { return block_; }

    std::shared_ptr<SubscriptionBlock> block_;
};

// Aggregates subscriptions for one owner. Destructor performs a
// non-blocking logical unsubscribe of everything it holds (S I.5 rule 3)
// — safe to run from anywhere, including from one of its own callbacks.
//
// unsubscribe_all_and_wait() is the separate, explicit quiescence barrier
// for cross-thread owners (S I.5/I.6): it marks every held subscription
// inactive, then blocks until every one's in-flight count reaches zero.
// It must never be called from inside one of this set's own callbacks or
// from the affected lane's pump — that is fail-fast in every build via
// evt::fail_fast (S II.4 item 7), not merely asserted, because either
// case would deadlock waiting for the calling thread's own stack frame
// to return.
class SubscriptionSet {
public:
    SubscriptionSet() = default;
    ~SubscriptionSet() { reset_all(); }

    SubscriptionSet(const SubscriptionSet&) = delete;
    SubscriptionSet& operator=(const SubscriptionSet&) = delete;

    SubscriptionSet& operator+=(Subscription sub) {
        std::lock_guard<std::mutex> lk(m_);
        subs_.push_back(std::move(sub));
        return *this;
    }

    // Non-blocking logical unsubscribe of everything currently held.
    void reset_all() {
        std::vector<Subscription> local;
        {
            std::lock_guard<std::mutex> lk(m_);
            local.swap(subs_);
        }
        for (auto& s : local) s.reset();
    }

    // The quiescence barrier. See class comment. Idempotent-ish: safe to
    // call on an empty set (returns immediately).
    void unsubscribe_all_and_wait();

    size_t size() const {
        std::lock_guard<std::mutex> lk(m_);
        return subs_.size();
    }

private:
    mutable std::mutex m_;
    std::vector<Subscription> subs_;
};

}  // namespace matter::evt
