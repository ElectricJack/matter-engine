// matter/events/physics_events.h — physics-domain hub events (E6).
//
// Design authority: MatterEngine3/docs/event-system.md
//   S I.4  — approach C: entity-shaped gameplay events (contact/sensor per
//            entity) go through flecs, NOT the in-house hub. Those payload
//            structs live in matter/physics.h (PhysContactBegin, ...).
//   S I.8  — flecs bridge: "ECS-side emissions mirror one record into the
//            trace (name-prefixed) ... Mirroring is trace-only — no double
//            dispatch."
//   S I.11 — PhysicsEvents migration row: "Aggregate per-step mirror into the
//            hub trace for the inspector."
//
// This header holds ONLY the aggregate mirror event. The physics pull stage
// emits one PhysStep onto the session hub for every fixed step that produced
// contact or sensor activity, so the E4 Events inspector shows physics in the
// unified timeline alongside bake/stream/scene events. It is a hub event (a
// non-entity, cross-cutting per-step summary), not a per-entity gameplay event.
#pragma once
#include <cstdint>

#include "matter/event/event_name.h"

namespace matter::events {

// phys.step — one fixed physics step produced `contacts` contact-pair
// transitions (begin + end) and `sensors` sensor-pair transitions (enter +
// exit). Emitted only when at least one occurred, so an idle sim is silent in
// the trace. The per-entity events (physics::PhysContactBegin, ...) carry the
// gameplay detail; this is the inspector-facing aggregate.
struct PhysStep {
    MT_EVENT_NAME("phys.step");
    uint32_t contacts = 0;
    uint32_t sensors = 0;
};

}  // namespace matter::events
