// MatterEngine3/src/ecs/streaming_systems.cpp
//
// The ECS half of sector streaming. The streaming work itself lives in
// `matter::streaming::detail::Coordinator`
// (src/streaming/sector_streaming_coordinator.h); this file only decides which
// entity drives it and moves data across the ECS boundary in both directions.
//
// OWNERSHIP ARBITRATION. Exactly one entity at a time may own the streaming
// anchor. Adding or re-setting a `SectorStreaming` tag runs
// `claim_streaming_owner`, which asks the coordinator to attach; if another
// entity already holds it, the loser gets a `SectorStreamingError` with
// `OwnerAlreadyClaimed` and the winner's id instead of an attachment. Removing
// the tag runs `release_streaming_owner`. The `StreamingArbitration` singleton
// caches two ids: who the coordinator currently intends as owner, and which
// entity the engine last WROTE published components onto — the latter is what
// lets the publish step clean up the previous owner's components when the role
// moves.
//
// PER-FRAME FLOW. "MatterStreamingUpdate" runs in the `StreamingUpdate` phase,
// tagged `ecs::FramePipelineSystem`, so once per rendered frame:
//   1. read the owner entity's `ecs::WorldTransform`
//   2. `Coordinator::submit_anchor` with its world-space position in metres
//      (or `clear_anchor` when the entity has no WorldTransform yet)
//   3. `publish_streaming_snapshot` copies the coordinator's snapshot back onto
//      the owner as `SectorStreamingStatus` (+ `SectorStreamingError`).
// A dead owner is detached first, so a destroyed anchor entity releases the
// role rather than stalling streaming.
//
// THREADING. Everything here runs on the ECS tick. The coordinator is the
// thread boundary: it takes its own mutex internally, and `snapshot()` returns
// a consistent point-in-time copy rather than a live view of worker state.
//
// The components written here (`SectorStreamingStatus`, `SectorStreamingError`)
// are engine-owned outputs — see matter/streaming.h; callers must not set them.

#include "streaming_systems.h"

#include "matter/ecs.h"
#include "matter/streaming.h"
#include "../streaming/sector_streaming_coordinator.h"

namespace matter::streaming::detail {
namespace {

// Private singleton tracking the two owner identities that can legitimately
// differ for a moment. Registered and seeded by `register_streaming_systems`;
// never exposed to callers.
struct StreamingArbitration {
    flecs::entity_t active_owner = 0;     // Coordinator::intended_owner(), mirrored
    flecs::entity_t published_owner = 0;  // entity we last wrote status onto; 0 = none
};

// Strips the engine-written status/error components off a former owner. Safe to
// call with 0 or with an already-destroyed entity.
void remove_published_components(
    flecs::world& world,
    flecs::entity_t owner) {
    if (owner == 0 || !world.is_alive(owner)) {
        return;
    }

    flecs::entity entity = world.entity(owner);
    entity.remove<SectorStreamingStatus>();
    entity.remove<SectorStreamingError>();
}

// Tries to make `entity` the streaming anchor owner. On refusal it records
// `OwnerAlreadyClaimed` on the entity together with the id of whoever actually
// holds the role, and returns without touching arbitration. A world with no
// StreamingContextRef (streaming not enabled) is a silent no-op.
//
// Note the success path also covers the re-claim case: if the coordinator
// already considers this entity the intended owner, `attach` returning false is
// not an error.
void claim_streaming_owner(flecs::entity entity) {
    flecs::world world = entity.world();
    const StreamingContextRef* ref = world.try_get<StreamingContextRef>();
    const StreamingArbitration* current =
        world.try_get<StreamingArbitration>();
    if (ref == nullptr || ref->value == nullptr || current == nullptr) {
        return;
    }

    const bool attached = ref->value->attach(entity.id());
    const flecs::entity_t active_owner = ref->value->intended_owner();
    if (!attached && active_owner != entity.id()) {
        entity.set<SectorStreamingError>({
            SectorStreamingErrorCode::OwnerAlreadyClaimed,
            active_owner});
        return;
    }

    world.set<StreamingArbitration>(
        StreamingArbitration{active_owner, current->published_owner});
    entity.remove<SectorStreamingError>();
}

void release_streaming_owner(flecs::entity entity) {
    flecs::world world = entity.world();
    const StreamingContextRef* ref = world.try_get<StreamingContextRef>();
    const StreamingArbitration* current =
        world.try_get<StreamingArbitration>();
    if (ref == nullptr || ref->value == nullptr || current == nullptr) {
        return;
    }

    ref->value->detach(entity.id());
    world.set<StreamingArbitration>(
        StreamingArbitration{
            ref->value->intended_owner(),
            current->published_owner});
}

// One frame of anchor sampling, called by "MatterStreamingUpdate". Handles the
// three cases — no owner, dead owner (detach first), live owner — and always
// ends by publishing the coordinator's current snapshot, so status keeps
// flowing even on the frames where no anchor was submitted.
void sample_streaming_anchor(
    flecs::world& world,
    Coordinator& coordinator) {
    const StreamingArbitration* arbitration =
        world.try_get<StreamingArbitration>();
    if (arbitration == nullptr) {
        return;
    }

    const flecs::entity_t owner = coordinator.intended_owner();
    if (owner == 0) {
        publish_streaming_snapshot(world, coordinator.snapshot());
        return;
    }

    if (!world.is_alive(owner)) {
        coordinator.detach(owner);
        world.set<StreamingArbitration>(
            StreamingArbitration{
                coordinator.intended_owner(),
                arbitration->published_owner});
        publish_streaming_snapshot(world, coordinator.snapshot());
        return;
    }

    const flecs::entity entity = world.entity(owner);
    const ecs::WorldTransform* transform =
        entity.try_get<ecs::WorldTransform>();
    if (transform != nullptr) {
        // m[7] is the transform's Y translation, threaded alongside m[3]/m[11]
        // by volumetric-sectors M1 and READ since M3-WP2: under
        // `volumetricSectors` it is the third term in `tile_near_dist`, which
        // is what turns the bands from cylinders into spheres. With the flag
        // off it still reaches SectorStreamer::update and is still read by
        // nothing there. Wiring it a milestone early is why the octree's first
        // diff was the selection rule alone.
        coordinator.submit_anchor(
            owner,
            transform->matrix.m[3],
            transform->matrix.m[7],
            transform->matrix.m[11]);
    } else {
        coordinator.clear_anchor(owner);
    }
    publish_streaming_snapshot(world, coordinator.snapshot());
}

} // namespace

// Copies a coordinator snapshot onto the owner entity as `SectorStreamingStatus`
// and, when one applies, `SectorStreamingError`. Also the ownership-handover
// point: components are removed from the previously published owner before the
// new one is written, and an owner-less snapshot clears them entirely.
//
// Drops the snapshot on the floor — writing nothing — when it names an owner
// the coordinator no longer intends, when that entity is dead, or when it no
// longer carries the `SectorStreaming` tag. That is the normal way a snapshot
// captured just before a handover is discarded, not an error.
//
// The error component is only re-set when its contents actually changed, to
// avoid an OnSet storm every frame.
void publish_streaming_snapshot(
    flecs::world& world,
    const Snapshot& snapshot) {
    const StreamingContextRef* ref = world.try_get<StreamingContextRef>();
    const StreamingArbitration* current =
        world.try_get<StreamingArbitration>();
    if (ref == nullptr || ref->value == nullptr || current == nullptr) {
        return;
    }

    StreamingArbitration next = *current;
    next.active_owner = ref->value->intended_owner();
    if (snapshot.owner == 0) {
        remove_published_components(world, next.published_owner);
        next.published_owner = 0;
        world.set<StreamingArbitration>(next);
        return;
    }

    if (snapshot.owner != ref->value->intended_owner() ||
        !world.is_alive(snapshot.owner)) {
        return;
    }

    flecs::entity owner = world.entity(snapshot.owner);
    if (!owner.has<SectorStreaming>()) {
        return;
    }

    if (next.published_owner != snapshot.owner) {
        remove_published_components(world, next.published_owner);
    }
    owner.set<SectorStreamingStatus>(snapshot.status);
    if (snapshot.error.code != SectorStreamingErrorCode::None) {
        const SectorStreamingError* current =
            owner.try_get<SectorStreamingError>();
        if (current == nullptr || current->code != snapshot.error.code ||
            current->active_owner != snapshot.error.active_owner) {
            owner.set<SectorStreamingError>(snapshot.error);
        }
    } else {
        owner.remove<SectorStreamingError>();
    }
    next.published_owner = snapshot.owner;
    world.set<StreamingArbitration>(next);
}

// Registers the singletons, the claim/release observers and the per-frame
// update system. Called once per world by `streaming::StreamingModule`; the
// world must already have imported `ecs::CoreModule` so the phase chain and
// `ecs::WorldTransform` exist. The actual coordinator is bound separately by
// setting the `StreamingContextRef` singleton — until then every path here is a
// no-op.
void register_streaming_systems(flecs::world& world) {
    world.component<StreamingContextRef>("StreamingContextRef");
    world.component<StreamingArbitration>("StreamingArbitration");
    world.set<StreamingArbitration>({});

    world.observer("ClaimSectorStreamingOwner")
        .event(flecs::OnAdd)
        .event(flecs::OnSet)
        .with<SectorStreaming>()
        .each([](flecs::entity entity) {
            claim_streaming_owner(entity);
        });

    world.observer("ReleaseSectorStreamingOwner")
        .event(flecs::OnRemove)
        .with<SectorStreaming>()
        .each([](flecs::entity entity) {
            release_streaming_owner(entity);
        });

    flecs::system system =
        world.system<const StreamingContextRef>("MatterStreamingUpdate")
            .term_at(0).src<StreamingContextRef>()
            .kind<StreamingUpdate>()
            .read<ecs::WorldTransform>()
            .each([](
                flecs::iter& iterator,
                size_t,
                const StreamingContextRef& ref) {
                if (ref.value != nullptr) {
                    flecs::world world = iterator.world();
                    sample_streaming_anchor(world, *ref.value);
                }
            });
    system.add<ecs::FramePipelineSystem>();
}

} // namespace matter::streaming::detail
