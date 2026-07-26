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
#pragma once
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
    int tile_tx = -1, tile_tz = -1;      // the (tx,tz) tile that changed
};

}  // namespace matter::events
