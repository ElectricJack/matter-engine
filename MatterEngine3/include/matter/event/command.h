// matter/event/command.h — evt::CommandRegistry, the deliver-once,
// single-handler command layer built on the E1 evt core (E4a).
//
// Design authority: MatterEngine3/docs/event-system.md
//   S I.10 — THE command-layer design: CommandResult, execute vs
//            dispatch + CommandTicket, must_register_handler /
//            try_register_handler, CommandScopeToken{App|ActiveSession},
//            result finalization / HistorySink / NullHistorySink,
//            cmd.completed / cmd.failed notifications, MT_COMMAND_NAME,
//            coalesce_key, inverse-captured-at-execute-time.
//   S II.4 item 2 — typed results + correlated ticket.
//   S II.4 item 5 — scope epochs: an ActiveSession command stamped at
//            submit-time completes StaleScope if the epoch closed/rotated
//            before it ran, so entity ids can never drift across a world
//            switch.
//   S I.5/I.6 — commands SHARE the transport (evt::Channel) and the
//            fail-fast / RAII discipline of the notification core.
//   S I.13   — SessionBinding (E4b) will USE the scope tokens this header
//            exposes (set_active_scope / close_active_scope). E4a builds
//            only the registry side; no SessionBinding, no viewer wiring.
//
// Scope (E4a, per the milestone brief): the general command registry,
// engine-only, headless-testable. No real viewer commands are wired here
// (that is E4b). The bake matter_async::CommandQueue is NOT reimplemented
// here — S I.10 records it as a command channel specialized for the bake
// worker; facade-level commands route to it in E4b.
//
// Relationship to the hub: a CommandRegistry is constructed over an
// evt::Hub. Every finalized command emits a generic cmd.completed /
// cmd.failed notification on that hub (id / name / status / duration /
// scope) so journaling and inspection observe command flow as ordinary
// subscribers, without reconstructing typed results (S I.10 "typed
// completion plus generic observability").
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "matter/event/channel.h"
#include "matter/event/event_name.h"  // MT_EVENT_NAME for the cmd.* notifications
#include "matter/event/registration_error.h"
#include "matter/event/subscription.h"  // evt::lane, evt::make_lane, evt::fail_fast

namespace matter::evt {

class Hub;  // command.cpp includes event_hub.h; the header only needs the fwd decl.

// ---------------------------------------------------------------------
// MT_COMMAND_NAME — the compile-time command descriptor, mirroring
// MT_EVENT_NAME (event_name.h): a static constexpr dotted name plus a
// stable, RTTI-free per-type id (the address of a function-local static
// byte, unique per struct that expands the macro, stable for the process,
// ODR-safe across TUs). The dotted string is the trace / journal /
// registry key; the id keys the handler map.
// ---------------------------------------------------------------------
#define MT_COMMAND_NAME(name_literal)                                \
    static constexpr const char* mt_command_name = (name_literal);   \
    static const void* mt_command_type_id() {                        \
        static const char marker = 0;                                \
        return static_cast<const void*>(&marker);                    \
    }                                                                 \
    static_assert(true, "MT_COMMAND_NAME requires a trailing semicolon")

// ---------------------------------------------------------------------
// Terminal command status (S I.10). Every dispatched command reaches
// EXACTLY ONE of these terminal states; Pending is the pre-terminal
// value a ticket reads before completion. Success/HandlerFailed mean the
// single handler ran; the rest mean it did not.
// ---------------------------------------------------------------------
enum class CommandStatus {
    Pending,        // not yet terminal (ticket-only, never stored in a result)
    Success,        // handler ran and returned ok
    HandlerFailed,  // handler ran and returned a failure result
    Rejected,       // no handler registered / handler unregistered before run
    QueueFull,      // bounded command channel rejected the submission (S I.5)
    ShutDown,       // registry shut down before the command ran
    Superseded,     // displaced by an explicit supersession policy (bake queue, E4b)
    StaleScope,     // ActiveSession epoch closed/rotated before the command ran
};

const char* to_string(CommandStatus s);

// ---------------------------------------------------------------------
// Command scope (S I.10 "Session scope is part of the envelope").
// ---------------------------------------------------------------------
enum class CommandScope { App, ActiveSession };

struct CommandScopeToken {
    CommandScope kind = CommandScope::App;
    uint64_t session_id = 0;
    uint64_t generation = 0;
};

// ---------------------------------------------------------------------
// CommandResult<Receipt> (S I.10). Carries the typed receipt for the
// caller plus the generic history metadata (inverse memento + coalesce
// key) the registry hands to its HistorySink before the result becomes
// visible. The inverse is TYPE-ERASED (shared_ptr<void>): E4a carries it
// but does not interpret it; the later world-scoped UndoStack (the real
// HistorySink) owns and replays it. "Inverse captured at execute time"
// means the handler builds it while it runs and returns it here.
// ---------------------------------------------------------------------
template <class Receipt>
struct CommandResult {
    using ReceiptType = Receipt;

    CommandStatus status = CommandStatus::Success;
    std::string error;                     // populated on failure
    std::optional<Receipt> value;          // typed receipt on success
    std::shared_ptr<void> inverse;         // opaque undo memento (nullable)
    std::optional<uint64_t> coalesce_key;  // consecutive-same-key = one undo step

    bool ok() const { return status == CommandStatus::Success; }

    static CommandResult succeeded(Receipt v) {
        CommandResult r;
        r.status = CommandStatus::Success;
        r.value = std::move(v);
        return r;
    }
    static CommandResult failed(std::string msg) {
        CommandResult r;
        r.status = CommandStatus::HandlerFailed;
        r.error = std::move(msg);
        return r;
    }
};

// ---------------------------------------------------------------------
// HistorySink (S I.10). E4a installs a NullHistorySink; the later
// world-scoped UndoStack implements the same interface. The registry
// hands one record per EXECUTED command (the handler ran) here, before
// the result becomes visible. Non-executed terminal causes (StaleScope,
// QueueFull, ShutDown, Superseded, no-handler Rejected) never produce a
// history entry — they never ran, so they are not undoable.
// ---------------------------------------------------------------------
struct CommandRecord {
    uint64_t id = 0;
    const char* name = nullptr;
    CommandStatus status = CommandStatus::Pending;
    double duration_ms = 0.0;
    CommandScopeToken scope;
    std::optional<uint64_t> coalesce_key;
    std::shared_ptr<void> inverse;  // opaque memento (nullable)
};

class HistorySink {
public:
    virtual ~HistorySink() = default;
    // Called once per executed command, before its result is visible.
    virtual void on_command_completed(const CommandRecord& rec) = 0;
};

class NullHistorySink final : public HistorySink {
public:
    void on_command_completed(const CommandRecord&) override {}
};

// ---------------------------------------------------------------------
// The generic command-flow notifications emitted on the hub (S I.10).
// Deliberately carry ONLY generic trace fields — never the typed result —
// so a journaling subscriber sees command flow without reconstructing
// typed payloads. Plain-struct events with MT_EVENT_NAME, emitted through
// Hub::emit exactly like any other notification.
// ---------------------------------------------------------------------
struct CommandCompleted {
    MT_EVENT_NAME("cmd.completed");
    uint64_t id = 0;
    const char* name = nullptr;
    CommandStatus status = CommandStatus::Success;
    double duration_ms = 0.0;
    CommandScopeToken scope;
};

struct CommandFailed {
    MT_EVENT_NAME("cmd.failed");
    uint64_t id = 0;
    const char* name = nullptr;
    CommandStatus status = CommandStatus::HandlerFailed;
    double duration_ms = 0.0;
    CommandScopeToken scope;
    std::string error;
};

// The try_register_handler error channel (S I.10). Deliberately separate
// from evt::RegistrationError (registration_error.h documents that the
// DuplicateHandler analogue "belongs to the command layer (E4), not
// here"), so E1's header is untouched.
enum class CommandRegistrationError { DuplicateHandler };

// A command's handler: consumes the typed command, returns its typed
// result. Runs on the registered lane's owner thread (execute inline;
// dispatch at pump).
template <class C>
using Handler = std::function<typename C::Result(const C&)>;

// ---------------------------------------------------------------------
// Registration — RAII handle for one registered handler. Move-only.
// Mirrors evt::Subscription: it holds NO registry pointer, only a shared
// control block, so destroying it after the registry is a safe no-op. Its
// destructor logically unregisters the handler (clears the active bit); a
// later must_register_handler for the same command type may then re-bind.
// ---------------------------------------------------------------------
struct RegistrationBlock {
    std::atomic<bool> active{true};
};

class Registration {
public:
    Registration() = default;
    explicit Registration(std::shared_ptr<RegistrationBlock> block) : block_(std::move(block)) {}

    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;
    Registration(Registration&&) noexcept = default;
    Registration& operator=(Registration&& other) noexcept {
        if (this != &other) {
            reset();
            block_ = std::move(other.block_);
        }
        return *this;
    }
    ~Registration() { reset(); }

    void reset() {
        if (block_) block_->active.store(false, std::memory_order_release);
        block_.reset();
    }
    bool valid() const { return block_ != nullptr; }

private:
    std::shared_ptr<RegistrationBlock> block_;
};

// ---------------------------------------------------------------------
// Ticket internals (non-public). A non-template base lets the registry
// hold weak references to every live ticket and finalize the pending ones
// on shut_down() — so a caller of a deliver-once command is NEVER left
// unable to tell whether the command vanished (S I.10).
// ---------------------------------------------------------------------
class CommandRegistry;  // forward: TicketState + CommandTicket call back into it.

struct TicketStateBase {
    std::atomic<bool> finalized{false};  // the exactly-once gate
    uint64_t id = 0;
    const char* name = nullptr;
    CommandScopeToken scope;
    lane handler_lane{};
    CommandRegistry* registry = nullptr;

    std::mutex m;
    std::condition_variable cv;
    bool done = false;

    virtual ~TicketStateBase() = default;
    // Complete with a terminal cause that did NOT run the handler.
    virtual void finalize_terminal(CommandStatus status) = 0;
};

template <class Result>
struct TicketState final : TicketStateBase {
    Result result{};
    std::vector<std::pair<lane, std::function<void(const Result&)>>> continuations;

    void finalize_terminal(CommandStatus status) override {
        Result r{};
        r.status = status;
        finalize(std::move(r), /*duration_ms=*/0.0, /*executed=*/false);
    }

    // The single completion point for THIS ticket. Idempotent via the
    // atomic gate; every terminal path (success, handler failure, queue
    // rejection, shutdown, supersession, stale scope) funnels here so the
    // ticket is completed exactly once. Records history + emits the hub
    // notification (through the registry) BEFORE marking the result
    // visible, then wakes wait()ers and posts then()-continuations.
    void finalize(Result r, double duration_ms, bool executed);
};

// ---------------------------------------------------------------------
// CommandTicket<Result> (S I.10) — the correlated completion handle for
// the queued dispatch path. Monotonic id; non-blocking then(lane, cb);
// wait() legal only OFF the handler's owner lane. A ticket on a live but
// un-pumped lane stays pending until the owner pumps or a terminal cause
// fires — it is never dropped.
// ---------------------------------------------------------------------
template <class Result>
class CommandTicket {
public:
    CommandTicket() = default;
    explicit CommandTicket(std::shared_ptr<TicketState<Result>> state) : state_(std::move(state)) {}

    uint64_t id() const { return state_ ? state_->id : 0; }
    bool valid() const { return state_ != nullptr; }

    bool ready() const {
        if (!state_) return false;
        std::lock_guard<std::mutex> lk(state_->m);
        return state_->done;
    }

    CommandStatus status() const {
        if (!state_) return CommandStatus::Pending;
        std::lock_guard<std::mutex> lk(state_->m);
        return state_->done ? state_->result.status : CommandStatus::Pending;
    }

    // Blocking wait for completion; returns the terminal result. Legal
    // only OFF the handler's owner lane: blocking on the very thread that
    // must pump the command to complete it would deadlock, so calling it
    // from the owner lane is all-build fail-fast (S I.10).
    Result wait();

    // Non-blocking completion continuation, delivered on `ln` (queued onto
    // that lane; runs when the lane owner next pumps the registry). If the
    // ticket is already complete, the continuation is posted immediately.
    void then(lane ln, std::function<void(const Result&)> cb);

private:
    std::shared_ptr<TicketState<Result>> state_;
};

// ---------------------------------------------------------------------
// CommandRegistry (S I.10). One handler per command name (all-build
// unique). execute() = synchronous owner-lane path returning the typed
// result; dispatch() = any-thread queued path returning a ticket. Both
// route through one internal finalization path (finalize_common) that
// records timing, hands inverse/coalesce metadata to the HistorySink, and
// emits the generic cmd.* notification.
// ---------------------------------------------------------------------
class CommandRegistry {
public:
    // history_sink_ is set to the process-wide NullHistorySink in the body
    // (command.cpp) so the default install matches S I.10 without a fragile
    // member-init-order dependency.
    explicit CommandRegistry(Hub& hub);
    ~CommandRegistry() { shut_down(); }

    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;

    // Swap in the real (undo-stack) history sink. Default is a process-wide
    // NullHistorySink. Pointer is not owned.
    void set_history_sink(HistorySink* sink);

    // ---- Scope epochs (S I.10 / S II.4 item 5) ------------------------
    // E4b's SessionBinding drives these on world switch: open a fresh epoch
    // after rebind, close the old one first. An ActiveSession command
    // stamped under generation G completes StaleScope if it is pumped after
    // the epoch closed or rotated to G' != G.
    void set_active_scope(CommandScopeToken token);
    void close_active_scope();
    CommandScopeToken active_scope() const;
    bool active_scope_open() const;

    // ---- Lane ownership + pump ----------------------------------------
    // Register the calling thread as lane `ln`'s owner (required before
    // pump / execute on that lane; also what wait()'s off-lane guard reads).
    void claim_lane(lane ln);
    // Owner thread only: run queued commands for lane `ln` (each through the
    // finalization path) then deliver any queued then()-continuations for
    // that lane. Returns the number of jobs run. Progress-guaranteed like
    // Hub::pump / Channel::pump.
    int pump(lane ln, double ms_budget);

    // Fail every still-pending ticket ShutDown, reject future dispatch, and
    // shut down the lane channels. Idempotent.
    void shut_down();

    // ---- Registration -------------------------------------------------
    // Exactly one handler per command name, all-build fail-fast on a live
    // duplicate (matching Hub::must_subscribe). try_register_handler returns
    // DuplicateHandler WITHOUT mutating the registry.
    template <class C>
    [[nodiscard]] Registration must_register_handler(CommandScope scope, lane ln, Handler<C> h) {
        auto res = register_generic(C::mt_command_type_id(), C::mt_command_name, scope, ln,
                                    std::static_pointer_cast<void>(
                                        std::make_shared<Handler<C>>(std::move(h))));
        if (res.duplicate) {
            std::string msg =
                std::string("evt::CommandRegistry::must_register_handler: duplicate handler for "
                            "command '") +
                C::mt_command_name + "'";
            fail_fast(msg.c_str());
            return Registration{};
        }
        return std::move(res.reg);
    }

    template <class C>
    [[nodiscard]] std::variant<Registration, CommandRegistrationError> try_register_handler(
        CommandScope scope, lane ln, Handler<C> h) {
        auto res = register_generic(C::mt_command_type_id(), C::mt_command_name, scope, ln,
                                    std::static_pointer_cast<void>(
                                        std::make_shared<Handler<C>>(std::move(h))));
        if (res.duplicate) return CommandRegistrationError::DuplicateHandler;
        return std::variant<Registration, CommandRegistrationError>(std::move(res.reg));
    }

    // ---- execute (owner-lane, synchronous, typed) ---------------------
    template <class C>
    typename C::Result execute(C cmd) {
        using Result = typename C::Result;
        const uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);

        HandlerLookup lk = lookup(C::mt_command_type_id());
        if (!lk.found) {
            Result r{};
            r.status = CommandStatus::Rejected;
            r.error = "no handler registered";
            finalize_common(id, C::mt_command_name, r.status, 0.0, CommandScopeToken{},
                            r.coalesce_key, r.inverse, /*executed=*/false, r.error);
            return r;
        }
        assert_execute_lane(lk.ln);  // execute is legal only on the handler's lane

        CommandScopeToken scope = stamp_scope(lk.scope);
        if (lk.scope == CommandScope::ActiveSession && !scope_valid(scope)) {
            Result r{};
            r.status = CommandStatus::StaleScope;
            finalize_common(id, C::mt_command_name, r.status, 0.0, scope, r.coalesce_key, r.inverse,
                            /*executed=*/false, r.error);
            return r;
        }

        auto handler = std::static_pointer_cast<Handler<C>>(lk.handler);
        auto t0 = std::chrono::steady_clock::now();
        Result r = (*handler)(cmd);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        finalize_common(id, C::mt_command_name, r.status, ms, scope, r.coalesce_key, r.inverse,
                        /*executed=*/true, r.error);
        return r;
    }

    // ---- dispatch (any-thread, queued, ticketed) ----------------------
    template <class C>
    CommandTicket<typename C::Result> dispatch(C cmd) {
        using Result = typename C::Result;
        const uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);

        HandlerLookup lk = lookup(C::mt_command_type_id());
        lane ln = lk.found ? lk.ln : lane{};

        auto state = std::make_shared<TicketState<Result>>();
        state->id = id;
        state->name = C::mt_command_name;
        state->handler_lane = ln;
        state->registry = this;
        track_ticket(state);
        CommandTicket<Result> ticket(state);

        if (shutdown_.load(std::memory_order_acquire)) {
            state->finalize_terminal(CommandStatus::ShutDown);
            return ticket;
        }
        if (!lk.found) {
            Result r{};
            r.status = CommandStatus::Rejected;
            r.error = "no handler registered";
            state->finalize(std::move(r), 0.0, /*executed=*/false);
            return ticket;
        }

        CommandScopeToken scope = stamp_scope(lk.scope);
        state->scope = scope;

        auto handler = std::static_pointer_cast<Handler<C>>(lk.handler);
        auto block = lk.block;
        CommandScope cmd_scope = lk.scope;

        // The queued job runs on the handler's lane at pump time. It is the
        // sole place the epoch is re-checked (S II.4 item 5): if the epoch
        // closed/rotated between dispatch and pump, complete StaleScope and
        // NEVER run the handler, so a queued old-epoch command cannot mutate
        // the new world.
        auto self = this;
        std::function<void()> job =
            [self, state, cmd = std::move(cmd), handler, block, scope, cmd_scope, id]() mutable {
                if (state->finalized.load(std::memory_order_acquire)) return;  // already terminal
                if (self->shutdown_.load(std::memory_order_acquire)) {
                    state->finalize_terminal(CommandStatus::ShutDown);
                    return;
                }
                if (!block->active.load(std::memory_order_acquire)) {
                    Result r{};
                    r.status = CommandStatus::Rejected;
                    r.error = "handler unregistered before dispatch ran";
                    state->finalize(std::move(r), 0.0, /*executed=*/false);
                    return;
                }
                if (cmd_scope == CommandScope::ActiveSession && !self->scope_valid(scope)) {
                    state->finalize_terminal(CommandStatus::StaleScope);
                    return;
                }
                auto t0 = std::chrono::steady_clock::now();
                Result r = (*handler)(cmd);
                auto t1 = std::chrono::steady_clock::now();
                double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                state->finalize(std::move(r), ms, /*executed=*/true);
            };

        PushResult pr = post_command(ln, std::move(job));
        if (pr == PushResult::RejectedFull) {
            state->finalize_terminal(CommandStatus::QueueFull);
        } else if (pr == PushResult::ShutDown) {
            state->finalize_terminal(CommandStatus::ShutDown);
        }
        return ticket;
    }

    // ---- internals reached by TicketState / CommandTicket -------------
    // (public because the header-defined templates call them; not part of
    // the intended consumer surface.)
    void finalize_common(uint64_t id, const char* name, CommandStatus status, double duration_ms,
                         const CommandScopeToken& scope, std::optional<uint64_t> coalesce_key,
                         std::shared_ptr<void> inverse, bool executed, const std::string& error);
    void post_continuation(lane ln, std::function<void()> cont);
    bool is_lane_owner_current_thread(lane ln) const;

private:
    // Bounded command channel (S I.5: externally fed command channels use
    // bounded RejectNewest and complete the rejected ticket QueueFull).
    // Non-dropping by contract -- RejectNewest never silently drops; it
    // rejects and the ticket is told. Generous default; the bake queue's
    // own unbounded policy lives in its specialized channel (E4b), not here.
    static constexpr size_t kCommandChannelCapacity = 4096;

    struct HandlerRecord {
        CommandScope scope = CommandScope::App;
        lane ln{};
        std::shared_ptr<void> handler;  // shared_ptr<Handler<C>>
        std::shared_ptr<RegistrationBlock> block;
        const char* name = nullptr;
    };
    struct HandlerLookup {
        bool found = false;
        CommandScope scope = CommandScope::App;
        lane ln{};
        std::shared_ptr<void> handler;
        std::shared_ptr<RegistrationBlock> block;
    };
    struct RegisterResult {
        Registration reg;
        bool duplicate = false;
    };
    struct LaneState {
        std::unique_ptr<Channel<std::function<void()>>> commands;       // bounded RejectNewest
        std::unique_ptr<Channel<std::function<void()>>> continuations;  // unbounded
        std::thread::id owner{};
        bool owner_set = false;
    };

    RegisterResult register_generic(const void* type_id, const char* name, CommandScope scope,
                                    lane ln, std::shared_ptr<void> handler);
    HandlerLookup lookup(const void* type_id) const;

    CommandScopeToken stamp_scope(CommandScope scope) const;
    bool scope_valid(const CommandScopeToken& stamped) const;

    LaneState& get_or_create_lane(lane ln);
    PushResult post_command(lane ln, std::function<void()> job);
    void track_ticket(const std::shared_ptr<TicketStateBase>& state);
    void assert_execute_lane(lane ln) const;

    Hub& hub_;
    HistorySink* history_sink_ = nullptr;  // set to NullHistorySink in ctor body

    mutable std::mutex handlers_mu_;
    std::unordered_map<const void*, HandlerRecord> handlers_;

    mutable std::mutex lanes_mu_;
    std::unordered_map<uint32_t, LaneState> lanes_;

    mutable std::mutex scope_mu_;
    bool active_scope_open_ = false;
    CommandScopeToken active_token_{CommandScope::ActiveSession, 0, 0};

    std::mutex tickets_mu_;
    std::vector<std::weak_ptr<TicketStateBase>> live_tickets_;

    std::atomic<uint64_t> next_id_{1};
    std::atomic<bool> shutdown_{false};
};

// ---- out-of-line template method bodies (need CommandRegistry) --------

template <class Result>
void TicketState<Result>::finalize(Result r, double duration_ms, bool executed) {
    if (finalized.exchange(true, std::memory_order_acq_rel)) return;  // exactly-once gate

    // Record + notify BEFORE the result is visible (S I.10).
    if (registry) {
        registry->finalize_common(id, name, r.status, duration_ms, scope, r.coalesce_key, r.inverse,
                                  executed, r.error);
    }

    std::vector<std::pair<lane, std::function<void(const Result&)>>> conts;
    {
        std::lock_guard<std::mutex> lk(m);
        result = std::move(r);
        done = true;
        conts.swap(continuations);
    }
    cv.notify_all();

    // Deliver then()-continuations on their target lanes (queued; the lane
    // owner runs them at pump). Read the now-visible result by value.
    if (registry) {
        const Result snapshot = result;
        for (auto& c : conts) {
            lane ln = c.first;
            auto cb = std::move(c.second);
            registry->post_continuation(ln, [cb = std::move(cb), snapshot]() { cb(snapshot); });
        }
    }
}

template <class Result>
Result CommandTicket<Result>::wait() {
    // Off-lane-only guard (S I.10): waiting on the handler's own owner lane
    // would deadlock (the command completes only when that thread pumps).
    if (state_->registry && state_->handler_lane.valid() &&
        state_->registry->is_lane_owner_current_thread(state_->handler_lane)) {
        fail_fast(
            "evt::CommandTicket::wait: called from the handler's owner lane -- this would "
            "deadlock waiting for a command only this thread can pump. wait() is legal only "
            "off the owner lane.");
        return Result{};  // a recording test handler does not abort; don't block after fail_fast.
    }
    std::unique_lock<std::mutex> lk(state_->m);
    state_->cv.wait(lk, [&] { return state_->done; });
    return state_->result;
}

template <class Result>
void CommandTicket<Result>::then(lane ln, std::function<void(const Result&)> cb) {
    bool deliver_now = false;
    Result snapshot{};
    {
        std::lock_guard<std::mutex> lk(state_->m);
        if (state_->done) {
            deliver_now = true;
            snapshot = state_->result;
        } else {
            state_->continuations.emplace_back(ln, std::move(cb));
        }
    }
    if (deliver_now && state_->registry) {
        state_->registry->post_continuation(
            ln, [cb = std::move(cb), snapshot]() { cb(snapshot); });
    }
}

}  // namespace matter::evt
