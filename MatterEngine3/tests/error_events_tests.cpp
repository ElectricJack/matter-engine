// error_events_tests.cpp — headless tests for the I.11 error-sink migration
// (see MatterEngine3/docs/event-system.md S I.11 "Notification sink" row,
// S I.6 immediate delivery). Proves the two hub-backed sink adapters emit the
// right error.* events with the right fields:
//   * live_edit::HubErrorSink::report(LiveEditError) -> error.live_edit
//   * scene::make_hub_error_sink(hub).on_error(...)  -> error.part_instance
//   * scene::make_hub_error_sink(hub).on_error_clear -> error.part_instance_clear
// Self-contained like event_hub_tests.cpp: compiles the event impl objects
// directly (no kernel lib, no flecs, no GL). The adapters are header-only.
#include "matter/event/event_hub.h"
#include "matter/events/error_events.h"
#include "live_edit_error_hub.h"        // live_edit::HubErrorSink
#include "ecs/bridge_error_hub.h"       // scene::make_hub_error_sink
#include "check.h"

#include <cstdio>
#include <string>

using matter::evt::Hub;
using matter::evt::Subscription;
namespace scene = matter::scene;

// ---------------------------------------------------------------------------
// 1. Live-edit ErrorSink adapter: report() emits error.live_edit immediate,
//    carrying LiveEditError's cause/part/message/where verbatim.
// ---------------------------------------------------------------------------
static void test_live_edit_sink_emits_error_live_edit() {
    printf("[test_live_edit_sink_emits_error_live_edit]\n");
    Hub hub;
    int seen = 0;
    matter::events::ErrorLiveEdit got;
    Subscription sub = hub.must_subscribe<matter::events::ErrorLiveEdit>(
        "test.error_live_edit", matter::evt::immediate,
        [&](const matter::events::ErrorLiveEdit& e) { ++seen; got = e; });

    // The sink is constructed with the hub; callers (LiveEditSession) are
    // unchanged — they hold an ErrorSink& and call report().
    live_edit::HubErrorSink sink(hub);
    live_edit::ErrorSink& iface = sink;  // exercise through the base interface

    live_edit::LiveEditError err;
    err.cause   = live_edit::LiveEditError::Cause::FlattenFailed;
    err.part    = "obj/Tree";
    err.message = "budget exceeded during flatten";
    err.where   = "Tree.js:42";
    iface.report(err);

    CHECK(seen == 1, "error.live_edit delivered exactly once on report()");
    CHECK(got.cause == static_cast<uint8_t>(live_edit::LiveEditError::Cause::FlattenFailed),
          "error.live_edit cause matches LiveEditError::Cause");
    CHECK(got.part == "obj/Tree", "error.live_edit part matches");
    CHECK(got.message == "budget exceeded during flatten", "error.live_edit message matches");
    CHECK(got.where == "Tree.js:42", "error.live_edit where matches");
    printf("ok live_edit_sink_emits_error_live_edit\n");
}

// ---------------------------------------------------------------------------
// 2. Bridge sink adapter: on_error emits error.part_instance immediate with
//    the SceneEntityId + PartInstanceError code/hash; on_error_clear emits
//    error.part_instance_clear with the id.
// ---------------------------------------------------------------------------
static void test_bridge_sink_emits_part_instance_events() {
    printf("[test_bridge_sink_emits_part_instance_events]\n");
    Hub hub;
    int errs = 0, clears = 0;
    matter::events::ErrorPartInstance got_err;
    matter::events::ErrorPartInstanceClear got_clear;

    Subscription s_err = hub.must_subscribe<matter::events::ErrorPartInstance>(
        "test.error_part_instance", matter::evt::immediate,
        [&](const matter::events::ErrorPartInstance& e) { ++errs; got_err = e; });
    Subscription s_clear = hub.must_subscribe<matter::events::ErrorPartInstanceClear>(
        "test.error_part_instance_clear", matter::evt::immediate,
        [&](const matter::events::ErrorPartInstanceClear& e) { ++clears; got_clear = e; });

    // The factory yields a plain BridgeErrorSink — the reconcile call site is
    // unchanged (it still takes a const BridgeErrorSink&).
    scene::BridgeErrorSink sink = scene::make_hub_error_sink(hub);

    scene::SceneEntityId id{0xABCDEF01u};
    scene::PartInstanceError pe{scene::PartInstanceErrorCode::RendererCapacity, 0x1234u};
    sink.on_error(id, pe);

    CHECK(errs == 1, "error.part_instance delivered exactly once on on_error()");
    CHECK(got_err.id.value == 0xABCDEF01u, "error.part_instance id matches SceneEntityId");
    CHECK(got_err.code == scene::PartInstanceErrorCode::RendererCapacity,
          "error.part_instance code matches PartInstanceError::code");
    CHECK(got_err.part_hash == 0x1234u, "error.part_instance part_hash matches");

    sink.on_error_clear(id);
    CHECK(clears == 1, "error.part_instance_clear delivered exactly once on on_error_clear()");
    CHECK(got_clear.id.value == 0xABCDEF01u, "error.part_instance_clear id matches");
    printf("ok bridge_sink_emits_part_instance_events\n");
}

int main() {
    printf("=== error_events_tests ===\n");
    test_live_edit_sink_emits_error_live_edit();
    test_bridge_sink_emits_part_instance_events();
    if (g_failures == 0) { printf("ALL PASS\n"); return 0; }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
