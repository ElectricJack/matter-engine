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
#include "camera_controller.h"
#include "camera_focus.h"
#include "editor_model.h"
#include "properties_panel.h"
#include "properties_registry.h"
#include "selection_outline.h"
#include "selection_set.h"
#include "toolbar_panel.h"
#include "console_panel.h"
#include "ui.h"
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
    camera.near_plane = 1.0f;
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

std::string examples_root() {
    if (std::filesystem::is_directory("MatterEngine3/examples"))
        return "MatterEngine3/examples";
    return "../MatterEngine3/examples";
}

std::string shared_lib_root() {
    if (std::filesystem::is_directory("MatterEngine3/shared-lib"))
        return "MatterEngine3/shared-lib";
    return "../MatterEngine3/shared-lib";
}

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
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(
        1280, 720, "MatterEngine3 World Viewer", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "FATAL: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }

    std::string error;
    // Validation layers are a development dependency; requesting them
    // unconditionally makes viewer.exe fatal on machines without the Vulkan
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
    const char* cache_root_env = std::getenv("MATTER_CACHE_ROOT");
    engine_desc.cache_root = cache_root_env ? cache_root_env : "cache";
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
    if (const char* value = std::getenv("MATTER_CAM")) {
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
    if (const char* value = std::getenv("MATTER_WORLD")) {
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
        result->request_bake();
        return result;
    };
    auto session = open_world(worlds[initial_world]);
    if (!session) {
        ui.shutdown(); engine.reset(); vulkan.reset();
        glfwDestroyWindow(window); glfwTerminate();
        return 1;
    }

    viewer::EditorModel editor_model;
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
    scene_commands.query_records = [&session]() -> std::vector<matter::scene::SceneRecord> {
        std::vector<matter::scene::SceneRecord> records;
        if (!session) return records;
        session->ecs().each(
            [&](flecs::entity e, const matter::scene::SceneEntityId& id) {
                matter::scene::SceneRecord rec;
                rec.id = id;
                if (e.parent().is_valid() && e.parent().has<matter::scene::SceneEntityId>()) {
                    rec.parent_id = e.parent().get<matter::scene::SceneEntityId>();
                }
                const char* n = e.name().c_str();
                rec.name = (n && n[0]) ? n : "Entity";
                // LocalTransform is always present (instantiate() sets it
                // unconditionally) but is still listed here for consistency
                // with PropertiesRegistry lookups that key off component_names.
                rec.component_names.push_back("LocalTransform");
                if (e.has<matter::physics::RigidBody>())
                    rec.component_names.push_back("RigidBody");
                if (e.has<matter::physics::PhysicsVelocity>())
                    rec.component_names.push_back("PhysicsVelocity");
                if (e.has<matter::physics::SphereCollider>())
                    rec.component_names.push_back("SphereCollider");
                if (e.has<matter::physics::CapsuleCollider>())
                    rec.component_names.push_back("CapsuleCollider");
                if (e.has<matter::physics::BoxCollider>())
                    rec.component_names.push_back("BoxCollider");
                if (e.has<matter::physics::ConvexHullCollider>())
                    rec.component_names.push_back("ConvexHullCollider");
                if (e.has<matter::scene::PartInstance>()) {
                    rec.component_names.push_back("PartInstance");
                }
                if (e.has<matter::streaming::SectorStreaming>())
                    rec.component_names.push_back("SectorStreaming");
                records.push_back(std::move(rec));
            });
        return records;
    };
    scene_commands.generation = [&session]() -> uint64_t {
        if (!session) return 0;
        return static_cast<uint64_t>(session->ecs().get_info()->frame_count_total);
    };
    // Task 13: entity CRUD backing the scene-tree context menu. Editor-created
    // entities use their own flecs entity id as the SceneEntityId value (so
    // they line up with SelectionSet/viewport-pick/gizmo, which all key off
    // flecs ids for Entity selections — see viewport_pick.cpp) rather than
    // the content hash scene_registry.cpp assigns to authored/baked entities.
    scene_commands.create_empty =
        [&session](const std::string& name) -> matter::scene::SceneEditResult {
        matter::scene::SceneEditResult result;
        if (!session) {
            result.error = matter::scene::SceneEditError::InvalidTarget;
            return result;
        }
        flecs::world& world = session->ecs();
        flecs::entity e = world.entity();
        e.set<matter::scene::SceneEntityId>({static_cast<uint64_t>(e.id())});
        if (!name.empty()) e.set_name(name.c_str());
        e.set<matter::ecs::LocalTransform>({});
        result.created_id = matter::scene::SceneEntityId{static_cast<uint64_t>(e.id())};
        return result;
    };
    scene_commands.duplicate =
        [&session](matter::scene::SceneEntityId src) -> matter::scene::SceneEditResult {
        matter::scene::SceneEditResult result;
        if (!session) {
            result.error = matter::scene::SceneEditError::InvalidTarget;
            return result;
        }
        flecs::entity src_e = find_scene_entity(session->ecs(), src);
        if (!src_e.is_valid()) {
            result.error = matter::scene::SceneEditError::EntityNotFound;
            return result;
        }
        flecs::entity copy = src_e.clone();
        // clone() copies component values verbatim, including SceneEntityId --
        // overwrite with the copy's own flecs id so it's unique.
        copy.set<matter::scene::SceneEntityId>({static_cast<uint64_t>(copy.id())});
        const char* orig_name = src_e.name().c_str();
        if (orig_name && orig_name[0]) {
            copy.set_name((std::string(orig_name) + " Copy").c_str());
        }
        if (src_e.parent().is_valid()) copy.child_of(src_e.parent());
        result.created_id = matter::scene::SceneEntityId{static_cast<uint64_t>(copy.id())};
        return result;
    };
    scene_commands.delete_entity =
        [&session](matter::scene::SceneEntityId target) -> matter::scene::SceneEditResult {
        matter::scene::SceneEditResult result;
        if (!session) {
            result.error = matter::scene::SceneEditError::InvalidTarget;
            return result;
        }
        flecs::entity e = find_scene_entity(session->ecs(), target);
        if (!e.is_valid()) {
            result.error = matter::scene::SceneEditError::EntityNotFound;
            return result;
        }
        // Destroying the parent cascades to ECS children by default (flecs'
        // default ChildOf cleanup policy).
        e.destruct();
        return result;
    };
    scene_commands.reparent =
        [&session](matter::scene::SceneEntityId child,
                  matter::scene::SceneEntityId new_parent) -> matter::scene::SceneEditResult {
        matter::scene::SceneEditResult result;
        if (!session) {
            result.error = matter::scene::SceneEditError::InvalidTarget;
            return result;
        }
        flecs::entity child_e = find_scene_entity(session->ecs(), child);
        if (!child_e.is_valid()) {
            result.error = matter::scene::SceneEditError::EntityNotFound;
            return result;
        }
        if (new_parent.value == 0) {
            child_e.remove(flecs::ChildOf, flecs::Wildcard);
            return result;
        }
        flecs::entity parent_e = find_scene_entity(session->ecs(), new_parent);
        if (!parent_e.is_valid()) {
            result.error = matter::scene::SceneEditError::InvalidTarget;
            return result;
        }
        for (flecs::entity cursor = parent_e; cursor.is_valid();
             cursor = cursor.parent()) {
            if (cursor == child_e) {
                result.error = matter::scene::SceneEditError::CycleDetected;
                return result;
            }
        }
        child_e.child_of(parent_e);
        return result;
    };

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
    const char* screenshot_env = std::getenv("MATTER_SCREENSHOT");
    const std::string screenshot_path = screenshot_env ? screenshot_env : "";
    int screenshot_settle = 0;
    int screenshot_failures = 0;
    bool bake_ready = false;
    bool selected_world_reported = false;
    const bool test_resize = std::getenv("MATTER_TEST_RESIZE") != nullptr;
    const bool hide_ui = std::getenv("MATTER_HIDE_UI") != nullptr;
    bool resize_exercised = false;
    if (hide_ui) std::printf("viewer: UI hidden by MATTER_HIDE_UI\n");

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

    while (!glfwWindowShouldClose(window) && !quit_requested && !fatal_error) {
        // This starts before event polling and begin_frame(), whose fence wait and
        // swapchain acquire are part of the user-visible frame cadence.
        const auto perf_frame_start = std::chrono::steady_clock::now();
        glfwPollEvents();
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - previous_time).count();
        previous_time = now;
        if (key_pressed(window, GLFW_KEY_TAB, tab_down)) {
            camera_capture = !camera_capture;
            camera_controller.set_capture(window, camera_capture);
        }
        if (key_pressed(window, GLFW_KEY_F9, f9_down))
            std::printf("wireframe: not available in Vulkan milestone\n");
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
                float c[6]; char word[256];
                if (std::sscanf(line.c_str(), "cam %f %f %f %f %f %f",
                                &c[0], &c[1], &c[2], &c[3], &c[4], &c[5]) == 6) {
                    camera.position = {c[0], c[1], c[2]};
                    camera.target = {c[3], c[4], c[5]};
                } else if (std::sscanf(line.c_str(), "shot %255s", word) == 1) {
                    shot_path = word; shot_settle = 3;
                } else if (std::sscanf(line.c_str(), "stats %255s", word) == 1) {
                    stats_label = word;
                } else if (std::sscanf(line.c_str(), "budget %f", &c[0]) == 1) {
                    stats.pixel_budget = std::max(0.05f, std::min(4.0f, c[0]));
                } else if (std::sscanf(line.c_str(), "hiz %255s", word) == 1) {
                    std::printf("hiz: not available in Vulkan milestone\n");
                } else if (std::sscanf(line.c_str(), "dlss %255s", word) == 1) {
                    if (std::strcmp(word, "native") == 0)
                        selected_dlss_mode = matter::DlssMode::Native;
                    else if (std::strcmp(word, "quality") == 0)
                        selected_dlss_mode = matter::DlssMode::Quality;
                    else if (std::strcmp(word, "balanced") == 0)
                        selected_dlss_mode = matter::DlssMode::Balanced;
                    else if (std::strcmp(word, "performance") == 0)
                        selected_dlss_mode = matter::DlssMode::Performance;
                    else
                        std::printf("dlss: expected native, quality, balanced, or performance\n");
                } else if (line == "reload") {
                    stats.reload_requested = true;
                } else if (line == "wireframe" || line == "wireframe toggle") {
                    std::printf("wireframe: not available in Vulkan milestone\n");
                } else if (line == "wireframe on" || line == "wireframe off") {
                    std::printf("wireframe: not available in Vulkan milestone\n");
                } else if (line == "quit") {
                    quit_requested = true;
                } else if (!line.empty()) {
                    std::printf("cmd: unrecognized '%s'\n", line.c_str());
                }
            }
        }

        if (test_resize && bake_ready && !resize_exercised) {
            glfwSetWindowSize(window, 960, 540);
            glfwPollEvents();
            screenshot_settle = 0;
            resize_exercised = true;
        }

        matter::VulkanFrame frame{};
        if (!vulkan->begin_frame(frame, error)) {
            if (error.find("zero-sized") != std::string::npos) {
                glfwWaitEventsTimeout(0.05);
                continue;
            }
            std::fprintf(stderr, "FATAL: begin_frame: %s\n", error.c_str());
            break;
        }

        matter_viewer::CurrentFrameInputOrder camera_input_order{};
        const bool ui_frame_ready = ui.begin_frame(frame, error);
        if (!ui_frame_ready) {
            std::fprintf(stderr, "FATAL: ImGui Vulkan prepare: %s\n",
                         error.c_str());
            fatal_error = true;
        } else {
            camera_input_order.begin_ui();
            editor_model.refresh(scene_commands);
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
                        session->graph_snapshot(cached_snapshot);
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
                ui.draw_console_panel(console_log);
                ui.draw_debug_panel(stats);
                ui.draw_worlds_panel(worlds, stats);
                ui.draw_camera_panel(camera);
                // draw_sector_streaming_panel retired in Phase 4 Task 12 — sector
                // streaming editing now lives in the Properties panel via
                // SpecializedEditors (MatterViewer/specialized_editors.h).
                {
                    // Task 10: transform gizmo. Full-window viewport rect for
                    // now — the manual docking layout has no fixed viewport
                    // sub-region to clip to yet.
                    int fb_width = 0, fb_height = 0;
                    glfwGetFramebufferSize(window, &fb_width, &fb_height);
                    ui.draw_gizmo(selection_set, field_commands, camera,
                                 sim_control.mode(), 0.0f, 0.0f,
                                 static_cast<float>(fb_width),
                                 static_cast<float>(fb_height));
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
                const viewer::PickResult pick = viewer::viewport_pick(
                    static_cast<float>(cursor_x), static_cast<float>(cursor_y),
                    fb_width, fb_height, frame_camera, *session);
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

        ui.update_sector_streaming(*session, frame_camera);
        matter::TickDesc tick{};
        tick.frame_delta_seconds = dt;
        session->tick(tick);
        camera_input_order.tick_scene();
        session->pump_gpu_jobs(4.0f);
        matter::Event event;
        while (session->poll_event(event)) {
            if (event.type == matter::EventType::BakePartDone)
                std::printf("bake %d/%d %s\n", event.done, event.total,
                            event.module.c_str());
            else if (event.type == matter::EventType::BakeFinished) {
                std::printf("bake finished (%d errors)\n", event.errors);
                bake_ready = event.errors == 0;
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
        options.vulkan_ray_tracing.enabled =
            vulkan->ray_tracing_available() && !disable_vulkan_rt;
        if (!session->render(frame_camera, frame, options, error)) {
            std::fprintf(stderr, "FATAL: render: %s\n", error.c_str());
            fatal_error = true;
        } else {
            camera_input_order.render_scene();
        }
        if (ui_frame_ready && !fatal_error) {
            viewer::draw_selection_outlines(selection_set, frame_camera,
                                            static_cast<int>(frame.extent.width),
                                            static_cast<int>(frame.extent.height),
                                            *session);
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
        std::string capture_path;
        if (!screenshot_path.empty() && bake_ready && frame_stats.instances_drawn > 0 &&
            ++screenshot_settle >= 3) {
            capture = true; capture_path = screenshot_path;
        } else if (shot_settle > 0 && frame_stats.instances_drawn > 0 &&
                   --shot_settle == 0) {
            capture = true; capture_path = shot_path;
        }
        std::vector<uint8_t> rgba;
        if (capture && !session->readback_swapchain_rgba8(frame, rgba, error)) {
            ++screenshot_failures;
            std::fprintf(stderr, "screenshot readback retry %d/5: %s\n",
                         screenshot_failures, error.c_str());
            capture = false;
            if (capture_path == screenshot_path) screenshot_settle = 1;
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
        const double perf_frame_cadence_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - perf_frame_start).count();
        hud_frame_ms = hud_frame_ms <= 0.0
                           ? perf_frame_cadence_ms
                           : hud_frame_ms * 0.9 + perf_frame_cadence_ms * 0.1;
        if (!frame_completed) {
            std::fprintf(stderr, "FATAL: end_frame: %s\n", error.c_str());
            fatal_error = true;
        } else {
            if (ui_frame_completed && !fatal_error) {
                camera_input_order.end_frame();
                if (camera_input_order.camera_update_allowed()) {
                    // Free-fly affects the next frame, after this frame's scene
                    // and UI have been submitted at the presentation boundary.
                    camera_controller.update(window, dt, camera);
                }
            }
            if (capture) {
                if (!write_png(capture_path, rgba, frame.extent.width,
                               frame.extent.height)) {
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
                            perf_finish_counters, frame_stats,
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
            std::printf("STATS,%s,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d\n",
                        stats_label.c_str(), stats.frame_ms, stats.resolve_ms,
                        stats.build_ms, stats.draw_ms, stats.instances_active,
                        stats.raster_batches, stats.raster_tris,
                        stats.culled_clusters, stats.gpu_culled_hiz);
            stats_label.clear();
        }
        if (stats.reload_requested) {
            stats.reload_requested = false;
            bake_ready = false; screenshot_settle = 0;
            selection_set.clear();
            editor_model.clear_selection();
            sim_control = matter::scene::SimulationControl{};
            viewer::prepare_world_reload(stats);
            session->reload();
        }
        if (stats.world_switch_requested >= 0 &&
            stats.world_switch_requested < static_cast<int>(worlds.size())) {
            const int selected = stats.world_switch_requested;
            stats.world_switch_requested = -1;
            auto next_session = open_world(worlds[selected]);
            if (!next_session) {
                viewer::complete_world_switch(stats, false);
                continue;
            }
            session = std::move(next_session);
            selection_set.clear();
            editor_model.clear_selection();
            sim_control = matter::scene::SimulationControl{};
            viewer::complete_world_switch(stats, true);
            stats.world_current = selected;
            selected_world_reported = false;
            console_log.push(viewer::LogSeverity::Info,
                              "Connected to " + worlds[selected].world_name);
            bake_ready = false; screenshot_settle = 0;
            apply_world_resolver_defaults(worlds[selected].world_name,
                                          active_radius,
                                          min_projected_size, stats);
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
    ui.shutdown();
    engine.reset();
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
