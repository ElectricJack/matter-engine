#pragma once

// MatterEngine3/include/matter/events.h
//
// The engine's outbound progress/error record. A WorldSession queues these
// during bake and streaming; the host drains them one at a time with
// `WorldSession::poll_event` (matter/world_session.h) and drives its HUD,
// console and logs from them. The channel is one-way — nothing here is a
// command into the engine.
//
// Reading the stream:
//   * `Event` is append-only BY CONTRACT. New fields go at the end with
//     neutral defaults so an older consumer keeps compiling and simply
//     ignores them; never reorder or repurpose an existing field. The
//     per-field comments below record which phase added what.
//   * Only some fields are meaningful for a given EventType — the field
//     comments say which.
//   * Progress counters are advisory: `total == 0` means indeterminate, and
//     `total` may GROW mid-sequence, so a progress bar has to tolerate a
//     percentage that goes backwards.
//   * Terminal-event contract (smart-dune.7): every BakeStarted is followed by
//     exactly one terminal event for that `bake_generation` — BakeFinished
//     (the run published) or BakeAborted (the run gave up). A consumer that
//     tracks a bake therefore never has to time out to learn it ended, and
//     never has to guess whether a BakeError was fatal: the fatal ones are the
//     ones followed by BakeAborted. The one documented exception is the second
//     BakeFinished a streamed world emits when its disc fills (see
//     `bake_generation` below). That deferred finish is the contract's known
//     residual: it has no BakeAborted counterpart, so a streamed fill that
//     stalls forever still ends the stream in silence. The BAKE RUN that
//     installed the world is covered; the fill that follows it is not.

#include <cstdint>
#include <string>

namespace matter {

// BakeStarted/BakePartDone/BakeFinished/BakeError are reused for world-kind
// (infinite-world) sessions. In that mode BakeStarted fires at request_bake,
// BakePartDone fires during the asset install phase, and BakeFinished fires
// once the first streaming cycle completes with no remaining holes. BakeError
// may fire for individual sector bake failures (phase = "stream").
enum class EventType { BakeStarted, BakePartDone, BakeFinished, BakeError,
                       // Phase C Task 6: camera-driven refine loop event.
                       // Emitted after each tile is upgraded (coarse→full) or evicted
                       // (full→coarse).  done = current full-resident tile count;
                       // total = total tile count from the RefineController.
                       // phase = "refine".  module = "Terrain" (always, for now).
                       RefineTileDone,
                       // smart-dune.7: the terminal event for a bake run that
                       // ENDED WITHOUT FINISHING — a fatal install/compose/
                       // publish failure, a cancellation, or an exception that
                       // unwound out of the bake worker. Exactly one of
                       // BakeFinished or BakeAborted follows every BakeStarted,
                       // which is what makes "did this bake end, and how?"
                       // answerable from the stream alone. Before it existed a
                       // fatal bake emitted only a BakeError and then went
                       // silent, indistinguishable from the per-part
                       // skip-and-continue BakeErrors that DO go on to a
                       // BakeFinished. Carries the last BakeError's
                       // code/phase/module/message plus `errors` = how many
                       // BakeErrors the run emitted.
                       BakeAborted };

// Structured bake-error classification (Phase B). None on non-error events.
enum class BakeErrorCode { None, Cancelled, OutOfMemory, ScriptError, GpuError, IoError, Internal };

// One queued engine event. Freely copyable; the strings are owned by the
// event. A default-constructed Event is a BakeStarted with every optional
// field neutral, which is what makes the append-only rule safe.
struct Event {
    EventType type = EventType::BakeStarted;
    std::string module;        // BakePartDone/BakeError: part module name (may be empty)
    // BakePartDone counters: total 0 = indeterminate phase.
    // Phase C Task 14 (demand-bake): total may INCREASE between events as
    // FlatInstanceRef children are discovered during publish. HUD consumers
    // must not assume total is constant across a single bake sequence.
    int done = 0, total = 0;
    std::string message;       // BakeError: error detail
    // --- Phase B additions (struct is append-only) ---
    // Task 15: BakePartDone with phase="tileset" may follow BakeFinished (deferred
    // tileset phase runs after the initial publish so silhouette is not blocked).
    std::string phase;         // "install" | "compose" | "parts" | "gl" | "cone" | "tileset" |
                               // "terrain-collision" | "stream" | ""
    BakeErrorCode code = BakeErrorCode::None;   // BakeError/BakeAborted classification
    int errors = 0;            // BakeFinished: failed-part count (skip-and-continue)
                               // BakeAborted: BakeErrors this run emitted
    // --- Phase C Task 6 additions (struct is append-only) ---
    // RefineTileDone: identity of the tile that was upgraded or evicted.
    // Both are -1 when the event does not correspond to a specific (tx,tz) tile.
    int tile_tx = -1;          // Terrain tx param
    int tile_tz = -1;          // Terrain tz param
    // --- smart-dune.7 additions (struct is append-only) ---
    // Identity of the bake run that emitted this event. A monotonic counter
    // bumped once per bake command the worker executes (BakeAll / Reload /
    // RebakeCone), stamped at EMIT time — not at poll time — so an event that
    // was queued by the bake worker just before a consumer's drain is still
    // attributable to the run that produced it. 0 means "emitted before any
    // bake run" (and is the default an older producer leaves behind).
    //
    // Streaming-phase events (phase == "stream", RefineTileDone) carry the
    // generation of the bake run that installed the world they belong to,
    // because the counter only advances on a new bake command.
    //
    // Note a STREAMED (world-kind) world emits TWO BakeFinished events for one
    // generation: one when the roots are published, and one when the first
    // streaming cycle completes with no remaining holes (see the EventType
    // comment above). The generation is what lets a consumer see that the
    // second one is a second finish for the same run rather than a new bake.
    uint64_t bake_generation = 0;
};

} // namespace matter
