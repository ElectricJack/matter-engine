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
// ---------------------------------------------------------------------------
// Emission site, conditions and usage
// ---------------------------------------------------------------------------
// PhysicsContext::pull (src/ecs/physics_context.cpp) emits this at the END of
// the pull stage, after the per-entity flecs events for the same fixed step,
// and only when contacts + sensors > 0 — so an idle sim contributes nothing to
// the trace. The hub pointer is optional (PhysicsContext::set_event_hub, null
// by default so headless physics tests need no hub); no hub means no mirror,
// and nothing else about the step changes.
//
// Delivery therefore happens on whichever thread drives the fixed-step pull
// stage; an immediate subscriber runs inline on that thread. Subscribe through
// the session's evt::Hub (matter/event/event_hub.h):
//
//   auto sub = hub.must_subscribe<matter::events::PhysStep>(
//       "events-inspector", lane_or_immediate, [](const auto& e) { ... });
//
// must_subscribe is [[nodiscard]]; the returned Subscription owns the
// registration and must outlive the interest.
#pragma once
#include <cstdint>

#include "matter/event/event_name.h"

namespace matter::events {

// phys.step — one fixed physics step produced `contacts` contact-pair
// transitions (begin + end) and `sensors` sensor-pair transitions (enter +
// exit). Emitted only when at least one occurred, so an idle sim is silent in
// the trace. The per-entity events (physics::PhysContactBegin, ...) carry the
// gameplay detail; this is the inspector-facing aggregate.
// Each counter is a SUM of two transition lists — contact begin + contact end
// for `contacts`, sensor enter + sensor exit for `sensors` — so a pair that
// both begins and ends inside one step contributes 2, and neither number is a
// count of currently-touching pairs. The emit site (physics_context.cpp's
// pull()) assigns the two fields BY NAME rather than aggregate-initializing
// positionally; keep it that way, or inserting a field here silently
// re-assigns what the inspector reports.
struct PhysStep {
    MT_EVENT_NAME("phys.step");
    uint32_t contacts = 0;
    uint32_t sensors = 0;
};

}  // namespace matter::events
