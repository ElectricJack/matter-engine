// matter/events/bake_events.h — typed bake-domain events (E3).
//
// Design authority: MatterEngine3/docs/event-system.md
//   S I.5   — "Declaring an event type": plain struct + MT_EVENT_NAME.
//   S I.11  — migration map, "Bake Event queue" row: the legacy
//             EventType enum (events.h) becomes typed events
//             bake.started/part_done/finished/error on the session hub.
//   S II.1 E3, S II.3.5 — bake.* is the primary migration domain.
//
// These structs replace the append-only `matter::Event` / `EventType`
// dumping ground (matter/events.h): each carries ONLY the fields its
// legacy variant actually populated, so the E3 legacy_poll compat shim
// (WorldSession::poll_event) can reconstruct the exact pre-E3
// `matter::Event` losslessly (matter_engine.cpp to_legacy_event()).
//
// Delivery: every bake/stream site was already queued-to-app via the old
// mutex+deque `events` queue, so the E3 migration preserves that timing —
// the session's private compat subscriptions target lane::legacy_poll and
// poll_event pumps that lane one envelope per call (S II.4 item 6). New
// consumers subscribe lane::app / immediate as they wish; the emitter does
// not know or care.
#pragma once
#include <string>

#include "matter/event/event_name.h"
#include "matter/events.h"  // BakeErrorCode (shared classification enum)

namespace matter::events {

// bake.started — a BakeAll / Reload / RebakeCone run has begun. The legacy
// Event carried no payload for this type (all defaults), so neither does
// this struct.
struct BakeStarted {
    MT_EVENT_NAME("bake.started");
};

// bake.part_done — one part reached a pipeline milestone. `total == 0`
// means the indeterminate install phase; `total > 0` is a concrete count
// that MAY GROW between events as FlatInstanceRefs are discovered (see
// matter/events.h — HUD consumers must not assume a constant total).
struct BakePartDone {
    MT_EVENT_NAME("bake.part_done");
    std::string module;                  // may be empty (ref-streamed children)
    int done = 0, total = 0;
    std::string phase;                   // "install" | "parts" | "tileset"
};

// bake.finished — the initial publish completed. `errors` is the
// skip-and-continue failed-part count (0 = clean).
struct BakeFinished {
    MT_EVENT_NAME("bake.finished");
    int errors = 0;
};

// bake.error — a structured bake failure (skip-and-continue or fatal,
// classified by `code`). `module` may be empty for whole-pipeline errors.
struct BakeError {
    MT_EVENT_NAME("bake.error");
    std::string module;                  // failed part (may be empty)
    std::string message;                 // human-readable detail
    std::string phase;                   // "" | install | parts | gl | cone | tileset | stream
    BakeErrorCode code = BakeErrorCode::None;
};

}  // namespace matter::events
