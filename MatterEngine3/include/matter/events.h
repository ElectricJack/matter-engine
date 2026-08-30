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
                       RefineTileDone };

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
    std::string phase;         // "install" | "compose" | "parts" | "gl" | "cone" | "tileset" | ""
    BakeErrorCode code = BakeErrorCode::None;   // BakeError classification
    int errors = 0;            // BakeFinished: failed-part count (skip-and-continue)
    // --- Phase C Task 6 additions (struct is append-only) ---
    // RefineTileDone: identity of the tile that was upgraded or evicted.
    // Both are -1 when the event does not correspond to a specific (tx,tz) tile.
    int tile_tx = -1;          // Terrain tx param
    int tile_tz = -1;          // Terrain tz param
};

} // namespace matter
