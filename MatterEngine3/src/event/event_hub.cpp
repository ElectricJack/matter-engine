// src/event/event_hub.cpp — the non-template half of evt::Hub: subscribe
// bookkeeping, lane channel management, queued dispatch, close(),
// registry/trace snapshots.
//
// Design authority: MatterEngine3/docs/event-system.md S I.5-I.8. See
// matter/event/event_hub.h for the full contract; the template methods
// (must_subscribe/try_subscribe/emit, which need E at the call site) are
// defined inline there.
#include "matter/event/event_hub.h"

#include <cassert>
#include <chrono>
#include <thread>

namespace matter::evt {

Hub::SubscribeResult Hub::subscribe_generic(detail::TypeState& ts, const char* name,
                                             bool is_immediate, lane ln, phase ph, int priority,
                                             const char* file, int line,
                                             std::function<void(const void*)> invoke) {
    std::lock_guard<std::mutex> lk(ts.mu);

    // Names are unique per event type in EVERY build (S I.5 rule 2, S II.4
    // item 7) -- this check (and the early return that skips the insert)
    // is a plain runtime branch, not `assert`, so it holds in a release
    // (NDEBUG) build too.
    for (auto& rec : ts.subscribers) {
        if (rec.name == name) return SubscribeResult{Subscription{}, /*duplicate=*/true};
    }

    auto block = std::make_shared<SubscriptionBlock>();
    detail::SubscriberRecord rec;
    rec.name = name;
    rec.ph = ph;
    rec.priority = priority;
    rec.is_immediate = is_immediate;
    rec.ln = ln;
    rec.file = file;
    rec.line = line;
    rec.block = block;
    rec.invoke = std::move(invoke);

    // Insert keeping the list sorted by (phase, priority, name) -- the S
    // I.7 total order. Subscriber counts per event type are small (tens,
    // not thousands), so a linear insertion-sort is fine; correctness
    // first (S I.2 non-goals: no lock-free/wait-free heroics).
    auto pos =
        std::upper_bound(ts.subscribers.begin(), ts.subscribers.end(), rec, detail::SubscriberOrder{});
    ts.subscribers.insert(pos, std::move(rec));

    return SubscribeResult{Subscription(block), /*duplicate=*/false};
}

Channel<detail::QueuedEnvelope>* Hub::get_or_create_lane_channel(lane ln) {
    std::lock_guard<std::mutex> lk(lanes_mu_);
    auto it = lane_channels_.find(ln.id);
    if (it != lane_channels_.end()) return it->second.get();
    // Bounded 4096, DropOldest (S I.6: "Bounded by default (4096,
    // matching today), but never silently" -- drops are counted
    // out-of-band via Channel::dropped()/rejected(), read by
    // registry_snapshot()/the future inspector, never re-emitted onto the
    // overflowing lane).
    auto ch = std::make_unique<Channel<detail::QueuedEnvelope>>(
        Channel<detail::QueuedEnvelope>::Policy{4096, OnFull::DropOldest});
    Channel<detail::QueuedEnvelope>* p = ch.get();
    lane_channels_.emplace(ln.id, std::move(ch));
    return p;
}

void Hub::claim_lane(lane ln) {
    get_or_create_lane_channel(ln);
    std::lock_guard<std::mutex> lk(lanes_mu_);
    lane_owners_[ln.id] = std::this_thread::get_id();
}

void Hub::check_lane_owner(lane ln) {
#ifndef NDEBUG
    std::lock_guard<std::mutex> lk(lanes_mu_);
    auto it = lane_owners_.find(ln.id);
    assert(it != lane_owners_.end() &&
           "evt::Hub::pump: lane was never claimed -- call claim_lane() from the owning thread "
           "before pump()");
    assert(it->second == std::this_thread::get_id() &&
           "evt::Hub::pump: called from a thread that did not claim this lane (S I.6)");
#else
    (void)ln;
#endif
}

void Hub::dispatch_envelope(detail::QueuedEnvelope& env, lane ln) {
    detail::TypeState* ts = nullptr;
    {
        std::lock_guard<std::mutex> lk(types_mu_);
        auto it = types_.find(env.type_id);
        if (it != types_.end()) ts = it->second.get();
    }
    if (!ts) return;  // no subscribers were ever registered for this type

    // Snapshot this event type's currently active lane subscribers for
    // `ln` (S I.6: "the hub snapshots that event type's currently active
    // lane subscribers" at pump time -- resolving the LIVE registry here,
    // not at emit time, is what lets logical unsubscribe-before-pump
    // suppress delivery).
    std::vector<detail::SubscriberRecord> snapshot;
    {
        std::lock_guard<std::mutex> lk(ts->mu);
        for (auto& rec : ts->subscribers) {
            if (!rec.is_immediate && rec.ln == ln) snapshot.push_back(rec);
        }
    }

    const bool tracing = trace_enabled_.load(std::memory_order_relaxed);
    TraceRecord trace_rec;
    if (tracing) {
        trace_rec.event_name = env.event_name ? env.event_name : "<unknown>";
        trace_rec.thread = std::this_thread::get_id();
        trace_rec.emit_time = env.enqueue_time;
        trace_rec.delivery_time = std::chrono::steady_clock::now();
    }

    int depth = ++detail::emit_depth();
    struct Guard {
        ~Guard() { --detail::emit_depth(); }
    } depth_guard;
    if (depth > kMaxEmitDepth) {
        fail_fast("evt::Hub::pump: handler-emitted-event nesting depth exceeded (cap=8)");
        return;
    }
    if (tracing) trace_rec.nesting_depth = depth - 1;

    for (auto& rec : snapshot) {
        // Lifetime barrier (S I.6, S II.4 item 1): bump in_flight FIRST (in
        // the guard's ctor), THEN check the active bit -- mirrors the
        // immediate-dispatch site in event_hub.h. Teardown stores
        // active=false and then waits for in_flight to reach zero;
        // incrementing before the seq_cst active load closes the
        // store-load double-miss so a callback that passes the active check
        // cannot run after unsubscribe_all_and_wait()/close() returns. The
        // guard decrements on EVERY exit path incl. skip-continue and throw.
        struct InFlightGuard {
            SubscriptionBlock* b;
            explicit InFlightGuard(SubscriptionBlock* blk) : b(blk) {
                b->in_flight.fetch_add(1, std::memory_order_seq_cst);
            }
            ~InFlightGuard() { b->in_flight.fetch_sub(1, std::memory_order_acq_rel); }
            InFlightGuard(const InFlightGuard&) = delete;
            InFlightGuard& operator=(const InFlightGuard&) = delete;
        } in_flight_guard(rec.block.get());
        // Test seam: park the dispatcher in the increment->active-load window
        // (null/no-op in production).
        if (detail::g_dispatch_barrier_probe) detail::g_dispatch_barrier_probe(rec.block.get());
        // Active-bit check immediately before entry (S I.6); the guard
        // above still decrements the count on continue.
        if (!rec.block->active.load(std::memory_order_seq_cst)) continue;
        double handler_ms = 0.0;
        {
            detail::ScopedDispatch dg(rec.block.get());
            auto t0 = std::chrono::steady_clock::now();
            rec.invoke(env.event.get());
            auto t1 = std::chrono::steady_clock::now();
            handler_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        }
        rec.block->delivery_count.fetch_add(1, std::memory_order_relaxed);
        if (tracing) {
            trace_rec.subscribers.push_back(
                TraceSubscriberRecord{rec.name, false, rec.ln.id, handler_ms, depth});
        }
    }

    if (tracing) {
        std::lock_guard<std::mutex> lk(trace_mu_);
        push_trace_record_locked(std::move(trace_rec));
    }
}

int Hub::pump(lane ln, double ms_budget) {
    check_lane_owner(ln);
    Channel<detail::QueuedEnvelope>* ch = get_or_create_lane_channel(ln);

    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    int total = 0;
    while (true) {
        detail::QueuedEnvelope env;
        int ran = ch->pump_one([&](detail::QueuedEnvelope& e) { env = std::move(e); });
        if (ran == 0) break;  // lane empty
        dispatch_envelope(env, ln);
        ++total;
        double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        // Progress guarantee: the first delivery above always happens
        // regardless of budget (mirrors Channel<T>::pump's min-one rule);
        // this only gates further iterations.
        if (elapsed >= ms_budget) break;
    }
    return total;
}

int Hub::pump_one(lane ln) {
    check_lane_owner(ln);
    Channel<detail::QueuedEnvelope>* ch = get_or_create_lane_channel(ln);
    detail::QueuedEnvelope env;
    int ran = ch->pump_one([&](detail::QueuedEnvelope& e) { env = std::move(e); });
    if (ran == 0) return 0;  // lane empty
    dispatch_envelope(env, ln);
    return 1;
}

void Hub::close() {
    if (closed_.exchange(true, std::memory_order_acq_rel)) return;  // idempotent

    // Phase 1: mark every subscription inactive so no NEW dispatch begins.
    std::vector<std::shared_ptr<SubscriptionBlock>> blocks;
    {
        std::lock_guard<std::mutex> lk(types_mu_);
        for (auto& kv : types_) {
            detail::TypeState& ts = *kv.second;
            std::lock_guard<std::mutex> lk2(ts.mu);
            for (auto& rec : ts.subscribers) {
                // seq_cst pairs with the seq_cst in_flight increment + active
                // load at the dispatch sites: store-then-wait here can never
                // miss an increment that a dispatcher is about to make.
                rec.block->active.store(false, std::memory_order_seq_cst);
                blocks.push_back(rec.block);
            }
        }
    }

    // Cancel queued delivery: shutting down every lane channel drops
    // whatever is still queued (Channel<T>::shut_down semantics) rather
    // than dispatching it.
    {
        std::lock_guard<std::mutex> lk(lanes_mu_);
        for (auto& kv : lane_channels_) kv.second->shut_down();
    }

    // Phase 2: wait for in-flight callbacks (already executing elsewhere
    // when phase 1 ran) to finish.
    for (auto& b : blocks) {
        while (b->in_flight.load(std::memory_order_seq_cst) != 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
}

std::vector<RegistrySnapshotEntry> Hub::registry_snapshot() const {
    std::vector<RegistrySnapshotEntry> out;
    std::lock_guard<std::mutex> lk(types_mu_);
    out.reserve(types_.size());
    for (auto& kv : types_) {
        detail::TypeState& ts = *kv.second;
        RegistrySnapshotEntry entry;
        entry.event_name = ts.event_type_name;
        std::lock_guard<std::mutex> lk2(ts.mu);
        entry.subscribers.reserve(ts.subscribers.size());
        for (auto& rec : ts.subscribers) {
            SubscriberInfo info;
            info.name = rec.name;
            info.ph = rec.ph;
            info.priority = rec.priority;
            info.is_immediate = rec.is_immediate;
            info.lane_id = rec.ln.id;
            info.file = rec.file;
            info.line = rec.line;
            info.delivery_count = rec.block->delivery_count.load(std::memory_order_relaxed);
            info.active = rec.block->active.load(std::memory_order_relaxed);
            entry.subscribers.push_back(std::move(info));
        }
        out.push_back(std::move(entry));
    }
    return out;
}

void Hub::push_trace_record_locked(TraceRecord rec) {
    trace_ring_.push_back(std::move(rec));
    while (trace_ring_.size() > trace_capacity_) trace_ring_.pop_front();
}

std::vector<TraceRecord> Hub::trace_snapshot() const {
    std::lock_guard<std::mutex> lk(trace_mu_);
    return std::vector<TraceRecord>(trace_ring_.begin(), trace_ring_.end());
}

}  // namespace matter::evt
