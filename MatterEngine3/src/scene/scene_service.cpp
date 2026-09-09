// scene/scene_service.cpp — see scene_service.h for the design.
#include "scene/scene_service.h"

#include "matter/ecs.h"
#include "matter/physics.h"
#include "matter/character.h"
#include "matter/streaming.h"

namespace matter::scene {
namespace {

// Add (default-construct) a component of `kind`. Returns false for an
// unrecognized kind.
bool add_kind(flecs::entity e, ComponentKind kind) {
    switch (kind) {
        case ComponentKind::Transform:          e.set<ecs::LocalTransform>({}); return true;
        case ComponentKind::RigidBody:          e.set<physics::RigidBody>({}); return true;
        case ComponentKind::Velocity:           e.set<physics::PhysicsVelocity>({}); return true;
        case ComponentKind::SphereCollider:     e.set<physics::SphereCollider>({}); return true;
        case ComponentKind::CapsuleCollider:    e.set<physics::CapsuleCollider>({}); return true;
        case ComponentKind::BoxCollider:        e.set<physics::BoxCollider>({}); return true;
        case ComponentKind::ConvexHullCollider: e.set<physics::ConvexHullCollider>({}); return true;
        case ComponentKind::PartInstance:       e.set<PartInstance>({}); return true;
        case ComponentKind::SectorStreaming:    e.add<streaming::SectorStreaming>(); return true;
        case ComponentKind::CharacterController: {
            character::CharacterController controller;
            std::string error;
            if (!validate_character_component(e, controller, error)) return false;
            e.set<character::CharacterController>(controller);
            return true;
        }
    }
    return false;
}

// Remove a component of `kind`. Removing an absent component is a Flecs no-op.
bool remove_kind(flecs::entity e, ComponentKind kind) {
    switch (kind) {
        case ComponentKind::Transform:          e.remove<ecs::LocalTransform>(); return true;
        case ComponentKind::RigidBody:          e.remove<physics::RigidBody>(); return true;
        case ComponentKind::Velocity:           e.remove<physics::PhysicsVelocity>(); return true;
        case ComponentKind::SphereCollider:     e.remove<physics::SphereCollider>(); return true;
        case ComponentKind::CapsuleCollider:    e.remove<physics::CapsuleCollider>(); return true;
        case ComponentKind::BoxCollider:        e.remove<physics::BoxCollider>(); return true;
        case ComponentKind::ConvexHullCollider: e.remove<physics::ConvexHullCollider>(); return true;
        case ComponentKind::PartInstance:       e.remove<PartInstance>(); return true;
        case ComponentKind::SectorStreaming:    e.remove<streaming::SectorStreaming>(); return true;
        case ComponentKind::CharacterController: e.remove<character::CharacterController>(); return true;
    }
    return false;
}

// Copy every present SceneRecord component value from `src` to `dst`.
template <class C>
void copy_one(flecs::entity src, flecs::entity dst) {
    if (const C* v = src.try_get<C>()) dst.set<C>(*v);
}

void copy_components(flecs::entity src, flecs::entity dst) {
    copy_one<ecs::LocalTransform>(src, dst);
    copy_one<physics::RigidBody>(src, dst);
    copy_one<physics::PhysicsVelocity>(src, dst);
    copy_one<physics::SphereCollider>(src, dst);
    copy_one<physics::CapsuleCollider>(src, dst);
    copy_one<physics::BoxCollider>(src, dst);
    copy_one<physics::ConvexHullCollider>(src, dst);
    copy_one<PartInstance>(src, dst);
    copy_one<character::CharacterController>(src, dst);
    if (src.has<character::CharacterController>()) dst.set<character::MoveIntent>({});
    if (src.has<streaming::SectorStreaming>()) dst.add<streaming::SectorStreaming>();
}

} // namespace

SceneService::SceneService(flecs::world& world) : world_(world) {}

// Resolve a SceneEntityId by scanning every SceneEntityId-bearing entity.
// flecs::world::each has no early exit, so the full set is visited even after a
// match (and with duplicate ids the LAST match wins). Every entry point below
// calls this at least once and allocate_id() calls it once per candidate, so a
// single scene edit is O(live scene entities). id 0 is the "no id" sentinel and
// resolves to an invalid entity without scanning.
flecs::entity SceneService::find_entity(SceneEntityId id) const {
    if (id.value == 0) return flecs::entity();
    flecs::entity found;
    world_.each([&](flecs::entity e, SceneEntityId& sid) {
        if (sid.value == id.value) found = e;
    });
    return found;
}

uint64_t SceneService::allocate_id() {
    // Runtime ids live in the HIGH half of the SceneEntityId::value space:
    // kRuntimeIdBit set, the monotonic counter in the low 63 bits. Authored
    // ids are hash_authored_id() FNV-1a hashes with that bit cleared
    // (scene_registry.h), so the two allocators are disjoint by construction.
    // That is what actually upholds the no-collision claim — the liveness scan
    // below only sees CURRENTLY loaded entities, so on its own it could not
    // stop a later world reload from bringing in an authored id this service
    // had already handed out. The scan remains as a guard against a collision
    // inside the runtime half itself (a duplicated id, or a counter that wraps
    // in a session that outlives 2^63 allocations).
    //
    // Masking the counter rather than trusting it to stay in range also keeps
    // the invariant true at wraparound: the candidate always has the bit set
    // and is therefore never the 0 "no id" sentinel.
    while (true) {
        const uint64_t candidate = kRuntimeIdBit | (next_id_++ & ~kRuntimeIdBit);
        if (!find_entity(SceneEntityId{candidate}).is_valid())
            return candidate;
    }
}

SceneEditResult SceneService::create_empty(const std::string& name) {
    SceneEditResult result;
    uint64_t id = allocate_id();
    flecs::entity e = world_.entity();
    e.set<SceneEntityId>({id});
    e.set<ecs::LocalTransform>({});
    if (!name.empty()) e.set_name(name.c_str());
    result.created_id = SceneEntityId{id};
    return result;
}

// Duplicates the entity ITSELF only — children are NOT copied. The result is a
// sibling of the source: same display name, same parent link, and a value copy
// of every SceneRecord component present on the source (copy_components above;
// the SectorStreaming tag is re-added rather than copied, since it holds no
// data). A source that is a root, or whose parent is an internal non-scene
// entity, yields a root. The new id comes from allocate_id().
SceneEditResult SceneService::duplicate(SceneEntityId src) {
    SceneEditResult result;
    flecs::entity source = find_entity(src);
    if (!source.is_valid()) {
        result.error = SceneEditError::EntityNotFound;
        return result;
    }
    uint64_t id = allocate_id();
    flecs::entity e = world_.entity();
    e.set<SceneEntityId>({id});
    if (const char* n = source.name().c_str()) {
        if (n[0] != '\0') e.set_name(n);
    }
    copy_components(source, e);
    flecs::entity parent = source.parent();
    if (parent.is_valid() && parent.try_get<SceneEntityId>() != nullptr)
        e.child_of(parent);
    result.created_id = SceneEntityId{id};
    return result;
}

SceneEditResult SceneService::delete_entity(SceneEntityId target) {
    SceneEditResult result;
    flecs::entity e = find_entity(target);
    if (!e.is_valid()) {
        result.error = SceneEditError::EntityNotFound;
        return result;
    }
    // Flecs' built-in ChildOf ownership deletes the whole subtree; the
    // tracker's OnRemove observer fires per descendant, so the removal batch
    // includes every cascade-removed id (S I.14).
    e.destruct();
    return result;
}

SceneEditResult SceneService::reparent(SceneEntityId child, SceneEntityId new_parent) {
    SceneEditResult result;
    flecs::entity c = find_entity(child);
    flecs::entity p = find_entity(new_parent);
    if (!c.is_valid() || !p.is_valid()) {
        result.error = SceneEditError::EntityNotFound;
        return result;
    }
    if (c == p) {
        result.error = SceneEditError::InvalidTarget;
        return result;
    }
    // Cycle check: new_parent must not be `child` itself or any descendant of
    // child. Walk new_parent's ancestor chain; if it passes through child, the
    // reparent would create a cycle.
    for (flecs::entity a = p; a.is_valid(); a = a.parent()) {
        if (a == c) {
            result.error = SceneEditError::CycleDetected;
            return result;
        }
    }
    c.child_of(p);
    return result;
}

SceneEditResult SceneService::rename(SceneEntityId id, const std::string& name) {
    SceneEditResult result;
    flecs::entity e = find_entity(id);
    if (!e.is_valid()) {
        result.error = SceneEditError::EntityNotFound;
        return result;
    }
    e.set_name(name.c_str());
    return result;
}

SceneEditResult SceneService::add_component(SceneEntityId id, ComponentKind kind) {
    SceneEditResult result;
    flecs::entity e = find_entity(id);
    if (!e.is_valid()) {
        result.error = SceneEditError::EntityNotFound;
        return result;
    }
    if (!add_kind(e, kind)) result.error = SceneEditError::InvalidTarget;
    return result;
}

SceneEditResult SceneService::remove_component(SceneEntityId id, ComponentKind kind) {
    SceneEditResult result;
    flecs::entity e = find_entity(id);
    if (!e.is_valid()) {
        result.error = SceneEditError::EntityNotFound;
        return result;
    }
    if (!remove_kind(e, kind)) result.error = SceneEditError::InvalidTarget;
    return result;
}

} // namespace matter::scene
