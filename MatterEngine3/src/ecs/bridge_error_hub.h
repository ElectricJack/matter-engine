// bridge_error_hub.h — hub-backed BridgeErrorSink adapter (I.11).
//
// Design authority: MatterEngine3/docs/event-system.md S I.11 (Notification
// sink row) + S I.6 (immediate delivery). make_hub_error_sink is the
// "one-line adapter (construct with a hub, emit)" the migration calls for: it
// returns a scene::BridgeErrorSink whose two callbacks republish each
// per-entity error as an immediate events::ErrorPartInstance /
// events::ErrorPartInstanceClear on the session hub. Because it still yields a
// plain BridgeErrorSink, the reconcile call site
// (DynamicSceneBridge::reconcile(world, sink, err)) is UNCHANGED.
//
// The BridgeErrorSink struct is intentionally NOT deleted in this pass (the
// spec's "then delete" is a later step once every consumer subscribes).
#pragma once

#include "ecs/bridge_error_sink.h"
#include "matter/event/event_hub.h"
#include "matter/events/error_events.h"

namespace matter::scene {

// Build a BridgeErrorSink that emits immediate error.part_instance{,_clear}
// events on `hub`. Fires on the thread running the bridge reconcile (the
// app/render thread) — see error_events.h for the threading contract. `hub`
// must outlive the returned sink (in practice the sink is a per-frame local
// and the hub is the session's).
inline BridgeErrorSink make_hub_error_sink(evt::Hub& hub) {
    BridgeErrorSink sink;
    sink.on_error = [&hub](SceneEntityId id, PartInstanceError error) {
        events::ErrorPartInstance ev;
        ev.id        = id;
        ev.code      = error.code;
        ev.part_hash = error.part_hash;
        hub.emit(std::move(ev));
    };
    sink.on_error_clear = [&hub](SceneEntityId id) {
        events::ErrorPartInstanceClear ev;
        ev.id = id;
        hub.emit(std::move(ev));
    };
    return sink;
}

}  // namespace matter::scene
