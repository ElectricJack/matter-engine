// MatterEngine3 Vulkan world viewer. The production path creates a GLFW
// NO_API window and presents genuine WorldSession data through VkSceneRenderer.
// MATTER_CAM, MATTER_WORLD, MATTER_HIZ, MATTER_SCREENSHOT and FIFO commands are
// retained from the legacy viewer.
#include "matter/engine_context.h"
#include "matter/vulkan_device.h"
#include "matter/world_session.h"
#include "matter/ecs.h"
#include "matter/physics.h"
#include "matter/scene.h"
#include "matter/streaming.h"
#include "ecs/simulation_control.h"
#include "ecs/scene_registry.h"
#include "scene/scene_service.h"  // E5c: session->scene_service() (world_session.h fwd-decls it)
#include "camera_controller.h"
#include "camera_focus.h"
#include "editor_model.h"
#include "image_preview.h"
#include "issue_reporter.h"
#include "shot_replay.h"
#include "properties_panel.h"
#include "properties_registry.h"
#include "reveal_part.h"
#include "selection_bounds.h"
#include "selection_outline.h"
#include "selection_set.h"
#include "toolbar_panel.h"
#include "console_panel.h"
#include "ui.h"
#include "session_binding.h"
#include "scene_model_adapter.h"
#include "viewer_commands.h"
#include "matter/event/event_hub.h"
#include "matter/event/command.h"
#include "matter/event/property.h"
#include "viewport_pick.h"

#include "imgui.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "external/stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// Properties panel field access (Phase 5 Task 7). There is no generic
// reflection API for ECS components, so field get/set is hardcoded per
// ComponentKind here, dispatching on the component/field name strings that
// PropertiesRegistry hands back (which mirror ecs/scene_registry.h's
// ComponentDescriptor/FieldDescriptor tables).
// ---------------------------------------------------------------------------

// flecs' entity_view::get<T>() returns `const T&` (asserts if absent), not a
// pointer — wrap it as a has<T>()-checked pointer so the field-access helpers
// below can use the usual "null means absent" pattern.
template <typename T>
const T* get_ptr(flecs::entity e) {
    return e.has<T>() ? &e.get<T>() : nullptr;
}

flecs::entity find_scene_entity(flecs::world& world, matter::scene::SceneEntityId id) {
    flecs::entity found;
    world.each([&](flecs::entity e, const matter::scene::SceneEntityId& sid) {
        if (!found && sid.value == id.value) found = e;
    });
    return found;
}

bool collider_prop_get_float(const matter::physics::ColliderProperties& p,
                             const char* field, float& out) {
    if (!std::strcmp(field, "density")) { out = p.density; return true; }
    if (!std::strcmp(field, "friction")) { out = p.friction; return true; }
    if (!std::strcmp(field, "restitution")) { out = p.restitution; return true; }
    return false;
}
bool collider_prop_set_float(matter::physics::ColliderProperties& p,
                             const char* field, float value) {
    if (!std::strcmp(field, "density")) { p.density = value; return true; }
    if (!std::strcmp(field, "friction")) { p.friction = value; return true; }
    if (!std::strcmp(field, "restitution")) { p.restitution = value; return true; }
    return false;
}

bool field_get_float(matter::WorldSession* session, matter::scene::SceneEntityId id,
                     const char* component, const char* field, float& out) {
    if (!session) return false;
    flecs::entity e = find_scene_entity(session->ecs(), id);
    if (!e.is_valid()) return false;

    if (!std::strcmp(component, "RigidBody")) {
        const auto* rb = get_ptr<matter::physics::RigidBody>(e);
        if (!rb) return false;
        if (!std::strcmp(field, "linear_damping")) { out = rb->linear_damping; return true; }
        if (!std::strcmp(field, "angular_damping")) { out = rb->angular_damping; return true; }
        if (!std::strcmp(field, "gravity_scale")) { out = rb->gravity_scale; return true; }
        if (!std::strcmp(field, "sleep_threshold")) { out = rb->sleep_threshold; return true; }
        return false;
    }
    if (!std::strcmp(component, "SphereCollider")) {
        const auto* c = get_ptr<matter::physics::SphereCollider>(e);
        if (!c) return false;
        if (!std::strcmp(field, "radius")) { out = c->radius; return true; }
        return collider_prop_get_float(c->properties, field, out);
    }
    if (!std::strcmp(component, "CapsuleCollider")) {
        const auto* c = get_ptr<matter::physics::CapsuleCollider>(e);
        if (!c) return false;
        if (!std::strcmp(field, "radius")) { out = c->radius; return true; }
        return collider_prop_get_float(c->properties, field, out);
    }
    if (!std::strcmp(component, "BoxCollider")) {
        const auto* c = get_ptr<matter::physics::BoxCollider>(e);
        if (!c) return false;
        return collider_prop_get_float(c->properties, field, out);
    }
    if (!std::strcmp(component, "ConvexHullCollider")) {
        const auto* c = get_ptr<matter::physics::ConvexHullCollider>(e);
        if (!c) return false;
        return collider_prop_get_float(c->properties, field, out);
    }
    return false;
}

bool field_set_float(matter::WorldSession* session, matter::scene::SceneEntityId id,
                     const char* component, const char* field, float value) {
    if (!session) return false;
    flecs::entity e = find_scene_entity(session->ecs(), id);
    if (!e.is_valid()) return false;

    if (!std::strcmp(component, "RigidBody")) {
        const auto* rb = get_ptr<matter::physics::RigidBody>(e);
        if (!rb) return false;
        matter::physics::RigidBody copy = *rb;
        bool ok = true;
        if (!std::strcmp(field, "linear_damping")) copy.linear_damping = value;
        else if (!std::strcmp(field, "angular_damping")) copy.angular_damping = value;
        else if (!std::strcmp(field, "gravity_scale")) copy.gravity_scale = value;
        else if (!std::strcmp(field, "sleep_threshold")) copy.sleep_threshold = value;
        else ok = false;
        if (ok) e.set<matter::physics::RigidBody>(copy);
        return ok;
    }
    if (!std::strcmp(component, "SphereCollider")) {
        const auto* c = get_ptr<matter::physics::SphereCollider>(e);
        if (!c) return false;
        matter::physics::SphereCollider copy = *c;
        bool ok = true;
        if (!std::strcmp(field, "radius")) copy.radius = value;
        else ok = collider_prop_set_float(copy.properties, field, value);
        if (ok) e.set<matter::physics::SphereCollider>(copy);
        return ok;
    }
    if (!std::strcmp(component, "CapsuleCollider")) {
        const auto* c = get_ptr<matter::physics::CapsuleCollider>(e);
        if (!c) return false;
        matter::physics::CapsuleCollider copy = *c;
        bool ok = true;
        if (!std::strcmp(field, "radius")) copy.radius = value;
        else ok = collider_prop_set_float(copy.properties, field, value);
        if (ok) e.set<matter::physics::CapsuleCollider>(copy);
        return ok;
    }
    if (!std::strcmp(component, "BoxCollider")) {
        const auto* c = get_ptr<matter::physics::BoxCollider>(e);
        if (!c) return false;
        matter::physics::BoxCollider copy = *c;
        const bool ok = collider_prop_set_float(copy.properties, field, value);
        if (ok) e.set<matter::physics::BoxCollider>(copy);
        return ok;
    }
    if (!std::strcmp(component, "ConvexHullCollider")) {
        const auto* c = get_ptr<matter::physics::ConvexHullCollider>(e);
        if (!c) return false;
        matter::physics::ConvexHullCollider copy = *c;
        const bool ok = collider_prop_set_float(copy.properties, field, value);
        if (ok) e.set<matter::physics::ConvexHullCollider>(copy);
        return ok;
    }
    return false;
}

bool field_get_int(matter::WorldSession* session, matter::scene::SceneEntityId id,
                   const char* component, const char* field, int& out) {
    if (!session) return false;
    flecs::entity e = find_scene_entity(session->ecs(), id);
    if (!e.is_valid()) return false;
    if (!std::strcmp(component, "RigidBody") && !std::strcmp(field, "type")) {
        const auto* rb = get_ptr<matter::physics::RigidBody>(e);
        if (!rb) return false;
        out = static_cast<int>(rb->type);
        return true;
    }
    return false;
}

bool field_set_int(matter::WorldSession* session, matter::scene::SceneEntityId id,
                   const char* component, const char* field, int value) {
    if (!session) return false;
    flecs::entity e = find_scene_entity(session->ecs(), id);
    if (!e.is_valid()) return false;
    if (!std::strcmp(component, "RigidBody") && !std::strcmp(field, "type")) {
        const auto* rb = get_ptr<matter::physics::RigidBody>(e);
        if (!rb) return false;
        matter::physics::RigidBody copy = *rb;
        copy.type = static_cast<matter::physics::RigidBodyType>(
            std::max(0, std::min(2, value)));
        e.set<matter::physics::RigidBody>(copy);
        return true;
    }
    return false;
}

bool field_get_uint(matter::WorldSession* session, matter::scene::SceneEntityId id,
                    const char* component, const char* field, uint32_t& out) {
    if (!session) return false;
    flecs::entity e = find_scene_entity(session->ecs(), id);
    if (!e.is_valid()) return false;
    if (!std::strcmp(component, "PartInstance") && !std::strcmp(field, "part_hash")) {
        const auto* pi = get_ptr<matter::scene::PartInstance>(e);
        if (!pi) return false;
        out = static_cast<uint32_t>(pi->part_hash);
        return true;
    }
    if (!std::strcmp(component, "ConvexHullCollider") && !std::strcmp(field, "point_count")) {
        const auto* c = get_ptr<matter::physics::ConvexHullCollider>(e);
        if (!c) return false;
        out = c->point_count;
        return true;
    }
    return false;
}

bool field_set_uint(matter::WorldSession* session, matter::scene::SceneEntityId id,
                    const char* component, const char* field, uint32_t value) {
    if (!session) return false;
    // PartInstance.part_hash is uint64_t — the generic uint32 write path would
    // silently truncate the upper 32 bits. Part assignment is routed through
    // the specialized picker (assign_part callback) which uses the full 64-bit
    // hash. Reject writes here to prevent silent corruption.
    if (!std::strcmp(component, "PartInstance") && !std::strcmp(field, "part_hash"))
        return false;
    flecs::entity e = find_scene_entity(session->ecs(), id);
    if (!e.is_valid()) return false;
    (void)value;
    return false;
}

bool field_get_bool(matter::WorldSession* session, matter::scene::SceneEntityId id,
                    const char* component, const char* field, bool& out) {
    if (!session) return false;
    flecs::entity e = find_scene_entity(session->ecs(), id);
    if (!e.is_valid()) return false;
    if (!std::strcmp(component, "RigidBody")) {
        const auto* rb = get_ptr<matter::physics::RigidBody>(e);
        if (!rb) return false;
        if (!std::strcmp(field, "enable_sleep")) { out = rb->enable_sleep; return true; }
        if (!std::strcmp(field, "continuous")) { out = rb->continuous; return true; }
        return false;
    }
    if (!std::strcmp(component, "SphereCollider")) {
        const auto* c = get_ptr<matter::physics::SphereCollider>(e);
        if (c && !std::strcmp(field, "sensor")) { out = c->properties.sensor; return true; }
        return false;
    }
    if (!std::strcmp(component, "CapsuleCollider")) {
        const auto* c = get_ptr<matter::physics::CapsuleCollider>(e);
        if (c && !std::strcmp(field, "sensor")) { out = c->properties.sensor; return true; }
        return false;
    }
    if (!std::strcmp(component, "BoxCollider")) {
        const auto* c = get_ptr<matter::physics::BoxCollider>(e);
        if (c && !std::strcmp(field, "sensor")) { out = c->properties.sensor; return true; }
        return false;
    }
    if (!std::strcmp(component, "ConvexHullCollider")) {
        const auto* c = get_ptr<matter::physics::ConvexHullCollider>(e);
        if (c && !std::strcmp(field, "sensor")) { out = c->properties.sensor; return true; }
        return false;
    }
    if (!std::strcmp(component, "PartInstance")) {
        const auto* pi = get_ptr<matter::scene::PartInstance>(e);
        if (!pi) return false;
        if (!std::strcmp(field, "visible")) { out = pi->visible; return true; }
        if (!std::strcmp(field, "casts_shadow")) { out = pi->casts_shadow; return true; }
        return false;
    }
    return false;
}

bool field_set_bool(matter::WorldSession* session, matter::scene::SceneEntityId id,
                    const char* component, const char* field, bool value) {
    if (!session) return false;
    flecs::entity e = find_scene_entity(session->ecs(), id);
    if (!e.is_valid()) return false;
    if (!std::strcmp(component, "RigidBody")) {
        const auto* rb = get_ptr<matter::physics::RigidBody>(e);
        if (!rb) return false;
        matter::physics::RigidBody copy = *rb;
        bool ok = true;
        if (!std::strcmp(field, "enable_sleep")) copy.enable_sleep = value;
        else if (!std::strcmp(field, "continuous")) copy.continuous = value;
        else ok = false;
        if (ok) e.set<matter::physics::RigidBody>(copy);
        return ok;
    }
    if (!std::strcmp(component, "SphereCollider")) {
        const auto* c = get_ptr<matter::physics::SphereCollider>(e);
        if (!c || std::strcmp(field, "sensor")) return false;
        matter::physics::SphereCollider copy = *c;
        copy.properties.sensor = value;
        e.set<matter::physics::SphereCollider>(copy);
        return true;
    }
    if (!std::strcmp(component, "CapsuleCollider")) {
        const auto* c = get_ptr<matter::physics::CapsuleCollider>(e);
        if (!c || std::strcmp(field, "sensor")) return false;
        matter::physics::CapsuleCollider copy = *c;
        copy.properties.sensor = value;
        e.set<matter::physics::CapsuleCollider>(copy);
        return true;
    }
    if (!std::strcmp(component, "BoxCollider")) {
        const auto* c = get_ptr<matter::physics::BoxCollider>(e);
        if (!c || std::strcmp(field, "sensor")) return false;
        matter::physics::BoxCollider copy = *c;
        copy.properties.sensor = value;
        e.set<matter::physics::BoxCollider>(copy);
        return true;
    }
    if (!std::strcmp(component, "ConvexHullCollider")) {
        const auto* c = get_ptr<matter::physics::ConvexHullCollider>(e);
        if (!c || std::strcmp(field, "sensor")) return false;
        matter::physics::ConvexHullCollider copy = *c;
        copy.properties.sensor = value;
        e.set<matter::physics::ConvexHullCollider>(copy);
        return true;
    }
    if (!std::strcmp(component, "PartInstance")) {
        const auto* pi = get_ptr<matter::scene::PartInstance>(e);
        if (!pi) return false;
        matter::scene::PartInstance copy = *pi;
        bool ok = true;
        if (!std::strcmp(field, "visible")) copy.visible = value;
        else if (!std::strcmp(field, "casts_shadow")) copy.casts_shadow = value;
        else ok = false;
        if (ok) e.set<matter::scene::PartInstance>(copy);
        return ok;
    }
    return false;
}

bool field_get_float3(matter::WorldSession* session, matter::scene::SceneEntityId id,
                      const char* component, const char* field, matter::Float3& out) {
    if (!session) return false;
    flecs::entity e = find_scene_entity(session->ecs(), id);
    if (!e.is_valid()) return false;
    if (!std::strcmp(component, "LocalTransform")) {
        const auto* t = get_ptr<matter::ecs::LocalTransform>(e);
        if (!t) return false;
        if (!std::strcmp(field, "translation")) { out = t->translation; return true; }
        if (!std::strcmp(field, "scale")) { out = t->scale; return true; }
        return false;
    }
    if (!std::strcmp(component, "PhysicsVelocity")) {
        const auto* v = get_ptr<matter::physics::PhysicsVelocity>(e);
        if (!v) return false;
        if (!std::strcmp(field, "linear")) { out = v->linear; return true; }
        if (!std::strcmp(field, "angular")) { out = v->angular; return true; }
        return false;
    }
    if (!std::strcmp(component, "SphereCollider")) {
        const auto* c = get_ptr<matter::physics::SphereCollider>(e);
        if (c && !std::strcmp(field, "center")) { out = c->center; return true; }
        return false;
    }
    if (!std::strcmp(component, "CapsuleCollider")) {
        const auto* c = get_ptr<matter::physics::CapsuleCollider>(e);
        if (!c) return false;
        if (!std::strcmp(field, "point_a")) { out = c->point_a; return true; }
        if (!std::strcmp(field, "point_b")) { out = c->point_b; return true; }
        return false;
    }
    if (!std::strcmp(component, "BoxCollider")) {
        const auto* c = get_ptr<matter::physics::BoxCollider>(e);
        if (!c) return false;
        if (!std::strcmp(field, "center")) { out = c->center; return true; }
        if (!std::strcmp(field, "half_extents")) { out = c->half_extents; return true; }
        return false;
    }
    return false;
}

bool field_set_float3(matter::WorldSession* session, matter::scene::SceneEntityId id,
                      const char* component, const char* field, matter::Float3 value) {
    if (!session) return false;
    flecs::entity e = find_scene_entity(session->ecs(), id);
    if (!e.is_valid()) return false;
    if (!std::strcmp(component, "LocalTransform")) {
        const auto* t = get_ptr<matter::ecs::LocalTransform>(e);
        if (!t) return false;
        matter::ecs::LocalTransform copy = *t;
        bool ok = true;
        if (!std::strcmp(field, "translation")) copy.translation = value;
        else if (!std::strcmp(field, "scale")) copy.scale = value;
        else ok = false;
        if (ok) e.set<matter::ecs::LocalTransform>(copy);
        return ok;
    }
    if (!std::strcmp(component, "PhysicsVelocity")) {
        const auto* v = get_ptr<matter::physics::PhysicsVelocity>(e);
        if (!v) return false;
        matter::physics::PhysicsVelocity copy = *v;
        bool ok = true;
        if (!std::strcmp(field, "linear")) copy.linear = value;
        else if (!std::strcmp(field, "angular")) copy.angular = value;
        else ok = false;
        if (ok) e.set<matter::physics::PhysicsVelocity>(copy);
        return ok;
    }
    if (!std::strcmp(component, "SphereCollider")) {
        const auto* c = get_ptr<matter::physics::SphereCollider>(e);
        if (!c || std::strcmp(field, "center")) return false;
        matter::physics::SphereCollider copy = *c;
        copy.center = value;
        e.set<matter::physics::SphereCollider>(copy);
        return true;
    }
    if (!std::strcmp(component, "CapsuleCollider")) {
        const auto* c = get_ptr<matter::physics::CapsuleCollider>(e);
        if (!c) return false;
        matter::physics::CapsuleCollider copy = *c;
        bool ok = true;
        if (!std::strcmp(field, "point_a")) copy.point_a = value;
        else if (!std::strcmp(field, "point_b")) copy.point_b = value;
        else ok = false;
        if (ok) e.set<matter::physics::CapsuleCollider>(copy);
        return ok;
    }
    if (!std::strcmp(component, "BoxCollider")) {
        const auto* c = get_ptr<matter::physics::BoxCollider>(e);
        if (!c) return false;
        matter::physics::BoxCollider copy = *c;
        bool ok = true;
        if (!std::strcmp(field, "center")) copy.center = value;
        else if (!std::strcmp(field, "half_extents")) copy.half_extents = value;
        else ok = false;
        if (ok) e.set<matter::physics::BoxCollider>(copy);
        return ok;
    }
    return false;
}

bool field_get_quat(matter::WorldSession* session, matter::scene::SceneEntityId id,
                    const char* component, const char* field, matter::Quaternion& out) {
    if (!session || std::strcmp(field, "rotation")) return false;
    flecs::entity e = find_scene_entity(session->ecs(), id);
    if (!e.is_valid()) return false;
    if (!std::strcmp(component, "LocalTransform")) {
        const auto* t = get_ptr<matter::ecs::LocalTransform>(e);
        if (!t) return false;
        out = t->rotation;
        return true;
    }
    if (!std::strcmp(component, "BoxCollider")) {
        const auto* c = get_ptr<matter::physics::BoxCollider>(e);
        if (!c) return false;
        out = c->rotation;
        return true;
    }
    return false;
}

bool field_set_quat(matter::WorldSession* session, matter::scene::SceneEntityId id,
                    const char* component, const char* field, matter::Quaternion value) {
    if (!session || std::strcmp(field, "rotation")) return false;
    flecs::entity e = find_scene_entity(session->ecs(), id);
    if (!e.is_valid()) return false;
    if (!std::strcmp(component, "LocalTransform")) {
        const auto* t = get_ptr<matter::ecs::LocalTransform>(e);
        if (!t) return false;
        matter::ecs::LocalTransform copy = *t;
        copy.rotation = value;
        e.set<matter::ecs::LocalTransform>(copy);
        return true;
    }
    if (!std::strcmp(component, "BoxCollider")) {
        const auto* c = get_ptr<matter::physics::BoxCollider>(e);
        if (!c) return false;
        matter::physics::BoxCollider copy = *c;
        copy.rotation = value;
        e.set<matter::physics::BoxCollider>(copy);
        return true;
    }
    return false;
}

// Adds a default-constructed component instance to a scene entity by name.
// Mirrors ecs/scene_registry.cpp's instantiate() switch, minus Transform
// (always present) — used by the Properties panel's "+ Add Component" menu.
matter::scene::SceneEditResult component_add(matter::WorldSession* session,
                                             matter::scene::SceneEntityId id,
                                             const char* component_name) {
    using matter::scene::SceneEditError;
    using matter::scene::SceneEditResult;
    if (!session) return SceneEditResult{SceneEditError::InvalidTarget, {}};
    flecs::entity e = find_scene_entity(session->ecs(), id);
    if (!e.is_valid()) return SceneEditResult{SceneEditError::EntityNotFound, {}};

    if (!std::strcmp(component_name, "RigidBody")) e.set<matter::physics::RigidBody>({});
    else if (!std::strcmp(component_name, "PhysicsVelocity")) e.set<matter::physics::PhysicsVelocity>({});
    else if (!std::strcmp(component_name, "SphereCollider")) e.set<matter::physics::SphereCollider>({});
    else if (!std::strcmp(component_name, "CapsuleCollider")) e.set<matter::physics::CapsuleCollider>({});
    else if (!std::strcmp(component_name, "BoxCollider")) e.set<matter::physics::BoxCollider>({});
    else if (!std::strcmp(component_name, "ConvexHullCollider")) e.set<matter::physics::ConvexHullCollider>({});
    else if (!std::strcmp(component_name, "PartInstance")) e.set<matter::scene::PartInstance>({});
    else if (!std::strcmp(component_name, "SectorStreaming")) e.add<matter::streaming::SectorStreaming>();
    else return SceneEditResult{SceneEditError::InvalidTarget, {}};

    return SceneEditResult{SceneEditError::None, id};
}

matter::scene::SceneEditResult component_remove(matter::WorldSession* session,
                                                matter::scene::SceneEntityId id,
                                                const char* component_name) {
    using matter::scene::SceneEditError;
    using matter::scene::SceneEditResult;
    if (!session) return SceneEditResult{SceneEditError::InvalidTarget, {}};
    flecs::entity e = find_scene_entity(session->ecs(), id);
    if (!e.is_valid()) return SceneEditResult{SceneEditError::EntityNotFound, {}};

    if (!std::strcmp(component_name, "RigidBody")) e.remove<matter::physics::RigidBody>();
    else if (!std::strcmp(component_name, "PhysicsVelocity")) e.remove<matter::physics::PhysicsVelocity>();
    else if (!std::strcmp(component_name, "SphereCollider")) e.remove<matter::physics::SphereCollider>();
    else if (!std::strcmp(component_name, "CapsuleCollider")) e.remove<matter::physics::CapsuleCollider>();
    else if (!std::strcmp(component_name, "BoxCollider")) e.remove<matter::physics::BoxCollider>();
    else if (!std::strcmp(component_name, "ConvexHullCollider")) e.remove<matter::physics::ConvexHullCollider>();
    else if (!std::strcmp(component_name, "PartInstance")) e.remove<matter::scene::PartInstance>();
    else if (!std::strcmp(component_name, "SectorStreaming")) e.remove<matter::streaming::SectorStreaming>();
    else return SceneEditResult{SceneEditError::InvalidTarget, {}};

    return SceneEditResult{SceneEditError::None, id};
}

void init_camera(matter::CameraDesc& camera) {
    camera.position = {20.0f, 16.0f, 34.0f};
    camera.target = {0.0f, 9.0f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 0.78539816339f;
    // 0.1 m near plane: a 1.0 m near plane clipped every ground fragment
    // within a meter of a low (walk/crouch-height) camera, opening a razor
    // straight horizontal hole in the flat ground mesh with the distant
    // horizon strip showing through. Reversed-Z depth (near -> 1, far -> 0)
    // keeps plenty of precision at near = 0.1 even with far = 5000.
    camera.near_plane = 0.1f;
    camera.far_plane = 5000.0f;
}

void apply_world_resolver_defaults(const std::string& world_name,
                                   float& active_radius,
                                   float& min_projected_size,
                                   viewer::ViewerStats& stats) {
    if (world_name == "Meadow") {
        active_radius = 400.0f;
        min_projected_size = 0.0015f;
        stats.resolver_choice = 1;
    } else {
        active_radius = 64.0f;
        min_projected_size = 0.0f;
        stats.resolver_choice = 0;
    }
}

bool key_pressed(GLFWwindow* window, int key, bool& previous) {
    const bool down = glfwGetKey(window, key) == GLFW_PRESS;
    const bool pressed = down && !previous;
    previous = down;
    return pressed;
}

bool write_png(const std::string& path, const std::vector<uint8_t>& rgba,
               uint32_t width, uint32_t height) {
    if (rgba.size() != static_cast<size_t>(width) * height * 4) return false;
    const std::filesystem::path output(path);
    std::error_code ec;
    if (output.has_parent_path())
        std::filesystem::create_directories(output.parent_path(), ec);
    return stbi_write_png(path.c_str(), static_cast<int>(width),
                          static_cast<int>(height), 4, rgba.data(),
                          static_cast<int>(width * 4)) != 0;
}

// Directory holding this executable, or empty if it cannot be determined.
static std::filesystem::path executable_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf, buf + n).parent_path();
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return p.parent_path();
#endif
}

// Locate an asset directory by NAME rather than by a fixed number of "../".
//
// Three layouts have to work and they sit at different depths:
//   dev, launched from MatterEditor/      -> ../projects
//   dev, launched beside the binary       -> ../../../projects  (build/windows/)
//   a packaged build (`make dist`)        -> ./projects         (next to the exe)
//
// Hard-coding "../" for one breaks the others -- which is exactly what happened
// when the binary moved from MatterEditor/ into MatterEditor/build/windows/.
// Search next to the executable first (so a package always wins), then upward
// from the executable, then upward from the working directory.
static std::string resolve_asset_root(const char* name) {
    namespace fs = std::filesystem;
    std::error_code ec;

    auto walk_up = [&](fs::path dir) -> std::string {
        for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
            const fs::path candidate = dir / name;
            if (fs::is_directory(candidate, ec))
                return candidate.string();
            const fs::path parent = dir.parent_path();
            if (parent == dir) break;   // reached the filesystem root
            dir = parent;
        }
        return {};
    };

    if (const fs::path exe = executable_dir(); !exe.empty())
        if (std::string hit = walk_up(exe); !hit.empty()) return hit;

    if (fs::path cwd = fs::current_path(ec); !ec)
        if (std::string hit = walk_up(cwd); !hit.empty()) return hit;

    return name;   // preserve the old string so the failure message stays familiar
}

std::string examples_root() { return resolve_asset_root("projects"); }

// Repo-root issues/ directory, resolved the same way as the asset roots above.
// issues/ is committed (it carries README.md), so the direct lookup normally
// wins; the MatterEditor/ fallback covers a tree where it has been deleted.
std::string issues_root() {
    if (std::string hit = resolve_asset_root("issues"); hit != "issues")
        return hit;
    if (std::string editor = resolve_asset_root("MatterEditor");
        editor != "MatterEditor")
        return (std::filesystem::path(editor).parent_path() / "issues").string();
    return "issues";
}

std::string shared_lib_root() { return resolve_asset_root("MatterEngine3/shared-lib"); }

struct PerfRunConfig {
    bool enabled = false;
    std::string output_path;
    double warmup_seconds = 0.0;
    double sample_seconds = 0.0;
};

struct PerfCounters {
    uint64_t vertex_uploads = 0;
    uint64_t cluster_uploads = 0;
    uint64_t instance_uploads = 0;
    uint64_t immediate_submits = 0;
};

bool parse_perf_seconds(const char* value, const char* name, double& result,
                        std::string& error) {
    char* end = nullptr;
    result = std::strtod(value, &end);
    if (end == value || *end != '\0' || !std::isfinite(result) || result < 0.0) {
        error = std::string(name) + " must be a finite non-negative number";
        return false;
    }
    return true;
}

bool read_perf_run_config(PerfRunConfig& config, std::string& error) {
    const char* output = std::getenv("MATTER_PERF_OUTPUT");
    const char* warmup = std::getenv("MATTER_PERF_WARMUP_SECONDS");
    const char* sample = std::getenv("MATTER_PERF_SAMPLE_SECONDS");
    if (!output && !warmup && !sample) return true;
    if (!output || !*output || !warmup || !*warmup || !sample || !*sample) {
        error = "MATTER_PERF_OUTPUT, MATTER_PERF_WARMUP_SECONDS, and "
                "MATTER_PERF_SAMPLE_SECONDS must be set together";
        return false;
    }
    config.enabled = true;
    config.output_path = output;
    if (!parse_perf_seconds(warmup, "MATTER_PERF_WARMUP_SECONDS",
                            config.warmup_seconds, error) ||
        !parse_perf_seconds(sample, "MATTER_PERF_SAMPLE_SECONDS",
                            config.sample_seconds, error)) {
        return false;
    }
    if (!(config.sample_seconds > 0.0)) {
        error = "MATTER_PERF_SAMPLE_SECONDS must be greater than zero";
        return false;
    }
    return true;
}

PerfCounters capture_perf_counters(const matter::FrameStats& stats) {
    return {stats.vk_vertex_uploads, stats.vk_cluster_uploads,
            stats.vk_instance_uploads, stats.vk_immediate_submits};
}

double median_of_sorted(const std::vector<double>& sorted) {
    const size_t middle = sorted.size() / 2;
    return (sorted.size() & 1) != 0
               ? sorted[middle]
               : (sorted[middle - 1] + sorted[middle]) * 0.5;
}

std::string json_string(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char c : value) {
        switch (c) {
            case '\"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (c < 0x20) {
                    char encoded[7]{};
                    std::snprintf(encoded, sizeof(encoded), "\\u%04x", c);
                    escaped += encoded;
                } else {
                    escaped += static_cast<char>(c);
                }
        }
    }
    return escaped;
}

bool write_perf_result(const PerfRunConfig& config, const std::string& world,
                       std::vector<double> frame_times, const PerfCounters& start,
                       const PerfCounters& finish,
                       const matter::FrameStats& frame_stats,
                       const viewer::ViewerStats& loop_stats,
                       uint64_t dlss_reset_start,
                       uint32_t validation_errors,
                       std::string& error) {
    if (frame_times.empty()) {
        error = "no performance frames were sampled";
        return false;
    }
    std::sort(frame_times.begin(), frame_times.end());
    const double median_frame_ms = median_of_sorted(frame_times);
    const size_t p95_index = static_cast<size_t>(
        std::ceil(static_cast<double>(frame_times.size()) * 0.95)) - 1;
    const double p95_frame_ms = frame_times[p95_index];
    const double median_fps = median_frame_ms > 0.0 ? 1000.0 / median_frame_ms : 0.0;
    std::ofstream output(config.output_path, std::ios::out | std::ios::trunc);
    if (!output) {
        error = "could not write MATTER_PERF_OUTPUT '" + config.output_path + "'";
        return false;
    }
    output << std::fixed << std::setprecision(6)
           << "{\"world\":\"" << world << "\",\"frames\":"
           << frame_times.size() << ",\"frame_metric\":\"end_to_end_cadence\""
           << ",\"median_frame_ms\":" << median_frame_ms
           << ",\"median_fps\":" << median_fps
           << ",\"p95_frame_ms\":" << p95_frame_ms
           << ",\"static_vertex_upload_delta\":"
           << (finish.vertex_uploads - start.vertex_uploads)
           << ",\"static_cluster_upload_delta\":"
           << (finish.cluster_uploads - start.cluster_uploads)
           << ",\"stable_instance_upload_delta\":"
           << (finish.instance_uploads - start.instance_uploads)
           << ",\"immediate_submit_delta\":"
           << (finish.immediate_submits - start.immediate_submits)
           << ",\"selected_dlss_mode\":\""
           << matter::dlss_mode_name(frame_stats.dlss_selected_mode) << "\""
           << ",\"active_dlss_mode\":\""
           << matter::dlss_mode_name(frame_stats.dlss_active_mode) << "\""
           << ",\"dlss_internal_width\":" << frame_stats.dlss_internal_width
           << ",\"dlss_internal_height\":" << frame_stats.dlss_internal_height
           << ",\"dlss_output_width\":" << frame_stats.dlss_output_width
           << ",\"dlss_output_height\":" << frame_stats.dlss_output_height
           << ",\"dlss_reset_delta\":"
           << (frame_stats.dlss_reset_count >= dlss_reset_start
                   ? frame_stats.dlss_reset_count - dlss_reset_start
                   : frame_stats.dlss_reset_count)
           << ",\"rt_available\":"
           << (frame_stats.vk_rt_available ? "true" : "false")
           << ",\"rt_enabled\":"
           << (frame_stats.vk_rt_effective ? "true" : "false")
           << ",\"rt_samples\":" << frame_stats.vk_rt_samples
           << ",\"rt_debug_view\":"
           << (frame_stats.vk_rt_debug_view ? "true" : "false")
           << ",\"vk_rt_available\":"
           << (frame_stats.vk_rt_available ? "true" : "false")
           << ",\"vk_rt_effective\":"
           << (frame_stats.vk_rt_effective ? "true" : "false")
           << ",\"vk_rt_trace_dispatches\":"
           << frame_stats.vk_rt_trace_dispatches
           << ",\"vk_rt_fallback_reason\":\""
           << json_string(frame_stats.vk_rt_fallback_reason) << "\""
           << ",\"fallback_reason\":\""
           << json_string(frame_stats.dlss_reason) << "\""
           // Main-loop phase attribution. These already existed as
           // ViewerStats::loop_*_ms and drove the HUD, but never reached this
           // file — so a perf run could tell you a frame took 4 s and not one
           // thing about where the 4 s went. The phases partition
           // perf_frame_start..end_frame exactly, so they sum to ~frame_ms.
           //
           // peak_pump/peak_acquire are peak-hold rather than EMA on purpose:
           // for a streaming world the interesting stalls are spiky (a sector
           // publish landing inside a frame) and an average hides them.
           << ",\"loop_poll_ms\":" << loop_stats.loop_poll_ms
           << ",\"loop_acquire_ms\":" << loop_stats.loop_acquire_ms
           << ",\"loop_ui_ms\":" << loop_stats.loop_ui_ms
           << ",\"loop_tick_ms\":" << loop_stats.loop_tick_ms
           << ",\"loop_pump_ms\":" << loop_stats.loop_pump_ms
           << ",\"loop_lab_ms\":" << loop_stats.loop_lab_ms
           << ",\"loop_render_ms\":" << loop_stats.loop_render_ms
           << ",\"loop_present_ms\":" << loop_stats.loop_present_ms
           << ",\"loop_peak_pump_ms\":" << loop_stats.loop_peak_pump_ms
           << ",\"loop_peak_acquire_ms\":" << loop_stats.loop_peak_acquire_ms
           << ",\"validation_errors\":" << validation_errors << "}\n";
    if (!output) {
        error = "failed while writing MATTER_PERF_OUTPUT '" + config.output_path + "'";
        return false;
    }
    return true;
}

} // namespace

int main() {
    PerfRunConfig perf;
    std::string perf_error;
    if (!read_perf_run_config(perf, perf_error)) {
        std::fprintf(stderr, "FATAL: %s\n", perf_error.c_str());
        return 1;
    }
    if (!glfwInit()) {
        std::fprintf(stderr, "FATAL: glfwInit failed\n");
        return 1;
    }
    // Replay run (shot_replay.h): reproduce a recorded issue shot and exit.
    // Loaded before the window exists because the recorded framebuffer size is
    // what makes the same geometry land on the same pixels — resizing after the
    // fact would reflow the docked panels and move the viewport.
    const viewer::ShotReplay replay = viewer::load_replay_from_env();
    if (!replay.valid && !replay.error.empty()) {
        std::fprintf(stderr, "FATAL: MATTER_REPLAY: %s\n", replay.error.c_str());
        glfwTerminate();
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(
        replay.valid && replay.frame_width > 0
            ? static_cast<int>(replay.frame_width) : 1280,
        replay.valid && replay.frame_height > 0
            ? static_cast<int>(replay.frame_height) : 720,
        "MatterEngine3 World Viewer", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "FATAL: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }

    std::string error;
    // Validation layers are a development dependency; requesting them
    // unconditionally makes editor.exe fatal on machines without the Vulkan
    // SDK installed. Opt in via MATTER_VK_VALIDATION=1 (test harnesses do).
    const bool enable_validation =
        std::getenv("MATTER_VK_VALIDATION") != nullptr;
    auto vulkan = matter::VulkanDevice::create(window, enable_validation, error);
    if (!vulkan) {
        std::fprintf(stderr, "FATAL: %s\n", error.c_str());
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    const bool disable_vulkan_rt =
        std::getenv("MATTER_DISABLE_VK_RT") != nullptr;
    std::printf("Vulkan RT available=%s enabled=%s reason=%s\n",
                vulkan->ray_tracing_available() ? "true" : "false",
                vulkan->ray_tracing_available() && !disable_vulkan_rt
                    ? "true"
                    : "false",
                vulkan->ray_tracing_available()
                    ? (disable_vulkan_rt ? "disabled by MATTER_DISABLE_VK_RT"
                                         : "none")
                    : vulkan->ray_tracing_unavailable_reason().c_str());
    matter::EngineDesc engine_desc;
    // Phase 1 cache-leak fix: MATTER_CACHE_ROOT is an explicit override and
    // still wins when set, but it is canonicalized to absolute here (rather
    // than handed to the engine as-is) so a relative override behaves
    // identically regardless of the directory editor.exe was launched from.
    // EngineContext::create() also canonicalizes/requires cache_root itself
    // (see matter_engine.cpp), but engine_desc.cache_root is informational
    // only -- the per-world cache_root actually used for baking is derived
    // from WorldDesc::project_dir via LocalProviderConfig::for_project() in
    // open_world() -- so cache_root must never be left null here or
    // EngineContext::create() fails loudly for no functional reason.
    const char* cache_root_env = std::getenv("MATTER_CACHE_ROOT");
    std::string cache_root_abs;
    {
        std::error_code ec;
        const std::string requested =
            (cache_root_env && cache_root_env[0] != '\0') ? cache_root_env : "cache";
        std::filesystem::path abs =
            std::filesystem::absolute(std::filesystem::path(requested), ec);
        cache_root_abs = ec ? requested : abs.string();
    }
    engine_desc.cache_root = cache_root_abs.c_str();
    engine_desc.render_device = vulkan.get();
    auto engine = matter::EngineContext::create(engine_desc, error);
    if (!engine) {
        std::fprintf(stderr, "FATAL: %s\n", error.c_str());
        vulkan.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    viewer::Ui ui;
    if (!ui.setup(window, *vulkan, error)) {
        std::fprintf(stderr, "FATAL: %s\n", error.c_str());
        engine.reset();
        vulkan.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // Replay reproduces the panel LAYOUT, not just the window size. The
    // viewport rect is entirely a function of the docked layout (ui.cpp's
    // prepare_viewport_rect reads the Viewport window's content region), so
    // without this a replay inherits whatever arrangement the last interactive
    // session left in imgui.ini — measured: the same crop moved by 24.7% of its
    // pixels between two window sizes, and the viewport grew 400x340 -> 720x520.
    //
    // Clearing IniFilename also stops the replay SAVING over the user's layout
    // on exit, which every headless capture run was quietly doing.
    if (replay.valid) {
        ImGui::GetIO().IniFilename = nullptr;
        if (!replay.layout_ini.empty()) {
            ImGui::LoadIniSettingsFromMemory(replay.layout_ini.c_str(),
                                             replay.layout_ini.size());
            std::printf("replay: restored recorded panel layout (%zu bytes)\n",
                        replay.layout_ini.size());
        } else {
            std::printf("replay: shot recorded no layout; using ImGui defaults "
                        "(viewport may differ from the capture)\n");
        }
    }

    auto worlds = viewer::scan_worlds(examples_root());
    std::printf("worlds available (%d):\n", static_cast<int>(worlds.size()));
    for (size_t i = 0; i < worlds.size(); ++i)
        std::printf("  [%zu] %s  (%s)\n", i, worlds[i].label.c_str(),
                    worlds[i].project_dir.c_str());
    if (worlds.empty()) {
        std::fprintf(stderr, "FATAL: no worlds found under %s\n",
                     examples_root().c_str());
        ui.shutdown();
        engine.reset();
        vulkan.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    matter::CameraDesc camera{};
    init_camera(camera);
    const char* initial_camera_env = std::getenv("MATTER_CAM");
    if (replay.valid) {
        // Full projection, not just eye/target: a different fov or near/far
        // reprojects everything and the diff would be all noise.
        camera.position = {replay.eye[0], replay.eye[1], replay.eye[2]};
        camera.target = {replay.target[0], replay.target[1], replay.target[2]};
        camera.up = {replay.up[0], replay.up[1], replay.up[2]};
        if (replay.fov_radians > 0.0f)
            camera.vertical_fov_radians = replay.fov_radians;
        if (replay.near_plane > 0.0f) camera.near_plane = replay.near_plane;
        if (replay.far_plane > 0.0f) camera.far_plane = replay.far_plane;
    }
    if (const char* value = initial_camera_env) {
        float c[6];
        if (std::sscanf(value, "%f,%f,%f,%f,%f,%f", &c[0], &c[1], &c[2],
                        &c[3], &c[4], &c[5]) == 6) {
            camera.position = {c[0], c[1], c[2]};
            camera.target = {c[3], c[4], c[5]};
            std::printf("MATTER_CAM: eye(%.1f,%.1f,%.1f) target(%.1f,%.1f,%.1f)\n",
                        c[0], c[1], c[2], c[3], c[4], c[5]);
        }
    }

    int initial_world = 0;
    // MATTER_WORLD still wins, so a replay can be re-aimed at another world
    // deliberately; absent that, the shot's own world is authoritative.
    const char* world_env = std::getenv("MATTER_WORLD");
    const std::string replay_world = replay.valid ? replay.world : std::string();
    if (!world_env && !replay_world.empty()) world_env = replay_world.c_str();
    if (const char* value = world_env) {
        std::string wanted(value);
        std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        bool found = false;
        for (size_t i = 0; i < worlds.size(); ++i) {
            std::string candidate = worlds[i].world_name;
            std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (candidate == wanted) {
                initial_world = static_cast<int>(i);
                found = true;
                break;
            }
        }
        if (!found) {
            std::fprintf(stderr,
                         "FATAL: MATTER_WORLD '%s' is not a committed world\n",
                         value);
            ui.shutdown();
            engine.reset();
            vulkan.reset();
            glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
    }
    viewer::ViewerStats stats{};
    stats.world_current = initial_world;
    stats.gpu_cull_active = true;
    stats.connected = true;
    if (std::getenv("MATTER_HIZ"))
        std::printf("MATTER_HIZ: not available in Vulkan milestone; ignored\n");
    float active_radius = 64.0f;
    float min_projected_size = 0.0f;
    apply_world_resolver_defaults(worlds[initial_world].world_name,
                                  active_radius, min_projected_size, stats);
    bool wireframe = false;

    const std::string shared_lib = shared_lib_root();
    auto open_world = [&](const viewer::WorldEntry& entry) {
        matter::WorldDesc desc;
        desc.project_dir = entry.project_dir.c_str();
        desc.world_name = entry.world_name.c_str();
        desc.engine_shared_lib_dir = shared_lib.c_str();
        desc.enable_live_edit = std::getenv("MATTER_LIVE_EDIT") != nullptr;
        std::string world_error;
        auto result = engine->open_world(desc, world_error);
        if (!result) {
            std::fprintf(stderr, "open_world: %s\n", world_error.c_str());
            return result;
        }
        // NOTE (E4b): the bake is NOT requested here. SessionBinding owns bake
        // ordering (event-system.md S I.13): it builds the app<->session bridge
        // and opens the command epoch FIRST, then requests the initial bake, so
        // no bake.started can precede the subscribers.
        return result;
    };
    auto session = open_world(worlds[initial_world]);
    if (!session) {
        ui.shutdown(); engine.reset(); vulkan.reset();
        glfwDestroyWindow(window); glfwTerminate();
        return 1;
    }

    // E5c: app-owned observable-model scheduler (event-system.md S I.9). Declared
    // BEFORE editor_model so it OUTLIVES it — the EditorModel's revision Property
    // unregisters from this scheduler in its destructor, so the scheduler must
    // still be alive then. Claimed on this (main/UI) thread; flushed once per
    // frame after session->tick() (S I.14 flush-after-tick, wired below).
    matter::evt::PropertyScheduler property_scheduler;
    property_scheduler.claim();

    viewer::EditorModel editor_model;
    editor_model.attach_scheduler(property_scheduler);
    viewer::SelectionSet selection_set;
    matter::scene::SimulationControl sim_control;
    viewer::ConsoleLog console_log;
    console_log.push(viewer::LogSeverity::Info,
                      "Connected to " + worlds[initial_world].world_name);

    // Task 9: cached part graph snapshot for the Properties panel's baked-root
    // info card. Refreshed only when graph_generation() changes so the panel
    // doesn't re-copy the whole snapshot every frame.
    part_graph_snapshot::Snapshot cached_snapshot;
    uint64_t cached_graph_gen = 0;
    viewer::SceneCommands scene_commands;
    // E5c (event-system.md S I.14): the mutation closures are assigned
    // LATER (after the command registry + scene-edit handlers exist) so each
    // routes through registry.execute() of a SceneService ActiveSession
    // command. The old per-frame query_records/generation POLL path is gone —
    // the model is now delta-driven by the SessionBinding scene adapter.

    // Properties panel (Phase 5 Task 7) wiring: PropertiesRegistry supplies
    // the field/widget layout, FieldCommands/ComponentCommands bridge it to
    // the live ECS via the free functions defined above.
    viewer::PropertiesRegistry properties_registry;
    viewer::FieldCommands field_commands;
    field_commands.get_float = [&session](matter::scene::SceneEntityId id, const char* c, const char* f, float& out) {
        return field_get_float(session.get(), id, c, f, out);
    };
    field_commands.set_float = [&session](matter::scene::SceneEntityId id, const char* c, const char* f, float v) {
        return field_set_float(session.get(), id, c, f, v);
    };
    field_commands.get_int = [&session](matter::scene::SceneEntityId id, const char* c, const char* f, int& out) {
        return field_get_int(session.get(), id, c, f, out);
    };
    field_commands.set_int = [&session](matter::scene::SceneEntityId id, const char* c, const char* f, int v) {
        return field_set_int(session.get(), id, c, f, v);
    };
    field_commands.get_uint = [&session](matter::scene::SceneEntityId id, const char* c, const char* f, uint32_t& out) {
        return field_get_uint(session.get(), id, c, f, out);
    };
    field_commands.set_uint = [&session](matter::scene::SceneEntityId id, const char* c, const char* f, uint32_t v) {
        return field_set_uint(session.get(), id, c, f, v);
    };
    field_commands.get_bool = [&session](matter::scene::SceneEntityId id, const char* c, const char* f, bool& out) {
        return field_get_bool(session.get(), id, c, f, out);
    };
    field_commands.set_bool = [&session](matter::scene::SceneEntityId id, const char* c, const char* f, bool v) {
        return field_set_bool(session.get(), id, c, f, v);
    };
    field_commands.get_float3 = [&session](matter::scene::SceneEntityId id, const char* c, const char* f, matter::Float3& out) {
        return field_get_float3(session.get(), id, c, f, out);
    };
    field_commands.set_float3 = [&session](matter::scene::SceneEntityId id, const char* c, const char* f, matter::Float3 v) {
        return field_set_float3(session.get(), id, c, f, v);
    };
    field_commands.get_quat = [&session](matter::scene::SceneEntityId id, const char* c, const char* f, matter::Quaternion& out) {
        return field_get_quat(session.get(), id, c, f, out);
    };
    field_commands.set_quat = [&session](matter::scene::SceneEntityId id, const char* c, const char* f, matter::Quaternion v) {
        return field_set_quat(session.get(), id, c, f, v);
    };
    viewer::ComponentCommands component_commands;
    component_commands.add_component = [&session](matter::scene::SceneEntityId id, const char* name) {
        return component_add(session.get(), id, name);
    };
    component_commands.remove_component = [&session](matter::scene::SceneEntityId id, const char* name) {
        return component_remove(session.get(), id, name);
    };

    // Specialized editors (Task 8): component-specific actions the generic
    // field grid can't express — part picking, physics runtime actions,
    // sector streaming attach/detach. Wired the same way as FieldCommands
    // above: free functions hardcoded per ComponentKind, bridged through
    // std::function callbacks the UI layer invokes without knowing about
    // flecs/WorldSession.
    viewer::SpecializedEditors specialized_editors;
    specialized_editors.part_commands().assign_part =
        [&session](matter::scene::SceneEntityId id, uint64_t new_hash) {
            if (!session) return false;
            flecs::entity e = find_scene_entity(session->ecs(), id);
            if (!e.is_valid() || !e.has<matter::scene::PartInstance>()) return false;
            matter::scene::PartInstance copy = e.get<matter::scene::PartInstance>();
            copy.part_hash = new_hash;
            e.set<matter::scene::PartInstance>(copy);
            return true;
        };
    specialized_editors.part_commands().list_available_parts =
        []() -> std::vector<std::pair<uint64_t, std::string>> {
            // Stub: the part store (part_asset/part_graph) isn't easily
            // reachable from main.cpp's WorldSession handle today. Returning
            // an empty list keeps the picker popup functional (shows
            // "No parts available") without crashing; wiring a real lookup
            // is future work once WorldSession exposes a part enumeration API.
            return {};
        };

    specialized_editors.physics_commands().set_linear_velocity =
        [&session](matter::scene::SceneEntityId id, matter::Float3 velocity) {
            if (!session) return false;
            flecs::entity e = find_scene_entity(session->ecs(), id);
            if (!e.is_valid()) return false;
            matter::physics::PhysicsVelocity copy =
                e.has<matter::physics::PhysicsVelocity>()
                    ? e.get<matter::physics::PhysicsVelocity>()
                    : matter::physics::PhysicsVelocity{};
            copy.linear = velocity;
            e.set<matter::physics::PhysicsVelocity>(copy);
            return true;
        };
    specialized_editors.physics_commands().apply_impulse =
        [&session](matter::scene::SceneEntityId id, matter::Float3 impulse) {
            // Stub: no direct Box3d body handle is threaded through
            // WorldSession, so this approximates an impulse as an
            // instantaneous PhysicsVelocity delta (mass is not accounted
            // for). Good enough for interactive nudging in the editor;
            // real impulse application belongs in the physics_systems.cpp
            // Box3d bridge.
            if (!session) return false;
            flecs::entity e = find_scene_entity(session->ecs(), id);
            if (!e.is_valid()) return false;
            matter::physics::PhysicsVelocity copy =
                e.has<matter::physics::PhysicsVelocity>()
                    ? e.get<matter::physics::PhysicsVelocity>()
                    : matter::physics::PhysicsVelocity{};
            copy.linear.x += impulse.x;
            copy.linear.y += impulse.y;
            copy.linear.z += impulse.z;
            e.set<matter::physics::PhysicsVelocity>(copy);
            return true;
        };
    specialized_editors.physics_commands().wake =
        [&session](matter::scene::SceneEntityId id) {
            // Stub: no sleep/wake state is tracked on RigidBody yet (see
            // matter/physics.h — RigidBody has enable_sleep/sleep_threshold
            // but no runtime "is asleep" flag exposed to the editor).
            if (!session) return false;
            flecs::entity e = find_scene_entity(session->ecs(), id);
            return e.is_valid() && e.has<matter::physics::RigidBody>();
        };
    specialized_editors.physics_commands().teleport =
        [&session](matter::scene::SceneEntityId id, matter::Float3 position) {
            if (!session) return false;
            flecs::entity e = find_scene_entity(session->ecs(), id);
            if (!e.is_valid() || !e.has<matter::ecs::LocalTransform>()) return false;
            matter::ecs::LocalTransform copy = e.get<matter::ecs::LocalTransform>();
            copy.translation = position;
            e.set<matter::ecs::LocalTransform>(copy);
            return true;
        };

    specialized_editors.streaming_commands().attach_streaming =
        [&session](matter::scene::SceneEntityId id) {
            if (!session) return false;
            flecs::entity e = find_scene_entity(session->ecs(), id);
            if (!e.is_valid()) return false;
            e.add<matter::streaming::SectorStreaming>();
            return true;
        };
    specialized_editors.streaming_commands().remove_streaming =
        [&session](matter::scene::SceneEntityId id) {
            if (!session) return false;
            flecs::entity e = find_scene_entity(session->ecs(), id);
            if (!e.is_valid()) return false;
            e.remove<matter::streaming::SectorStreaming>();
            return true;
        };
    specialized_editors.streaming_commands().set_follow_camera =
        [](bool /*follow*/) {
            // Stub: per-anchor follow-camera toggling isn't wired to
            // matter_viewer::StreamingAnchorState yet (that controller
            // currently tracks a single global anchor, not a per-entity
            // flag). Follow-camera behavior today is still driven by
            // Ui::update_sector_streaming / streaming_anchor_controller.
        };
    specialized_editors.streaming_commands().regenerate =
        [](uint64_t /*seed*/) {
            // Stub: no reseed entry point is exposed by
            // matter::streaming::SectorStreaming / sector_streamer.cpp yet.
        };

    bool left_mouse_down = false;
    bool camera_capture = false;
    bool tab_down = false;
    bool f9_down = false;
    bool f10_down = false;
    bool f8_down = false;
    bool dlss_modes_supported = false;
    matter::DlssMode selected_dlss_mode = matter::DlssMode::Native;
    if (const char* initial_dlss_mode = std::getenv("MATTER_DLSS_MODE")) {
        if (std::strcmp(initial_dlss_mode, "quality") == 0)
            selected_dlss_mode = matter::DlssMode::Quality;
        else if (std::strcmp(initial_dlss_mode, "balanced") == 0)
            selected_dlss_mode = matter::DlssMode::Balanced;
        else if (std::strcmp(initial_dlss_mode, "performance") == 0)
            selected_dlss_mode = matter::DlssMode::Performance;
        else if (std::strcmp(initial_dlss_mode, "native") != 0)
            std::fprintf(stderr,
                         "MATTER_DLSS_MODE: expected native, quality, balanced, or performance; using native\n");
    }
    matter::DlssMode reported_selected_dlss_mode =
        static_cast<matter::DlssMode>(255);
    matter::DlssMode reported_active_dlss_mode =
        static_cast<matter::DlssMode>(255);
    uint32_t reported_dlss_internal_width = UINT32_MAX;
    uint32_t reported_dlss_internal_height = UINT32_MAX;
    uint32_t reported_dlss_output_width = UINT32_MAX;
    uint32_t reported_dlss_output_height = UINT32_MAX;
    uint64_t reported_dlss_resets = UINT64_MAX;
    bool reported_vk_rt_effective = false;
    uint32_t reported_vk_rt_dispatches = UINT32_MAX;
    std::string reported_vk_rt_reason;
    bool reported_vk_rt_once = false;
    viewer::CameraController camera_controller;
    // Bake Lab shell (task 2.1): window drawn with the other panels below;
    // tick_frame runs each frame beside session tick/pump.
    viewer::BakeLab bake_lab;
    // Standalone Assets pane (promoted out of Bake Lab's former "Assets"
    // tab): a loop-scope AssetBrowser owner, same pattern as bake_lab above.
    // Its "Open in Workbench" / "Load" actions now issue the workbench.open_part
    // / lab.focus_tab / viewer.switch_world commands through the app registry
    // (event-system.md S I.11, E4b) instead of a shared handoff struct.
    viewer::AssetBrowser asset_browser;
    // In-editor issue reporter (issue_reporter.h): F9 freezes the screen for a
    // drag-selected crop, F10 grabs the viewport, both accumulating into one
    // report. Declared AFTER `vulkan` so the preview textures are torn down
    // before the device that owns them.
    viewer::IssueReporterState issue_state;
    viewer::ImagePreviewCache issue_previews;
    issue_previews.configure(vulkan.get());
    // Resolve now, once, and say where: a report written somewhere unexpected
    // is a report nobody finds.
    viewer::set_issues_dir(issues_root());
    std::printf("issues: reports go to %s\n", viewer::issues_dir().c_str());
    // Declared here rather than beside the other capture env vars below because
    // stamp_replay_state records it per shot.
    const bool hide_ui = std::getenv("MATTER_HIDE_UI") != nullptr ||
                         (replay.valid && !replay.ui_visible);
    // Drops every preview texture and the draft. Used on file, on Discard, and
    // whenever the panel asks by dropping back to Idle.
    auto reset_issue_state = [&]() {
        issue_previews.destroy_all();
        issue_state = viewer::IssueReporterState{};
    };
    // Stamps the state a replay needs (shot_replay.h). Kept in one place so the
    // F9 and F10 paths cannot drift into recording different things — a shot
    // missing one of these is a shot that cannot be taken again.
    auto stamp_replay_state = [&](viewer::IssueShot& shot, uint32_t fb_w,
                                  uint32_t fb_h) {
        shot.world = worlds[stats.world_current].world_name;
        shot.camera = camera;
        shot.sim_mode = sim_control.mode();
        shot.time_scale = ui.sim_time_scale();
        shot.frame_width = fb_w;
        shot.frame_height = fb_h;
        shot.dlss_mode = matter::dlss_mode_name(selected_dlss_mode);
        shot.pixel_budget = stats.pixel_budget;
        shot.resolver_choice = stats.resolver_choice;
        shot.debug_view_mode = stats.debug_view_mode;
        shot.ui_visible = !hide_ui;
        // The layout IS the viewport geometry; without it a replay reproduces
        // the window size and still puts the 3D view somewhere else.
        if (const char* ini = ImGui::SaveIniSettingsToMemory(nullptr))
            shot.layout_ini = ini;
        const viewer::ViewportRect& vp = ui.viewport_rect();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float sx = display.x > 0 ? fb_w / display.x : 1.0f;
        const float sy = display.y > 0 ? fb_h / display.y : 1.0f;
        shot.viewport = viewer::ShotRect{static_cast<int32_t>(vp.x * sx),
                                         static_cast<int32_t>(vp.y * sy),
                                         static_cast<int32_t>(vp.w * sx),
                                         static_cast<int32_t>(vp.h * sy)};
    };
    // Uploads a thumbnail for a shot. A failed upload costs the preview only —
    // the PNG on disk is the artifact that matters.
    auto attach_preview = [&](viewer::IssueShot& shot,
                              const std::vector<uint8_t>& rgba, uint32_t w,
                              uint32_t h) {
        uint32_t tw = 0, th = 0;
        const std::vector<uint8_t> small =
            viewer::downscale_rgba(rgba, w, h, 512, tw, th);
        std::string preview_error;
        shot.preview = issue_previews.create(small, tw, th, preview_error);
        shot.preview_width = tw;
        shot.preview_height = th;
        if (!shot.preview && !preview_error.empty())
            std::fprintf(stderr, "issue preview: %s\n", preview_error.c_str());
    };
    // Part Workbench (part-workbench.md W2): private isolation session, see
    // part_workbench.h's architecture note. cache/lab-scratch is entirely
    // separate from production worlds' <project>/.cache/<world> roots.
    bake_lab.workbench().configure(vulkan.get(), examples_root(), shared_lib);
    // Frames to hold after the world is ready before reading back. Three is
    // enough for a raster frame, but RT worlds accumulate through a temporal
    // denoiser, so an early capture catches whatever the accumulation happened
    // to be at — two replays of one descriptor then differ by more than any
    // real fix would. Replays default high so they can be compared to each
    // other; MATTER_REPLAY_SETTLE tunes it.
    int settle_frames = 3;
    if (replay.valid) {
        settle_frames = 90;
        if (const char* value = std::getenv("MATTER_REPLAY_SETTLE")) {
            const int parsed = std::atoi(value);
            if (parsed > 0) settle_frames = parsed;
        }
    }
    const char* screenshot_env = std::getenv("MATTER_SCREENSHOT");
    // A replay run IS a screenshot run: it reuses the settle/readback/quit path
    // wholesale, and only differs in cropping the result to the recorded rect.
    const char* replay_out_env = std::getenv("MATTER_REPLAY_OUT");
    const std::string screenshot_path =
        screenshot_env  ? screenshot_env
        : replay.valid  ? (replay_out_env ? replay_out_env : "replay.png")
                        : "";
    int screenshot_settle = 0;
    int screenshot_failures = 0;
    bool bake_ready = false;
    bool selected_world_reported = false;
    bool apply_world_camera_after_bake =
        !replay.valid && initial_camera_env == nullptr;
    const bool test_resize = std::getenv("MATTER_TEST_RESIZE") != nullptr;
    if (replay.valid) {
        // The toggles that change pixels. DLSS is deliberately NOT restored: it
        // is temporal, so a shot taken after seconds of accumulation cannot be
        // matched by a freshly-settled replay, and forcing Native at least makes
        // the comparison honest and repeatable. Ask for the recorded mode with
        // MATTER_DLSS_MODE if you want it back.
        stats.pixel_budget = replay.pixel_budget;
        stats.resolver_choice = replay.resolver_choice;
        stats.debug_view_mode = replay.debug_view_mode;
        ui.set_sim_time_scale(replay.time_scale);
        if (replay.dlss_mode != "native")
            std::printf("replay: shot used DLSS '%s'; forcing native "
                        "(temporal accumulation is not reproducible)\n",
                        replay.dlss_mode.c_str());
        std::printf("replay: world=%s shot rect %dx%d at (%d,%d) of %ux%u\n",
                    replay.world.c_str(), replay.rect.w, replay.rect.h,
                    replay.rect.x, replay.rect.y, replay.frame_width,
                    replay.frame_height);
    }
    if (const char* scale = std::getenv("MATTER_TIME_SCALE")) {
        const float value = static_cast<float>(std::atof(scale));
        if (std::isfinite(value) && value >= viewer::kToolbarMinTimeScale &&
            value <= viewer::kToolbarMaxTimeScale)
            ui.set_sim_time_scale(value);
        else
            std::fprintf(stderr, "MATTER_TIME_SCALE ignored: '%s' outside [%.2f, %.2f]\n",
                         scale, viewer::kToolbarMinTimeScale,
                         viewer::kToolbarMaxTimeScale);
    }
    bool resize_exercised = false;
    if (hide_ui) {
        std::printf("viewer: UI hidden by MATTER_HIDE_UI\n");
        ui.set_hide_ui(true);
    }

    int cmd_fd = -1;
#ifdef _WIN32
    HANDLE cmd_handle = INVALID_HANDLE_VALUE;
    LARGE_INTEGER cmd_offset{};
#endif
    std::string cmd_buffer;
    const char* fifo_path = std::getenv("MATTER_CMD_FIFO");
#ifndef _WIN32
    if (fifo_path) {
        mkfifo(fifo_path, 0600);
        cmd_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
        if (cmd_fd >= 0)
            std::printf("MATTER_CMD_FIFO: listening on %s\n", fifo_path);
        else
            std::printf("MATTER_CMD_FIFO: failed to open %s\n", fifo_path);
    }
#else
    if (fifo_path) {
        // Windows has no POSIX FIFO. Poll an append-only command file so the
        // documented command stream remains practical and nonblocking.
        cmd_handle = CreateFileA(fifo_path, GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE |
                                     FILE_SHARE_DELETE,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
        if (cmd_handle != INVALID_HANDLE_VALUE)
            std::printf("MATTER_CMD_FIFO: polling command file %s\n", fifo_path);
        else
            std::printf("MATTER_CMD_FIFO: failed to open command file %s\n",
                        fifo_path);
    }
#endif
    std::string shot_path;
    std::string stats_label;
    int shot_settle = 0;
    bool quit_requested = false;
    bool fatal_error = false;
    enum class PerfPhase { WaitingForBake, Warming, Sampling, Complete };
    PerfPhase perf_phase = PerfPhase::WaitingForBake;
    std::chrono::steady_clock::time_point perf_phase_start{};
    PerfCounters perf_start_counters{};
    uint64_t perf_start_dlss_resets = 0;
    std::vector<double> perf_frame_times;
    auto previous_time = std::chrono::steady_clock::now();
    double hud_frame_ms = 0.0;

    // ---- Event system E4b: app hub + command registry + SessionBinding ------
    // The app hub (event-system.md S I.13) is distinct from the per-session hub
    // (session->events()): it carries editor/UI notifications, the command
    // registry, and observable models, and lives for the whole editor. The
    // command registry runs on the app lane, which THIS (main/UI) thread owns
    // and pumps once per frame — same thread that pumps the session and drains
    // poll_event.
    matter::evt::Hub app_hub;
    matter::evt::CommandRegistry registry(app_hub);
    const matter::evt::lane app_lane = matter::evt::lane::app;
    registry.claim_lane(app_lane);

    // Clears app-side models referencing a dead/reloaded world. The E5 scene
    // adapter resnapshots here; E4b clears selection + editor selection + sim.
    auto clear_app_models = [&]() {
        selection_set.clear();
        editor_model.clear_selection();
        sim_control = matter::scene::SimulationControl{};
        // The two graph caches key their refresh on graph_generation(), a
        // PER-SESSION counter — a value carried across the switch collides
        // with the new session's count (both typically stop at 1) and locks
        // the refresh out (issues/editor-scene-panel-stale). These resets were
        // wired at the pre-E4b switch/reload sites and lost in a merge.
        ui.reset_scene_tree_cache();
        cached_snapshot = part_graph_snapshot::Snapshot{};
        cached_graph_gen = 0;
    };
    // E5c: the SessionBinding-owned scene adapter (scene_model_adapter.*) is the
    // concrete app<->session bridge SessionBinding (re)builds on bind / world
    // switch and quiesces before the old session hub dies. It snapshot-primes
    // the EditorModel and subscribes immediate to the canonical scene deltas.
    // Constructed BEFORE the binding so binding.initialize()'s rebuild_bridge
    // populates the model from the initial session's snapshot.
    viewer::SceneModelAdapter scene_adapter(editor_model, session);
    viewer::SessionBinding binding(
        app_hub, registry, app_lane, session, clear_app_models,
        [&scene_adapter](matter::evt::Hub& session_hub,
                         std::vector<matter::evt::Subscription>& out) {
            scene_adapter.build(session_hub, out);
        });
    // Startup bind-then-request: builds the bridge (snapshot-primes the scene
    // model) + opens the first command epoch BEFORE requesting the initial bake.
    binding.initialize();

    // ---- Registered viewer commands (S I.11 migration map) ------------------
    // Handlers live where the poll-site code lived (this main loop / the lab
    // shell). All App-scoped and non-undoable. Same-thread UI triggers reach
    // them through execute() (via the ViewerCommands bridge below); the FIFO
    // reaches them through dispatch() (pumped after the FIFO parse). The
    // Registration handles must outlive the frame loop, so they live here.
    auto reg_reload = registry.must_register_handler<viewer::ViewerReload>(
        matter::evt::CommandScope::App, app_lane, [&](const viewer::ViewerReload&) {
            // RECORD intent only; the model-clear + session->reload() heavy op
            // runs at the post-frame seam (S I.13), never mid-ImGui-draw. The
            // request is accepted and ACKed synchronously here.
            binding.request_reload();
            return viewer::ViewerReload::Result::succeeded(true);
        });
    auto reg_switch = registry.must_register_handler<viewer::ViewerSwitchWorld>(
        matter::evt::CommandScope::App, app_lane, [&](const viewer::ViewerSwitchWorld& cmd) {
            const int selected = cmd.index;
            if (selected < 0 || selected >= static_cast<int>(worlds.size()))
                return viewer::ViewerSwitchWorld::Result::failed("world index out of range");
            // RECORD intent only; SessionBinding::replace (the S I.13 epoch
            // sequence) runs at the post-frame seam, not synchronously mid-draw.
            // The request is accepted and ACKed here; the actual session
            // destroy/recreate (and success/failure bookkeeping) happens at the
            // seam, matching the original flag-based timing.
            binding.request_switch(selected);
            return viewer::ViewerSwitchWorld::Result::succeeded(true);
        });
    auto reg_open_part = registry.must_register_handler<viewer::WorkbenchOpenPart>(
        matter::evt::CommandScope::App, app_lane, [&](const viewer::WorkbenchOpenPart& cmd) {
            bake_lab.open_workbench_part(cmd.project, cmd.module);
            return viewer::WorkbenchOpenPart::Result::succeeded(true);
        });
    auto reg_focus_tab = registry.must_register_handler<viewer::LabFocusTab>(
        matter::evt::CommandScope::App, app_lane, [&](const viewer::LabFocusTab& cmd) {
            if (cmd.tab == "Workbench") bake_lab.focus_workbench_tab();
            return viewer::LabFocusTab::Result::succeeded(true);
        });
    auto reg_reveal = registry.must_register_handler<viewer::ViewerRevealPart>(
        matter::evt::CommandScope::App, app_lane, [&](const viewer::ViewerRevealPart& cmd) {
            // Reveal = the Scene tree's baked-root click + Focus, addressed by
            // module name (Asset Browser "Reveal"). Reuses the loop's cached
            // snapshot, refreshed by generation like the draw site below —
            // plus an empty-cache fetch, because graph_generation() can sit
            // at 0 for a whole session (resolve-cache-hit worlds) and the
            // loop's 0-initialized sentinel then never fetches at all.
            const uint64_t gen = session->graph_generation();
            if ((gen != cached_graph_gen || cached_snapshot.nodes.empty()) &&
                session->graph_snapshot(cached_snapshot))
                cached_graph_gen = gen;
            const uint64_t hash =
                viewer::reveal_part_in_world(cached_snapshot, cmd.module, selection_set);
            if (hash == 0) {
                // Not an error: the world simply doesn't carry the part. Say
                // so where the user can see it — a silent no-op here is the
                // exact defect this command replaced.
                const bool known = cached_snapshot.nodes.count(cmd.module) != 0;
                console_log.push(viewer::LogSeverity::Info,
                                 known ? "Reveal: " + cmd.module +
                                             " only appears inside other parts here "
                                             "(no world instance of its own)"
                                       : "Reveal: " + cmd.module +
                                             " is not loaded in the current world");
                return viewer::ViewerRevealPart::Result::succeeded(false);
            }
            editor_model.clear_selection();
            ui.select_baked_root(hash);
            viewer::focus_camera_on_selection(
                camera, selection_set, field_commands,
                [&](uint64_t part_hash, viewer::SelectionBounds& out) {
                    viewer::SelectedObject obj{viewer::SelectedObject::BakedRoot, part_hash};
                    return viewer::bounds_for_object(obj, *session, out);
                });
            return viewer::ViewerRevealPart::Result::succeeded(true);
        });

    // ---- FIFO dev-convenience command handlers (S II.3.4) -------------------
    auto reg_fifo_cam = registry.must_register_handler<viewer::FifoSetCamera>(
        matter::evt::CommandScope::App, app_lane, [&](const viewer::FifoSetCamera& cmd) {
            camera.position = {cmd.eye[0], cmd.eye[1], cmd.eye[2]};
            camera.target = {cmd.target[0], cmd.target[1], cmd.target[2]};
            return viewer::FifoSetCamera::Result::succeeded(true);
        });
    auto reg_fifo_shot = registry.must_register_handler<viewer::FifoScreenshot>(
        matter::evt::CommandScope::App, app_lane, [&](const viewer::FifoScreenshot& cmd) {
            shot_path = cmd.path;
            shot_settle = 3;
            return viewer::FifoScreenshot::Result::succeeded(true);
        });
    auto reg_fifo_stats = registry.must_register_handler<viewer::FifoStatsLabel>(
        matter::evt::CommandScope::App, app_lane, [&](const viewer::FifoStatsLabel& cmd) {
            stats_label = cmd.label;
            return viewer::FifoStatsLabel::Result::succeeded(true);
        });
    auto reg_fifo_budget = registry.must_register_handler<viewer::FifoBudget>(
        matter::evt::CommandScope::App, app_lane, [&](const viewer::FifoBudget& cmd) {
            stats.pixel_budget = std::max(0.05f, std::min(4.0f, cmd.value));
            return viewer::FifoBudget::Result::succeeded(true);
        });
    auto reg_fifo_dlss = registry.must_register_handler<viewer::FifoDlss>(
        matter::evt::CommandScope::App, app_lane, [&](const viewer::FifoDlss& cmd) {
            if (cmd.mode == "native") selected_dlss_mode = matter::DlssMode::Native;
            else if (cmd.mode == "quality") selected_dlss_mode = matter::DlssMode::Quality;
            else if (cmd.mode == "balanced") selected_dlss_mode = matter::DlssMode::Balanced;
            else if (cmd.mode == "performance") selected_dlss_mode = matter::DlssMode::Performance;
            else {
                std::printf("dlss: expected native, quality, balanced, or performance\n");
                return viewer::FifoDlss::Result::failed("bad dlss mode");
            }
            return viewer::FifoDlss::Result::succeeded(true);
        });
    auto reg_fifo_quit = registry.must_register_handler<viewer::FifoQuit>(
        matter::evt::CommandScope::App, app_lane, [&](const viewer::FifoQuit&) {
            quit_requested = true;
            return viewer::FifoQuit::Result::succeeded(true);
        });
    // Same SimulationControl calls the toolbar buttons make, so a FIFO-driven
    // capture exercises the identical transport path the UI does.
    auto reg_fifo_sim = registry.must_register_handler<viewer::FifoSimTransport>(
        matter::evt::CommandScope::App, app_lane,
        [&](const viewer::FifoSimTransport& cmd) {
            using Action = viewer::FifoSimTransport::Action;
            std::string sim_err;
            bool ok = false;
            switch (cmd.action) {
                case Action::Play: ok = sim_control.play(session->ecs(), sim_err); break;
                case Action::Pause: ok = sim_control.pause(sim_err); break;
                case Action::Step: ok = sim_control.step(sim_err); break;
                case Action::Stop: ok = sim_control.stop(session->ecs(), sim_err); break;
            }
            if (!ok) {
                std::fprintf(stderr, "sim transport: %s\n", sim_err.c_str());
                return viewer::FifoSimTransport::Result::failed(sim_err);
            }
            if (cmd.action == Action::Stop) {
                selection_set.clear();
                editor_model.clear_selection();
            }
            return viewer::FifoSimTransport::Result::succeeded(true);
        });

    // ---- E5c scene-edit handlers (ActiveSession; SceneService) --------------
    // The first ActiveSession-scoped commands (event-system.md S I.14): each
    // mutates world entity state through the session's SceneService — the one
    // supported mutation path — and returns the typed SceneEditResult (with
    // created_id for create/duplicate). The mutation is observed by
    // SceneChangeTracker and published as a canonical delta at end-of-tick; the
    // handlers never hand-patch the model. Stamped with the ActiveSession epoch,
    // so a stale-epoch submission completes StaleScope (ids can't drift across a
    // world switch).
    auto reg_scene_create = registry.must_register_handler<viewer::SceneCreateEntity>(
        matter::evt::CommandScope::ActiveSession, app_lane,
        [&](const viewer::SceneCreateEntity& cmd) {
            return viewer::SceneCreateEntity::Result::succeeded(
                session->scene_service().create_empty(cmd.name));
        });
    auto reg_scene_dup = registry.must_register_handler<viewer::SceneDuplicateEntity>(
        matter::evt::CommandScope::ActiveSession, app_lane,
        [&](const viewer::SceneDuplicateEntity& cmd) {
            return viewer::SceneDuplicateEntity::Result::succeeded(
                session->scene_service().duplicate(cmd.src));
        });
    auto reg_scene_del = registry.must_register_handler<viewer::SceneDeleteEntity>(
        matter::evt::CommandScope::ActiveSession, app_lane,
        [&](const viewer::SceneDeleteEntity& cmd) {
            return viewer::SceneDeleteEntity::Result::succeeded(
                session->scene_service().delete_entity(cmd.target));
        });
    auto reg_scene_reparent = registry.must_register_handler<viewer::SceneReparentEntity>(
        matter::evt::CommandScope::ActiveSession, app_lane,
        [&](const viewer::SceneReparentEntity& cmd) {
            return viewer::SceneReparentEntity::Result::succeeded(
                session->scene_service().reparent(cmd.child, cmd.new_parent));
        });

    // Scene-tree mutation bridge (E5c): same std::function idiom as
    // FieldCommands, but each closure now issues a SceneService ActiveSession
    // command via execute() (synchronous, typed) instead of touching the ECS
    // directly. execute() returns the typed SceneEditResult so create/duplicate
    // still expose created_id for immediate selection; a StaleScope/Rejected
    // (value-less) result maps to an InvalidTarget edit error.
    auto scene_edit_result = [](matter::scene::SceneEditResult r,
                                bool has_value) -> matter::scene::SceneEditResult {
        return has_value ? r
                         : matter::scene::SceneEditResult{
                               matter::scene::SceneEditError::InvalidTarget, {}};
    };
    scene_commands.create_empty =
        [&, scene_edit_result](const std::string& name) -> matter::scene::SceneEditResult {
        viewer::SceneCreateEntity cmd;
        cmd.name = name;
        auto res = registry.execute(cmd);
        return scene_edit_result(res.value.value_or(matter::scene::SceneEditResult{}),
                                 res.value.has_value());
    };
    scene_commands.duplicate =
        [&, scene_edit_result](matter::scene::SceneEntityId src) -> matter::scene::SceneEditResult {
        viewer::SceneDuplicateEntity cmd;
        cmd.src = src;
        auto res = registry.execute(cmd);
        return scene_edit_result(res.value.value_or(matter::scene::SceneEditResult{}),
                                 res.value.has_value());
    };
    scene_commands.delete_entity =
        [&, scene_edit_result](matter::scene::SceneEntityId target) -> matter::scene::SceneEditResult {
        viewer::SceneDeleteEntity cmd;
        cmd.target = target;
        auto res = registry.execute(cmd);
        return scene_edit_result(res.value.value_or(matter::scene::SceneEditResult{}),
                                 res.value.has_value());
    };
    scene_commands.reparent =
        [&, scene_edit_result](matter::scene::SceneEntityId child,
                               matter::scene::SceneEntityId new_parent)
        -> matter::scene::SceneEditResult {
        viewer::SceneReparentEntity cmd;
        cmd.child = child;
        cmd.new_parent = new_parent;
        auto res = registry.execute(cmd);
        return scene_edit_result(res.value.value_or(matter::scene::SceneEditResult{}),
                                 res.value.has_value());
    };

    // ---- ViewerCommands bridge (issued by UI panels; S I.11) ----------------
    // Same idiom as SceneCommands/FieldCommands: each closure runs on the app
    // lane (the UI/main thread owns it) and issues the typed command through
    // execute() — synchronous, preserving today's immediate handling.
    viewer::ViewerCommands viewer_commands;
    viewer_commands.reload = [&]() { registry.execute(viewer::ViewerReload{}); };
    viewer_commands.switch_world = [&](int index) {
        viewer::ViewerSwitchWorld cmd;
        cmd.index = index;
        registry.execute(cmd);
    };
    viewer_commands.open_in_workbench = [&](const std::string& project,
                                            const std::string& module) {
        viewer::WorkbenchOpenPart open_cmd;
        open_cmd.project = project;
        open_cmd.module = module;
        registry.execute(open_cmd);
        viewer::LabFocusTab focus_cmd;
        focus_cmd.tab = "Workbench";
        registry.execute(focus_cmd);
    };
    viewer_commands.reveal_part = [&](const std::string& module) {
        viewer::ViewerRevealPart cmd;
        cmd.module = module;
        registry.execute(cmd);
    };

    while (!glfwWindowShouldClose(window) && !quit_requested && !fatal_error) {
        // This starts before event polling and begin_frame(), whose fence wait and
        // swapchain acquire are part of the user-visible frame cadence.
        const auto perf_frame_start = std::chrono::steady_clock::now();
        // Main-loop phase attribution (ViewerStats::loop_*_ms). Rolling split
        // timer: each phase_split() returns the ms since the previous split, so
        // the phases exactly partition perf_frame_start..end_frame. Added
        // because resolve/build/draw accounted for under a third of the frame
        // and the remainder had no attribution at all.
        struct LoopPhase {
            double poll = 0, acquire = 0, ui = 0, tick = 0;
            double pump = 0, lab = 0, render = 0, present = 0;
        } phase{};
        auto phase_mark = perf_frame_start;
        auto phase_split = [&phase_mark]() -> double {
            const auto split_now = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(
                                  split_now - phase_mark).count();
            phase_mark = split_now;
            return ms;
        };
        glfwPollEvents();
        // Retire preview textures queued on earlier frames. Must run before any
        // drawing: they are freed here precisely because freeing them at the
        // point of retirement would pull a descriptor out from under the draw
        // list of the frame that retired it.
        issue_previews.collect();
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - previous_time).count();
        previous_time = now;
        if (key_pressed(window, GLFW_KEY_TAB, tab_down)) {
            camera_capture = !camera_capture;
            camera_controller.set_capture(window, camera_capture);
        }
        // Issue capture hotkeys. Handled HERE, at the GLFW level, rather than
        // beside the ImGui hotkeys below: ui.cpp enables
        // ImGuiConfigFlags_NavEnableKeyboard, which makes io.WantCaptureKeyboard
        // true whenever a panel holds nav focus, so an ImGui-gated hotkey fires
        // only sometimes. Neither key is a text character, so claiming them
        // unconditionally costs nothing.
        //
        // F9 was previously a wireframe toggle that only ever printed "not
        // available in Vulkan milestone"; the Vulkan path hardcodes
        // options.wireframe = false. The FIFO `wireframe` verbs keep their stub.
        const bool issue_capture_idle =
            issue_state.phase != viewer::ReporterPhase::AwaitingCapture &&
            issue_state.phase != viewer::ReporterPhase::SelectingRegion;
        if (key_pressed(window, GLFW_KEY_F9, f9_down) && issue_capture_idle) {
            viewer::begin_region_capture(issue_state);
            std::printf("issue capture: freezing screen for region select\n");
        }
        if (key_pressed(window, GLFW_KEY_F10, f10_down) && issue_capture_idle) {
            viewer::begin_viewport_capture(issue_state);
            std::printf("issue capture: viewport\n");
        }
        if (key_pressed(window, GLFW_KEY_F8, f8_down)) {
            if (!dlss_modes_supported) {
                selected_dlss_mode = matter::DlssMode::Native;
                std::printf("DLSS: Native (%s)\n",
                            vulkan->dlss_unavailable_reason().c_str());
            } else {
                switch (selected_dlss_mode) {
                    case matter::DlssMode::Native:
                        selected_dlss_mode = matter::DlssMode::Quality;
                        break;
                    case matter::DlssMode::Quality:
                        selected_dlss_mode = matter::DlssMode::Balanced;
                        break;
                    case matter::DlssMode::Balanced:
                        selected_dlss_mode = matter::DlssMode::Performance;
                        break;
                    case matter::DlssMode::Performance:
                        selected_dlss_mode = matter::DlssMode::Native;
                        break;
                }
            }
        }
#ifndef _WIN32
        if (cmd_fd >= 0) {
            char bytes[512];
            ssize_t count = 0;
            while ((count = read(cmd_fd, bytes, sizeof(bytes))) > 0)
                cmd_buffer.append(bytes, static_cast<size_t>(count));
        }
#else
        if (cmd_handle != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size{};
            if (GetFileSizeEx(cmd_handle, &size) &&
                size.QuadPart < cmd_offset.QuadPart)
                cmd_offset.QuadPart = 0;
            if (size.QuadPart > cmd_offset.QuadPart) {
                SetFilePointerEx(cmd_handle, cmd_offset, nullptr, FILE_BEGIN);
                char bytes[512];
                DWORD count = 0;
                while (ReadFile(cmd_handle, bytes, sizeof(bytes), &count,
                                nullptr) && count > 0) {
                    cmd_buffer.append(bytes, static_cast<size_t>(count));
                    cmd_offset.QuadPart += count;
                    if (count < sizeof(bytes)) break;
                }
            }
        }
#endif
        {
            size_t newline = 0;
            while ((newline = cmd_buffer.find('\n')) != std::string::npos) {
                std::string line = cmd_buffer.substr(0, newline);
                cmd_buffer.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                // MATTER_CMD_FIFO is a cross-thread command source (S II.3.4):
                // parse each line into its typed command and dispatch() it so
                // every external submission is named/traced/journaled and gets
                // an explicit ticket completion. The queued jobs run at the
                // registry pump right below (still this frame, before render).
                float c[6]; char word[256];
                if (std::sscanf(line.c_str(), "cam %f %f %f %f %f %f",
                                &c[0], &c[1], &c[2], &c[3], &c[4], &c[5]) == 6) {
                    viewer::FifoSetCamera cmd;
                    cmd.eye[0] = c[0]; cmd.eye[1] = c[1]; cmd.eye[2] = c[2];
                    cmd.target[0] = c[3]; cmd.target[1] = c[4]; cmd.target[2] = c[5];
                    registry.dispatch(cmd);
                } else if (std::sscanf(line.c_str(), "shot %255s", word) == 1) {
                    viewer::FifoScreenshot cmd; cmd.path = word;
                    registry.dispatch(cmd);
                } else if (std::sscanf(line.c_str(), "stats %255s", word) == 1) {
                    viewer::FifoStatsLabel cmd; cmd.label = word;
                    registry.dispatch(cmd);
                } else if (std::sscanf(line.c_str(), "budget %f", &c[0]) == 1) {
                    viewer::FifoBudget cmd; cmd.value = c[0];
                    registry.dispatch(cmd);
                } else if (std::sscanf(line.c_str(), "hiz %255s", word) == 1) {
                    std::printf("hiz: not available in Vulkan milestone\n");
                } else if (std::sscanf(line.c_str(), "dlss %255s", word) == 1) {
                    viewer::FifoDlss cmd; cmd.mode = word;
                    registry.dispatch(cmd);
                } else if (std::sscanf(line.c_str(), "workbench %255s", word) == 1) {
                    // Same two commands the Asset Browser's "Open in Workbench"
                    // button issues, so a headless/scripted run can exercise the
                    // exact chain a click does (and screenshot the result). The
                    // project is found by probing each scanned project for the
                    // module's source, mirroring how the browser scopes its rows.
                    std::string project_dir;
                    for (const viewer::WorldEntry& w : worlds) {
                        std::error_code probe_ec;
                        if (std::filesystem::is_regular_file(
                                std::filesystem::path(w.project_dir) / "objects" /
                                    (std::string(word) + ".js"),
                                probe_ec)) {
                            project_dir = w.project_dir;
                            break;
                        }
                    }
                    if (project_dir.empty()) {
                        std::printf("workbench: no project has objects/%s.js\n", word);
                    } else {
                        viewer::WorkbenchOpenPart open_cmd;
                        open_cmd.project = project_dir;
                        open_cmd.module = word;
                        registry.dispatch(open_cmd);
                        viewer::LabFocusTab focus_cmd;
                        focus_cmd.tab = "Workbench";
                        registry.dispatch(focus_cmd);
                    }
                } else if (std::sscanf(line.c_str(), "reveal %255s", word) == 1) {
                    // Same command the Asset Browser's "Reveal" button issues,
                    // for scripted/headless verification of select+focus.
                    viewer::ViewerRevealPart cmd;
                    cmd.module = word;
                    registry.dispatch(cmd);
                } else if (line == "reload") {
                    registry.dispatch(viewer::ViewerReload{});
                } else if (line == "wireframe" || line == "wireframe toggle") {
                    std::printf("wireframe: not available in Vulkan milestone\n");
                } else if (line == "wireframe on" || line == "wireframe off") {
                    std::printf("wireframe: not available in Vulkan milestone\n");
                } else if (line == "quit") {
                    registry.dispatch(viewer::FifoQuit{});
                } else if (std::sscanf(line.c_str(), "timescale %f", &c[0]) == 1) {
                    if (std::isfinite(c[0]) && c[0] >= viewer::kToolbarMinTimeScale &&
                        c[0] <= viewer::kToolbarMaxTimeScale)
                        ui.set_sim_time_scale(c[0]);
                    else
                        std::printf("timescale: %.3f outside [%.2f, %.2f]\n", c[0],
                                    viewer::kToolbarMinTimeScale,
                                    viewer::kToolbarMaxTimeScale);
                } else if (line == "play" || line == "pause" ||
                           line == "step" || line == "sim stop") {
                    using Action = viewer::FifoSimTransport::Action;
                    viewer::FifoSimTransport cmd;
                    cmd.action = line == "play"  ? Action::Play
                               : line == "pause" ? Action::Pause
                               : line == "step"  ? Action::Step
                                                 : Action::Stop;
                    registry.dispatch(cmd);
                } else if (!line.empty()) {
                    std::printf("cmd: unrecognized '%s'\n", line.c_str());
                }
            }
        }
        // Frame-loop command point: run the FIFO-dispatched commands (and any
        // then()-continuations) on the app lane. Placed here — before begin_frame
        // and the camera snapshot — so a FIFO `cam`/`budget` applies to THIS
        // frame's render exactly as the old inline handling did.
        registry.pump(app_lane, 5.0);

        if (test_resize && bake_ready && !resize_exercised) {
            glfwSetWindowSize(window, 960, 540);
            glfwPollEvents();
            screenshot_settle = 0;
            resize_exercised = true;
        }

        phase.poll = phase_split();   // events + input, everything up to acquire
        matter::VulkanFrame frame{};
        if (!vulkan->begin_frame(frame, error)) {
            if (error.find("zero-sized") != std::string::npos) {
                glfwWaitEventsTimeout(0.05);
                continue;
            }
            std::fprintf(stderr, "FATAL: begin_frame: %s\n", error.c_str());
            break;
        }

        phase.acquire = phase_split();   // fence wait + swapchain acquire
        matter_viewer::CurrentFrameInputOrder camera_input_order{};
        const bool ui_frame_ready = ui.begin_frame(frame, error);
        matter::VulkanFrame render_frame = frame;
        // Reset before the Bake Lab tab bar draws so wants_viewport() below
        // reflects whether the Workbench tab is actually focused THIS frame
        // (see part_workbench.h's modal-isolation note).
        bake_lab.workbench().begin_frame();
        if (!ui_frame_ready) {
            std::fprintf(stderr, "FATAL: ImGui Vulkan prepare: %s\n",
                         error.c_str());
            fatal_error = true;
        } else {
            camera_input_order.begin_ui();
            // E5c: no per-frame editor_model.refresh() — the scene tree is
            // delta-driven by the SessionBinding scene adapter, and its flattened
            // rows are re-derived at property_scheduler.flush_dirty() (after tick)
            // only on ticks that actually changed rows.
            // Gizmo mode hotkeys (G/T = translate, R = rotate, S = scale).
            // Only when ImGui isn't capturing keyboard/text input, so typing
            // in a Properties field doesn't retarget the gizmo.
            {
                const ImGuiIO& io = ImGui::GetIO();
                if (!io.WantTextInput && !io.WantCaptureKeyboard) {
                    ui.update_gizmo_hotkeys();
                    // Task 13: F focuses the camera on the current selection;
                    // Delete removes every selected entity (Edit/Pause only).
                    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
                        viewer::focus_camera_on_selection(camera, selection_set,
                                                          field_commands);
                    }
                    if (sim_control.mode() != matter::scene::SimulationMode::Play &&
                        ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
                        const std::vector<viewer::SelectedObject> to_delete =
                            selection_set.items();
                        for (const auto& obj : to_delete) {
                            if (obj.kind != viewer::SelectedObject::Entity) continue;
                            flecs::entity check = find_scene_entity(
                                session->ecs(),
                                matter::scene::SceneEntityId{obj.id});
                            if (!check.is_valid()) continue;
                            editor_model.select(matter::scene::SceneEntityId{obj.id});
                            const matter::scene::SceneEditResult result =
                                editor_model.delete_selected(scene_commands);
                            if (result.error != matter::scene::SceneEditError::None) {
                                console_log.push(viewer::LogSeverity::Error,
                                                 "Delete: entity mutation failed");
                            }
                        }
                        selection_set.clear();
                        editor_model.clear_selection();
                    }
                }
            }
            if (!hide_ui) ui.prepare_viewport_rect();
            render_frame = ui.viewport_render_frame(frame, error);
            if (!error.empty()) {
                std::fprintf(stderr, "viewport target: %s\n", error.c_str());
                error.clear();
            }
            if (!hide_ui) {
                const viewer::ToolbarActions toolbar =
                    ui.draw_toolbar(sim_control.mode());
                if (toolbar.play_clicked) {
                    std::string sim_err;
                    if (!sim_control.play(session->ecs(), sim_err))
                        std::fprintf(stderr, "play: %s\n", sim_err.c_str());
                }
                if (toolbar.pause_clicked) {
                    std::string sim_err;
                    if (!sim_control.pause(sim_err))
                        std::fprintf(stderr, "pause: %s\n", sim_err.c_str());
                }
                if (toolbar.step_clicked) {
                    std::string sim_err;
                    if (!sim_control.step(sim_err))
                        std::fprintf(stderr, "step: %s\n", sim_err.c_str());
                }
                if (toolbar.stop_clicked) {
                    std::string sim_err;
                    if (!sim_control.stop(session->ecs(), sim_err)) {
                        std::fprintf(stderr, "stop: %s\n", sim_err.c_str());
                    } else {
                        selection_set.clear();
                        editor_model.clear_selection();
                    }
                }
                {
                    const uint64_t gen = session->graph_generation();
                    if (gen != cached_graph_gen) {
                        if (!session->graph_snapshot(cached_snapshot)) {
                            // Only fails while THIS session has published
                            // nothing — anything cached is a dead world's
                            // (see sync_scene_tree_graph_cache).
                            cached_snapshot = part_graph_snapshot::Snapshot{};
                        }
                        cached_graph_gen = gen;
                    }
                }
                const std::unordered_set<uint64_t>* authored_ptr = nullptr;
                std::unordered_set<uint64_t> authored_entity_ids;
                if (sim_control.has_snapshot()) {
                    for (const auto& ent : sim_control.snapshot().entities) {
                        authored_entity_ids.insert(ent.id.value);
                    }
                    authored_ptr = &authored_entity_ids;
                }
                ui.draw_scene_panel(editor_model, session.get(), &scene_commands,
                                   sim_control.mode(), &camera, &selection_set,
                                   &field_commands, &console_log, authored_ptr);
                ui.draw_properties_panel(selection_set, editor_model, properties_registry,
                                        field_commands, component_commands, sim_control.mode(),
                                        &cached_snapshot, specialized_editors, camera.position);
                ui.draw_viewport_window();
                ui.draw_console_panel(console_log);
                ui.draw_debug_panel(stats, viewer_commands);
                ui.draw_bake_lab_panel(bake_lab, &app_hub, session.get(), worlds, stats);
                ui.draw_asset_browser_panel(asset_browser, worlds, stats, shared_lib,
                                           viewer_commands);
                ui.draw_worlds_panel(worlds, stats, viewer_commands);
                ui.draw_camera_panel(camera);
                // Issue reporter (F9 region / F10 viewport). Drawn last so the
                // selection overlay and the window sit above the panels they
                // might be reporting on.
                const bool file_issue = viewer::draw_issue_reporter(
                    issue_state, frame.extent.width, frame.extent.height);

                // A drag just resolved: crop the FROZEN frame (not the live
                // one — the scene may have moved since F9) and write the shot.
                if (issue_state.selection_committed) {
                    issue_state.selection_committed = false;
                    if (viewer::ensure_report_dir(issue_state)) {
                        viewer::ShotRect rect = issue_state.pending_rect;
                        const std::vector<uint8_t> cropped = viewer::crop_rgba(
                            issue_state.frozen, issue_state.frozen_width,
                            issue_state.frozen_height, rect);
                        const std::string path =
                            issue_state.dir + "/shot-" +
                            std::to_string(issue_state.next_shot_index++) + ".png";
                        if (write_png(path, cropped,
                                      static_cast<uint32_t>(rect.w),
                                      static_cast<uint32_t>(rect.h))) {
                            viewer::IssueShot shot;
                            shot.file = path.substr(path.find_last_of('/') + 1);
                            shot.region = issue_state.pending_region;
                            shot.rect = rect;
                            // Against the FROZEN frame's dimensions: the
                            // swapchain may have been resized since F9.
                            stamp_replay_state(shot, issue_state.frozen_width,
                                               issue_state.frozen_height);
                            shot.width = static_cast<uint32_t>(rect.w);
                            shot.height = static_cast<uint32_t>(rect.h);
                            // session->frame_stats() rather than the loop's
                            // `frame_stats` reference: that binding is made
                            // later in the frame, below the UI pass.
                            const matter::FrameStats& fs = session->frame_stats();
                            shot.frame_ms = stats.frame_ms;
                            shot.instances_drawn = fs.instances_drawn;
                            shot.triangles = fs.triangles;
                            shot.draw_batches = fs.draw_batches;
                            attach_preview(shot, cropped, shot.width, shot.height);
                            viewer::record_shot(issue_state, shot);
                            std::printf("issue shot written to %s\n", path.c_str());
                        } else {
                            issue_state.status = "could not write " + path;
                            issue_state.status_is_error = true;
                            console_log.push(viewer::LogSeverity::Error,
                                             "Issue shot: " + issue_state.status);
                        }
                    } else {
                        console_log.push(viewer::LogSeverity::Error,
                                         "Issue shot: " + issue_state.status);
                    }
                }

                // The frozen frame is only live while a selection is being
                // made. Releasing on the phase (rather than inside the commit
                // above) also covers Esc, which would otherwise strand 8 MB and
                // a descriptor until the report was filed.
                if (issue_state.phase != viewer::ReporterPhase::SelectingRegion &&
                    issue_state.phase != viewer::ReporterPhase::AwaitingCapture &&
                    (issue_state.frozen_preview || !issue_state.frozen.empty())) {
                    if (issue_state.frozen_preview) {
                        issue_previews.destroy(issue_state.frozen_preview);
                        issue_state.frozen_preview = nullptr;
                    }
                    issue_state.frozen.clear();
                    issue_state.frozen.shrink_to_fit();
                    issue_state.frozen_width = issue_state.frozen_height = 0;
                }

                // Discard: the panel signals by dropping to Idle with shots
                // still attached, since it cannot free the textures itself.
                if (issue_state.phase == viewer::ReporterPhase::Idle &&
                    (!issue_state.shots.empty() || issue_state.frozen_preview))
                    reset_issue_state();

                if (file_issue) {
                    viewer::IssueContext context;
                    context.world = worlds[stats.world_current].world_name;
                    context.project_dir = worlds[stats.world_current].project_dir;
                    context.camera = camera;
                    context.sim_mode = sim_control.mode();
                    context.time_scale = ui.sim_time_scale();
                    context.frame_width = frame.extent.width;
                    context.frame_height = frame.extent.height;
                    const std::string filed = viewer::write_issue_report(
                        issue_state, context, stats, session->frame_stats(),
                        console_log);
                    if (filed.empty()) {
                        console_log.push(viewer::LogSeverity::Error,
                                         "Issue report: " + issue_state.status);
                    } else {
                        std::printf("issue filed: %s\n", filed.c_str());
                        console_log.push(viewer::LogSeverity::Info,
                                         "Issue report written to " + filed);
                        // Reset for the next one; the window stays closed until
                        // the next capture, so filing does not swallow keys.
                        reset_issue_state();
                    }
                }
                // draw_sector_streaming_panel retired in Phase 4 Task 12 — sector
                // streaming editing now lives in the Properties panel via
                // SpecializedEditors (MatterEditor/src/specialized_editors.h).
                {
                    const auto& vp = ui.viewport_rect();
                    ui.draw_gizmo(selection_set, field_commands, camera,
                                 sim_control.mode(), vp.x, vp.y, vp.w, vp.h);
                }
            }
            camera_input_order.build_ui();
            camera_input_order.decide_capture(ui.camera_input_allowed());
        }

        // UI actions (including Frame Anchor) and the gizmo have finished. Keep
        // this snapshot immutable through streaming, tick, scene render, and UI
        // submission so every current-frame camera consumer agrees.
        const matter::CameraDesc frame_camera = camera;

        {
            const bool mouse_down =
                glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            const bool mouse_clicked = mouse_down && !left_mouse_down;
            left_mouse_down = mouse_down;
            if (mouse_clicked && ui_frame_ready && ui.camera_input_allowed()) {
                double cursor_x = 0.0, cursor_y = 0.0;
                glfwGetCursorPos(window, &cursor_x, &cursor_y);
                int fb_width = 0, fb_height = 0;
                glfwGetFramebufferSize(window, &fb_width, &fb_height);
                const auto& vp = ui.viewport_rect();
                const bool in_viewport =
                    cursor_x >= vp.x && cursor_x < vp.x + vp.w &&
                    cursor_y >= vp.y && cursor_y < vp.y + vp.h;
                const viewer::PickResult pick = in_viewport
                    ? viewer::viewport_pick(
                          static_cast<float>(cursor_x - vp.x),
                          static_cast<float>(cursor_y - vp.y),
                          static_cast<int>(vp.w),
                          static_cast<int>(vp.h),
                          frame_camera, *session)
                    : viewer::PickResult{};
                if (pick.hit) {
                    selection_set.replace(pick.object);
                    // Keep the outliner's single-slot selection mirrored on
                    // the primary pick (viewport and outliner share the same
                    // selection).
                    if (pick.object.kind == viewer::SelectedObject::Entity) {
                        editor_model.select(
                            matter::scene::SceneEntityId{pick.object.id});
                    } else {
                        editor_model.clear_selection();
                    }
                } else {
                    selection_set.clear();
                    editor_model.clear_selection();
                }
            }
        }

        selection_set.validate([&](const viewer::SelectedObject& obj) {
            if (obj.kind == viewer::SelectedObject::BakedRoot) {
                const uint32_t count = session->instance_count();
                for (uint32_t i = 0; i < count; ++i) {
                    matter::InstanceInfo info;
                    if (session->instance_info(i, info) && info.part_hash == obj.id)
                        return true;
                }
                return false;
            }
            // Entity selections are keyed by SceneEntityId (the stable
            // authored-id hash), not by flecs entity id — resolve through the
            // SceneEntityId component so dynamic ECS entities stay selected
            // across frames.
            return find_scene_entity(session->ecs(),
                                     matter::scene::SceneEntityId{obj.id})
                .is_valid();
        });

        // Streaming (world-kind) sessions render nothing until an ECS entity
        // carries matter::streaming::SectorStreaming — that component is what
        // makes the coordinator claim an owner and build a SectorStreamer.
        // Nothing else in the viewer creates one since the sector panel was
        // retired, so auto-create + attach the anchor as soon as the session is
        // Ready. Runs every frame on purpose: it is idempotent against live ECS
        // state, so a reload or world switch (which resets bake_ready and
        // installs a fresh Flecs world) transparently re-creates the anchor. A
        // no-op for closed-world sessions.
        if (bake_ready) ui.ensure_streaming_anchor(*session);
        ui.update_sector_streaming(*session, frame_camera);
        matter::TickDesc tick{};
        // Slow motion scales the frame delta only. fixed_delta_seconds is left
        // alone so the fixed step keeps its size and simply occurs less often;
        // scaling it would change what the simulation does, not how fast it
        // runs. A single-frame Step is exempt -- it must advance one whole
        // fixed step regardless of the inspection rate.
        tick.frame_delta_seconds = dt * ui.sim_time_scale();
        // Presentation cadence (animation pose-LOD refresh) always runs on the
        // unscaled wall delta: slow motion changes what is simulated per frame,
        // never how often the shown pose refreshes.
        tick.presentation_delta_seconds = dt;
        if (sim_control.should_advance_fixed()) {
            // Play mode: run physics normally.
        } else if (sim_control.consume_pending_step()) {
            tick.max_fixed_steps = 1;
        } else {
            // Edit/Pause. This used to pass max_fixed_steps = 0, which
            // Runtime::tick defines as MALFORMED -- so WorldSession::tick
            // returned at its invalid guard and skipped everything below it,
            // including animation reconciliation. The visible symptom was that
            // an animated entity never produced a binding until you pressed
            // Play, so the Part Workbench animation tabs were empty in exactly
            // the mode an author inspects a rig in.
            //
            // advance_fixed = false is the sanctioned form: an ordinary,
            // VALID frame tick that advances no fixed simulation and leaves the
            // accumulator untouched, so Stop means stopped and resuming
            // continues from the exact sub-step position it froze at.
            tick.advance_fixed = false;
            // Frame-cadence work must not creep forward either while stopped;
            // the frame pipeline still RUNS (lifecycle reconciliation needs it),
            // it simply advances nothing.
            tick.frame_delta_seconds = 0.0f;
        }
        phase.ui = phase_split();   // ImGui panel building
        session->tick(tick);
        phase.tick = phase_split();   // ECS systems, physics, transform propagation
        // E5c (event-system.md S I.14): flush observable models AFTER tick, so
        // this frame's tick -> SceneChangeTracker::flush -> scene-adapter apply ->
        // Property::set have all run and their coalesced deliveries land now
        // (scene deltas from tick N reach the UI this frame, not N+1).
        property_scheduler.flush_dirty();
        camera_input_order.tick_scene();
        session->pump_gpu_jobs(4.0f);
        // Sector publish lands here. Channel::pump checks its budget AFTER
        // running a job, so one long publish can blow past the 4ms argument.
        phase.pump = phase_split();
        // Part Workbench (W2): the isolation session ticks/pumps every frame
        // regardless of tab focus, so switching back to it is instant (no
        // reload) and a bake keeps progressing while you're on another tab.
        bake_lab.workbench().tick(dt);
        bake_lab.workbench().pump_gpu_jobs(2.0f);
        bake_lab.tick_frame(viewer::BakeLab::kDefaultTickBudgetMs);
        matter::Event event;
        while (session->poll_event(event)) {
            if (event.type == matter::EventType::BakePartDone)
                std::printf("bake %d/%d %s\n", event.done, event.total,
                            event.module.c_str());
            else if (event.type == matter::EventType::BakeFinished) {
                std::printf("bake finished (%d errors)\n", event.errors);
                bake_ready = event.errors == 0;
                if (bake_ready && apply_world_camera_after_bake) {
                    if (session->apply_authored_camera(camera)) {
                        std::printf(
                            "world camera: eye(%.1f,%.1f,%.1f) "
                            "target(%.1f,%.1f,%.1f)\n",
                            camera.position.x, camera.position.y,
                            camera.position.z, camera.target.x,
                            camera.target.y, camera.target.z);
                    }
                    apply_world_camera_after_bake = false;
                }
                console_log.push(
                    event.errors == 0 ? viewer::LogSeverity::Info
                                       : viewer::LogSeverity::Warning,
                    "Bake finished: " + std::to_string(event.done) +
                        " parts, " + std::to_string(event.errors) + " errors");
                matter::InstanceInfo selected{};
                if (bake_ready && !selected_world_reported &&
                    session->instance_info(0, selected)) {
                    std::printf("selected world %s hash %016llx\n",
                                worlds[stats.world_current].world_name.c_str(),
                                static_cast<unsigned long long>(selected.part_hash));
                    selected_world_reported = true;
                }
                if (fifo_path) std::printf("viewer: bake ready\n");
            } else if (event.type == matter::EventType::BakeError) {
                std::printf("bake error [%s]: %s\n", event.module.c_str(),
                            event.message.c_str());
                console_log.push(viewer::LogSeverity::Error,
                                  "[" + event.module + "] " + event.message);
            }
        }
        matter::RenderOptions options;
        options.path = matter::RenderPath::GpuDriven;
        options.resolver = stats.resolver_choice == 1
                               ? matter::ResolverKind::SectorLod
                               : matter::ResolverKind::PassThrough;
        options.wireframe = false;
        options.hiz_occlusion = false;
        options.pixel_budget = stats.pixel_budget;
        options.active_radius = active_radius;
        options.min_projected_size = min_projected_size;
        options.dlss_mode = selected_dlss_mode;
        options.vulkan_lighting = stats.lighting;
        options.vulkan_lighting.composite_debug_view =
            stats.debug_view_mode == 1 ? 2.0f : 0.0f;
        options.vulkan_volumetrics = stats.volumetrics;
        options.vulkan_volumetrics.vol_debug_view =
            static_cast<float>(stats.vol_debug_view);
        options.vulkan_tileset_pom = stats.tileset_pom;
        options.vulkan_ray_tracing.enabled =
            vulkan->ray_tracing_available() && !disable_vulkan_rt;
        // Part Workbench (W2, "modal isolation" — see part_workbench.h):
        // VulkanFrame/render() always draws the whole frame extent and
        // begin_frame() yields exactly one frame per call, so only ONE
        // session's render() can run this frame. The Workbench tab being
        // focused (wants_viewport()) swaps which one — the production
        // session keeps ticking/pumping in the background either way, so
        // flipping tabs back is instant.
        const bool show_isolation =
            bake_lab.workbench().wants_viewport() && bake_lab.workbench().session();
        matter::WorldSession* render_session =
            show_isolation ? bake_lab.workbench().session() : session.get();
        const matter::CameraDesc& render_camera =
            show_isolation ? bake_lab.workbench().camera() : frame_camera;
        // Part Workbench W4 (part-workbench.md SS-I.5): the LOD Inspector's
        // force_lod/hide_child_instances debug toggles only ever apply to the
        // isolation session's own render — `options` (built above from the
        // production HUD's controls) stays untouched, and force_lod defaults
        // to -1 / hide_child_instances to false whenever the Inspector hasn't
        // been interacted with, so this is a no-op until the user acts.
        if (show_isolation) bake_lab.workbench().apply_lod_inspector_options(options);
        // Bake Lab/Workbench per-frame work plus the session event drain.
        phase.lab = phase_split();
        if (!render_session->render(render_camera, render_frame, options, error)) {
            std::fprintf(stderr, "FATAL: render: %s\n", error.c_str());
            fatal_error = true;
        } else if (!show_isolation) {
            camera_input_order.render_scene();
        }
        // Contains the engine's own resolve/build/draw spans — the line above
        // it in the panel breaks this down further; don't double-count.
        phase.render = phase_split();
        if (ui_frame_ready && !fatal_error) {
            // Required every frame regardless of which session rendered —
            // this transitions the offscreen viewport render target so
            // ImGui can sample it as a texture (Ui::draw_viewport_window).
            ui.transition_viewport_for_sampling(frame.command_buffer);
            // Overlay diagnostics default to a clean state; the production
            // branch below fills them in only when it actually queries the
            // session, so an isolation frame never reports stale counts.
            stats.animation_debug_query_ok = true;
            stats.animation_debug_instances = 0;
            if (!show_isolation) {
                // Selection outlines are keyed to the production session's
                // ECS/selection state; skip while the isolation session owns
                // the viewport image (nothing in `selection_set` refers to
                // the isolation world's entities).
                const auto& sel_vp = ui.viewport_rect();
                viewer::draw_selection_outlines(selection_set, frame_camera,
                                                static_cast<int>(render_frame.extent.width),
                                                static_cast<int>(render_frame.extent.height),
                                                *session, sel_vp.x, sel_vp.y);
                // The overlay reads that same production session and viewport
                // rect, so it lives under the isolation guard too.
                if (stats.animation_overlay.enabled) {
                    std::vector<matter::AnimationDebugInstanceSnapshot>
                        animation_debug;
                    stats.animation_debug_query_ok =
                        session->animation_debug_snapshots(animation_debug);
                    stats.animation_debug_instances =
                        static_cast<uint32_t>(animation_debug.size());
                    if (stats.animation_debug_query_ok) {
                        for (const auto& snapshot : animation_debug)
                            viewer::draw_animation_debug_overlay(
                                snapshot, frame_camera,
                                static_cast<int>(render_frame.extent.width),
                                static_cast<int>(render_frame.extent.height),
                                sel_vp.x, sel_vp.y, stats.animation_overlay);
                    }
                }
            }
        }
        const matter::FrameStats& frame_stats = session->frame_stats();
        dlss_modes_supported = vulkan->dlss_available() &&
                               frame_stats.dlss_reason.empty();
        if (reported_selected_dlss_mode != frame_stats.dlss_selected_mode ||
            reported_active_dlss_mode != frame_stats.dlss_active_mode ||
            reported_dlss_internal_width != frame_stats.dlss_internal_width ||
            reported_dlss_internal_height != frame_stats.dlss_internal_height ||
            reported_dlss_output_width != frame_stats.dlss_output_width ||
            reported_dlss_output_height != frame_stats.dlss_output_height ||
            reported_dlss_resets != frame_stats.dlss_reset_count) {
            std::printf(
                "DLSS selected=%s active=%s internal=%ux%u output=%ux%u resets=%llu reason=%s\n",
                matter::dlss_mode_name(frame_stats.dlss_selected_mode),
                matter::dlss_mode_name(frame_stats.dlss_active_mode),
                frame_stats.dlss_internal_width,
                frame_stats.dlss_internal_height,
                frame_stats.dlss_output_width, frame_stats.dlss_output_height,
                static_cast<unsigned long long>(frame_stats.dlss_reset_count),
                frame_stats.dlss_reason.empty() ? "none"
                                                : frame_stats.dlss_reason.c_str());
            reported_selected_dlss_mode = frame_stats.dlss_selected_mode;
            reported_active_dlss_mode = frame_stats.dlss_active_mode;
            reported_dlss_internal_width = frame_stats.dlss_internal_width;
            reported_dlss_internal_height = frame_stats.dlss_internal_height;
            reported_dlss_output_width = frame_stats.dlss_output_width;
            reported_dlss_output_height = frame_stats.dlss_output_height;
            reported_dlss_resets = frame_stats.dlss_reset_count;
        }
        const bool vk_rt_observation_valid =
            frame_stats.vk_rt_effective ||
            !frame_stats.vk_rt_fallback_reason.empty();
        if (vk_rt_observation_valid &&
            (!reported_vk_rt_once ||
             reported_vk_rt_effective != frame_stats.vk_rt_effective ||
             reported_vk_rt_dispatches != frame_stats.vk_rt_trace_dispatches ||
             reported_vk_rt_reason != frame_stats.vk_rt_fallback_reason)) {
            std::printf(
                "Vulkan RT observed effective=%s dispatches=%u reason=%s\n",
                frame_stats.vk_rt_effective ? "true" : "false",
                frame_stats.vk_rt_trace_dispatches,
                frame_stats.vk_rt_fallback_reason.empty()
                    ? "none"
                    : frame_stats.vk_rt_fallback_reason.c_str());
            reported_vk_rt_effective = frame_stats.vk_rt_effective;
            reported_vk_rt_dispatches = frame_stats.vk_rt_trace_dispatches;
            reported_vk_rt_reason = frame_stats.vk_rt_fallback_reason;
            reported_vk_rt_once = true;
        }
        stats.frame_ms = static_cast<float>(hud_frame_ms);
        stats.fps = hud_frame_ms > 0.0
                        ? static_cast<float>(1000.0 / hud_frame_ms)
                        : 0.0f;
        stats.cam_pos[0] = frame_camera.position.x;
        stats.cam_pos[1] = frame_camera.position.y;
        stats.cam_pos[2] = frame_camera.position.z;
        stats.resolve_ms = frame_stats.resolve_ms;
        stats.build_ms = frame_stats.build_ms;
        stats.draw_ms = frame_stats.draw_ms;
        stats.instances_active = static_cast<int>(frame_stats.instances_resolved);
        stats.gpu_emitted = static_cast<int>(frame_stats.instances_drawn);
        stats.gpu_culled = static_cast<int>(frame_stats.clusters_culled);
        stats.gpu_culled_hiz = static_cast<int>(frame_stats.hiz_culled);
        stats.culled_clusters = stats.gpu_culled;
        stats.raster_tris = static_cast<int>(frame_stats.triangles);
        stats.raster_batches = static_cast<int>(frame_stats.draw_batches);
        stats.instances_total = static_cast<int>(frame_stats.instances_total);
        stats.parts_baked = static_cast<int>(frame_stats.parts_baked);
        stats.cache_hits = static_cast<int>(frame_stats.cache_hits);
        stats.gpu_timers_supported   = frame_stats.gpu_timers_supported;
        stats.gpu_total_ms           = frame_stats.gpu_total_ms;
        stats.gpu_cull_ms            = frame_stats.gpu_cull_ms;
        stats.gpu_gbuffer_ms         = frame_stats.gpu_gbuffer_ms;
        stats.gpu_blas_ms            = frame_stats.gpu_blas_ms;
        stats.gpu_tlas_ms            = frame_stats.gpu_tlas_ms;
        stats.gpu_rt_ms              = frame_stats.gpu_rt_ms;
        stats.gpu_denoise_ms         = frame_stats.gpu_denoise_ms;
        stats.gpu_dlss_ms            = frame_stats.gpu_dlss_ms;
        stats.gpu_composite_ms       = frame_stats.gpu_composite_ms;

        bool ui_frame_completed = false;
        if (ui_frame_ready) {
            ui_frame_completed = ui.end_frame(frame, error);
            if (!ui_frame_completed) {
                std::fprintf(stderr, "FATAL: ImGui Vulkan backend: %s\n",
                             error.c_str());
                fatal_error = true;
            }
        }

        bool capture = false;
        bool issue_capture = false;
        std::string capture_path;
        if (!screenshot_path.empty() && bake_ready && frame_stats.instances_drawn > 0 &&
            ++screenshot_settle >= settle_frames) {
            capture = true; capture_path = screenshot_path;
        } else if (shot_settle > 0 && frame_stats.instances_drawn > 0 &&
                   --shot_settle == 0) {
            capture = true; capture_path = shot_path;
        } else if (issue_state.phase == viewer::ReporterPhase::AwaitingCapture &&
                   --issue_state.capture_settle <= 0) {
            // Deliberately NOT gated on instances_drawn: "the world renders
            // nothing" is exactly the kind of defect worth a screenshot.
            capture = true;
            issue_capture = true;
        }
        std::vector<uint8_t> rgba;
        if (capture && !session->readback_swapchain_rgba8(frame, rgba, error)) {
            ++screenshot_failures;
            std::fprintf(stderr, "screenshot readback retry %d/5: %s\n",
                         screenshot_failures, error.c_str());
            capture = false;
            if (issue_capture) { issue_capture = false; issue_state.capture_settle = 2; }
            else if (capture_path == screenshot_path) screenshot_settle = 1;
            else shot_settle = 2;
            if (screenshot_failures >= 5) {
                std::fprintf(stderr, "FATAL: screenshot readback exhausted retries\n");
                fatal_error = true;
            }
        }
        bool frame_presented = false;
        const bool frame_completed =
            vulkan->end_frame(frame, frame_presented, error);
        session->finish_vulkan_frame(
            frame.serial, frame_presented && !fatal_error);
        // end_frame() records the queue submit and present boundary. The
        // smoothed cadence below also feeds the HUD frame time on the next frame.
        phase.present = phase_split();
        const double perf_frame_cadence_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - perf_frame_start).count();
        hud_frame_ms = hud_frame_ms <= 0.0
                           ? perf_frame_cadence_ms
                           : hud_frame_ms * 0.9 + perf_frame_cadence_ms * 0.1;
        // Phase attribution -> HUD. Same EMA weight as hud_frame_ms so the
        // phase sum stays comparable to the frame time it is decomposing.
        {
            auto ema = [](float prev, double sample) -> float {
                return prev <= 0.0f ? (float)sample
                                    : prev * 0.9f + (float)sample * 0.1f;
            };
            stats.loop_poll_ms    = ema(stats.loop_poll_ms,    phase.poll);
            stats.loop_acquire_ms = ema(stats.loop_acquire_ms, phase.acquire);
            stats.loop_ui_ms      = ema(stats.loop_ui_ms,      phase.ui);
            stats.loop_tick_ms    = ema(stats.loop_tick_ms,    phase.tick);
            stats.loop_pump_ms    = ema(stats.loop_pump_ms,    phase.pump);
            stats.loop_lab_ms     = ema(stats.loop_lab_ms,     phase.lab);
            stats.loop_render_ms  = ema(stats.loop_render_ms,  phase.render);
            stats.loop_present_ms = ema(stats.loop_present_ms, phase.present);
            // Peak-hold with slow decay: a spiky publish or fence stall stays
            // readable for a second or two instead of being averaged away.
            stats.loop_peak_pump_ms =
                std::max((float)phase.pump, stats.loop_peak_pump_ms * 0.98f);
            stats.loop_peak_acquire_ms =
                std::max((float)phase.acquire, stats.loop_peak_acquire_ms * 0.98f);
        }
        if (!frame_completed) {
            std::fprintf(stderr, "FATAL: end_frame: %s\n", error.c_str());
            fatal_error = true;
        } else {
            if (ui_frame_completed && !fatal_error) {
                camera_input_order.end_frame();
                // A region drag must not also fly the camera — the rubber band
                // reads the mouse straight off the IO, outside any ImGui window.
                if ((camera_input_order.camera_update_allowed() ||
                     camera_capture) &&
                    !viewer::issue_reporter_wants_mouse(issue_state)) {
                    camera_controller.update(window, dt, camera);
                }
            }
            if (capture && issue_capture) {
                screenshot_failures = 0;
                if (issue_state.capture_is_region_pick) {
                    // F9: hold the whole frame so the selection is made against
                    // a still, and put it on the GPU as the drag backdrop.
                    issue_state.frozen = rgba;
                    issue_state.frozen_width = frame.extent.width;
                    issue_state.frozen_height = frame.extent.height;
                    std::string preview_error;
                    issue_state.frozen_preview = issue_previews.create(
                        rgba, frame.extent.width, frame.extent.height,
                        preview_error);
                    if (!issue_state.frozen_preview)
                        std::fprintf(stderr, "issue freeze preview: %s\n",
                                     preview_error.c_str());
                    issue_state.phase = viewer::ReporterPhase::SelectingRegion;
                    issue_state.drag_active = false;
                } else if (viewer::ensure_report_dir(issue_state)) {
                    // F10: the active viewport, straight to a shot. The rect is
                    // resolved here, not at keypress, so it reflects the layout
                    // of the frame actually read back.
                    const viewer::ViewportRect& vp = ui.viewport_rect();
                    // Viewport rect is in window coords; the swapchain may be a
                    // different size (HiDPI, scaled present).
                    const ImVec2 display = ImGui::GetIO().DisplaySize;
                    const float sx =
                        display.x > 0 ? frame.extent.width / display.x : 1.0f;
                    const float sy =
                        display.y > 0 ? frame.extent.height / display.y : 1.0f;
                    viewer::ShotRect rect{
                        static_cast<int32_t>(vp.x * sx),
                        static_cast<int32_t>(vp.y * sy),
                        static_cast<int32_t>(vp.w * sx),
                        static_cast<int32_t>(vp.h * sy)};
                    if (rect.w < 8 || rect.h < 8) rect = viewer::ShotRect{};
                    const std::vector<uint8_t> cropped = viewer::crop_rgba(
                        rgba, frame.extent.width, frame.extent.height, rect);
                    const std::string path =
                        issue_state.dir + "/shot-" +
                        std::to_string(issue_state.next_shot_index++) + ".png";
                    if (!write_png(path, cropped,
                                   static_cast<uint32_t>(rect.w),
                                   static_cast<uint32_t>(rect.h))) {
                        // Never fatal: losing a shot must not end the session
                        // you were in the middle of reporting on.
                        issue_state.status = "could not write " + path;
                        issue_state.status_is_error = true;
                        console_log.push(viewer::LogSeverity::Error,
                                         "Issue shot: " + issue_state.status);
                    } else {
                        viewer::IssueShot shot;
                        shot.file = path.substr(path.find_last_of('/') + 1);
                        shot.region = viewer::ShotRegion::Viewport;
                        shot.rect = rect;
                        stamp_replay_state(shot, frame.extent.width,
                                           frame.extent.height);
                        shot.width = static_cast<uint32_t>(rect.w);
                        shot.height = static_cast<uint32_t>(rect.h);
                        shot.frame_ms = stats.frame_ms;
                        shot.instances_drawn = frame_stats.instances_drawn;
                        shot.triangles = frame_stats.triangles;
                        shot.draw_batches = frame_stats.draw_batches;
                        attach_preview(shot, cropped, shot.width, shot.height);
                        viewer::record_shot(issue_state, shot);
                        std::printf("issue shot written to %s\n", path.c_str());
                    }
                    issue_state.phase = viewer::ReporterPhase::Editing;
                    issue_state.window_open = true;
                } else {
                    console_log.push(viewer::LogSeverity::Error,
                                     "Issue shot: " + issue_state.status);
                    issue_state.phase = viewer::ReporterPhase::Editing;
                    issue_state.window_open = true;
                }
            } else if (capture) {
                // A replay crops to the recorded rect so its PNG is directly
                // diffable against the shot it came from (img_diff.py rejects a
                // size mismatch outright).
                std::vector<uint8_t> out_rgba = rgba;
                uint32_t out_w = frame.extent.width;
                uint32_t out_h = frame.extent.height;
                if (replay.valid && capture_path == screenshot_path &&
                    !replay.rect.empty()) {
                    // Validate the two things that silently invalidate a diff:
                    // a framebuffer the window manager would not grant, and a
                    // viewport the panel layout put somewhere else. Both leave
                    // the render CORRECT but not comparable, so saying nothing
                    // would hand back a plausible, wrong answer.
                    bool comparable = true;
                    if (replay.frame_width != frame.extent.width ||
                        replay.frame_height != frame.extent.height) {
                        std::fprintf(stderr,
                                     "replay WARNING: framebuffer is %ux%u but the "
                                     "shot recorded %ux%u; pixels are NOT comparable\n",
                                     frame.extent.width, frame.extent.height,
                                     replay.frame_width, replay.frame_height);
                        comparable = false;
                    }
                    const ImVec2 display = ImGui::GetIO().DisplaySize;
                    const float sx =
                        display.x > 0 ? frame.extent.width / display.x : 1.0f;
                    const float sy =
                        display.y > 0 ? frame.extent.height / display.y : 1.0f;
                    const viewer::ViewportRect& vp = ui.viewport_rect();
                    const viewer::ShotRect actual_vp{
                        static_cast<int32_t>(vp.x * sx),
                        static_cast<int32_t>(vp.y * sy),
                        static_cast<int32_t>(vp.w * sx),
                        static_cast<int32_t>(vp.h * sy)};
                    viewer::ShotRect rect = replay.rect;
                    const bool viewport_moved =
                        !replay.viewport.empty() && !actual_vp.empty() &&
                        (actual_vp.x != replay.viewport.x ||
                         actual_vp.y != replay.viewport.y ||
                         actual_vp.w != replay.viewport.w ||
                         actual_vp.h != replay.viewport.h);
                    if (viewport_moved) {
                        std::fprintf(stderr,
                                     "replay WARNING: viewport is %dx%d at (%d,%d) but the "
                                     "shot recorded %dx%d at (%d,%d) — the panel layout "
                                     "differs (imgui.ini). The projection differs, so "
                                     "pixels are NOT comparable\n",
                                     actual_vp.w, actual_vp.h, actual_vp.x, actual_vp.y,
                                     replay.viewport.w, replay.viewport.h,
                                     replay.viewport.x, replay.viewport.y);
                        comparable = false;
                        // Remap ONLY a crop that actually covered part of the
                        // 3D view. A UI-only crop has no meaningful position
                        // relative to the viewport — its uv lies outside 0..1,
                        // and scaling by the new viewport sends it somewhere
                        // arbitrary (measured: a left-column crop remapped to
                        // x = -331 and came out the wrong size). Panels keep
                        // their absolute geometry when the window grows, so
                        // leaving such a crop alone is strictly better.
                        const bool crop_covered_view =
                            replay.has_viewport_uv && replay.viewport_uv[2] > 0.0f &&
                            replay.viewport_uv[0] < 1.0f &&
                            replay.viewport_uv[3] > 0.0f &&
                            replay.viewport_uv[1] < 1.0f;
                        if (crop_covered_view) {
                            // Keep the same CONTENT in frame even though the
                            // pixels moved: remap through the fraction of the
                            // viewport the crop originally covered, clamped so
                            // it can never leave the framebuffer.
                            const auto clampi = [](int32_t v, int32_t lo, int32_t hi) {
                                return v < lo ? lo : (v > hi ? hi : v);
                            };
                            const int32_t fbw = static_cast<int32_t>(frame.extent.width);
                            const int32_t fbh = static_cast<int32_t>(frame.extent.height);
                            const int32_t x0 = clampi(
                                actual_vp.x + static_cast<int32_t>(
                                                  replay.viewport_uv[0] * actual_vp.w),
                                0, fbw);
                            const int32_t y0 = clampi(
                                actual_vp.y + static_cast<int32_t>(
                                                  replay.viewport_uv[1] * actual_vp.h),
                                0, fbh);
                            const int32_t x1 = clampi(
                                actual_vp.x + static_cast<int32_t>(
                                                  replay.viewport_uv[2] * actual_vp.w),
                                0, fbw);
                            const int32_t y1 = clampi(
                                actual_vp.y + static_cast<int32_t>(
                                                  replay.viewport_uv[3] * actual_vp.h),
                                0, fbh);
                            rect = viewer::ShotRect{x0, y0, x1 - x0, y1 - y0};
                            std::fprintf(stderr,
                                         "replay: crop covered the 3D view; remapped "
                                         "through viewport_uv to %dx%d at (%d,%d)\n",
                                         rect.w, rect.h, rect.x, rect.y);
                        } else {
                            std::fprintf(stderr,
                                         "replay: crop is outside the 3D view (UI only); "
                                         "keeping absolute rect. Panel CONTENT at these "
                                         "pixels depends on the layout\n");
                        }
                    }
                    if (!comparable && std::getenv("MATTER_REPLAY_STRICT")) {
                        std::fprintf(stderr,
                                     "FATAL: MATTER_REPLAY_STRICT set and this replay "
                                     "cannot reproduce the recorded shot\n");
                        fatal_error = true;
                    }
                    out_rgba = viewer::crop_rgba(rgba, frame.extent.width,
                                                 frame.extent.height, rect);
                    out_w = static_cast<uint32_t>(rect.w);
                    out_h = static_cast<uint32_t>(rect.h);
                }
                if (!write_png(capture_path, out_rgba, out_w, out_h)) {
                    std::fprintf(stderr, "screenshot FAILED %s\n",
                                 capture_path.c_str());
                    fatal_error = true;
                } else {
                    screenshot_failures = 0;
                    std::printf("screenshot written to %s\n",
                                capture_path.c_str());
#ifndef _WIN32
                    if (capture_path == shot_path) {
                        const std::string done = shot_path + ".done";
                        if (FILE* file = std::fopen(done.c_str(), "w"))
                            std::fclose(file);
                    }
#endif
                    if (capture_path == screenshot_path) quit_requested = true;
                }
            }
        }

        if (perf.enabled && perf_phase != PerfPhase::Complete && !fatal_error) {
            const auto perf_now = std::chrono::steady_clock::now();
            if (perf_phase == PerfPhase::WaitingForBake) {
                if (bake_ready && frame_stats.instances_drawn > 0) {
                    perf_phase = PerfPhase::Warming;
                    perf_phase_start = perf_now;
                    std::printf("perf: bake ready; warming for %.3f seconds\n",
                                perf.warmup_seconds);
                }
            } else if (perf_phase == PerfPhase::Warming &&
                       std::chrono::duration<double>(perf_now - perf_phase_start)
                               .count() >= perf.warmup_seconds) {
                perf_phase = PerfPhase::Sampling;
                perf_phase_start = perf_now;
                perf_start_counters = capture_perf_counters(frame_stats);
                perf_start_dlss_resets = frame_stats.dlss_reset_count;
                perf_frame_times.clear();
                std::printf("perf: sampling for %.3f seconds\n",
                            perf.sample_seconds);
            } else if (perf_phase == PerfPhase::Sampling) {
                perf_frame_times.push_back(perf_frame_cadence_ms);
                if (std::chrono::duration<double>(perf_now - perf_phase_start)
                        .count() >= perf.sample_seconds) {
                    const PerfCounters perf_finish_counters =
                        capture_perf_counters(frame_stats);
                    const uint32_t validation_errors =
                        vulkan->validation_error_count();
                    if (!write_perf_result(
                            perf, worlds[stats.world_current].world_name,
                            perf_frame_times, perf_start_counters,
                            perf_finish_counters, frame_stats, stats,
                            perf_start_dlss_resets,
                            validation_errors, perf_error)) {
                        std::fprintf(stderr, "FATAL: perf: %s\n",
                                     perf_error.c_str());
                        fatal_error = true;
                    } else if (validation_errors != 0) {
                        std::fprintf(stderr,
                                     "FATAL: perf observed %u Vulkan validation errors\n",
                                     validation_errors);
                        fatal_error = true;
                    } else {
                        std::printf("perf: wrote %zu frames to %s\n",
                                    perf_frame_times.size(),
                                    perf.output_path.c_str());
                        quit_requested = true;
                    }
                    perf_phase = PerfPhase::Complete;
                }
            }
        }

        if (!stats_label.empty()) {
            // APPEND-ONLY format (scripts parse by position). The five trailing
            // VT fields are the chart-space virtual-texturing census: a
            // vt_rejected != 0 means part of the world silently fell back to
            // the legacy path and is ignoring its surfaces() classification.
            std::printf("STATS,%s,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d"
                        ",%u,%u,%u,%.1f,%.1f\n",
                        stats_label.c_str(), stats.frame_ms, stats.resolve_ms,
                        stats.build_ms, stats.draw_ms, stats.instances_active,
                        stats.raster_batches, stats.raster_tris,
                        stats.culled_clusters, stats.gpu_culled_hiz,
                        frame_stats.vt_variants,
                        frame_stats.vt_rejected_variants,
                        frame_stats.vt_max_variants,
                        static_cast<double>(frame_stats.vt_mesh_bytes) /
                            (1024.0 * 1024.0),
                        static_cast<double>(
                            frame_stats.vt_mesh_budget_bytes) /
                            (1024.0 * 1024.0));
            std::printf("STATSVT,%s,active=%d,variants=%u/%u,rejected=%u,"
                        "mesh=%.1f/%.1f MiB,pool=%u/%u,pinned=%u,queue=%u,"
                        "fills=%llu,evictions=%llu\n",
                        stats_label.c_str(),
                        frame_stats.vt_active ? 1 : 0,
                        frame_stats.vt_variants,
                        frame_stats.vt_max_variants,
                        frame_stats.vt_rejected_variants,
                        static_cast<double>(frame_stats.vt_mesh_bytes) /
                            (1024.0 * 1024.0),
                        static_cast<double>(
                            frame_stats.vt_mesh_budget_bytes) /
                            (1024.0 * 1024.0),
                        frame_stats.vt_pool_used,
                        frame_stats.vt_pool_capacity,
                        frame_stats.vt_pool_pinned,
                        frame_stats.vt_queue_depth,
                        static_cast<unsigned long long>(
                            frame_stats.vt_fills_total),
                        static_cast<unsigned long long>(
                            frame_stats.vt_evictions_total));
            stats_label.clear();
        }
        // Post-frame seam (event-system.md S I.13). Reload / world-switch are
        // no longer polled flags but viewer.reload / viewer.switch_world
        // commands; their handlers only RECORD pending intent on the
        // SessionBinding (UI triggers via execute() during panel draw; FIFO
        // triggers via the registry pump after the FIFO parse). The heavy
        // session op is applied HERE — after end_frame, never mid-ImGui-draw —
        // matching the original flag-based timing. Both UI and FIFO switch/
        // reload funnel to this single apply point.
        if (binding.pending_reload()) {
            binding.clear_pending_reload();
            bake_ready = false;
            screenshot_settle = 0;
            apply_world_camera_after_bake =
                !replay.valid && initial_camera_env == nullptr;
            viewer::prepare_world_reload(stats);
            binding.reload();  // clears app models + session->reload()
        }
        const int pending_switch = binding.pending_switch();
        if (pending_switch >= 0 && pending_switch < static_cast<int>(worlds.size())) {
            binding.clear_pending_switch();
            const int selected = pending_switch;
            // SessionBinding::replace runs the S I.13 close/quiesce/replace/
            // rebind/open/request epoch sequence; a failed open leaves the old
            // session + epoch fully intact.
            const bool ok = binding.replace([&]() { return open_world(worlds[selected]); });
            if (!ok) {
                viewer::complete_world_switch(stats, false);
            } else {
                viewer::complete_world_switch(stats, true);
                stats.world_current = selected;
                selected_world_reported = false;
                console_log.push(viewer::LogSeverity::Info,
                                 "Connected to " + worlds[selected].world_name);
                bake_ready = false;
                screenshot_settle = 0;
                apply_world_camera_after_bake = true;
                apply_world_resolver_defaults(worlds[selected].world_name, active_radius,
                                              min_projected_size, stats);
            }
        }
    }

#ifndef _WIN32
    if (cmd_fd >= 0) close(cmd_fd);
    if (fifo_path) unlink(fifo_path);
#endif
    if (camera_capture) camera_controller.set_capture(window, false);
#ifndef _WIN32
    if (cmd_fd >= 0) close(cmd_fd);
#else
    if (cmd_handle != INVALID_HANDLE_VALUE) CloseHandle(cmd_handle);
#endif
    session.reset();
    // Before ui.shutdown(): the preview textures are ImGui descriptor sets, and
    // ImGui_ImplVulkan_RemoveTexture needs the backend alive. Explicit for the
    // same reason as bake_lab below — a stack local's destructor runs after
    // vulkan.reset(), which would be a dead device.
    issue_previews.shutdown();
    ui.shutdown();
    engine.reset();
    // Part Workbench (W2): the isolation session/engine own GPU resources tied
    // to the shared VulkanDevice below. bake_lab is a stack local destroyed
    // only when main() returns — i.e. AFTER vulkan.reset() — so release its
    // session+engine explicitly here, while the device is still alive (and so
    // any teardown validation errors are counted below).
    bake_lab.workbench().close();
    const uint32_t validation_errors = vulkan->validation_error_count();
    vulkan.reset();
    glfwDestroyWindow(window);
    glfwTerminate();
    if (validation_errors != 0) {
        std::fprintf(stderr, "FATAL: Vulkan validation errors: %u\n",
                     validation_errors);
        return 1;
    }
    return fatal_error ? 1 : 0;
}
