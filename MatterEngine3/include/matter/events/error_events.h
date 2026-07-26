// matter/events/error_events.h — typed error-notification events (completes I.11).
//
// Design authority: MatterEngine3/docs/event-system.md
//   S I.5    — "Declaring an event type": plain struct + MT_EVENT_NAME.
//   S I.11   — migration map, "Notification sink" row: the live-edit
//              ErrorSink (src/live_edit_interfaces.h) and BridgeErrorSink
//              (src/ecs/dynamic_scene_bridge.h) — each a bespoke
//              one-consumer contract for a semantically fan-out
//              notification — become error.live_edit{...},
//              error.part_instance{id,code} / error.part_instance_clear{id}
//              on the session hub. The sink interfaces survive this pass as
//              one-line hub-backed adapters; the eventual step is to delete
//              them once every consumer subscribes.
//   S I.3.1  — these are notifications (fire-and-forget fan-out), not
//              commands: the emitter neither knows nor cares who listens.
//   S I.6    — DELIVERY / THREADING CONTRACT. Both legacy sinks were
//              SYNCHRONOUS, inline, single-consumer calls — report() and the
//              on_error/on_error_clear std::functions ran on whatever thread
//              drove the producer. To preserve that timing exactly these
//              events are intended for IMMEDIATE subscription (evt::immediate):
//                * error.live_edit fires on the thread running the live-edit
//                  rebuild pass (LiveEditSession::rebuild) — the bake WORKER
//                  thread for the RebakeCone command path, or the host tick
//                  thread for LiveEditSession::tick().
//                * error.part_instance{,_clear} fire on the thread running
//                  DynamicSceneBridge::reconcile — the app/render thread,
//                  during the per-frame slot reconcile.
//              hub.emit() is thread-safe from any thread; an immediate
//              subscriber therefore runs inline on the producer's thread,
//              matching the old synchronous callbacks. Consumers that must
//              hop to the app thread subscribe a lane instead.
#pragma once
#include <cstdint>
#include <string>

#include "matter/event/event_name.h"
#include "matter/scene.h"  // scene::SceneEntityId, scene::PartInstanceErrorCode

namespace matter::events {

// error.live_edit — a dev live-edit bake/flatten failed, fail-closed (the
// last-good artifact is kept). Carries exactly what live_edit::LiveEditError
// carried. `cause` mirrors the numeric value of
// live_edit::LiveEditError::Cause (Script=0, SessionMisuse, BudgetExceeded,
// ResolveFailed, FlattenFailed) — kept as a plain uint8_t so this public
// header stays free of the src/ live-edit seam header; the adapter that emits
// this event performs the cast (src/live_edit_error_hub.h).
struct ErrorLiveEdit {
    MT_EVENT_NAME("error.live_edit");
    uint8_t     cause = 0;   // live_edit::LiveEditError::Cause value
    std::string part;        // the part whose bake/flatten failed
    std::string message;     // human-readable detail
    std::string where;       // best-effort "file:line" source location
};

// error.part_instance — the dynamic scene bridge could not realize an
// entity's PartInstance this frame (missing part, renderer capacity
// exhausted, ...). The caller applies this to the ECS world (e.g. a
// PartInstanceError component). Carries the SceneEntityId + the classified
// PartInstanceError the BridgeErrorSink::on_error callback carried.
struct ErrorPartInstance {
    MT_EVENT_NAME("error.part_instance");
    scene::SceneEntityId          id{};
    scene::PartInstanceErrorCode  code = scene::PartInstanceErrorCode::None;
    uint64_t                      part_hash = 0;
};

// error.part_instance_clear — a previously-reported entity error resolved;
// the caller clears the entity's error state. Mirrors the
// BridgeErrorSink::on_error_clear callback.
struct ErrorPartInstanceClear {
    MT_EVENT_NAME("error.part_instance_clear");
    scene::SceneEntityId id{};
};

}  // namespace matter::events
