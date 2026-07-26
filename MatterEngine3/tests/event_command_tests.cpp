// event_command_tests.cpp — headless tests for evt::CommandRegistry (E4a
// of the event system; see MatterEngine3/docs/event-system.md S I.10, S
// II.4 items 2 and 5). Plain assert + printf style, matching
// event_hub_tests.cpp; no GL, no window, no GALLIUM env needed.
//
// Coverage (per the milestone brief):
//   - register + execute: typed result synchronously; validation-failure
//     result synchronously.
//   - dispatch + ticket: queued; ticket completes Success with typed
//     result readable via then(); wait() off-lane works.
//   - all-build duplicate-handler rejection: try_register_handler returns
//     DuplicateHandler without mutating; must_register_handler fires the
//     swappable fail-fast handler -- exercised on the RELEASE path (-O2,
//     no NDEBUG gate; the check is a plain runtime branch).
//   - scope epochs (S II.4 item 5): dispatch an ActiveSession command,
//     close_active_scope() before pump -> StaleScope, handler NOT run;
//     after set_active_scope(new) a fresh command runs. Proves an
//     old-epoch queued command cannot execute against the new epoch.
//   - ticket completes exactly once on every outcome: Success,
//     HandlerFailed, QueueFull, ShutDown, StaleScope, no-handler Rejected.
//   - cmd.completed / cmd.failed emitted on the hub with generic fields
//     (journaling-as-subscriber proof).
//   - inverse/coalesce metadata reaches the HistorySink (undo-reachability
//     proof; a recording sink).
#include "matter/event/command.h"
#include "matter/event/event_hub.h"
#include "check.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using matter::evt::CommandRegistrationError;
using matter::evt::CommandRegistry;
using matter::evt::CommandResult;
using matter::evt::CommandScope;
using matter::evt::CommandScopeToken;
using matter::evt::CommandStatus;
using matter::evt::CommandRecord;
using matter::evt::HistorySink;
using matter::evt::Hub;
using matter::evt::lane;
using matter::evt::Registration;
using matter::evt::set_fail_fast_handler;

// ---------------------------------------------------------------------------
// Test commands. A create command returns a typed receipt (an id); a
// validating command can fail synchronously.
// ---------------------------------------------------------------------------
struct CreateReceipt {
    uint64_t entity_id = 0;
};

struct CreateEntity {
    MT_COMMAND_NAME("test.create_entity");
    using Result = CommandResult<CreateReceipt>;
    std::string name;
};

struct DeleteEntity {
    MT_COMMAND_NAME("test.delete_entity");
    using Result = CommandResult<int>;  // int = number of entities removed
    uint64_t entity_id = 0;
};

// An ActiveSession command that records which epoch it actually ran under,
// so a stale-epoch execution is provable if it (wrongly) runs.
struct SessionEdit {
    MT_COMMAND_NAME("test.session_edit");
    using Result = CommandResult<uint64_t>;  // returns the entity id it touched
    uint64_t entity_id = 0;
};

// ---------------------------------------------------------------------------
// A recording HistorySink (the undo stack's stand-in): captures the
// inverse memento + coalesce key handed to it by the registry.
// ---------------------------------------------------------------------------
struct RecordingSink : HistorySink {
    struct Entry {
        uint64_t id;
        CommandStatus status;
        std::shared_ptr<void> inverse;
        std::optional<uint64_t> coalesce_key;
    };
    std::mutex m;
    std::vector<Entry> entries;
    void on_command_completed(const CommandRecord& rec) override {
        std::lock_guard<std::mutex> lk(m);
        entries.push_back(Entry{rec.id, rec.status, rec.inverse, rec.coalesce_key});
    }
    size_t count() {
        std::lock_guard<std::mutex> lk(m);
        return entries.size();
    }
};

// ===========================================================================
// 1. register + execute: typed result returned synchronously; validation
//    failure returned synchronously.
// ===========================================================================
static void test_execute_typed_result() {
    printf("[test_execute_typed_result]\n");
    Hub hub;
    CommandRegistry reg(hub);
    reg.claim_lane(lane::app);  // execute is legal only on the handler's lane

    uint64_t next_id = 100;
    Registration r = reg.must_register_handler<CreateEntity>(
        CommandScope::App, lane::app, [&](const CreateEntity& c) {
            CreateReceipt rc;
            rc.entity_id = ++next_id;
            (void)c;
            return CreateEntity::Result::succeeded(rc);
        });

    CreateEntity::Result res = reg.execute(CreateEntity{"box"});
    CHECK(res.ok(), "execute returned Success");
    CHECK(res.status == CommandStatus::Success, "status Success");
    CHECK(res.value.has_value() && res.value->entity_id == 101,
          "typed receipt id returned synchronously");

    // Validation-failure path: a handler that returns a failure result.
    Registration rd = reg.must_register_handler<DeleteEntity>(
        CommandScope::App, lane::app, [&](const DeleteEntity& c) {
            if (c.entity_id == 0) return DeleteEntity::Result::failed("cannot delete id 0");
            return DeleteEntity::Result::succeeded(1);
        });
    DeleteEntity::Result bad = reg.execute(DeleteEntity{0});
    CHECK(!bad.ok() && bad.status == CommandStatus::HandlerFailed,
          "validation failure returned synchronously as HandlerFailed");
    CHECK(bad.error == "cannot delete id 0", "error message carried on the typed result");
    DeleteEntity::Result good = reg.execute(DeleteEntity{7});
    CHECK(good.ok() && good.value.has_value() && *good.value == 1, "valid delete returns receipt");
    printf("ok execute_typed_result\n");
}

// ===========================================================================
// 2. dispatch + ticket: queued, ticket completes Success with the typed
//    result readable via then(); wait() off-lane works.
// ===========================================================================
static void test_dispatch_ticket_then_and_wait() {
    printf("[test_dispatch_ticket_then_and_wait]\n");
    Hub hub;
    CommandRegistry reg(hub);

    // Handler runs on lane::app; a worker thread owns/pumps it.
    std::atomic<bool> stop{false};
    std::thread worker([&] {
        reg.claim_lane(lane::app);
        uint64_t next = 500;
        Registration r = reg.must_register_handler<CreateEntity>(
            CommandScope::App, lane::app, [&](const CreateEntity&) {
                CreateReceipt rc;
                rc.entity_id = ++next;
                return CreateEntity::Result::succeeded(rc);
            });
        while (!stop.load()) {
            reg.pump(lane::app, 5.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        reg.pump(lane::app, 5.0);  // final drain
    });

    // Give the worker a moment to register the handler.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // then(): the continuation is delivered on lane::app when the worker
    // pumps continuations.
    std::atomic<uint64_t> seen_via_then{0};
    auto t1 = reg.dispatch(CreateEntity{"a"});
    t1.then(lane::app, [&](const CreateEntity::Result& r) {
        if (r.ok() && r.value) seen_via_then.store(r.value->entity_id);
    });

    // wait(): called from THIS (main) thread, which is off lane::app's
    // owner (the worker), so it is legal and blocks until completion.
    auto t2 = reg.dispatch(CreateEntity{"b"});
    CreateEntity::Result r2 = t2.wait();
    CHECK(r2.ok() && r2.value.has_value(), "wait() off-lane returned the completed typed result");

    // Spin until the then-continuation has been pumped.
    for (int i = 0; i < 200 && seen_via_then.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(seen_via_then.load() != 0, "then(lane, cb) delivered the typed result on the lane");
    CHECK(t1.status() == CommandStatus::Success, "ticket t1 status Success");

    stop.store(true);
    worker.join();
    printf("ok dispatch_ticket_then_and_wait\n");
}

// ===========================================================================
// 3. all-build duplicate-handler rejection (RELEASE path).
// ===========================================================================
static void test_duplicate_handler_all_build() {
    printf("[test_duplicate_handler_all_build]\n");
    Hub hub;
    CommandRegistry reg(hub);
    reg.claim_lane(lane::app);

    Registration first = reg.must_register_handler<CreateEntity>(
        CommandScope::App, lane::app, [&](const CreateEntity&) {
            return CreateEntity::Result::succeeded(CreateReceipt{1});
        });

    // try_register_handler: DuplicateHandler WITHOUT mutating the registry.
    auto tr = reg.try_register_handler<CreateEntity>(
        CommandScope::App, lane::app, [&](const CreateEntity&) {
            return CreateEntity::Result::succeeded(CreateReceipt{999});
        });
    CHECK(std::holds_alternative<CommandRegistrationError>(tr),
          "try_register_handler returns DuplicateHandler on a live duplicate");

    // The original handler still governs (registry NOT mutated): execute
    // returns receipt id 1, not 999.
    CreateEntity::Result res = reg.execute(CreateEntity{"x"});
    CHECK(res.value.has_value() && res.value->entity_id == 1,
          "duplicate try_register did not replace the live handler");

    // must_register_handler: fires the swappable fail-fast handler on the
    // RELEASE path (this TU compiles at -O2 with no NDEBUG gate; the
    // uniqueness check is a plain runtime branch). A recording handler lets
    // us observe the fail-fast without aborting.
    std::atomic<int> fail_fast_hits{0};
    set_fail_fast_handler([&](const char*) { fail_fast_hits.fetch_add(1); });
    Registration dup = reg.must_register_handler<CreateEntity>(
        CommandScope::App, lane::app, [&](const CreateEntity&) {
            return CreateEntity::Result::succeeded(CreateReceipt{888});
        });
    set_fail_fast_handler({});  // restore default abort handler
    CHECK(fail_fast_hits.load() == 1, "must_register_handler fired the all-build fail-fast handler");
    CHECK(!dup.valid(), "the duplicate must_register_handler yielded no live Registration");

    // Still governed by the original handler.
    CreateEntity::Result res2 = reg.execute(CreateEntity{"y"});
    CHECK(res2.value.has_value() && res2.value->entity_id == 1,
          "registry unchanged after the duplicate must_register_handler");
    printf("ok duplicate_handler_all_build\n");
}

// ===========================================================================
// 4. Scope epochs (S II.4 item 5): a queued old-epoch ActiveSession
//    command completes StaleScope and NEVER runs; a fresh-epoch command
//    runs. Proves entity-id drift across a world switch cannot happen.
// ===========================================================================
static void test_scope_epochs_stale() {
    printf("[test_scope_epochs_stale]\n");
    Hub hub;
    CommandRegistry reg(hub);
    reg.claim_lane(lane::app);

    std::atomic<int> handler_runs{0};
    std::atomic<uint64_t> last_epoch_ran{0};
    Registration r = reg.must_register_handler<SessionEdit>(
        CommandScope::ActiveSession, lane::app, [&](const SessionEdit& c) {
            handler_runs.fetch_add(1);
            last_epoch_ran.store(reg.active_scope().generation);
            return SessionEdit::Result::succeeded(c.entity_id);
        });

    // Open epoch generation 1, dispatch a command, then CLOSE the epoch
    // before pumping. The queued old-epoch command must complete StaleScope
    // without running the handler.
    reg.set_active_scope(CommandScopeToken{CommandScope::ActiveSession, /*session*/ 42, /*gen*/ 1});
    auto stale = reg.dispatch(SessionEdit{7});
    reg.close_active_scope();  // world switch closes the old epoch
    reg.pump(lane::app, 5.0);

    CHECK(stale.status() == CommandStatus::StaleScope,
          "old-epoch command completed StaleScope after close_active_scope()");
    CHECK(handler_runs.load() == 0, "the stale command's handler NEVER ran (no entity-id drift)");

    // Open a NEW epoch (generation 2) and dispatch a fresh command: it runs.
    reg.set_active_scope(CommandScopeToken{CommandScope::ActiveSession, /*session*/ 43, /*gen*/ 2});
    auto fresh = reg.dispatch(SessionEdit{9});
    reg.pump(lane::app, 5.0);
    CHECK(fresh.status() == CommandStatus::Success, "new-epoch command ran to Success");
    CHECK(handler_runs.load() == 1, "exactly the fresh command ran the handler");
    CHECK(last_epoch_ran.load() == 2, "the handler ran under the NEW epoch, never the stale one");

    // Rotation (not just close): dispatch under gen 2, rotate to gen 3
    // before pump -> stale, even though an epoch is still open.
    auto rotated = reg.dispatch(SessionEdit{11});
    reg.set_active_scope(CommandScopeToken{CommandScope::ActiveSession, /*session*/ 44, /*gen*/ 3});
    reg.pump(lane::app, 5.0);
    CHECK(rotated.status() == CommandStatus::StaleScope,
          "a command stamped under gen 2 is stale after rotation to gen 3");
    CHECK(handler_runs.load() == 1, "the rotated command did not run against the new epoch");
    printf("ok scope_epochs_stale\n");
}

// ===========================================================================
// 5. Ticket completes exactly once on every outcome.
// ===========================================================================
static void test_ticket_exactly_once_all_outcomes() {
    printf("[test_ticket_exactly_once_all_outcomes]\n");

    // --- no-handler Rejected -------------------------------------------
    {
        Hub hub;
        CommandRegistry reg(hub);
        auto t = reg.dispatch(CreateEntity{"orphan"});
        CHECK(t.status() == CommandStatus::Rejected, "dispatch with no handler -> Rejected");
    }

    // --- Success + HandlerFailed ---------------------------------------
    {
        Hub hub;
        CommandRegistry reg(hub);
        reg.claim_lane(lane::app);
        Registration r = reg.must_register_handler<DeleteEntity>(
            CommandScope::App, lane::app, [&](const DeleteEntity& c) {
                if (c.entity_id == 0) return DeleteEntity::Result::failed("bad");
                return DeleteEntity::Result::succeeded(1);
            });
        auto ok = reg.dispatch(DeleteEntity{5});
        auto fail = reg.dispatch(DeleteEntity{0});
        reg.pump(lane::app, 5.0);
        CHECK(ok.status() == CommandStatus::Success, "Success outcome");
        CHECK(fail.status() == CommandStatus::HandlerFailed, "HandlerFailed outcome");

        // Exactly-once: pumping again does not re-run or change status.
        int before = 0;
        reg.pump(lane::app, 5.0);
        (void)before;
        CHECK(ok.status() == CommandStatus::Success, "status stable after a redundant pump");
    }

    // --- QueueFull: fill the bounded command channel without pumping ----
    {
        Hub hub;
        CommandRegistry reg(hub);
        // Register on a lane nobody pumps, so jobs accumulate. Capacity is
        // 4096; push past it from this thread.
        lane unpumped = matter::evt::make_lane();
        Registration r = reg.must_register_handler<CreateEntity>(
            CommandScope::App, unpumped, [&](const CreateEntity&) {
                return CreateEntity::Result::succeeded(CreateReceipt{1});
            });
        int queue_full_hits = 0;
        bool any_pending_ok = true;
        for (int i = 0; i < 4100; ++i) {
            auto t = reg.dispatch(CreateEntity{"flood"});
            if (t.status() == CommandStatus::QueueFull)
                ++queue_full_hits;
            else if (t.status() != CommandStatus::Pending)
                any_pending_ok = false;  // only Pending or QueueFull are expected here
        }
        CHECK(queue_full_hits > 0, "bounded command channel completed overflow tickets QueueFull");
        CHECK(any_pending_ok, "non-rejected tickets stayed Pending (never dropped)");
        reg.shut_down();  // fail the still-pending flood tickets so nothing hangs
    }

    // --- ShutDown: dispatch, then shut_down before pumping --------------
    {
        Hub hub;
        CommandRegistry reg(hub);
        lane unpumped = matter::evt::make_lane();
        Registration r = reg.must_register_handler<CreateEntity>(
            CommandScope::App, unpumped, [&](const CreateEntity&) {
                return CreateEntity::Result::succeeded(CreateReceipt{1});
            });
        auto t = reg.dispatch(CreateEntity{"pending"});
        CHECK(t.status() == CommandStatus::Pending, "queued command is Pending before shutdown");
        reg.shut_down();
        CHECK(t.status() == CommandStatus::ShutDown,
              "shut_down() completed the still-pending ticket ShutDown (never dropped)");
    }

    // --- Exactly-once across a shutdown/pump race ----------------------
    // A command that already completed Success must NOT be re-finalized by a
    // later shut_down(): its status stays Success (the atomic gate).
    {
        Hub hub;
        CommandRegistry reg(hub);
        reg.claim_lane(lane::app);
        Registration r = reg.must_register_handler<CreateEntity>(
            CommandScope::App, lane::app, [&](const CreateEntity&) {
                return CreateEntity::Result::succeeded(CreateReceipt{1});
            });
        auto t = reg.dispatch(CreateEntity{"done"});
        reg.pump(lane::app, 5.0);
        CHECK(t.status() == CommandStatus::Success, "completed Success before shutdown");
        reg.shut_down();
        CHECK(t.status() == CommandStatus::Success,
              "shut_down() did not re-finalize an already-completed ticket (exactly once)");
    }
    printf("ok ticket_exactly_once_all_outcomes\n");
}

// ===========================================================================
// 6. cmd.completed / cmd.failed emitted on the hub (journaling-as-subscriber).
// ===========================================================================
static void test_hub_notifications() {
    printf("[test_hub_notifications]\n");
    Hub hub;
    CommandRegistry reg(hub);
    reg.claim_lane(lane::app);

    std::vector<uint64_t> completed_ids;
    std::vector<std::string> failed_names;
    auto sc = hub.must_subscribe<matter::evt::CommandCompleted>(
        "journal.completed", matter::evt::immediate,
        [&](const matter::evt::CommandCompleted& e) { completed_ids.push_back(e.id); });
    auto sf = hub.must_subscribe<matter::evt::CommandFailed>(
        "journal.failed", matter::evt::immediate,
        [&](const matter::evt::CommandFailed& e) { failed_names.push_back(e.name); });

    Registration r = reg.must_register_handler<DeleteEntity>(
        CommandScope::App, lane::app, [&](const DeleteEntity& c) {
            if (c.entity_id == 0) return DeleteEntity::Result::failed("no");
            return DeleteEntity::Result::succeeded(1);
        });

    reg.execute(DeleteEntity{3});  // Success -> cmd.completed
    reg.execute(DeleteEntity{0});  // failure -> cmd.failed
    CHECK(completed_ids.size() == 1, "one cmd.completed emitted for the successful command");
    CHECK(failed_names.size() == 1 && failed_names[0] == std::string("test.delete_entity"),
          "cmd.failed emitted with the generic command name field");
    printf("ok hub_notifications\n");
}

// ===========================================================================
// 7. Inverse / coalesce metadata reaches the HistorySink (undo-reachability).
// ===========================================================================
static void test_history_sink_inverse_coalesce() {
    printf("[test_history_sink_inverse_coalesce]\n");
    Hub hub;
    CommandRegistry reg(hub);
    reg.claim_lane(lane::app);
    RecordingSink sink;
    reg.set_history_sink(&sink);

    // A handler that captures an inverse memento AT EXECUTE TIME and a
    // coalesce key (consecutive same-key = one undo step).
    Registration r = reg.must_register_handler<SessionEdit>(
        CommandScope::App, lane::app, [&](const SessionEdit& c) {
            auto memento = std::make_shared<uint64_t>(c.entity_id);  // opaque undo payload
            SessionEdit::Result res = SessionEdit::Result::succeeded(c.entity_id);
            res.inverse = std::static_pointer_cast<void>(memento);
            res.coalesce_key = 77;  // e.g. a gizmo-drag group key
            return res;
        });

    reg.execute(SessionEdit{123});
    CHECK(sink.count() == 1, "the executed command reached the HistorySink");
    {
        std::lock_guard<std::mutex> lk(sink.m);
        CHECK(sink.entries[0].inverse != nullptr, "inverse memento reached the sink");
        auto* payload = static_cast<uint64_t*>(sink.entries[0].inverse.get());
        CHECK(payload && *payload == 123, "inverse memento carries the execute-time state");
        CHECK(sink.entries[0].coalesce_key.has_value() && *sink.entries[0].coalesce_key == 77,
              "coalesce key reached the sink");
    }

    // A non-executed terminal cause (StaleScope) produces NO history entry.
    Registration rs = reg.must_register_handler<CreateEntity>(
        CommandScope::ActiveSession, lane::app, [&](const CreateEntity&) {
            return CreateEntity::Result::succeeded(CreateReceipt{1});
        });
    reg.set_active_scope(CommandScopeToken{CommandScope::ActiveSession, 1, 1});
    auto t = reg.dispatch(CreateEntity{"stale"});
    reg.close_active_scope();
    reg.pump(lane::app, 5.0);
    CHECK(t.status() == CommandStatus::StaleScope, "control: the command went stale");
    CHECK(sink.count() == 1, "a non-executed (stale) command produced NO undo entry");
    printf("ok history_sink_inverse_coalesce\n");
}

// ===========================================================================
// 8. Concurrency smoke: many threads dispatch while one worker pumps; every
//    ticket completes exactly once (Success), none left pending.
// ===========================================================================
static void test_concurrent_dispatch() {
    printf("[test_concurrent_dispatch]\n");
    Hub hub;
    CommandRegistry reg(hub);

    std::atomic<bool> stop{false};
    std::atomic<int> handled{0};
    std::thread worker([&] {
        reg.claim_lane(lane::app);
        Registration r = reg.must_register_handler<CreateEntity>(
            CommandScope::App, lane::app, [&](const CreateEntity&) {
                handled.fetch_add(1);
                return CreateEntity::Result::succeeded(CreateReceipt{1});
            });
        while (!stop.load()) reg.pump(lane::app, 2.0);
        reg.pump(lane::app, 50.0);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const int kThreads = 6, kEach = 200;
    std::vector<std::thread> producers;
    std::atomic<int> success{0}, other{0};
    std::mutex tmu;
    std::vector<matter::evt::CommandTicket<CreateEntity::Result>> tickets;
    for (int i = 0; i < kThreads; ++i) {
        producers.emplace_back([&] {
            for (int j = 0; j < kEach; ++j) {
                auto t = reg.dispatch(CreateEntity{"c"});
                std::lock_guard<std::mutex> lk(tmu);
                tickets.push_back(std::move(t));
            }
        });
    }
    for (auto& p : producers) p.join();

    // Drain: wait for all tickets to reach a terminal state.
    for (int spins = 0; spins < 2000; ++spins) {
        int pending = 0;
        {
            std::lock_guard<std::mutex> lk(tmu);
            for (auto& t : tickets)
                if (t.status() == CommandStatus::Pending) ++pending;
        }
        if (pending == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true);
    worker.join();

    {
        std::lock_guard<std::mutex> lk(tmu);
        for (auto& t : tickets) {
            if (t.status() == CommandStatus::Success)
                success.fetch_add(1);
            else
                other.fetch_add(1);
        }
    }
    CHECK(success.load() == kThreads * kEach, "every concurrently-dispatched command completed Success");
    CHECK(other.load() == 0, "no ticket left Pending or in a non-Success terminal state");
    CHECK(handled.load() == kThreads * kEach, "handler ran exactly once per command");
    printf("ok concurrent_dispatch (success=%d handled=%d)\n", success.load(), handled.load());
}

int main() {
    test_execute_typed_result();
    test_dispatch_ticket_then_and_wait();
    test_duplicate_handler_all_build();
    test_scope_epochs_stale();
    test_ticket_exactly_once_all_outcomes();
    test_hub_notifications();
    test_history_sink_inverse_coalesce();
    test_concurrent_dispatch();

    if (g_failures == 0) {
        printf("\nALL COMMAND TESTS PASSED\n");
        return 0;
    }
    printf("\n%d COMMAND TEST FAILURE(S)\n", g_failures);
    return 1;
}
