// MatterEngine3/src/ecs/streaming_systems.h
//
// Internal interface between the sector-streaming coordinator and the ECS. Not
// part of the public `matter/streaming.h` surface — everything here is in
// `matter::streaming::detail` and is used by the streaming module registration
// and the streaming tests.
//
// `Coordinator` and `Snapshot` are forward-declared on purpose so including
// this header does not drag in sector_streaming_coordinator.h.

#pragma once

#include "flecs.h"

namespace matter::streaming::detail {

class Coordinator;
struct Snapshot;

// Singleton binding a world to its streaming coordinator. NON-OWNING: the
// coordinator outlives the world from the ECS's point of view, and a null
// `value` (or the singleton being absent) makes every streaming observer and
// system a silent no-op, which is the state of a world that imported the
// streaming module but never started streaming.
struct StreamingContextRef {
    Coordinator* value = nullptr;
};

// Installs the owner claim/release observers, the arbitration singleton and the
// per-frame "MatterStreamingUpdate" system. Call once per world.
void register_streaming_systems(flecs::world& world);
// Copies a coordinator snapshot onto the owner entity as engine-written
// components, handling handover and cleanup. Called every frame by the update
// system; exposed here so tests can drive publication without a live worker.
void publish_streaming_snapshot(
    flecs::world& world,
    const Snapshot& snapshot);

} // namespace matter::streaming::detail
