// matter/events/stream_events.h — typed streaming-domain events (E3).
//
// Design authority: MatterEngine3/docs/event-system.md
//   S I.5   — plain struct + MT_EVENT_NAME.
//   S I.11  — migration map: the legacy EventType::RefineTileDone (bolted
//             onto the bake enum for streaming progress) becomes its own
//             honest `stream.refine_tile` event, dropping the "-1 when
//             unused" tile_tx/tile_tz fields that used to ride on every
//             bake Event (matter/events.h:39-41).
//   S II.1 E3 — stream.refine_tile ships in E3 alongside bake.*.
//
// Emitted by the camera-driven refine loop after a terrain tile is
// upgraded (coarse->full) or evicted (full->coarse). Like bake.*, it was
// already queued-to-app, so the E3 compat shim routes it through
// lane::legacy_poll unchanged.
// ---------------------------------------------------------------------------
// Using this event
// ---------------------------------------------------------------------------
// Payload struct only — no behavior. MT_EVENT_NAME
// (matter/event/event_name.h) declares the dotted registry/trace name and a
// stable per-type id. Emit and subscribe through the session's evt::Hub
// (matter/event/event_hub.h):
//
//   hub.emit(matter::events::RefineTileDone{...});
//   auto sub = hub.must_subscribe<matter::events::RefineTileDone>(
//       "hud", lane_or_immediate, [](const auto& e) { ... });
//
// must_subscribe is [[nodiscard]]; the returned Subscription owns the
// registration and must outlive the interest.
//
// Cost: Hub::emit takes the event by value and every queued lane stores its
// own copied envelope, so `module` is copied once per emit plus once per
// distinct subscribed lane. One of these fires per tile residency change, not
// once per bake.
//
// Gotcha: a field added here is invisible to legacy poll_event consumers until
// the hand-written to_legacy_event(const events::RefineTileDone&) overload in
// src/matter_engine.cpp copies it across.
#pragma once
#include <cstdint>
#include <string>

#include "matter/event/event_name.h"

namespace matter::events {

// stream.refine_tile — a terrain refine tile changed residency. `done` is
// the current full-resident tile count; `total` is the RefineController's
// total tile count. The legacy Event always tagged phase="refine" and
// module="Terrain"; module is carried here (currently always "Terrain")
// and phase is reconstructed by the compat shim.
struct RefineTileDone {
    MT_EVENT_NAME("stream.refine_tile");
    std::string module;                  // "Terrain" (for now)
    int done = 0, total = 0;
    // Integer tile coordinates in the RefineController's tile grid, not world
    // metres; -1/-1 is the struct default for "no tile identified".
    int tile_tx = -1, tile_tz = -1;      // the (tx,tz) tile that changed
    // smart-dune.7: the bake run whose world this tile belongs to (see
    // matter/events.h). Refine runs between bakes, so this is the generation of
    // the LAST bake command, not of a run that is still in flight. Stamped by
    // WorldSession::Impl::emit_bake(), never by the emit site.
    uint64_t bake_generation = 0;
};

}  // namespace matter::events
