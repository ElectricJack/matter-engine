// src/event/command.cpp — the non-template half of evt::CommandRegistry:
// construction/default sink, handler registration bookkeeping, scope
// epochs, lane channels + pump, continuation delivery, shut_down, and the
// single finalize_common path shared by execute() and dispatch().
//
// Design authority: MatterEngine3/docs/event-system.md S I.10 (and S II.4
// items 2 and 5). See matter/event/command.h for the full contract; the
// template methods (execute/dispatch/must_register_handler, which need C
// at the call site) are defined inline there.
#include "matter/event/command.h"

#include <algorithm>
#include <cassert>
#include <chrono>

#include "matter/event/event_hub.h"  // Hub::emit for the cmd.* notifications

namespace matter::evt {

const char* to_string(CommandStatus s) {
    switch (s) {
        case CommandStatus::Pending: return "Pending";
        case CommandStatus::Success: return "Success";
        case CommandStatus::HandlerFailed: return "HandlerFailed";
        case CommandStatus::Rejected: return "Rejected";
        case CommandStatus::QueueFull: return "QueueFull";
        case CommandStatus::ShutDown: return "ShutDown";
        case CommandStatus::Superseded: return "Superseded";
        case CommandStatus::StaleScope: return "StaleScope";
    }
    return "<unknown>";
}

namespace {
// Process-wide default sink (S I.10: "Install a NullHistorySink by
// default"). A no-op; the later world-scoped UndoStack replaces it via
// set_history_sink.
NullHistorySink& default_history_sink() {
    static NullHistorySink sink;
    return sink;
}
}  // namespace

CommandRegistry::CommandRegistry(Hub& hub) : hub_(hub) {
    history_sink_ = &default_history_sink();
}

void CommandRegistry::set_history_sink(HistorySink* sink) {
    std::lock_guard<std::mutex> lk(handlers_mu_);  // reuse a lock; sink swaps are rare
    history_sink_ = sink ? sink : &default_history_sink();
}

// ---------------------------------------------------------------------
// Scope epochs (S II.4 item 5).
// ---------------------------------------------------------------------
void CommandRegistry::set_active_scope(CommandScopeToken token) {
    std::lock_guard<std::mutex> lk(scope_mu_);
    active_scope_open_ = true;
    active_token_ = token;
    active_token_.kind = CommandScope::ActiveSession;
}

void CommandRegistry::close_active_scope() {
    std::lock_guard<std::mutex> lk(scope_mu_);
    active_scope_open_ = false;
}

CommandScopeToken CommandRegistry::active_scope() const {
    std::lock_guard<std::mutex> lk(scope_mu_);
    return active_token_;
}

bool CommandRegistry::active_scope_open() const {
    std::lock_guard<std::mutex> lk(scope_mu_);
    return active_scope_open_;
}

CommandScopeToken CommandRegistry::stamp_scope(CommandScope scope) const {
    if (scope == CommandScope::App) return CommandScopeToken{CommandScope::App, 0, 0};
    std::lock_guard<std::mutex> lk(scope_mu_);
    // Stamp with the current epoch (even if closed: a command stamped while
    // no epoch is open is inherently stale and will read StaleScope at run).
    CommandScopeToken t = active_token_;
    t.kind = CommandScope::ActiveSession;
    return t;
}

bool CommandRegistry::scope_valid(const CommandScopeToken& stamped) const {
    std::lock_guard<std::mutex> lk(scope_mu_);
    // Valid iff an epoch is currently open AND it is the same session and
    // generation the command was stamped under. A close or a rotation to a
    // new generation both invalidate it -> StaleScope, handler never runs.
    return active_scope_open_ && active_token_.session_id == stamped.session_id &&
           active_token_.generation == stamped.generation;
}

// ---------------------------------------------------------------------
// Registration (all-build unique handler per command name).
// ---------------------------------------------------------------------
CommandRegistry::RegisterResult CommandRegistry::register_generic(const void* type_id,
                                                                  const char* name,
                                                                  CommandScope scope, lane ln,
                                                                  std::shared_ptr<void> handler) {
    std::lock_guard<std::mutex> lk(handlers_mu_);
    auto it = handlers_.find(type_id);
    if (it != handlers_.end() && it->second.block &&
        it->second.block->active.load(std::memory_order_acquire)) {
        // A LIVE handler already owns this command name. Reject without
        // mutating (S I.10 / S II.4 item 7). This branch is a plain runtime
        // check -- not assert/NDEBUG-gated -- so it fires identically in a
        // release build; that is what makes the uniqueness "all-build".
        return RegisterResult{Registration{}, /*duplicate=*/true};
    }

    auto block = std::make_shared<RegistrationBlock>();
    HandlerRecord rec;
    rec.scope = scope;
    rec.ln = ln;
    rec.handler = std::move(handler);
    rec.block = block;
    rec.name = name;
    handlers_[type_id] = std::move(rec);  // inserts, or re-binds a stale (unregistered) slot
    return RegisterResult{Registration(block), /*duplicate=*/false};
}

CommandRegistry::HandlerLookup CommandRegistry::lookup(const void* type_id) const {
    std::lock_guard<std::mutex> lk(handlers_mu_);
    auto it = handlers_.find(type_id);
    if (it == handlers_.end()) return HandlerLookup{};
    const HandlerRecord& rec = it->second;
    if (!rec.block || !rec.block->active.load(std::memory_order_acquire)) return HandlerLookup{};
    HandlerLookup out;
    out.found = true;
    out.scope = rec.scope;
    out.ln = rec.ln;
    out.handler = rec.handler;
    out.block = rec.block;
    return out;
}

// ---------------------------------------------------------------------
// Lane channels + pump. A command lane owns two Channels of type-erased
// closures: a bounded RejectNewest command channel (non-dropping by
// contract -- rejection completes the ticket QueueFull, S I.5) and an
// unbounded continuation channel (then()-callbacks are never lost).
// ---------------------------------------------------------------------
CommandRegistry::LaneState& CommandRegistry::get_or_create_lane(lane ln) {
    // Caller holds lanes_mu_.
    auto it = lanes_.find(ln.id);
    if (it != lanes_.end()) return it->second;
    LaneState st;
    st.commands = std::make_unique<Channel<std::function<void()>>>(
        Channel<std::function<void()>>::Policy{kCommandChannelCapacity, OnFull::RejectNewest});
    st.continuations = std::make_unique<Channel<std::function<void()>>>(
        Channel<std::function<void()>>::Policy{0, OnFull::RejectNewest});  // 0 = unbounded
    auto res = lanes_.emplace(ln.id, std::move(st));
    return res.first->second;
}

void CommandRegistry::claim_lane(lane ln) {
    std::lock_guard<std::mutex> lk(lanes_mu_);
    LaneState& st = get_or_create_lane(ln);
    st.owner = std::this_thread::get_id();
    st.owner_set = true;
}

bool CommandRegistry::is_lane_owner_current_thread(lane ln) const {
    std::lock_guard<std::mutex> lk(lanes_mu_);
    auto it = lanes_.find(ln.id);
    if (it == lanes_.end() || !it->second.owner_set) return false;
    return it->second.owner == std::this_thread::get_id();
}

void CommandRegistry::assert_execute_lane(lane ln) const {
#ifndef NDEBUG
    std::lock_guard<std::mutex> lk(lanes_mu_);
    auto it = lanes_.find(ln.id);
    assert(it != lanes_.end() && it->second.owner_set &&
           "evt::CommandRegistry::execute: handler lane was never claimed -- call claim_lane() "
           "from the owning thread before execute()");
    assert(it->second.owner == std::this_thread::get_id() &&
           "evt::CommandRegistry::execute: called off the registered handler's lane (S I.10)");
#else
    (void)ln;
#endif
}

PushResult CommandRegistry::post_command(lane ln, std::function<void()> job) {
    Channel<std::function<void()>>* ch = nullptr;
    {
        std::lock_guard<std::mutex> lk(lanes_mu_);
        ch = get_or_create_lane(ln).commands.get();
    }
    return ch->push(std::move(job));
}

void CommandRegistry::post_continuation(lane ln, std::function<void()> cont) {
    Channel<std::function<void()>>* ch = nullptr;
    {
        std::lock_guard<std::mutex> lk(lanes_mu_);
        ch = get_or_create_lane(ln).continuations.get();
    }
    ch->push(std::move(cont));
}

int CommandRegistry::pump(lane ln, double ms_budget) {
    Channel<std::function<void()>>* cmds = nullptr;
    Channel<std::function<void()>>* conts = nullptr;
    {
        std::lock_guard<std::mutex> lk(lanes_mu_);
        LaneState& st = get_or_create_lane(ln);
#ifndef NDEBUG
        assert(st.owner_set && st.owner == std::this_thread::get_id() &&
               "evt::CommandRegistry::pump: called from a thread that did not claim this lane "
               "(S I.6/I.10)");
#endif
        cmds = st.commands.get();
        conts = st.continuations.get();
    }

    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    int total = 0;

    // Commands first (each runs its handler + finalizes its ticket), then
    // any continuations those completions posted. Budget gates further
    // iterations but the first delivery always happens (progress guarantee).
    total += cmds->pump(ms_budget, [](std::function<void()>& job) {
        if (job) job();
    });
    double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    double remaining = ms_budget - elapsed;
    if (remaining < 0.0) remaining = 0.0;
    total += conts->pump(remaining, [](std::function<void()>& cont) {
        if (cont) cont();
    });
    return total;
}

// ---------------------------------------------------------------------
// Ticket tracking + shutdown. Every live ticket is weak-tracked so
// shut_down() can fail the still-pending ones ShutDown -- a deliver-once
// caller is never left unable to tell whether the command vanished
// (S I.10).
// ---------------------------------------------------------------------
void CommandRegistry::track_ticket(const std::shared_ptr<TicketStateBase>& state) {
    std::lock_guard<std::mutex> lk(tickets_mu_);
    // Opportunistic prune of expired weak refs so the list cannot grow
    // without bound across a long-lived registry.
    if (live_tickets_.size() >= 64) {
        live_tickets_.erase(
            std::remove_if(live_tickets_.begin(), live_tickets_.end(),
                           [](const std::weak_ptr<TicketStateBase>& w) { return w.expired(); }),
            live_tickets_.end());
    }
    live_tickets_.push_back(state);
}

void CommandRegistry::shut_down() {
    if (shutdown_.exchange(true, std::memory_order_acq_rel)) return;  // idempotent

    // Shut the lane channels: reject future pushes and drop whatever is
    // still queued (the dropped command jobs would otherwise leave tickets
    // pending -- handled by the live-ticket sweep below).
    {
        std::lock_guard<std::mutex> lk(lanes_mu_);
        for (auto& kv : lanes_) {
            if (kv.second.commands) kv.second.commands->shut_down();
            if (kv.second.continuations) kv.second.continuations->shut_down();
        }
    }

    // Fail every still-pending ticket ShutDown. finalize_terminal is
    // idempotent (the ticket's atomic gate), so a ticket that already
    // completed -- or one whose pumped job races us -- is completed exactly
    // once regardless.
    std::vector<std::weak_ptr<TicketStateBase>> snapshot;
    {
        std::lock_guard<std::mutex> lk(tickets_mu_);
        snapshot.swap(live_tickets_);
    }
    for (auto& w : snapshot) {
        if (auto s = w.lock()) s->finalize_terminal(CommandStatus::ShutDown);
    }
}

// ---------------------------------------------------------------------
// The one finalization path (S I.10): both execute() and the dispatch
// ticket funnel here. Records trace timing, hands inverse/coalesce
// metadata to the HistorySink (executed commands only -- non-run terminal
// causes never produce an undo entry), and emits the generic cmd.completed
// / cmd.failed notification carrying only id/name/status/duration/scope.
// ---------------------------------------------------------------------
void CommandRegistry::finalize_common(uint64_t id, const char* name, CommandStatus status,
                                      double duration_ms, const CommandScopeToken& scope,
                                      std::optional<uint64_t> coalesce_key,
                                      std::shared_ptr<void> inverse, bool executed,
                                      const std::string& error) {
    if (executed) {
        HistorySink* sink = nullptr;
        {
            std::lock_guard<std::mutex> lk(handlers_mu_);
            sink = history_sink_;
        }
        if (sink) {
            CommandRecord rec;
            rec.id = id;
            rec.name = name;
            rec.status = status;
            rec.duration_ms = duration_ms;
            rec.scope = scope;
            rec.coalesce_key = coalesce_key;
            rec.inverse = std::move(inverse);
            sink->on_command_completed(rec);
        }
    }

    if (status == CommandStatus::Success) {
        CommandCompleted ev;
        ev.id = id;
        ev.name = name;
        ev.status = status;
        ev.duration_ms = duration_ms;
        ev.scope = scope;
        hub_.emit(ev);
    } else {
        CommandFailed ev;
        ev.id = id;
        ev.name = name;
        ev.status = status;
        ev.duration_ms = duration_ms;
        ev.scope = scope;
        ev.error = error;
        hub_.emit(ev);
    }
}

}  // namespace matter::evt
