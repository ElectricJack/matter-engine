#pragma once

// MatterEngine3/include/matter/streaming.h
//
// Public ECS surface of sector streaming: how a scene entity claims the role of
// STREAMING ANCHOR, and what the engine publishes back about it. The machinery
// lives in `src/ecs/streaming_systems.cpp` (observers + the update system) and
// `src/streaming/sector_streaming_coordinator.cpp` (the worker-side state
// machine that produces the snapshots).
//
// HOW TO USE IT. Import `StreamingModule`, then add a `SectorStreaming` tag to
// the entity whose `ecs::WorldTransform` should drive residency — the camera
// rig, usually. That is the whole authoring surface; everything else here is
// engine output:
//
//   * Adding/setting the tag CLAIMS ownership; removing it releases.
//   * Exactly one owner at a time. A second claimant is refused with
//     `SectorStreamingError{OwnerAlreadyClaimed, <the current owner>}`.
//   * Each `StreamingUpdate` phase samples the owner's world transform and
//     copies the coordinator's latest snapshot onto the owner entity as
//     `SectorStreamingStatus` (+ a `SectorStreamingError` while one applies,
//     removed again when it clears). Do not write those two yourself.
//
// THREADING. The components and this header are ECS-tick affine; the snapshot
// they carry is produced on the coordinator's worker side and read under its own
// mutex, so what lands on the entity is a consistent point-in-time copy rather
// than a live view.

#include <cstdint>
#include "flecs.h"

namespace matter::streaming {
// Tag: "this entity is the streaming anchor." Presence is the whole claim; it
// carries no data.
struct SectorStreaming {};
// Why streaming is not running for the claiming entity.
//   UnsupportedWorld     the connected world has no sector-streaming profile
//   OwnerAlreadyClaimed  another entity already holds the anchor role
enum class SectorStreamingErrorCode : uint8_t {
    None, UnsupportedWorld, OwnerAlreadyClaimed
};
// Set on the claiming entity while an error applies and removed once it clears.
struct SectorStreamingError {
    SectorStreamingErrorCode code = SectorStreamingErrorCode::None;
    flecs::entity_t active_owner = 0;  // only meaningful for OwnerAlreadyClaimed
};
// Where the coordinator is in bringing streaming up, as published by
// `Coordinator::publish_snapshot`:
//   Detached          nobody owns the anchor
//   PendingProfile    owner claimed, the world's stream profile not resolved yet
//   PendingTransform  profile resolved, still waiting for an anchor transform
//   Active            streaming; only in this state are the counters below filled
//   Detaching         declared and reflected to scripts, but publish_snapshot
//                     does not currently produce it
enum class SectorStreamingState : uint8_t {
    Detached, PendingProfile, PendingTransform, Active, Detaching
};
// Engine-written status, copied onto the owner entity each StreamingUpdate.
// Read-only from a caller's point of view. Note that `generation`,
// `resident_sectors` and `inflight_sectors` are only populated while `state ==
// Active` — outside that they read 0 because nothing filled them, not because
// the world emptied.
struct SectorStreamingStatus {
    SectorStreamingState state = SectorStreamingState::Detached;
    uint64_t generation = 0;         // bumped when the anchor re-seeds residency
    uint32_t resident_sectors = 0;   // sectors currently loaded
    uint32_t inflight_sectors = 0;   // requests issued but not yet completed
};
// Pipeline PHASE tag for the anchor-sampling system ("MatterStreamingUpdate"),
// not a component you add to an entity.
struct StreamingUpdate {};
// Import to enable streaming on a world: registers the owner claim/release
// observers, the arbitration singleton and the update system. Without it a
// `SectorStreaming` tag does nothing.
struct StreamingModule { explicit StreamingModule(flecs::world& world); };
} // namespace matter::streaming
