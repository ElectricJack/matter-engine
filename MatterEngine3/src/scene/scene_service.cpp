// scene/scene_service.cpp — see scene_service.h for the design.
#include "scene/scene_service.h"

#include "matter/ecs.h"
#include "matter/physics.h"
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
    if (src.has<streaming::SectorStreaming>()) dst.add<streaming::SectorStreaming>();
}

} // namespace

SceneService::SceneService(flecs::world& world) : world_(world) {}

flecs::entity SceneService::find_entity(SceneEntityId id) const {
    if (id.value == 0) return flecs::entity();
    flecs::entity found;
    world_.each([&](flecs::entity e, SceneEntityId& sid) {
        if (sid.value == id.value) found = e;
    });
    return found;
}

uint64_t SceneService::allocate_id() {
    // Monotonic candidate, verified unique against live scene ids so a
    // service-created id can never collide with a hash-authored id.
    while (true) {
        uint64_t candidate = next_id_++;
        if (candidate != 0 && !find_entity(SceneEntityId{candidate}).is_valid())
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
