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
// ---------------------------------------------------------------------------
// Using these events
// ---------------------------------------------------------------------------
// These are payload structs only — no behavior, no base class, no virtuals.
// Every field carries a default initializer, so `BakeStarted{}` and partial
// aggregate init are both well-formed. MT_EVENT_NAME
// (matter/event/event_name.h) declares the dotted registry/trace name plus a
// stable per-type id, and needs the trailing semicolon.
//
//   hub.emit(matter::events::BakeFinished{errors});               // producer
//   auto sub = hub.must_subscribe<matter::events::BakeFinished>(  // consumer
//       "hud", lane_or_immediate, [](const auto& e) { ... });
//
// evt::Hub lives in matter/event/event_hub.h. `must_subscribe` is
// [[nodiscard]] and hands back a Subscription that owns the registration
// (matter/event/subscription.h) — keep it alive for as long as you want the
// callback.
//
// Cost: Hub::emit takes the event BY VALUE and each queued lane stores its
// own copied envelope, so every std::string member here is copied once per
// emit and again once per distinct subscribed lane. bake.part_done fires per
// part milestone, so keep this payload small.
//
// Gotcha: a field added here does NOT reach legacy poll_event consumers until
// to_legacy_event() in src/matter_engine.cpp (one overload per struct below)
// copies it across — the compat shim is hand-written, not generated.
//
// bake_generation (smart-dune.7): every struct here carries it, and NOTHING
// here sets it — `WorldSession::Impl::emit_bake()` stamps it on the way to
// `hub_.emit()`, which is why every bake emit site in matter_engine.cpp goes
// through that helper instead of calling `hub_.emit` directly. Emit through
// the helper or the event ships with generation 0 and is unattributable.
#pragma once
#include <cstdint>
#include <string>

#include "matter/event/event_name.h"
#include "matter/events.h"  // BakeErrorCode (shared classification enum)

namespace matter::events {

// bake.started — a BakeAll / Reload / RebakeCone run has begun. The legacy
// Event carried no payload for this type (all defaults), so neither does
// this struct.
// Because it has no payload, a consumer cannot tell which of the three run
// kinds started from the event alone; code that must distinguish them has to
// track the command it issued.
struct BakeStarted {
    MT_EVENT_NAME("bake.started");
    // Bake run that produced this event; see matter/events.h. Stamped by
    // WorldSession::Impl::emit_bake(), never by the emit site.
    uint64_t bake_generation = 0;
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
    // Bake run that produced this event; see matter/events.h. Stamped by
    // WorldSession::Impl::emit_bake(), never by the emit site.
    uint64_t bake_generation = 0;
};

// bake.finished — the initial publish completed. `errors` is the
// skip-and-continue failed-part count (0 = clean).
struct BakeFinished {
    MT_EVENT_NAME("bake.finished");
    int errors = 0;
    // Bake run that produced this event; see matter/events.h. Stamped by
    // WorldSession::Impl::emit_bake(), never by the emit site.
    uint64_t bake_generation = 0;
};

// bake.error — a structured bake failure (skip-and-continue or fatal,
// classified by `code`). `module` may be empty for whole-pipeline errors.
struct BakeError {
    MT_EVENT_NAME("bake.error");
    std::string module;                  // failed part (may be empty)
    std::string message;                 // human-readable detail
    std::string phase;                   // "" | install | parts | gl | cone | tileset | stream
    BakeErrorCode code = BakeErrorCode::None;
    // Bake run that produced this event; see matter/events.h. Stamped by
    // WorldSession::Impl::emit_bake(), never by the emit site.
    uint64_t bake_generation = 0;
};

// bake.aborted — the TERMINAL event for a bake run that ended without
// publishing: a fatal install / compose / publish failure, a cancellation, or
// an exception that unwound out of the bake worker. Exactly one of
// bake.finished or bake.aborted follows every bake.started for a given
// bake_generation, so a consumer tracking a bake never has to time out to
// learn it ended, and never has to guess which bake.error was fatal — the
// fatal one is the one this event repeats.
//
// Nobody emits this by hand. `WorldSession::Impl::end_bake_run()` emits it
// from the bake run's scope guard when the run is about to leave the worker's
// command switch with no bake.finished behind it, filling module/message/
// phase/code in from the LAST bake.error the run emitted (Internal plus a
// "no terminal event" message if it somehow emitted none). That is what makes
// the contract hold for `return` paths nobody remembered to annotate and for
// exceptions alike. A run that never emitted bake.started (a RebakeCone with
// no changed paths, say) emits nothing.
struct BakeAborted {
    MT_EVENT_NAME("bake.aborted");
    std::string module;                  // last failing part (may be empty)
    std::string message;                 // detail from the last bake.error
    std::string phase;                   // phase the run died in
    BakeErrorCode code = BakeErrorCode::None;   // Cancelled for a cancelled run
    int errors = 0;                      // bake.errors this run emitted
    // Bake run that produced this event; see matter/events.h. Stamped by
    // WorldSession::Impl::emit_bake(), never by the emit site.
    uint64_t bake_generation = 0;
};

}  // namespace matter::events
