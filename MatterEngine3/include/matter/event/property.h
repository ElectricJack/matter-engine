// matter/event/property.h — evt::Property<T> + evt::PropertyScheduler, the
// observable-models / data-binding layer (E5a).
//
// Design authority: MatterEngine3/docs/event-system.md
//   S I.9   — observable models: prime-on-bind, dirty-coalescing (a burst of
//             N set()s -> one delivery per observer at flush, latest value
//             wins), equality suppression (set() with an unchanged value per
//             operator== does not dirty), set_now() must-see-every-step
//             escape hatch (trace-flagged), single-thread affinity (each
//             property belongs to a PropertyScheduler claimed+flushed by one
//             thread), non-templated PropertyScheduler so every Property<T>
//             shares one real flush registry with no templated-static
//             singleton, property destruction unregisters from the scheduler
//             (no retained raw pointer past destruction), and the edit-loop
//             rule: a set() from inside a property's own flush delivery is
//             applied but DEFERRED TO THE NEXT flush, with equality
//             suppression terminating ping-pong. The trace records property
//             flushes like events (`prop.<name>`).
//   S I.5 r3 — bind() returns an evt::Subscription (RAII logical unsubscribe);
//             the SubscriptionBlock control-block pattern (subscription.h) is
//             reused verbatim so a Subscription destroyed after the Property
//             is a safe no-op (the block outlives whichever side drops last).
//   S I.6    — the debug affinity-assert pattern (assert_gl_thread lineage);
//             here it routes through the same all-build evt::fail_fast seam as
//             the hub so the single-thread contract is testable without an
//             abort (tests install a recording handler).
//
// Scope (E5a): the Property/PropertyScheduler pair only. Engine-only,
// headless-testable. No scene/EditorModel binding here (that is E5b, under
// src/scene/, owned by a concurrent agent).
//
// Threading (S I.9): properties are single-threaded BY AFFINITY. The scheduler
// is claimed and flushed by exactly one thread; set/get/bind/set_now/flush all
// assert that thread. Cross-thread producers never touch a property directly —
// they emit a queued hub notification and an app-thread subscriber calls set().
// This keeps properties lock-free (no mutex on the hot path) and their
// callbacks reentrancy-simple.
//
// Movability: Property is NON-COPYABLE and NON-MOVABLE. It registers its own
// stable `this` with the scheduler and its observer control blocks are shared
// by outstanding Subscriptions; a move would leave those dangling. Hold a
// Property by stable address (a struct member, as the SelectionModel sketch in
// S I.9 does), never in a reallocating container by value.
//
// Trace hook (decoupled seam): the scheduler carries an optional TraceSink
// std::function, NOT a Hub pointer — wiring `prop.<name>` records into a hub
// trace would couple every Property TU to event_hub.h. Instead an owner that
// wants unified tracing installs a sink (set_trace_sink) that forwards to its
// hub. The sink fires once per property per flush delivery (immediate=false)
// and once per set_now (immediate=true), matching "the trace records property
// flushes like events" without the coupling.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "matter/event/registration_error.h"
#include "matter/event/subscription.h"

namespace matter::evt {

namespace detail {

// Type-erased handle the scheduler tracks in its dirty set and live registry
// (S I.9: "type-erased handles to dirty properties"). Property<T> derives from
// this so the non-templated scheduler can hold, coalesce, and flush every
// specialization through one registry.
struct PropertyBase {
    const char* prop_name = nullptr;
    // Membership flag in the scheduler's dirty list. This IS the coalescing
    // mechanism: mark_dirty is a no-op when already set, so a burst of N sets
    // enqueues the property exactly once. Reset by the scheduler BEFORE a
    // flush delivers, so a set() during delivery re-marks into the next batch.
    bool dirty = false;
    virtual ~PropertyBase() = default;
    // Deliver the property's latest value to its live observers exactly once.
    virtual void flush_deliver() = 0;
};

}  // namespace detail

// ---------------------------------------------------------------------------
// PropertyScheduler — non-templated (S I.9). Owns the dirty set (dedup /
// coalescing), the claimed owner thread + affinity assert, flush_dirty(), and
// property registration/deregistration. One instance is shared by every
// Property<T> the models under it use, giving one real flush registry with no
// templated-static singleton.
// ---------------------------------------------------------------------------
class PropertyScheduler {
public:
    // Called once per property per delivery. `immediate` is false for a flush
    // delivery, true for set_now. Install via set_trace_sink to forward to a
    // hub trace as `prop.<name>` records; left empty by default (zero cost).
    using TraceSink = std::function<void(const char* prop_name, bool immediate)>;

    PropertyScheduler() = default;
    ~PropertyScheduler();

    PropertyScheduler(const PropertyScheduler&) = delete;
    PropertyScheduler& operator=(const PropertyScheduler&) = delete;
    PropertyScheduler(PropertyScheduler&&) = delete;
    PropertyScheduler& operator=(PropertyScheduler&&) = delete;

    // Claim the calling thread as the owner. Idempotent for the same thread;
    // re-claiming from a different thread is a fail-fast configuration bug.
    void claim();
    void claim(std::thread::id owner);
    bool claimed() const { return owner_claimed_; }
    std::thread::id owner() const { return owner_; }

    // Deliver each currently-dirty property's latest value to its observers
    // once, then clear the dirty set. A set() from inside a delivery re-dirties
    // for the NEXT flush — this does NOT loop-until-clean (that is precisely
    // the edit-loop-termination rule, S I.9). Owner-thread only.
    void flush_dirty();

    void set_trace_sink(TraceSink sink) { trace_sink_ = std::move(sink); }
    bool has_trace_sink() const { return static_cast<bool>(trace_sink_); }

    // -- internal surface, called by Property<T> (public because the template
    //    lives in this header; not part of the intended user API) -----------
    void register_property(detail::PropertyBase* p);
    void unregister_property(detail::PropertyBase* p);
    void mark_dirty(detail::PropertyBase* p);
    void clear_dirty(detail::PropertyBase* p);
    void assert_owner(const char* op) const;
    void trace(const char* prop_name, bool immediate) const {
        if (trace_sink_) trace_sink_(prop_name, immediate);
    }

private:
    bool owner_claimed_ = false;
    std::thread::id owner_{};
    bool in_flush_ = false;
    // Coalesced dirty batch: unique (dedup via PropertyBase::dirty), FIFO by
    // first-dirty within a flush window for a deterministic delivery order.
    std::vector<detail::PropertyBase*> dirty_;
    // Live-property registry — dangling safety (S I.9: "the scheduler never
    // retains a raw pointer past destruction"). unregister purges from both.
    std::vector<detail::PropertyBase*> live_;
    TraceSink trace_sink_;
};

// ---------------------------------------------------------------------------
// Property<T> — a bindable value holder (S I.9). Constructed with a scheduler
// and a name; get() reads, set() coalesces, set_now() delivers immediately,
// bind() primes-then-subscribes.
//
// Requirements on T: EqualityComparable (operator==, for equality suppression)
// and CopyConstructible (deliver() snapshots the value so every observer in a
// flush sees one consistent value even if an earlier observer set()s a new one
// — the deferred delivery carries the newer value to the next flush).
// ---------------------------------------------------------------------------
template <class T>
class Property : public detail::PropertyBase {
public:
    Property(PropertyScheduler& scheduler, const char* name, T initial = T{})
        : scheduler_(scheduler), value_(std::move(initial)) {
        prop_name = name;
        scheduler_.register_property(this);
    }

    ~Property() override { scheduler_.unregister_property(this); }

    Property(const Property&) = delete;
    Property& operator=(const Property&) = delete;
    Property(Property&&) = delete;
    Property& operator=(Property&&) = delete;

    const T& get() const {
        scheduler_.assert_owner("Property::get");
        return value_;
    }

    // Coalesced mutation. Equality suppression (S I.9): an unchanged value per
    // operator== does not dirty, so a converging edit loop terminates. On a
    // real change the value is stored immediately (latest-wins) but delivery is
    // deferred to the next flush_dirty().
    void set(T v) {
        scheduler_.assert_owner("Property::set");
        if (value_ == v) return;
        value_ = std::move(v);
        scheduler_.mark_dirty(this);
    }

    // Opt-in must-see-every-step escape hatch (S I.9). Delivers immediately,
    // trace-flagged, WITHOUT equality suppression or coalescing; supersedes any
    // pending coalesced delivery so the value is not re-delivered at the next
    // flush.
    void set_now(T v) {
        scheduler_.assert_owner("Property::set_now");
        value_ = std::move(v);
        scheduler_.clear_dirty(this);
        deliver(/*immediate=*/true);
    }

    // Prime-on-bind (S I.9): the callback fires immediately with the current
    // value, then on every subsequent flush delivery. Returns an RAII
    // Subscription; destroying it stops future delivery (logical unsubscribe).
    [[nodiscard]] Subscription bind(const char* name, std::function<void(const T&)> cb) {
        scheduler_.assert_owner("Property::bind");
        auto block = std::make_shared<SubscriptionBlock>();
        // Prime with the current value before registering, so the initial
        // delivery is not itself subject to a concurrent flush walk.
        cb(value_);
        block->delivery_count.fetch_add(1, std::memory_order_relaxed);
        observers_.push_back(Observer{name ? name : "", block, std::move(cb)});
        return Subscription{block};
    }

    // detail::PropertyBase — called by the scheduler at flush time.
    void flush_deliver() override { deliver(/*immediate=*/false); }

private:
    struct Observer {
        std::string name;
        std::shared_ptr<SubscriptionBlock> block;
        std::function<void(const T&)> cb;
    };

    void deliver(bool immediate) {
        scheduler_.trace(prop_name, immediate);
        // Snapshot the value so every observer in THIS delivery sees one
        // consistent value; a set() from a callback mutates value_ but its
        // delivery is deferred to the next flush (edit-loop termination).
        const T snapshot_value = value_;
        // Snapshot live observers so a callback that binds/unsubscribes/sets
        // during delivery cannot invalidate the walk (mirrors Hub dispatch).
        // Compact away RAII-unsubscribed observers lazily first.
        prune_inactive();
        std::vector<Observer> snapshot = observers_;
        for (auto& o : snapshot) {
            if (!o.block->active.load(std::memory_order_acquire)) continue;
            o.cb(snapshot_value);
            o.block->delivery_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void prune_inactive() {
        // Only rebuild when something is actually dead — otherwise moving every
        // element into `live` and then NOT swapping would leave observers_ full
        // of moved-from (null-block) entries.
        bool any_dead = false;
        for (auto& o : observers_) {
            if (!o.block->active.load(std::memory_order_acquire)) {
                any_dead = true;
                break;
            }
        }
        if (!any_dead) return;
        std::vector<Observer> live;
        live.reserve(observers_.size());
        for (auto& o : observers_) {
            if (o.block->active.load(std::memory_order_acquire)) live.push_back(std::move(o));
        }
        observers_.swap(live);
    }

    PropertyScheduler& scheduler_;
    T value_;
    std::vector<Observer> observers_;
};

}  // namespace matter::evt
