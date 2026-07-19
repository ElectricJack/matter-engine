#include "streaming_systems.h"

#include "matter/ecs.h"
#include "matter/streaming.h"
#include "../streaming/sector_streaming_coordinator.h"

namespace matter::streaming::detail {
namespace {

struct StreamingArbitration {
    flecs::entity_t active_owner = 0;
    flecs::entity_t published_owner = 0;
};

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
        coordinator.submit_anchor(
            owner,
            transform->matrix.m[3],
            transform->matrix.m[11]);
    } else {
        coordinator.clear_anchor(owner);
    }
    publish_streaming_snapshot(world, coordinator.snapshot());
}

} // namespace

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
