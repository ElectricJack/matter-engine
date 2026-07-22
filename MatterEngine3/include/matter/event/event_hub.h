// matter/event/event_hub.h — evt::Hub, the typed pub-sub core (E1b).
//
// Design authority: MatterEngine3/docs/event-system.md
//   S I.5   — the core API this header implements: MT_EVENT_NAME,
//             must_subscribe/try_subscribe, subscribe signature (lane vs
//             immediate), emit, pump, tearing-down-a-cross-thread-
//             subscriber.
//   S I.6   — threading contract: immediate vs queued delivery,
//             reentrancy (active-check-before-entry + deferred mutation +
//             emit-depth cap), queued envelope = copied event + lane +
//             sequence, snapshot active subscribers at pump/dispatch,
//             in-flight counts.
//   S I.7   — deterministic ordering = (phase, priority, name) total
//             order; all-build unique names per event type.
//   S I.8   — registry snapshot + trace ring buffer.
//   S II.1 E1, S II.4 items 1 and 7 — the test list and the lifetime /
//             all-build-uniqueness corrections this MUST satisfy.
//
// Scope (E1b, per the milestone brief): the Hub, subscriptions, ordering,
// registry, trace. No consumers, no migration — matter_engine.cpp,
// async_bake.h, and the domain `matter/events/*.h` headers are untouched.
//
// Queued lanes are built directly on E1a's evt::Channel<T> (channel.h):
// each lane owns one Channel<QueuedEnvelope>, where QueuedEnvelope is a
// type-erased envelope (the event type varies by emit<E> call site, but
// one Channel instantiation serves every event type sharing a lane).
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include "matter/event/channel.h"
#include "matter/event/detail/dispatch_context.h"
#include "matter/event/event_name.h"
#include "matter/event/registration_error.h"
#include "matter/event/subscription.h"

namespace matter::evt {

template <class E>
using Callback = std::function<void(const E&)>;

// ---------------------------------------------------------------------
// Registry (S I.8): "who listens, in what order."
// ---------------------------------------------------------------------
struct SubscriberInfo {
    std::string name;
    phase ph = phase::Default;
    int priority = 0;
    bool is_immediate = false;
    uint32_t lane_id = 0;  // valid only when !is_immediate
    const char* file = nullptr;
    int line = 0;
    uint64_t delivery_count = 0;
    bool active = true;
};

struct RegistrySnapshotEntry {
    const char* event_name = nullptr;
    std::vector<SubscriberInfo> subscribers;  // in dispatch (phase,priority,name) order
};

// ---------------------------------------------------------------------
// Trace (S I.8): "what fired." A per-hub fixed-size ring buffer.
// ---------------------------------------------------------------------
struct TraceSubscriberRecord {
    std::string name;
    bool is_immediate = false;
    uint32_t lane_id = 0;
    double handler_ms = 0.0;
    int nesting_depth = 0;  // handler-emitted-event nesting, S I.6/I.8
};

struct TraceRecord {
    std::string event_name;
    std::thread::id thread;
    std::chrono::steady_clock::time_point emit_time{};
    // For immediate dispatch, delivery_time == emit_time. For a queued
    // envelope, delivery_time is when pump() actually dispatched it,
    // making queue latency visible (S I.8).
    std::chrono::steady_clock::time_point delivery_time{};
    int nesting_depth = 0;
    std::vector<TraceSubscriberRecord> subscribers;
};

// ---------------------------------------------------------------------
// Internal (non-public) storage types. Namespaced under evt::detail to
// signal "implementation detail, not API," but they must be visible here
// because Hub's public template methods (emit<E>, must_subscribe<E>, ...)
// are defined inline and need full type information.
// ---------------------------------------------------------------------
namespace detail {

struct SubscriberRecord {
    std::string name;
    phase ph = phase::Default;
    int priority = 0;
    bool is_immediate = false;
    lane ln{};
    const char* file = nullptr;
    int line = 0;
    std::shared_ptr<SubscriptionBlock> block;
    std::function<void(const void*)> invoke;  // casts back to `const E*` internally
};

// Total order = (phase, priority, name), the S I.7 ordering key.
struct SubscriberOrder {
    bool operator()(const SubscriberRecord& a, const SubscriberRecord& b) const {
        if (a.ph != b.ph) return static_cast<int>(a.ph) < static_cast<int>(b.ph);
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.name < b.name;
    }
};

struct TypeState {
    const char* event_type_name = nullptr;
    mutable std::mutex mu;
    std::vector<SubscriberRecord> subscribers;  // kept sorted by SubscriberOrder
};

// One queued delivery. `event` is a type-erased copy (S I.6: "a queued
// envelope stores the copied event, never a raw callback pointer").
struct QueuedEnvelope {
    const void* type_id = nullptr;
    const char* event_name = nullptr;
    uint32_t lane_id = 0;
    uint64_t sequence = 0;
    std::shared_ptr<void> event;
    std::chrono::steady_clock::time_point enqueue_time{};
};

}  // namespace detail

class Hub {
public:
    // trace_capacity: ring buffer size (S I.8 default 16k).
    explicit Hub(size_t trace_capacity = 16384) : trace_capacity_(trace_capacity) {}

    // Safety net mirroring Channel<T>'s destructor discipline: if the
    // owner didn't already call close() as part of an explicit teardown
    // sequence, do it here so no dangling invoke() is ever reachable
    // after ~Hub. This is NOT a substitute for the documented close()
    // precondition (external emitters already stopped) — a hub destroyed
    // while another thread is still emitting into it is still a bug; this
    // only prevents that bug from becoming a callback-into-freed-memory.
    ~Hub() { close(); }

    Hub(const Hub&) = delete;
    Hub& operator=(const Hub&) = delete;

    // -------------------------------------------------------------
    // Subscribe. Duplicate name for event type E is rejected in EVERY
    // build (S I.5 rule 2, S II.4 item 7): must_subscribe never inserts
    // the duplicate and routes the condition to the process fail-fast
    // handler (registration_error.h); try_subscribe returns
    // RegistrationError::DuplicateName without mutating the registry.
    //
    // file/line default to the call site via GCC's __builtin_FILE/LINE
    // (a GNU extension available on this project's g++ toolchain; C++17
    // has no std::source_location) so registry_snapshot() can report a
    // subscribe site without a macro at every call site.
    // -------------------------------------------------------------
    template <class E>
    [[nodiscard]] Subscription must_subscribe(const char* name, immediate_t, Callback<E> cb,
                                               phase ph = phase::Default, int priority = 0,
                                               const char* file = __builtin_FILE(),
                                               int line = __builtin_LINE()) {
        auto res = subscribe_generic(type_state<E>(), name, /*is_immediate=*/true, lane{}, ph,
                                      priority, file, line, make_invoke<E>(std::move(cb)));
        if (res.duplicate) {
            fail_fast_duplicate<E>(name);
            return Subscription{};
        }
        return std::move(res.sub);
    }

    template <class E>
    [[nodiscard]] Subscription must_subscribe(const char* name, lane ln, Callback<E> cb,
                                               phase ph = phase::Default, int priority = 0,
                                               const char* file = __builtin_FILE(),
                                               int line = __builtin_LINE()) {
        auto res = subscribe_generic(type_state<E>(), name, /*is_immediate=*/false, ln, ph, priority,
                                      file, line, make_invoke<E>(std::move(cb)));
        if (res.duplicate) {
            fail_fast_duplicate<E>(name);
            return Subscription{};
        }
        return std::move(res.sub);
    }

    template <class E>
    [[nodiscard]] std::variant<Subscription, RegistrationError> try_subscribe(
        const char* name, immediate_t, Callback<E> cb, phase ph = phase::Default, int priority = 0,
        const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
        auto res = subscribe_generic(type_state<E>(), name, /*is_immediate=*/true, lane{}, ph,
                                      priority, file, line, make_invoke<E>(std::move(cb)));
        if (res.duplicate) return RegistrationError::DuplicateName;
        return std::variant<Subscription, RegistrationError>(std::move(res.sub));
    }

    template <class E>
    [[nodiscard]] std::variant<Subscription, RegistrationError> try_subscribe(
        const char* name, lane ln, Callback<E> cb, phase ph = phase::Default, int priority = 0,
        const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
        auto res = subscribe_generic(type_state<E>(), name, /*is_immediate=*/false, ln, ph, priority,
                                      file, line, make_invoke<E>(std::move(cb)));
        if (res.duplicate) return RegistrationError::DuplicateName;
        return std::variant<Subscription, RegistrationError>(std::move(res.sub));
    }

    // -------------------------------------------------------------
    // Emit (S I.6). Thread-safe from any thread. Walks immediate
    // subscribers inline in (phase,priority,name) order on the caller's
    // thread; for each distinct lane among the type's subscribers,
    // enqueues exactly one copied envelope.
    // -------------------------------------------------------------
    template <class E>
    void emit(E ev) {
        if (closed_.load(std::memory_order_acquire)) return;

        int depth = ++detail::emit_depth();
        DepthGuard depth_guard;
        if (depth > kMaxEmitDepth) {
            fail_fast("evt::Hub::emit: handler-emitted-event nesting depth exceeded (cap=8)");
            return;
        }

        detail::TypeState& ts = type_state<E>();

        // Snapshot the live subscriber list under the type lock; dispatch
        // walks this copy so subscribe/unsubscribe from within a handler
        // never mutates the list we are iterating — takes effect after
        // this dispatch (S I.6: "snapshot walk, never UB").
        std::vector<detail::SubscriberRecord> snapshot;
        {
            std::lock_guard<std::mutex> lk(ts.mu);
            snapshot = ts.subscribers;
        }

        const bool tracing = trace_enabled_.load(std::memory_order_relaxed);
        TraceRecord trace_rec;
        if (tracing) {
            trace_rec.event_name = E::mt_event_name;
            trace_rec.thread = std::this_thread::get_id();
            trace_rec.emit_time = std::chrono::steady_clock::now();
            trace_rec.delivery_time = trace_rec.emit_time;
            trace_rec.nesting_depth = depth - 1;
        }

        std::vector<lane> lanes_needed;
        for (auto& rec : snapshot) {
            if (rec.is_immediate) {
                // Active-bit check immediately before entry (S I.6): a
                // handler removed before its turn (by an earlier handler
                // in this same walk) is skipped.
                if (!rec.block->active.load(std::memory_order_acquire)) continue;
                rec.block->in_flight.fetch_add(1, std::memory_order_acq_rel);
                double handler_ms = 0.0;
                {
                    detail::ScopedDispatch guard(rec.block.get());
                    auto t0 = std::chrono::steady_clock::now();
                    rec.invoke(&ev);
                    auto t1 = std::chrono::steady_clock::now();
                    handler_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                }
                rec.block->delivery_count.fetch_add(1, std::memory_order_relaxed);
                rec.block->in_flight.fetch_sub(1, std::memory_order_acq_rel);
                if (tracing) {
                    trace_rec.subscribers.push_back(
                        TraceSubscriberRecord{rec.name, true, 0, handler_ms, depth});
                }
            } else {
                bool have = false;
                for (const lane& l : lanes_needed) {
                    if (l == rec.ln) {
                        have = true;
                        break;
                    }
                }
                if (!have) lanes_needed.push_back(rec.ln);
            }
        }

        for (lane ln : lanes_needed) {
            Channel<detail::QueuedEnvelope>* ch = get_or_create_lane_channel(ln);
            detail::QueuedEnvelope env;
            env.type_id = E::mt_event_type_id();
            env.event_name = E::mt_event_name;
            env.lane_id = ln.id;
            env.sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
            env.event = std::static_pointer_cast<void>(std::make_shared<E>(ev));
            env.enqueue_time = std::chrono::steady_clock::now();
            // Bounded DropOldest lane (S I.6): drops are counted
            // out-of-band on the Channel itself (dropped()/rejected()),
            // never re-emitted onto the overflowing lane.
            ch->push(std::move(env));
        }

        if (tracing) {
            std::lock_guard<std::mutex> lk(trace_mu_);
            push_trace_record_locked(std::move(trace_rec));
        }
    }

    // -------------------------------------------------------------
    // Queued delivery drain. Owner thread only (claim_lane first);
    // pumping from a non-owner thread debug-aborts (S I.6), mirroring
    // Channel<T>'s own single-consumer guard.
    // -------------------------------------------------------------
    int pump(lane ln, double ms_budget);

    // Registers the calling thread as lane `ln`'s owner.
    void claim_lane(lane ln);

    // Invalidates every handle, cancels queued delivery, waits for
    // in-flight callbacks. Precondition: external emitters already
    // stopped (S I.5). Two-phase: mark inactive, then wait for in-flight
    // to hit zero (mirrors SubscriptionSet::unsubscribe_all_and_wait).
    // Idempotent.
    void close();

    // -------------------------------------------------------------
    // Observability (S I.8).
    // -------------------------------------------------------------
    std::vector<RegistrySnapshotEntry> registry_snapshot() const;

    void set_trace_enabled(bool on) { trace_enabled_.store(on, std::memory_order_relaxed); }
    bool trace_enabled() const { return trace_enabled_.load(std::memory_order_relaxed); }
    std::vector<TraceRecord> trace_snapshot() const;

private:
    struct DepthGuard {
        ~DepthGuard() { --detail::emit_depth(); }
    };

    struct SubscribeResult {
        Subscription sub;
        bool duplicate = false;
    };

    static constexpr int kMaxEmitDepth = 8;

    template <class E>
    static std::function<void(const void*)> make_invoke(Callback<E> cb) {
        return [cb = std::move(cb)](const void* p) { cb(*static_cast<const E*>(p)); };
    }

    template <class E>
    void fail_fast_duplicate(const char* name) {
        std::string msg = std::string("evt::Hub::must_subscribe: duplicate subscription name '") +
                           name + "' for event type '" + E::mt_event_name + "'";
        fail_fast(msg.c_str());
    }

    template <class E>
    detail::TypeState& type_state() {
        const void* id = E::mt_event_type_id();
        std::lock_guard<std::mutex> lk(types_mu_);
        auto it = types_.find(id);
        if (it != types_.end()) return *it->second;
        auto ts = std::make_unique<detail::TypeState>();
        ts->event_type_name = E::mt_event_name;
        detail::TypeState* p = ts.get();
        types_.emplace(id, std::move(ts));
        return *p;
    }

    SubscribeResult subscribe_generic(detail::TypeState& ts, const char* name, bool is_immediate,
                                       lane ln, phase ph, int priority, const char* file, int line,
                                       std::function<void(const void*)> invoke);

    Channel<detail::QueuedEnvelope>* get_or_create_lane_channel(lane ln);
    void dispatch_envelope(detail::QueuedEnvelope& env, lane ln);
    void push_trace_record_locked(TraceRecord rec);
    void check_lane_owner(lane ln);

    mutable std::mutex types_mu_;
    std::unordered_map<const void*, std::unique_ptr<detail::TypeState>> types_;

    std::mutex lanes_mu_;
    std::unordered_map<uint32_t, std::unique_ptr<Channel<detail::QueuedEnvelope>>> lane_channels_;
    std::unordered_map<uint32_t, std::thread::id> lane_owners_;

    std::atomic<uint64_t> sequence_{0};
    std::atomic<bool> closed_{false};

    std::atomic<bool> trace_enabled_{false};
    mutable std::mutex trace_mu_;
    std::deque<TraceRecord> trace_ring_;
    size_t trace_capacity_;
};

}  // namespace matter::evt
