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
#include "camera_orbit.h"
#include "editor_model.h"
#include "editor_props.h"
#include "image_preview.h"
#include "issue_reporter.h"
#include "profile.h"
#include "shot_replay.h"
// M1d fly-through determinism trace.
#include "render/lod_trace.h"
#include "render/vk_resources.h"
#include "properties_panel.h"
#include "properties_registry.h"
#include "reveal_part.h"
#include "selection_bounds.h"
#include "selection_outline.h"
#include "selection_set.h"
#include "toolbar_panel.h"
#include "console_panel.h"
#include "ui.h"
#include "wireframe_controls.h"
#include "session_binding.h"
#include "scene_model_adapter.h"
#include "viewer_commands.h"
#include "matter/event/event_hub.h"
#include "matter/event/command.h"
#include "matter/event/property.h"
// QA timeline `wait_event`: subscribes directly against session->events()
// (bake./stream.) and app_hub (cmd.*) by name, so the typed event structs
// must be visible here (world_session.h only pulls in the legacy events.h).
#include "matter/events/bake_events.h"
#include "matter/events/stream_events.h"
#include "viewport_pick.h"

#include "imgui.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "external/stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <type_traits>
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
// Properties panel field access (Phase 5 Task 7, generalized by the property
// system's Phase 3 — see docs/superpowers/specs/2026-07-31-property-system-
// design.md S7).
//
// Field *routing* is schema-driven: ecs/scene_registry.h's FieldDescriptor
// carries a byte offset, so the strcmp ladders that used to live here are gone.
// What stays hardcoded per ComponentKind is only the ECS boundary — "copy this
// component out of the entity" and "set it back" — because flecs get/set are
// typed on the C++ component type.
//
// The get/set asymmetry is deliberate and preserved: a setter mutates a stack
// COPY of the whole component and re-sets it, so the edit lands as one
// transactional component write (one observer fire, one physics reconcile),
// never as a poke into live storage.
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

// A stack buffer able to hold a copy of any registered ECS component.
struct ComponentBuffer {
    alignas(matter::scene::kMaxComponentStructAlign)
        unsigned char bytes[matter::scene::kMaxComponentStructSize];
    void* data() { return bytes; }
};

template <typename T>
bool fetch_component_copy(flecs::entity e, void* out) {
    static_assert(std::is_trivially_copyable<T>::value,
                  "component copies are raw-byte buffers; needs a trivially copyable type");
    static_assert(sizeof(T) <= matter::scene::kMaxComponentStructSize, "ComponentBuffer too small");
    const T* p = get_ptr<T>(e);
    if (!p) return false;
    new (out) T(*p);
    return true;
}

template <typename T>
bool store_component_copy(flecs::entity e, const void* in) {
    e.set<T>(*static_cast<const T*>(in));
    return true;
}

// The ECS boundary: the only per-ComponentKind code left in the field path.
bool component_fetch(flecs::entity e, matter::scene::ComponentKind kind, void* out) {
    using matter::scene::ComponentKind;
    switch (kind) {
        case ComponentKind::Transform:
            return fetch_component_copy<matter::ecs::LocalTransform>(e, out);
        case ComponentKind::RigidBody:
            return fetch_component_copy<matter::physics::RigidBody>(e, out);
        case ComponentKind::Velocity:
            return fetch_component_copy<matter::physics::PhysicsVelocity>(e, out);
        case ComponentKind::SphereCollider:
            return fetch_component_copy<matter::physics::SphereCollider>(e, out);
        case ComponentKind::CapsuleCollider:
            return fetch_component_copy<matter::physics::CapsuleCollider>(e, out);
        case ComponentKind::BoxCollider:
            return fetch_component_copy<matter::physics::BoxCollider>(e, out);
        case ComponentKind::ConvexHullCollider:
            return fetch_component_copy<matter::physics::ConvexHullCollider>(e, out);
        case ComponentKind::PartInstance:
            return fetch_component_copy<matter::scene::PartInstance>(e, out);
        case ComponentKind::SectorStreaming:
            return false;  // tag component, no fields
    }
    return false;
}

bool component_store(flecs::entity e, matter::scene::ComponentKind kind, const void* in) {
    using matter::scene::ComponentKind;
    switch (kind) {
        case ComponentKind::Transform:
            return store_component_copy<matter::ecs::LocalTransform>(e, in);
        case ComponentKind::RigidBody:
            return store_component_copy<matter::physics::RigidBody>(e, in);
        case ComponentKind::Velocity:
            return store_component_copy<matter::physics::PhysicsVelocity>(e, in);
        case ComponentKind::SphereCollider:
            return store_component_copy<matter::physics::SphereCollider>(e, in);
        case ComponentKind::CapsuleCollider:
            return store_component_copy<matter::physics::CapsuleCollider>(e, in);
        case ComponentKind::BoxCollider:
            return store_component_copy<matter::physics::BoxCollider>(e, in);
        case ComponentKind::ConvexHullCollider:
            return store_component_copy<matter::physics::ConvexHullCollider>(e, in);
        case ComponentKind::PartInstance:
            return store_component_copy<matter::scene::PartInstance>(e, in);
        case ComponentKind::SectorStreaming:
            return false;
    }
    return false;
}

// Resolved (entity, component kind, field descriptor) for one field access,
// with the component already copied into the caller's buffer.
struct ResolvedField {
    flecs::entity entity;
    matter::scene::ComponentKind kind{};
    const matter::scene::FieldDescriptor* field = nullptr;
};

bool resolve_entity_field(matter::WorldSession* session, matter::scene::SceneEntityId id,
                          const char* component, const char* field,
                          ResolvedField& out, ComponentBuffer& buf) {
    if (!session || !component || !field) return false;
    const matter::scene::ComponentDescriptor* cd = matter::scene::find_component(component);
    if (!cd) return false;
    const matter::scene::FieldDescriptor* fd = matter::scene::find_field(*cd, field);
    if (!fd) return false;
    flecs::entity e = find_scene_entity(session->ecs(), id);
    if (!e.is_valid()) return false;
    if (!component_fetch(e, cd->kind, buf.data())) return false;
    out.entity = e;
    out.kind = cd->kind;
    out.field = fd;
    return true;
}

bool field_get_float(matter::WorldSession* session, matter::scene::SceneEntityId id,
                     const char* component, const char* field, float& out) {
    ComponentBuffer buf;
    ResolvedField r;
    if (!resolve_entity_field(session, id, component, field, r, buf)) return false;
    return matter::scene::field_get_float(buf.data(), *r.field, out);
}

bool field_set_float(matter::WorldSession* session, matter::scene::SceneEntityId id,
                     const char* component, const char* field, float value) {
    ComponentBuffer buf;
    ResolvedField r;
    if (!resolve_entity_field(session, id, component, field, r, buf)) return false;
    if (!matter::scene::field_set_float(buf.data(), *r.field, value)) return false;
    return component_store(r.entity, r.kind, buf.data());
}

bool field_get_int(matter::WorldSession* session, matter::scene::SceneEntityId id,
                   const char* component, const char* field, int& out) {
    ComponentBuffer buf;
    ResolvedField r;
    if (!resolve_entity_field(session, id, component, field, r, buf)) return false;
    int32_t v = 0;
    if (!matter::scene::field_get_int(buf.data(), *r.field, v)) return false;
    out = static_cast<int>(v);
    return true;
}

bool field_set_int(matter::WorldSession* session, matter::scene::SceneEntityId id,
                   const char* component, const char* field, int value) {
    ComponentBuffer buf;
    ResolvedField r;
    if (!resolve_entity_field(session, id, component, field, r, buf)) return false;
    if (!matter::scene::field_set_int(buf.data(), *r.field, static_cast<int32_t>(value)))
        return false;
    return component_store(r.entity, r.kind, buf.data());
}

bool field_get_uint(matter::WorldSession* session, matter::scene::SceneEntityId id,
                    const char* component, const char* field, uint32_t& out) {
    ComponentBuffer buf;
    ResolvedField r;
    if (!resolve_entity_field(session, id, component, field, r, buf)) return false;
    return matter::scene::field_get_uint(buf.data(), *r.field, out);
}

// PartInstance.part_hash and ConvexHullCollider.point_count are FieldReadOnly
// in the schema, so this rejects them exactly as the hand-written setter did
// (a 64-bit hash cannot survive a 32-bit write; a point_count without its
// points[] would describe a garbage hull). Assignment goes through the
// specialized part picker instead.
bool field_set_uint(matter::WorldSession* session, matter::scene::SceneEntityId id,
                    const char* component, const char* field, uint32_t value) {
    ComponentBuffer buf;
    ResolvedField r;
    if (!resolve_entity_field(session, id, component, field, r, buf)) return false;
    if (!matter::scene::field_set_uint(buf.data(), *r.field, value)) return false;
    return component_store(r.entity, r.kind, buf.data());
}

bool field_get_bool(matter::WorldSession* session, matter::scene::SceneEntityId id,
                    const char* component, const char* field, bool& out) {
    ComponentBuffer buf;
    ResolvedField r;
    if (!resolve_entity_field(session, id, component, field, r, buf)) return false;
    return matter::scene::field_get_bool(buf.data(), *r.field, out);
}

bool field_set_bool(matter::WorldSession* session, matter::scene::SceneEntityId id,
                    const char* component, const char* field, bool value) {
    ComponentBuffer buf;
    ResolvedField r;
    if (!resolve_entity_field(session, id, component, field, r, buf)) return false;
    if (!matter::scene::field_set_bool(buf.data(), *r.field, value)) return false;
    return component_store(r.entity, r.kind, buf.data());
}

bool field_get_float3(matter::WorldSession* session, matter::scene::SceneEntityId id,
                      const char* component, const char* field, matter::Float3& out) {
    ComponentBuffer buf;
    ResolvedField r;
    if (!resolve_entity_field(session, id, component, field, r, buf)) return false;
    return matter::scene::field_get_float3(buf.data(), *r.field, out);
}

bool field_set_float3(matter::WorldSession* session, matter::scene::SceneEntityId id,
                      const char* component, const char* field, matter::Float3 value) {
    ComponentBuffer buf;
    ResolvedField r;
    if (!resolve_entity_field(session, id, component, field, r, buf)) return false;
    if (!matter::scene::field_set_float3(buf.data(), *r.field, value)) return false;
    return component_store(r.entity, r.kind, buf.data());
}

bool field_get_quat(matter::WorldSession* session, matter::scene::SceneEntityId id,
                    const char* component, const char* field, matter::Quaternion& out) {
    ComponentBuffer buf;
    ResolvedField r;
    if (!resolve_entity_field(session, id, component, field, r, buf)) return false;
    return matter::scene::field_get_quat(buf.data(), *r.field, out);
}

bool field_set_quat(matter::WorldSession* session, matter::scene::SceneEntityId id,
                    const char* component, const char* field, matter::Quaternion value) {
    ComponentBuffer buf;
    ResolvedField r;
    if (!resolve_entity_field(session, id, component, field, r, buf)) return false;
    if (!matter::scene::field_set_quat(buf.data(), *r.field, value)) return false;
    return component_store(r.entity, r.kind, buf.data());
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
    // 2026-07-29 alpine tuning pass: long alpine sightlines with the
    // heightfield LOD ladder keeping distant sectors cheap. Trimmed
    // 10951 -> 10241 on 2026-07-30 to sit just past StreamMountain's
    // outermost terrain band (10095 m) instead of well beyond it.
    camera.far_plane = 10241.0f;
}

// Per-world sub-pixel floor. This used to also pick a RESOLVER per world by
// name (Meadow got SectorLod, everything else PassThrough) and seed an
// activation radius. Both are gone: there is one resolver, and its radius is
// derived from the world's outermost terrain LOD band rather than guessed from
// the world's name.
void apply_world_resolver_defaults(const std::string& world_name,
                                   float& min_projected_size,
                                   viewer::ViewerStats& stats) {
    min_projected_size = world_name == "Meadow" ? 0.0015f : 0.0f;
    // Seed the live slider from the world's default. From here the Debug View
    // control owns the value, so a world switch re-seeds it rather than
    // fighting it -- and RenderOptions reads the slider, not this local.
    stats.min_projected_size = min_projected_size;
}

// The windowed placement saved on the way into presentation mode, so leaving
// it puts the window back exactly where it was rather than at some default.
struct WindowedPlacement {
    int x = 0, y = 0, w = 0, h = 0;
    bool valid = false;
};

// GLFW has no "which monitor is this window on" query, so pick the monitor
// whose video-mode rect overlaps the window rect most. On one screen that is
// just the primary; on a multi-head desk it is the screen the viewer is
// actually sitting on, which is the one the user means by "fullscreen".
GLFWmonitor* monitor_for_window(GLFWwindow* window) {
    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetWindowPos(window, &wx, &wy);
    glfwGetWindowSize(window, &ww, &wh);
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    GLFWmonitor* best = glfwGetPrimaryMonitor();
    int best_overlap = 0;
    for (int i = 0; i < count; ++i) {
        int mx = 0, my = 0;
        glfwGetMonitorPos(monitors[i], &mx, &my);
        const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
        if (!mode) continue;
        const int ox = std::max(0, std::min(wx + ww, mx + mode->width) -
                                       std::max(wx, mx));
        const int oy = std::max(0, std::min(wy + wh, my + mode->height) -
                                       std::max(wy, my));
        if (ox * oy > best_overlap) {
            best_overlap = ox * oy;
            best = monitors[i];
        }
    }
    return best;
}

// Presentation mode (F11): fullscreen on the current monitor AND every panel
// hidden, as ONE toggle. Two separate keys would be the wrong shape — a
// fullscreen window still framed by docked panels is not what anyone means by
// "fullscreen" — and the UI-hidden path already exists and is exactly right
// for this: Ui::hide_ui_ skips the dockspace, spans viewport_rect_ across the
// display, and makes viewport_render_frame hand back the swapchain frame
// directly, so the 3D view renders straight to the screen with no intermediate
// viewport target.
//
// Returns true if the viewer is now in presentation mode. The swapchain needs
// no special handling here: the resize path this goes through is the same one
// glfwSetWindowSize uses (see MATTER_TEST_RESIZE below), and begin_frame
// rebuilds on out-of-date.
bool toggle_presentation_mode(GLFWwindow* window, WindowedPlacement& saved) {
    if (glfwGetWindowMonitor(window)) {
        // Leaving: restore the saved rect. The fallback only fires if we
        // somehow entered fullscreen without saving one, which would otherwise
        // strand the window at 0,0 with no title bar to drag.
        if (!saved.valid) saved = WindowedPlacement{100, 100, 1280, 720, true};
        glfwSetWindowMonitor(window, nullptr, saved.x, saved.y, saved.w,
                             saved.h, GLFW_DONT_CARE);
        return false;
    }
    GLFWmonitor* monitor = monitor_for_window(window);
    const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
    // No monitor or no video mode: stay windowed rather than guess a size.
    // Reporting "still windowed" keeps the caller's hide_ui in step with what
    // actually happened.
    if (!mode) return false;
    glfwGetWindowPos(window, &saved.x, &saved.y);
    glfwGetWindowSize(window, &saved.w, &saved.h);
    saved.valid = true;
    glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height,
                         mode->refreshRate);
    return true;
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
// issues/ has been gitignored since 0cbb5e92 (untracked, not committed), so
// the direct lookup wins only because the directory exists on disk for
// developers who have generated reports into it; the MatterEditor/ fallback
// covers a tree where it is absent.
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
           // GPU pass timers, as of the last sampled frame. These are the
           // only way to cost a compute pass that is small next to the frame:
           // at 220 fps the froxel passes are well under the run-to-run
           // spread of median_frame_ms, so a sweep over them reads as pure
           // noise in the end-to-end number and as a clean signal here.
           // Added for the cloud-layer work (issue 80c66789), which had
           // exactly that problem.
           << ",\"gpu_total_ms\":" << frame_stats.gpu_total_ms
           << ",\"gpu_volumetrics_ms\":" << frame_stats.gpu_vol_ms
           << ",\"gpu_atmosphere_ms\":" << frame_stats.gpu_atmosphere_ms
           << ",\"gpu_cloud_shadows_ms\":" << frame_stats.gpu_cloud_shadows_ms
           << ",\"gpu_vol_density_ms\":" << frame_stats.gpu_vol_density_ms
           << ",\"gpu_vol_scatter_ms\":" << frame_stats.gpu_vol_scatter_ms
           << ",\"gpu_vol_integrate_ms\":" << frame_stats.gpu_vol_integrate_ms
           << ",\"vol_memory_bytes\":" << frame_stats.vol_memory_bytes
           << ",\"cloud_shadow_memory_bytes\":"
           << frame_stats.cloud_shadow_memory_bytes
           // Full per-pass GPU zone breakdown (last sampled frame). rt is the
           // primary/shadow trace; rt_gi is the separate GI/reflection trace.
           // Added so a fly-through capture can attribute a heavy RT frame to
           // primary-ray traversal (dense foliage) vs the GI bounce.
           << ",\"gpu_cull_ms\":" << frame_stats.gpu_cull_ms
           << ",\"gpu_gbuffer_ms\":" << frame_stats.gpu_gbuffer_ms
           << ",\"gpu_blas_ms\":" << frame_stats.gpu_blas_ms
           << ",\"gpu_tlas_ms\":" << frame_stats.gpu_tlas_ms
           << ",\"gpu_rt_ms\":" << frame_stats.gpu_rt_ms
           << ",\"gpu_rt_gi_ms\":" << frame_stats.gpu_rt_gi_ms
           << ",\"gpu_denoise_ms\":" << frame_stats.gpu_denoise_ms
           << ",\"gpu_dlss_ms\":" << frame_stats.gpu_dlss_ms
           << ",\"gpu_composite_ms\":" << frame_stats.gpu_composite_ms
           << ",\"gpu_vt_ms\":" << frame_stats.gpu_vt_ms
           // CPU render-thread split (last sampled frame).
           << ",\"cpu_resolve_ms\":" << frame_stats.resolve_ms
           << ",\"cpu_build_ms\":" << frame_stats.build_ms
           << ",\"cpu_draw_ms\":" << frame_stats.draw_ms
           << ",\"cpu_draw_vt_requests_ms\":" << frame_stats.draw_vt_requests_ms
           << ",\"cpu_draw_cull_render_ms\":" << frame_stats.draw_cull_render_ms
           << ",\"cpu_draw_skin_seal_ms\":" << frame_stats.draw_skin_seal_ms
           << ",\"cpu_draw_composite_ms\":" << frame_stats.draw_composite_ms
           << ",\"validation_errors\":" << validation_errors << "}\n";
    if (!output) {
        error = "failed while writing MATTER_PERF_OUTPUT '" + config.output_path + "'";
        return false;
    }
    return true;
}

} // namespace

int main() {
    // Native Windows stdout is fully buffered when the harness redirects it;
    // MSYS `stdbuf` cannot alter the MSVCRT stream. FIFO automation consumes
    // exact acknowledgements as synchronization, so publish them immediately.
    if (std::getenv("MATTER_CMD_FIFO"))
        std::setvbuf(stdout, nullptr, _IONBF, 0);
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
    // The startup RT line moved down to just after EditorProps::init — RT is now
    // the render.gpu.ray_tracing property, whose value is not known until the
    // User scope file and MATTER_DISABLE_VK_RT have both been applied. Printing
    // it here would have reported the compiled default and quietly stopped
    // matching what the frame loop does. (smoke_vulkan_viewer.ps1 greps the
    // line, not its position.)
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

    // MATTER_CAM_PATH=<file>: a scripted fly-through consumed ONE POSE PER
    // RENDERED FRAME, and frame-indexed rather than wall-clock — the M1d
    // determinism gate (docs/superpowers/plans/2026-08-04-lod-vt-migration.md)
    // may not depend on timing, and neither existing mechanism qualifies:
    // MATTER_REPLAY is a single pose, and MATTER_CMD_FIFO's drain loop consumes
    // every buffered line in one frame, which tools/viewer_shots.sh only paces
    // by sleeping between writes.
    //
    // Format: one pose per line,
    //     eye_x eye_y eye_z target_x target_y target_z
    // Blank lines and lines starting with '#' are ignored. Companion env vars:
    //     MATTER_CAM_PATH_EXIT=1   quit once the path (plus its drain tail) ends
    //     MATTER_CAM_PATH_WARMUP=n frames to hold at the first pose after the
    //                              world is drawable, default 30
    struct CamPathPose {
        float eye[3];
        float target[3];
    };
    std::vector<CamPathPose> cam_path;
    if (const char* value = std::getenv("MATTER_CAM_PATH")) {
        if (FILE* file = std::fopen(value, "rb")) {
            char line[512];
            while (std::fgets(line, sizeof(line), file)) {
                CamPathPose pose{};
                if (std::sscanf(line, "%f %f %f %f %f %f", &pose.eye[0],
                                &pose.eye[1], &pose.eye[2], &pose.target[0],
                                &pose.target[1], &pose.target[2]) == 6) {
                    cam_path.push_back(pose);
                }
            }
            std::fclose(file);
            std::printf("MATTER_CAM_PATH: %zu poses from %s\n", cam_path.size(),
                        value);
        } else {
            std::fprintf(stderr, "FATAL: MATTER_CAM_PATH: cannot open %s\n",
                         value);
            return 1;
        }
        if (cam_path.empty()) {
            std::fprintf(stderr, "FATAL: MATTER_CAM_PATH: %s has no poses\n",
                         value);
            return 1;
        }
        // Hold the LOD trace closed until the path actually starts, so the
        // world's streaming-in churn — the genuinely timing-dependent part of a
        // warm run — never reaches the compared stream.
        viewer::lod_trace::set_capture_enabled(false);
    }
    const bool cam_path_exit = [] {
        const char* value = std::getenv("MATTER_CAM_PATH_EXIT");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    int cam_path_warmup = 30;
    // MATTER_CAM_PATH_SETTLE: SECONDS of unchanged resident_sectors required
    // before the path starts. 0 = off (frame warmup only).
    //
    // SECONDS, not polls, and that distinction is the whole point. The first
    // version of this counted consecutive frames, which is the very
    // frame-rate dependence it exists to remove: at 193 fps a 120-frame
    // plateau is 0.6 s, far shorter than one sector bake, so an ordinary
    // mid-fill lull satisfies it. Measured: it released at 22 resident
    // sectors against a settled ~590 and produced a clean-looking PASS with
    // essentially no world drawn. A bake-latency-scale wall-clock window is
    // the only threshold that means "the fill converged" on any machine.
    double cam_path_settle_s = 0.0;
    std::chrono::steady_clock::time_point cam_path_settle_since =
        std::chrono::steady_clock::now();
    uint32_t cam_path_settle_last = UINT32_MAX;
    if (const char* s = std::getenv("MATTER_CAM_PATH_SETTLE")) {
        const double parsed = std::atof(s);
        if (parsed >= 0.0) cam_path_settle_s = parsed;
    }
    if (const char* value = std::getenv("MATTER_CAM_PATH_WARMUP")) {
        const int parsed = std::atoi(value);
        if (parsed >= 0) cam_path_warmup = parsed;
    }
    size_t cam_path_index = 0;
    bool cam_path_running = false;
    bool cam_path_finished = false;
    // Frames to keep rendering the last pose after the path ends. The trace is
    // captured one frame-slot rotation behind the frame that produced it (see
    // VkSceneRenderer::capture_lod_trace), so quitting the instant the path ends
    // would drop the last few poses' results.
    int cam_path_drain = 0;

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
    float min_projected_size = 0.0f;
    apply_world_resolver_defaults(worlds[initial_world].world_name,
                                  min_projected_size, stats);
    // Property registry (property-system design S3/S4). Bound BEFORE anything
    // writes the tunable structs, so bind() captures the compiled defaults;
    // the World-scope baseline is re-captured at the connect seam below, once
    // the world's authored values have landed.
    //
    // Persistence is disabled for a replay for the same reason IniFilename is
    // cleared above: a replay must reproduce the recorded state, not inherit
    // (or overwrite) an interactive tuning session's files.
    // camera.prefs (Scope::User): the far plane and the fly speed. far_plane
    // seeds from whatever the authored/replay camera already carried, so the
    // group's compiled default is the one the code has always used.
    viewer::CameraPrefs camera_prefs;
    camera_prefs.far_plane = camera.far_plane;
    viewer::EditorProps editor_props;
    editor_props.init(stats, camera_prefs, ui.toolbar_state(),
                      ui.console_state(), !replay.valid);
    camera.far_plane = camera_prefs.far_plane;
    // render.gpu.ray_tracing has now been through every layer that can set it
    // (compiled default -> User scope file -> MATTER_DISABLE_VK_RT via
    // env_negated), so this line reports what the first frame will actually do.
    // `enabled` is ANDed with the device capability here exactly as the frame
    // loop does it: the property says what the user wants, the device says what
    // is possible, and the renderer only ever sees the conjunction.
    std::printf("Vulkan RT available=%s enabled=%s reason=%s\n",
                vulkan->ray_tracing_available() ? "true" : "false",
                vulkan->ray_tracing_available() &&
                        editor_props.gpu_prefs().ray_tracing
                    ? "true"
                    : "false",
                vulkan->ray_tracing_available()
                    ? (editor_props.gpu_prefs().ray_tracing
                           ? "none"
                           : "disabled by render.gpu.ray_tracing "
                             "(MATTER_DISABLE_VK_RT / Performance panel)")
                    : vulkan->ray_tracing_unavailable_reason().c_str());
    // Read once, at the same seam as the RT report and for the same reason:
    // it is a device fact, not a preference. Every wireframe control is gated
    // on it, and a device that cannot draw lines SAYS so on startup rather
    // than letting a control silently render solid.
    stats.wireframe_available = vulkan->wireframe_available();
    stats.wireframe_unavailable_reason = vulkan->wireframe_unavailable_reason();
    std::printf("Vulkan wireframe available=%s reason=%s\n",
                stats.wireframe_available ? "true" : "false",
                stats.wireframe_available
                    ? "none"
                    : stats.wireframe_unavailable_reason.c_str());
    editor_props.set_world(worlds[initial_world].project_dir,
                           worlds[initial_world].world_name);
    // Set at every connect seam; consumed on the first successful bake, after
    // the world-authored values above it have been adopted.
    bool apply_world_props_after_bake = true;

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
        // Persisted streaming-LOD overrides (stream.lod, World scope) reach the
        // session HERE — after the session exists, before SessionBinding
        // requests the first bake — which is why a saved override applies on
        // the FIRST connect instead of needing an extra reload. EditorProps
        // loaded them in set_world(), which every caller runs before this.
        result->set_streaming_lod_overrides(
            viewer::streaming_config_from(editor_props.streaming_prefs()));
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
    bool f11_down = false;
    WindowedPlacement windowed_placement{};
    bool dlss_modes_supported = false;
    // The session's side of "why not DLSS", mirrored out of FrameStats so the
    // Performance panel can show it on the greyed combo. Empty until the first
    // render reports one; the placeholder is what the tooltip says until then.
    std::string last_dlss_reason = "not yet reported by the renderer";
    // There is no `selected_dlss_mode` variable any more. The mode lives in
    // exactly one place — render.gpu.dlss_mode (Scope::User, so it persists) —
    // and MATTER_DLSS_MODE reaches it through the schema's own env layer
    // (props::apply_env, which ran inside EditorProps::init above and also
    // prints the "unparsable" diagnostic the hand-rolled parser used to). Every
    // other affordance goes through EditorProps::set_dlss_mode. Two variables
    // that could disagree was the specific failure this replaces.
    //
    // A MATTER_REPLAY run passes persist=false to EditorProps::init, so no user
    // settings file is read and this stays at the compiled default (Native)
    // regardless of what an interactive session persisted — which is exactly
    // the guarantee issues/README.md documents, now held by construction rather
    // than by a special case. MATTER_DLSS_MODE still overrides it, which is the
    // documented escape hatch in the replay banner below.
    auto dlss_mode_from_index = [](int index) {
        static_assert(static_cast<int>(matter::DlssMode::Native) == 0 &&
                          static_cast<int>(matter::DlssMode::Quality) == 1 &&
                          static_cast<int>(matter::DlssMode::Balanced) == 2 &&
                          static_cast<int>(matter::DlssMode::Performance) == 3,
                      "viewer::kDlssModeLabels must stay index-aligned with "
                      "matter::DlssMode");
        if (index < 0 || index > 3) index = 0;
        return static_cast<matter::DlssMode>(index);
    };
    auto selected_dlss_mode = [&] {
        return dlss_mode_from_index(editor_props.gpu_prefs().dlss_mode);
    };
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

    // ---- MATTER_SEAM_TRACE: the seam welder's only reader --------------------
    //
    // WorldSession::seam_weld_status() (world_session.h) had ZERO callers: the
    // welder's whole accounting existed and nothing ever printed it. The
    // profiler's `#stream.seam_pairs` is not a substitute — it is a
    // PROFILE_COUNT summed over rebuild_welds_for calls, i.e. a rate, not the
    // pool gauge — and closing that gap is why the accessor exists.
    //
    // This is the M0 acceptance instrument (design §6): drive a
    // MATTER_CAM_PATH fly-through across nested-sector band boundaries and read
    // the welder every frame. It is deliberately NOT a second camera driver;
    // MATTER_CAM_PATH already is one.
    //
    // Rows print ON CHANGE ONLY, the idiom the DLSS reporter below uses, so a
    // 3000-pose path emits tens of lines rather than thousands. Rows are
    // stamped with lod_trace::frame_label — the cam-path POSE index, which is
    // run-independent by construction — so a seam row and an LOD-trace row from
    // the same frame join on #f<n>.
    //
    // Reading happens beside session->frame_stats() on the APP THREAD, which is
    // what world_session.h requires of this accessor.
    const bool seam_trace = std::getenv("MATTER_SEAM_TRACE") != nullptr;
    // Only the fields whose change should cost a line. The live-pool geometry
    // (crossings/quads/tris) moves on almost every publish, so gating on it
    // would print every frame and defeat the point; it rides along on rows the
    // fields below trigger.
    struct SeamTraceKey {
        int pairs = -1;
        int drawn_pairs = -1;
        int registered_parts = -1;
        int missing_landing = -1;
        int sign_conflicts = -1;
        int degenerate = -1;
        int fine_side_incomplete = -1;
        int coarse_side_nulls = -1;
        uint64_t level_gap_pairs = UINT64_MAX;
        uint64_t build_errors = UINT64_MAX;
        uint64_t level_holds = UINT64_MAX;
        // The merge direction's own mechanism (evictions deferred to keep a
        // parked coarse newcomer covered). In the change key because a row that
        // shows a violation without showing whether the hold fired cannot say
        // which of the two mechanisms was in play.
        uint64_t merge_coverage_holds = UINT64_MAX;
        uint64_t drawn_level_violations = UINT64_MAX;
        uint64_t drawn_without_record = UINT64_MAX;
        uint64_t register_failures = UINT64_MAX;
        uint64_t hash_collisions = UINT64_MAX;
        uint64_t id_collisions = UINT64_MAX;
        bool operator!=(const SeamTraceKey& o) const {
            return pairs != o.pairs || drawn_pairs != o.drawn_pairs ||
                   registered_parts != o.registered_parts ||
                   missing_landing != o.missing_landing ||
                   sign_conflicts != o.sign_conflicts ||
                   degenerate != o.degenerate ||
                   fine_side_incomplete != o.fine_side_incomplete ||
                   coarse_side_nulls != o.coarse_side_nulls ||
                   level_gap_pairs != o.level_gap_pairs ||
                   build_errors != o.build_errors ||
                   level_holds != o.level_holds ||
                   merge_coverage_holds != o.merge_coverage_holds ||
                   drawn_level_violations != o.drawn_level_violations ||
                   drawn_without_record != o.drawn_without_record ||
                   register_failures != o.register_failures ||
                   hash_collisions != o.hash_collisions ||
                   id_collisions != o.id_collisions;
        }
    };
    SeamTraceKey seam_last;
    // Run accumulators. Live-pool fields are gauges, so their verdict over a run
    // is the MAXIMUM; the cumulative counters only rise, so theirs is the last
    // value. Keeping the two shapes apart is the difference between "this never
    // happened" and "this is not happening right now".
    struct SeamTraceRun {
        uint64_t samples = 0;
        int peak_pairs = 0;
        int peak_drawn_pairs = 0;
        int peak_registered_parts = 0;
        int peak_missing_landing = 0;
        int peak_sign_conflicts = 0;
        int peak_degenerate = 0;
        int peak_fine_side_incomplete = 0;
        int peak_coarse_side_nulls = 0;
        uint64_t peak_triangles = 0;
        uint64_t peak_drawn_triangles = 0;
        // crossings == quads + tris + missing_landing + degenerate, per
        // world_session.h. It holds per weld_face call and all four are sums,
        // so it must hold on every sample; a break means the pool's accounting
        // is wrong, not that the geometry is.
        uint64_t identity_breaks = 0;
        // drawn_pairs <= pairs. Fail-soft by design (headless, the draw kill
        // switch, or a refused registration), so a gap is a diagnostic, but
        // drawn_pairs EXCEEDING pairs would be nonsense.
        uint64_t drawn_exceeds_pairs = 0;
        int64_t first_sign_conflict = -1;
        int64_t first_level_gap = -1;
        int64_t first_build_error = -1;
        int64_t first_id_collision = -1;
        int64_t first_level_violation = -1;
        int64_t first_identity_break = -1;
        matter::WorldSession::SeamWeldStatus last{};
    };
    SeamTraceRun seam_run;
    // Printed unconditionally at the MATTER_CAM_PATH completion seam (and again
    // at shutdown if the run ended some other way), so a soak always leaves a
    // verdict even when nothing ever changed. Idempotent.
    bool seam_summary_printed = false;
    auto print_seam_summary = [&](const char* reason) {
        if (!seam_trace || seam_summary_printed) return;
        seam_summary_printed = true;
        const matter::WorldSession::SeamWeldStatus& s = seam_run.last;
        std::printf("\n==== seam-weld summary (%s) ====\n", reason);
        std::printf("  samples                    %llu\n",
                    (unsigned long long)seam_run.samples);
        if (seam_run.samples == 0) {
            std::printf("  NO SAMPLES — the world never drew. No verdict.\n");
            std::printf("==== end seam-weld summary ====\n");
            return;
        }
        std::printf("  -- live pool (peak over the run) --\n");
        std::printf("  pairs                      %d\n", seam_run.peak_pairs);
        std::printf("  drawn_pairs                %d\n", seam_run.peak_drawn_pairs);
        std::printf("  registered_parts           %d\n",
                    seam_run.peak_registered_parts);
        std::printf("  triangles                  %llu\n",
                    (unsigned long long)seam_run.peak_triangles);
        std::printf("  drawn_triangles            %llu\n",
                    (unsigned long long)seam_run.peak_drawn_triangles);
        std::printf("  degenerate                 %d\n", seam_run.peak_degenerate);
        std::printf("  -- diagnostics: NUMBERS, NOT GATES --\n");
        std::printf("  missing_landing peak       %d\n",
                    seam_run.peak_missing_landing);
        std::printf("  fine_side_incomplete peak  %d\n",
                    seam_run.peak_fine_side_incomplete);
        std::printf("  coarse_side_nulls peak     %d\n",
                    seam_run.peak_coarse_side_nulls);
        std::printf("  drawn_without_record       %llu\n",
                    (unsigned long long)s.drawn_without_record);
        std::printf("  level_holds                %llu\n",
                    (unsigned long long)s.level_holds);
        // A PARK count (level_holds) and an EVICTION-DEFERRAL count
        // (merge_coverage_holds) are two different mechanisms on two different
        // tiles; printed apart so a run can attribute what closed the gap.
        std::printf("  merge_coverage_holds       %llu\n",
                    (unsigned long long)s.merge_coverage_holds);
        std::printf("  drawn_level_violations     %llu",
                    (unsigned long long)s.drawn_level_violations);
        if (seam_run.first_level_violation >= 0)
            std::printf("  (first at #f%lld)",
                        (long long)seam_run.first_level_violation);
        std::printf("\n");
        std::printf("  parts_registered           %llu\n",
                    (unsigned long long)s.parts_registered);
        std::printf("  parts_released             %llu\n",
                    (unsigned long long)s.parts_released);
        // world_session.h calls this "the one to watch": the fan-out reaches
        // every neighbour of a published tile, so in the steady state almost
        // every rebuild must re-derive the same content hash and change
        // nothing. A parts_registered : noop_rebuilds ratio that is NOT small
        // means the pool is churning renderer parts for geometry that did not
        // move.
        std::printf("  noop_rebuilds              %llu\n",
                    (unsigned long long)s.noop_rebuilds);
        if (s.noop_rebuilds != 0)
            std::printf("    parts_registered : noop  1 : %.1f\n",
                        s.parts_registered != 0
                            ? (double)s.noop_rebuilds / (double)s.parts_registered
                            : 0.0);
        std::printf("  register_failures          %llu\n",
                    (unsigned long long)s.register_failures);
        std::printf("  hash_collisions            %llu\n",
                    (unsigned long long)s.hash_collisions);
        std::printf("  pairs_peak                 %d\n", s.pairs_peak);
        std::printf("  -- invariants --\n");
        struct Gate { const char* name; unsigned long long value;
                      int64_t first; const char* note; };
        const Gate gates[] = {
            {"sign_conflicts", (unsigned long long)seam_run.peak_sign_conflicts,
             seam_run.first_sign_conflict, "both sides read the same samples"},
            {"level_gap_pairs", (unsigned long long)s.level_gap_pairs,
             seam_run.first_level_gap, "drawn +-1 invariant"},
            {"build_errors", (unsigned long long)s.build_errors,
             seam_run.first_build_error, ""},
            {"id_collisions", (unsigned long long)s.id_collisions,
             seam_run.first_id_collision, ""},
            {"identity_breaks", (unsigned long long)seam_run.identity_breaks,
             seam_run.first_identity_break,
             "crossings == quads+tris+missing_landing+degenerate"},
            {"drawn_pairs>pairs", (unsigned long long)seam_run.drawn_exceeds_pairs,
             -1, ""},
        };
        bool held = true;
        for (const Gate& g : gates) {
            const bool ok = g.value == 0;
            held = held && ok;
            std::printf("  %-26s %-10llu %s", g.name, g.value,
                        ok ? "OK" : "VIOLATED");
            if (!ok && g.first >= 0)
                std::printf(" (first at #f%lld)", (long long)g.first);
            if (g.note[0]) std::printf("   [%s]", g.note);
            std::printf("\n");
        }
        // kSeamWeldPairAlarm (matter_engine.cpp): not a silent-drop ceiling,
        // a GROWTH guard. ~60x the geometric expectation, so exceeding it means
        // the keying or the fan-out is wrong, never that the world got bigger.
        const bool under_alarm = s.pairs_peak < 4096;
        held = held && under_alarm;
        std::printf("  %-26s %-10d %s   [kSeamWeldPairAlarm 4096]\n",
                    "pairs_peak < 4096", s.pairs_peak,
                    under_alarm ? "OK" : "VIOLATED");
        std::printf("  VERDICT: %s\n",
                    held ? "PASS — every M0 seam invariant held"
                         : "FAIL — an M0 seam invariant was violated");
        std::printf("==== end seam-weld summary ====\n");
    };
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
    // ~8 s at 30 fps. Long enough that a hitch is still in the window by the
    // time a human reacts and presses the key, short enough to stay small in
    // the report (6 floats/ints per row).
    static constexpr size_t kIssueHistoryFrames = 240;
    std::vector<viewer::IssueFrameSample> issue_frame_history;
    size_t issue_history_cursor = 0;
    // Copies the ring into a shot in CAPTURE ORDER (oldest first) and derives
    // the peaks. Defined as a lambda beside the ring so the two cannot drift.
    auto fill_shot_history = [&](viewer::IssueShot& shot) {
        const size_t n = issue_frame_history.size();
        shot.history.clear();
        shot.history.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            // Full ring: start at the cursor (the oldest slot). Partial ring:
            // it has not wrapped, so slot order IS capture order.
            const size_t idx =
                (n == kIssueHistoryFrames) ? (issue_history_cursor + i) % n : i;
            shot.history.push_back(issue_frame_history[idx]);
        }
        for (const viewer::IssueFrameSample& h : shot.history) {
            shot.peak_frame_ms = std::max(shot.peak_frame_ms, h.frame_ms);
            shot.peak_render_ms = std::max(shot.peak_render_ms, h.render_ms);
            shot.peak_build_ms = std::max(shot.peak_build_ms, h.build_ms);
        }
    };
    viewer::ImagePreviewCache issue_previews;
    issue_previews.configure(vulkan.get());
    // Resolve now, once, and say where: a report written somewhere unexpected
    // is a report nobody finds.
    viewer::set_issues_dir(issues_root());
    std::printf("issues: reports go to %s\n", viewer::issues_dir().c_str());
    // Declared here rather than beside the other capture env vars below because
    // stamp_replay_state records it per shot.
    //
    // Two values, not one: `hide_ui_forced` is what the environment/replay asked
    // for and never changes, `hide_ui` is what this frame actually does. F11's
    // presentation mode ORs into the live one, so leaving presentation mode
    // restores the forced state instead of revealing a UI that MATTER_HIDE_UI
    // said to hide.
    const bool hide_ui_forced = std::getenv("MATTER_HIDE_UI") != nullptr ||
                                (replay.valid && !replay.ui_visible);
    bool hide_ui = hide_ui_forced;
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
        shot.dlss_mode = matter::dlss_mode_name(selected_dlss_mode());
        shot.pixel_budget = stats.pixel_budget;
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
    // MATTER_SCREENSHOT_SETTLE: the same dial for a plain screenshot run.
    // Three frames is enough for a world that is fully built at bake time, but
    // a STREAMED world has barely started at that point -- the disc fills over
    // seconds -- so an un-tunable 3 photographs an empty horizon and reads
    // exactly like a renderer that dropped the far field. Applies whether or
    // not this is a replay, so it can also shorten one.
    if (const char* value = std::getenv("MATTER_SCREENSHOT_SETTLE")) {
        const int parsed = std::atoi(value);
        if (parsed > 0) settle_frames = parsed;
    }
    const char* screenshot_env = std::getenv("MATTER_SCREENSHOT");
    // A replay run IS a screenshot run: it reuses the settle/readback/quit path
    // wholesale, and only differs in cropping the result to the recorded rect.
    const char* replay_out_env = std::getenv("MATTER_REPLAY_OUT");
    const std::string screenshot_path =
        screenshot_env  ? screenshot_env
        : replay.valid  ? (replay_out_env ? replay_out_env : "replay.png")
                        : "";
    // Capture-only aid for automated Lighting-panel verification. This is not a
    // command and cannot affect an ordinary editor session: the tab is focused
    // only while the caller explicitly asks for a screenshot capture.
    const bool capture_lighting_ui = std::getenv("MATTER_CAPTURE_LIGHTING_UI") != nullptr;
    int screenshot_settle = 0;
    int screenshot_failures = 0;
    bool bake_ready = false;
    bool selected_world_reported = false;
    bool apply_world_camera_after_bake =
        !replay.valid && initial_camera_env == nullptr;
    // World-authored volumetrics defaults adopt on every world load (initial
    // and switches); replays keep their recorded settings.
    bool apply_world_volumetrics_after_bake = !replay.valid;
    uint64_t last_rejected_froxel_generation = UINT64_MAX;
    bool apply_world_atmosphere_after_bake = !replay.valid;
    bool apply_world_cloud_shadows_after_bake = !replay.valid;
    // render.fog's own one-shot, deliberately NOT folded into the volumetrics
    // flag beside it: the two are published by different engine paths (a
    // closed-world connect publishes fog but never volumetrics, and a
    // resolve-cache hit publishes fog without re-running the world-kind
    // install), so sharing a flag would silently change when volumetrics
    // adopts.
    bool apply_world_fog_after_bake = !replay.valid;
    // Sun orientation/size gets its own one-shot for the same reason fog does,
    // and it must land BEFORE EditorProps::on_world_connected snapshots the
    // render.lighting baseline -- otherwise "Reset to World" would restore the
    // compiled default angles instead of the sun this world authored.
    //
    // NOT skipped in a replay, which is where this parts company with fog and
    // volumetrics above. Seeding is a no-op by construction here: the seeded
    // angles EQUAL the authored ones, so the engine's equality test passes and
    // it copies the authored sun_direction through untouched -- a replay stays
    // pixel-identical to a build with no sun controls at all. Fog has no such
    // test, which is why it has to abstain instead.
    //
    // Seeding in replays is also what makes the feature verifiable: with no
    // scriptable input on Windows, MATTER_SUN_ELEVATION_DEG et al (the props
    // env layer, applied after the seed) are the only way to capture a shot at
    // a different sun, and they need a seeded override to land on.
    bool apply_world_sun_after_bake = true;
    // Does ViewerStats::fog hold this world's authored fog yet? Only then may
    // RenderOptions::use_fog_override be set. Before the adoption below,
    // stats.fog is the compiled default and the session's own authored_fog_ is
    // the truth — and in a REPLAY the adoption never runs at all (same reason
    // volumetrics is not adopted there), so a replay keeps rendering the
    // engine's authored fog and stays pixel-identical to a pre-WS2 build.
    bool fog_override_ready = false;
    // Does ViewerStats::lighting hold this world's authored sun angles yet?
    // Same contract as fog_override_ready above: until it does, the angles in
    // the struct describe some other world (or nothing at all) and must not be
    // allowed to aim this one.
    bool sun_override_ready = false;
    const bool test_resize = std::getenv("MATTER_TEST_RESIZE") != nullptr;
    if (replay.valid) {
        // The toggles that change pixels. DLSS is deliberately NOT restored: it
        // is temporal, so a shot taken after seconds of accumulation cannot be
        // matched by a freshly-settled replay, and forcing Native at least makes
        // the comparison honest and repeatable. Ask for the recorded mode with
        // MATTER_DLSS_MODE if you want it back.
        stats.pixel_budget = replay.pixel_budget;
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
    if (fifo_path) std::fflush(stdout);
    std::string shot_path;
    std::string stats_label;
    int shot_settle = 0;
    viewer::FifoPresentSequencer fifo_present;

    // ---- QA timeline state (event-system.md-style FIFO blocking waits) ------
    // Lines already split out of cmd_buffer but not yet dispatched. Blocking
    // waits (wait_frames / wait_idle / wait_event) pause POPPING this queue --
    // reading more bytes/lines off the file above is always fine -- which is
    // what turns a pre-written command file into a timeline script instead of
    // "every buffered line lands in one frame" (the old drain-loop behavior).
    // One-command-at-a-time external writers (tools/viewer_shots.sh) see
    // identical behavior: nothing here holds a line back unless it, or one
    // before it, is itself a blocking wait.
    std::deque<std::string> fifo_pending_lines;
    enum class FifoBlockKind { None, WaitFrames, WaitIdle, WaitEvent };
    FifoBlockKind fifo_block = FifoBlockKind::None;
    // wait_idle <seconds>: released once resident_sectors has held steady for
    // `seconds` of wall-clock time AND the bake is ready. Mirrors the
    // MATTER_CAM_PATH_SETTLE plateau logic above (wall time, not frame count,
    // for the same reason: bake progress is rate-limited in wall time, not
    // frames).
    double fifo_wait_idle_seconds = 0.0;
    std::chrono::steady_clock::time_point fifo_wait_idle_start{};
    std::chrono::steady_clock::time_point fifo_wait_idle_last_change{};
    uint32_t fifo_wait_idle_last_resident = UINT32_MAX;
    // wait_event <name> [timeout_seconds]: `fired` is set from whatever
    // thread emits the event (bake.*/stream.* fire on the bake worker thread;
    // cmd.* always on the app thread, see command.cpp finalize_common) -- the
    // main-thread release check only ever touches it through the atomic.
    std::atomic<bool> fifo_wait_event_fired{false};
    matter::evt::Subscription fifo_wait_event_sub;
    std::string fifo_wait_event_name;
    double fifo_wait_event_timeout_s = 0.0;
    std::chrono::steady_clock::time_point fifo_wait_event_start{};
    // bake./stream. events are session-scoped: if the world switches while
    // we're waiting, the subscribed hub is about to die and the event we
    // asked for will never come. cmd.* lives on app_hub, which outlives
    // world switches, so it is never session-scoped.
    bool fifo_wait_event_session_scoped = false;
    uint64_t fifo_wait_event_session_id = 0;
    uint64_t fifo_wait_event_generation = 0;
    // Hub subscriber names must be unique per event type FOREVER (an
    // unsubscribed record stays in the registry, just inactive), so repeated
    // wait_event calls for the same name need a fresh name each time.
    uint64_t fifo_wait_event_seq = 0;
    // `quit` is deferred until no FIFO screenshot capture is still in flight
    // -- see the fifo_quit_pending resolution after end_frame below.
    bool fifo_quit_pending = false;

    matter::RenderPath fifo_render_path = matter::RenderPath::GpuDriven;
    bool fifo_render_path_override = false;
    stats.session_status.native_rt_available =
        vulkan->ray_tracing_available();
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
    auto reg_fifo_render_path =
        registry.must_register_handler<viewer::FifoRenderPath>(
            matter::evt::CommandScope::App, app_lane,
            [&](const viewer::FifoRenderPath& cmd) {
                if (cmd.requested == matter::RenderPath::Raytrace &&
                    !vulkan->ray_tracing_available()) {
                    stats.session_status.render_path =
                        viewer::ViewerRenderPathStatus::NativeRtUnavailable;
                    std::printf("render_path: native_rt unavailable\n");
                    return viewer::FifoRenderPath::Result::failed(
                        "native RT unavailable");
                }
                fifo_render_path = cmd.requested;
                fifo_render_path_override = true;
                stats.session_status.render_path =
                    cmd.requested == matter::RenderPath::Raytrace
                        ? viewer::ViewerRenderPathStatus::NativeRt
                        : viewer::ViewerRenderPathStatus::Raster;
                std::printf("render_path: %s\n",
                            cmd.requested == matter::RenderPath::Raytrace
                                ? "native_rt"
                                : "raster");
                return viewer::FifoRenderPath::Result::succeeded(true);
            });
    auto reg_fifo_history_reset =
        registry.must_register_handler<viewer::FifoHistoryReset>(
            matter::evt::CommandScope::App, app_lane,
            [&](const viewer::FifoHistoryReset&) {
                session->request_atmosphere_history_reset();
                std::printf("history_reset: requested\n");
                return viewer::FifoHistoryReset::Result::succeeded(true);
            });
    auto reg_fifo_wait_frames =
        registry.must_register_handler<viewer::FifoWaitFrames>(
            matter::evt::CommandScope::App, app_lane,
            [&](const viewer::FifoWaitFrames& cmd) {
                fifo_present.queue_wait(cmd.count);
                return viewer::FifoWaitFrames::Result::succeeded(true);
            });
    auto reg_fifo_shot_now =
        registry.must_register_handler<viewer::FifoScreenshotNow>(
            matter::evt::CommandScope::App, app_lane,
            [&](const viewer::FifoScreenshotNow& cmd) {
                fifo_present.queue_screenshot(cmd.path);
                std::printf("shot_now: queued %s\n", cmd.path.c_str());
                return viewer::FifoScreenshotNow::Result::succeeded(true);
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
    // `dlss <mode>` is kept (it is the documented spelling and predates the
    // property) but no longer owns any state: it resolves the word against the
    // SAME label table the combo box draws, then writes through
    // EditorProps::set_dlss_mode like every other affordance. `set
    // render.gpu.dlss_mode quality` reaches the identical field via FifoSetProp
    // below; both spellings now mean one thing.
    auto reg_fifo_dlss = registry.must_register_handler<viewer::FifoDlss>(
        matter::evt::CommandScope::App, app_lane, [&](const viewer::FifoDlss& cmd) {
            int index = -1;
            for (int i = 0; i < 4; ++i)
                if (matter::props::equals_ignore_case(viewer::kDlssModeLabels[i],
                                                      cmd.mode.c_str()))
                    index = i;
            if (index < 0) {
                std::printf("dlss: expected native, quality, balanced, or performance\n");
                return viewer::FifoDlss::Result::failed("bad dlss mode");
            }
            if (!editor_props.set_dlss_mode(index))
                return viewer::FifoDlss::Result::failed("env-forced");
            return viewer::FifoDlss::Result::succeeded(true);
        });
    // Generic property set/get over the editor's registry (S6.3). One handler
    // replaces what would otherwise be a bespoke FIFO command per tunable.
    //
    // The grammar is `set <group.path>.<field> <value>` — ONE path token, not a
    // group token followed by a field token. Writing `set draw.overrides
    // Rock/lod_bias 0.25` therefore resolves the GROUP path as if it were a
    // field path and takes the rest of the line as the value, and a bare
    // "unknown property 'draw.overrides'" reads as "that group is not in the
    // registry" — which sent one investigation hunting for a missing binding
    // that had been there all along (draw.overrides is bound by
    // EditorProps::on_world_connected, and resolve_field splits on the LAST '.'
    // precisely so "Rock/lod_bias" survives as the field half). Name the
    // near-miss instead of reporting the group as absent.
    auto reg_fifo_set_prop = registry.must_register_handler<viewer::FifoSetProp>(
        matter::evt::CommandScope::App, app_lane,
        [&](const viewer::FifoSetProp& cmd) {
            const viewer::FifoPropertyResult result =
                viewer::fifo_set_property(editor_props.registry(), cmd.path,
                                          cmd.value);
            std::printf("%s\n", result.line.c_str());
            return result.success
                       ? viewer::FifoSetProp::Result::succeeded(true)
                       : viewer::FifoSetProp::Result::failed(result.line);
        });
    auto reg_fifo_get_prop = registry.must_register_handler<viewer::FifoGetProp>(
        matter::evt::CommandScope::App, app_lane,
        [&](const viewer::FifoGetProp& cmd) {
            const viewer::FifoPropertyResult result =
                viewer::fifo_get_property(editor_props.registry(), cmd.path);
            std::printf("%s\n", result.line.c_str());
            return result.success
                       ? viewer::FifoGetProp::Result::succeeded(true)
                       : viewer::FifoGetProp::Result::failed(result.line);
        });
    auto reg_fifo_quit = registry.must_register_handler<viewer::FifoQuit>(
        matter::evt::CommandScope::App, app_lane, [&](const viewer::FifoQuit&) {
            // Deferred, not immediate: a `shot <path>` settles over a few
            // frame iterations (shot_settle) and `shot_now` drains through
            // fifo_present, so a timeline's `quit` line right after its last
            // `shot` (no intervening wait) must not truncate that capture.
            // Resolved once per frame after end_frame below, once nothing is
            // still in flight.
            fifo_quit_pending = true;
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

    // What a RequiresReload property group's "Apply & Reload" runs, from EITHER
    // the Performance panel or the generic Tunables panel. The ordering is the
    // whole point: the applied override must reach the session BEFORE the
    // reload, because set_streaming_lod_overrides is consumed at the next
    // connect. Panels never have to know that.
    editor_props.set_reload_request([&]() {
        if (session)
            session->set_streaming_lod_overrides(
                viewer::streaming_config_from(editor_props.streaming_prefs()));
        registry.execute(viewer::ViewerReload{});
    });

    // `wait_event <name>` name -> subscription table (S I.11-style FIFO dev
    // convenience, not a registered viewer::Fifo* command: it must NOT itself
    // go through the registry, or its own cmd.completed would immediately
    // satisfy a `wait_event cmd.completed` before any LATER command's
    // completion could). bake./stream. subscribe on the session hub
    // (immediate -- these fire on the bake worker thread, see
    // matter_engine.cpp's execute_bake); cmd.* subscribe on app_hub (always
    // the app thread, command.cpp finalize_common). Returns false for an
    // unrecognized name; the caller does not block in that case.
    auto fifo_begin_wait_event = [&](const std::string& name, double timeout_s) -> bool {
        fifo_wait_event_fired.store(false, std::memory_order_relaxed);
        fifo_wait_event_name = name;
        fifo_wait_event_timeout_s = timeout_s;
        fifo_wait_event_start = std::chrono::steady_clock::now();
        fifo_wait_event_session_scoped = false;
        std::atomic<bool>* fired = &fifo_wait_event_fired;
        const std::string sub_name =
            "qa.wait_event." + std::to_string(fifo_wait_event_seq++) + "." + name;
        if (name == "bake.started") {
            fifo_wait_event_sub = session->events().must_subscribe<matter::events::BakeStarted>(
                sub_name.c_str(), matter::evt::immediate,
                [fired](const matter::events::BakeStarted&) {
                    fired->store(true, std::memory_order_release);
                });
        } else if (name == "bake.finished") {
            fifo_wait_event_sub = session->events().must_subscribe<matter::events::BakeFinished>(
                sub_name.c_str(), matter::evt::immediate,
                [fired](const matter::events::BakeFinished&) {
                    fired->store(true, std::memory_order_release);
                });
        } else if (name == "bake.part_done") {
            fifo_wait_event_sub = session->events().must_subscribe<matter::events::BakePartDone>(
                sub_name.c_str(), matter::evt::immediate,
                [fired](const matter::events::BakePartDone&) {
                    fired->store(true, std::memory_order_release);
                });
        } else if (name == "bake.error") {
            fifo_wait_event_sub = session->events().must_subscribe<matter::events::BakeError>(
                sub_name.c_str(), matter::evt::immediate,
                [fired](const matter::events::BakeError&) {
                    fired->store(true, std::memory_order_release);
                });
        } else if (name == "stream.refine_tile") {
            fifo_wait_event_sub =
                session->events().must_subscribe<matter::events::RefineTileDone>(
                    sub_name.c_str(), matter::evt::immediate,
                    [fired](const matter::events::RefineTileDone&) {
                        fired->store(true, std::memory_order_release);
                    });
        } else if (name == "cmd.completed") {
            fifo_wait_event_sub = app_hub.must_subscribe<matter::evt::CommandCompleted>(
                sub_name.c_str(), matter::evt::immediate,
                [fired](const matter::evt::CommandCompleted&) {
                    fired->store(true, std::memory_order_release);
                });
        } else if (name == "cmd.failed") {
            fifo_wait_event_sub = app_hub.must_subscribe<matter::evt::CommandFailed>(
                sub_name.c_str(), matter::evt::immediate,
                [fired](const matter::evt::CommandFailed&) {
                    fired->store(true, std::memory_order_release);
                });
        } else {
            return false;
        }
        if (name.rfind("bake.", 0) == 0 || name.rfind("stream.", 0) == 0) {
            fifo_wait_event_session_scoped = true;
            fifo_wait_event_session_id = binding.current_session_id();
            fifo_wait_event_generation = binding.current_generation();
        }
        return true;
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
        // User-scope autosave debounce (S5): fires ~1 s after the last edit.
        editor_props.tick(dt);
        // What the Performance panel greys render.gpu's two controls on, pushed
        // here rather than at the panel because main.cpp is what holds the
        // VulkanDevice. Before the panels draw, so the reason is never a frame
        // stale. `dlss_modes_supported` additionally folds in the session's own
        // per-frame dlss_reason (computed after render, below) — an unavailable
        // reason from the device is permanent, one from the session can clear.
        editor_props.set_gpu_capabilities(
            vulkan->ray_tracing_available(),
            vulkan->ray_tracing_unavailable_reason(),
            vulkan->dlss_available() && dlss_modes_supported,
            vulkan->dlss_available() ? last_dlss_reason
                                     : vulkan->dlss_unavailable_reason());
        stats.session_status.native_rt_available =
            vulkan->ray_tracing_available();
        // camera.prefs -> the live camera. One-way and every frame, the same
        // shape as the RenderOptions copy block below: the property group is
        // the edit site, CameraDesc stays the thing everything else reads.
        camera.far_plane = camera_prefs.far_plane;
        if (key_pressed(window, GLFW_KEY_TAB, tab_down)) {
            camera_capture = !camera_capture;
            camera_controller.set_capture(window, camera_capture,
                                          camera_prefs.raw_mouse_motion);
        }
        // Issue capture hotkeys. Handled HERE, at the GLFW level, rather than
        // beside the ImGui hotkeys below: ui.cpp enables
        // ImGuiConfigFlags_NavEnableKeyboard, which makes io.WantCaptureKeyboard
        // true whenever a panel holds nav focus, so an ImGui-gated hotkey fires
        // only sometimes. Neither key is a text character, so claiming them
        // unconditionally costs nothing.
        //
        // F9 is reserved for issue capture. Wireframe is controlled by the
        // capability-gated Debug View checkbox / combo entry, or by the FIFO
        // `wireframe` verbs, which now do something.
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
        // F11: presentation mode -- fullscreen on the current monitor with every
        // panel hidden (see toggle_presentation_mode). Deliberately NOT gated on
        // issue_capture_idle: a region capture freezes the screen to select on,
        // and changing the window size out from under it would invalidate the
        // rect being dragged -- but F11 during a capture is far more likely to be
        // someone trying to get out of a mode than someone resizing mid-drag, so
        // the escape hatch wins. Camera capture (TAB) is unaffected either way.
        if (key_pressed(window, GLFW_KEY_F11, f11_down)) {
            const bool presenting =
                toggle_presentation_mode(window, windowed_placement);
            hide_ui = presenting || hide_ui_forced;
            ui.set_hide_ui(hide_ui);
            std::printf("viewer: presentation mode %s\n",
                        presenting ? "on (F11 to exit)" : "off");
        }
        // F8 still cycles, but through the property rather than its own copy,
        // so the Performance panel's combo follows the key press and vice
        // versa. set_dlss_mode also refuses while MATTER_DLSS_MODE is forcing
        // the field, which the old key path did not — a forced value that a
        // keystroke could quietly beat was not much of a force.
        if (key_pressed(window, GLFW_KEY_F8, f8_down)) {
            if (!dlss_modes_supported) {
                editor_props.set_dlss_mode(
                    static_cast<int>(matter::DlssMode::Native));
                std::printf("DLSS: Native (%s)\n",
                            vulkan->dlss_unavailable_reason().c_str());
            } else {
                editor_props.set_dlss_mode(
                    (editor_props.gpu_prefs().dlss_mode + 1) % 4);
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
                fifo_pending_lines.push_back(std::move(line));
            }
        }
        // Timeline semantics (QA timeline feature): a blocking wait
        // (wait_frames / wait_idle / wait_event) pauses POPPING fifo_pending_lines
        // until it releases, so a `set`/`shot`/etc. line written after a
        // `wait_frames` in the file cannot dispatch before the wait completes.
        // Reading more bytes into cmd_buffer above still happens every frame
        // regardless -- only dispatch is gated. This loop can still drain
        // several non-blocking lines in one frame (unchanged from before) --
        // it only stops early when a blocking wait is (re)armed.
        while (fifo_block == FifoBlockKind::None && !fifo_pending_lines.empty()) {
            std::string line = std::move(fifo_pending_lines.front());
            fifo_pending_lines.pop_front();
            {
                // MATTER_CMD_FIFO is a cross-thread command source (S II.3.4):
                // parse each line into its typed command and dispatch() it so
                // every external submission is named/traced/journaled and gets
                // an explicit ticket completion. The queued jobs run at the
                // registry pump right below (still this frame, before render).
                float c[6]; char word[256];
                const viewer::FifoParseResult presentation_command =
                    viewer::parse_fifo_line(line);
                if (presentation_command.recognized) {
                    if (!presentation_command.success) {
                        std::printf("%s\n",
                                    presentation_command.error.c_str());
                    } else if (const auto* command =
                                   std::get_if<viewer::FifoRenderPath>(
                                       &presentation_command.command)) {
                        registry.dispatch(*command);
                    } else if (const auto* command =
                                   std::get_if<viewer::FifoHistoryReset>(
                                       &presentation_command.command)) {
                        registry.dispatch(*command);
                    } else if (const auto* command =
                                   std::get_if<viewer::FifoWaitFrames>(
                                       &presentation_command.command)) {
                        registry.dispatch(*command);
                        // Semantics upgrade: wait_frames now blocks every
                        // later line, not just screenshots (the present-count
                        // itself is still FifoPresentSequencer's, released
                        // below in the completed_waits loop after advance()).
                        fifo_block = FifoBlockKind::WaitFrames;
                    } else if (const auto* command =
                                   std::get_if<viewer::FifoScreenshotNow>(
                                       &presentation_command.command)) {
                        registry.dispatch(*command);
                    }
                } else if (std::sscanf(line.c_str(), "cam %f %f %f %f %f %f",
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
                } else if (line.compare(0, 4, "set ") == 0) {
                    // `set <group.path>.<field> <value>` — the value is the
                    // whole rest of the line (unquoted), so a String field can
                    // carry spaces and commas ("128:2, 384:1").
                    const size_t name_start = 4;
                    const size_t name_end = line.find(' ', name_start);
                    if (name_end == std::string::npos) {
                        std::printf("set: expected `set <group.field> <value>`\n");
                    } else {
                        viewer::FifoSetProp cmd;
                        cmd.path = line.substr(name_start, name_end - name_start);
                        size_t value_start = name_end;
                        while (value_start < line.size() && line[value_start] == ' ')
                            ++value_start;
                        cmd.value = line.substr(value_start);
                        registry.dispatch(cmd);
                    }
                } else if (std::sscanf(line.c_str(), "get %255s", word) == 1) {
                    viewer::FifoGetProp cmd; cmd.path = word;
                    registry.dispatch(cmd);
                } else if (std::sscanf(line.c_str(), "hiz %255s", word) == 1) {
                    // The HZB is gone (it could not work on tile-sized
                    // clusters). Kept as a recognised command so old scripts
                    // get an answer rather than "unrecognized", and pointed at
                    // the occlusion cull that replaced it.
                    std::printf("hiz: removed -- use "
                                "`set viewer.debug.occlusion_draw_cull true`\n");
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
                } else if (const auto wireframe_result =
                               viewer::apply_wireframe_console_command(
                                   line, stats.wireframe_available,
                                   stats.wireframe);
                           wireframe_result !=
                               viewer::WireframeConsoleCommandResult::
                                   Unrecognized) {
                    if (wireframe_result ==
                        viewer::WireframeConsoleCommandResult::Unavailable) {
                        std::printf(
                            "wireframe: unavailable (%s)\n",
                            stats.wireframe_unavailable_reason.empty()
                                ? "device does not support "
                                  "VK_POLYGON_MODE_LINE"
                                : stats.wireframe_unavailable_reason.c_str());
                    } else {
                        std::printf("wireframe: %s\n",
                                    stats.wireframe ? "on" : "off");
                    }
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
                } else if (line.compare(0, 10, "wait_idle ") == 0 ||
                           line == "wait_idle") {
                    // wait_idle <seconds>: blocks until resident_sectors has
                    // held steady for <seconds> of wall-clock time AND the
                    // bake is ready. Released in the frame_stats-driven check
                    // below (mirrors MATTER_CAM_PATH_SETTLE's plateau logic).
                    std::istringstream input(line);
                    std::string verb, seconds_text, extra;
                    input >> verb >> seconds_text;
                    double seconds = 0.0;
                    bool parse_ok = !seconds_text.empty();
                    if (parse_ok) {
                        try {
                            size_t consumed = 0;
                            seconds = std::stod(seconds_text, &consumed);
                            parse_ok = consumed == seconds_text.size();
                        } catch (...) {
                            parse_ok = false;
                        }
                    }
                    if (input >> extra) parse_ok = false;
                    if (!parse_ok) {
                        std::printf("wait_idle: expected `wait_idle <seconds>`\n");
                    } else if (seconds <= 0.0) {
                        std::printf("wait_idle: <seconds> must be > 0\n");
                    } else {
                        fifo_wait_idle_seconds = seconds;
                        fifo_wait_idle_start = std::chrono::steady_clock::now();
                        fifo_wait_idle_last_change = fifo_wait_idle_start;
                        fifo_wait_idle_last_resident = UINT32_MAX;  // force resync below
                        fifo_block = FifoBlockKind::WaitIdle;
                    }
                } else if (line.compare(0, 11, "wait_event ") == 0 ||
                           line == "wait_event") {
                    // wait_event <name> [timeout_seconds]: blocks until the
                    // named engine event fires (or the optional timeout
                    // expires -- the script then continues, it does not
                    // quit). Released in the check below.
                    std::istringstream input(line);
                    std::string verb, name, timeout_text, extra;
                    input >> verb >> name >> timeout_text;
                    bool has_timeout = !timeout_text.empty();
                    double timeout_s = 0.0;
                    bool parse_ok = true;
                    if (has_timeout) {
                        try {
                            size_t consumed = 0;
                            timeout_s = std::stod(timeout_text, &consumed);
                            parse_ok = consumed == timeout_text.size() && timeout_s > 0.0;
                        } catch (...) {
                            parse_ok = false;
                        }
                    }
                    if (input >> extra) parse_ok = false;
                    if (name.empty() || !parse_ok) {
                        std::printf(
                            "wait_event: expected `wait_event <name> "
                            "[timeout_seconds]`\n");
                    } else if (!fifo_begin_wait_event(name, has_timeout ? timeout_s : 0.0)) {
                        std::printf("wait_event: unknown event '%s'\n", name.c_str());
                    } else {
                        fifo_block = FifoBlockKind::WaitEvent;
                    }
                } else if (std::sscanf(line.c_str(), "world %255s", word) == 1) {
                    // Same case-insensitive resolution as MATTER_WORLD, and
                    // the same intent-recording path `reload` uses: the
                    // actual session destroy/recreate runs at the post-frame
                    // seam (SessionBinding::replace), never mid-parse. Not
                    // itself a blocking wait -- pair it with wait_idle for a
                    // multi-world sweep.
                    std::string wanted(word);
                    std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                                   [](unsigned char ch) { return std::tolower(ch); });
                    int world_index = -1;
                    for (size_t i = 0; i < worlds.size(); ++i) {
                        std::string candidate = worlds[i].world_name;
                        std::transform(candidate.begin(), candidate.end(),
                                       candidate.begin(),
                                       [](unsigned char ch) { return std::tolower(ch); });
                        if (candidate == wanted) {
                            world_index = static_cast<int>(i);
                            break;
                        }
                    }
                    if (world_index < 0) {
                        std::printf("world: unknown '%s'\n", word);
                    } else {
                        viewer::ViewerSwitchWorld cmd;
                        cmd.index = world_index;
                        registry.dispatch(cmd);
                    }
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

        // The "Orbit selection" pivot (issue a4203d22 part 1). Computed ONCE
        // per frame, here rather than at either use site, because two
        // consumers need the same answer — the Camera panel's orbit buttons
        // (inside the UI block below) and the viewport drag/wheel orbit (after
        // it, alongside the pick, so it inherits the same gizmo/ImGui
        // priority) — and because resolving a BakedRoot's bounds walks the
        // part's geometry clusters, which is not something to do twice.
        //
        // selection_focus_point is the same merged AABB the F-key focus
        // frames on, so the orbit pivot and Focus can never disagree.
        matter::Float3 selection_pivot{};
        bool selection_pivot_valid = false;
        if (session) {
            float pivot_radius = 0.0f;
            selection_pivot_valid = viewer::selection_focus_point(
                selection_set, field_commands,
                [&](uint64_t part_hash, viewer::SelectionBounds& out) {
                    viewer::SelectedObject obj{viewer::SelectedObject::BakedRoot,
                                               part_hash};
                    return viewer::bounds_for_object(obj, *session, out);
                },
                selection_pivot, pivot_radius);
        }

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
                ui.draw_console_panel(console_log, editor_props);
                ui.draw_debug_panel(stats, viewer_commands, editor_props);
                ui.draw_tunables_panel(editor_props);
                ui.draw_lighting_panel(editor_props);
                if (capture_lighting_ui && !screenshot_path.empty())
                    ImGui::SetWindowFocus("Lighting");
                ui.draw_vt_warning_banner(stats);
                ui.draw_bake_lab_panel(bake_lab, &app_hub, session.get(), worlds, stats);
                ui.draw_asset_browser_panel(asset_browser, worlds, stats, shared_lib,
                                           viewer_commands);
                ui.draw_camera_panel(camera, camera_prefs, editor_props,
                                     selection_pivot_valid, selection_pivot);
                ui.draw_performance_panel(session.get(), editor_props,
                                          viewer_commands, camera);
                ui.draw_profiler_panel(stats);
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
                            fill_shot_history(shot);
                            shot.instance_cache_expansions =
                                fs.vk_instance_cache_expansions;
                            shot.command_layout_rebuilds =
                                fs.vk_command_layout_rebuilds;
                            shot.immediate_submits = fs.vk_immediate_submits;
                            shot.resident_sectors = fs.resident_sectors;
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
                    context.props = &editor_props.registry();
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

        // MATTER_CAM_PATH: one pose per rendered frame. This sits immediately
        // before the frame_camera snapshot below so the scripted pose is what
        // streaming, the tick, and the scene render all see, and after the UI
        // so nothing can overwrite it later in the frame.
        if (!cam_path.empty() && !cam_path_finished) {
            if (!cam_path_running) {
                // Start only once the world is genuinely drawing. Same signal
                // the perf harness and the screenshot path already gate on.
                if (bake_ready && session->frame_stats().instances_drawn > 0) {
                    // MATTER_CAM_PATH_SETTLE=<n>: hold until the streamed world
                    // stops growing, instead of (or as well as) counting frames.
                    //
                    // WHY THIS EXISTS. `cam_path_warmup` counts FRAMES, but the
                    // sector fill is bake-rate-limited in WALL TIME. So any
                    // change that alters frame rate silently changes how much
                    // world exists when the path starts -- and a smaller world
                    // trivially shows fewer seams, fewer weld pairs and fewer
                    // level violations. Measured on StreamCaverns: two builds at
                    // the same warmup=4000 settled to 590 vs 431 sectors purely
                    // because the second ran at 193 fps against 50 and so gave
                    // the baker half the wall time. Comparing their seam numbers
                    // was meaningless until this was controlled for.
                    //
                    // Plateau in WALL TIME: residency unchanged for N seconds
                    // means the fill converged. Measured in seconds precisely
                    // because a frame count reintroduces the dependence this
                    // removes -- see the note where cam_path_settle_s is read.
                    // The frame warmup still applies first, so default
                    // behaviour is unchanged when this is unset.
                    const uint32_t resident = session->frame_stats().resident_sectors;
                    const auto settle_now = std::chrono::steady_clock::now();
                    if (resident != cam_path_settle_last) {
                        cam_path_settle_last = resident;
                        cam_path_settle_since = settle_now;
                    }
                    const double settled_for =
                        std::chrono::duration<double>(settle_now -
                                                      cam_path_settle_since).count();
                    if (cam_path_warmup > 0) {
                        --cam_path_warmup;
                    } else if (cam_path_settle_s > 0.0 &&
                               (resident == 0 || settled_for < cam_path_settle_s)) {
                        // keep holding the first pose
                    } else {
                        cam_path_running = true;
                        viewer::lod_trace::set_capture_enabled(true);
                        std::printf("MATTER_CAM_PATH: starting (%zu poses, "
                                    "%u resident sectors at settle)\n",
                                    cam_path.size(), resident);
                    }
                }
                // Hold the first pose through the warmup so the trace's opening
                // census describes the pose the path starts from.
                camera.position = {cam_path[0].eye[0], cam_path[0].eye[1],
                                   cam_path[0].eye[2]};
                camera.target = {cam_path[0].target[0], cam_path[0].target[1],
                                 cam_path[0].target[2]};
            }
            if (cam_path_running && cam_path_index < cam_path.size()) {
                const CamPathPose& pose = cam_path[cam_path_index];
                camera.position = {pose.eye[0], pose.eye[1], pose.eye[2]};
                camera.target = {pose.target[0], pose.target[1], pose.target[2]};
                // Stamp the trace with the POSE index, not the frame serial:
                // one pose per rendered frame makes this a run-independent
                // clock, while how many frames a world took to become drawable
                // is not (37 vs 36 between two warm runs, measured).
                viewer::lod_trace::set_frame_label(cam_path_index);
                ++cam_path_index;
                if (cam_path_index == cam_path.size()) cam_path_drain = 8;
            } else if (cam_path_running) {
                if (--cam_path_drain <= 0) {
                    cam_path_finished = true;
                    viewer::lod_trace::set_capture_enabled(false);
                    viewer::lod_trace::close();
                    std::printf("MATTER_CAM_PATH: complete\n");
                    // The soak's verdict, at the seam that ends the soak.
                    // Unconditional: a run in which nothing ever changed emits
                    // no [seam] rows at all, and silence must not read as a
                    // pass.
                    print_seam_summary("MATTER_CAM_PATH complete");
                    if (cam_path_exit) quit_requested = true;
                }
            }
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
                if (camera_capture) {
                    // Free-fly: the cursor is GLFW_CURSOR_DISABLED, so
                    // glfwGetCursorPos reports an unbounded VIRTUAL position
                    // that has nothing to do with anything on screen. It used
                    // to land near the window centre only because
                    // CameraController::update warped it there every frame;
                    // removing that warp (issue a4203d22 part 3) would
                    // otherwise turn click-to-select while flying into a pick
                    // at a random, ever-drifting coordinate. Pin it to the
                    // window centre explicitly, which is what the warp
                    // effectively produced — this keeps free-fly clicking
                    // behaving exactly as before.
                    int win_w = 0, win_h = 0;
                    glfwGetWindowSize(window, &win_w, &win_h);
                    cursor_x = win_w * 0.5;
                    cursor_y = win_h * 0.5;
                }
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

            // Viewport drag/wheel orbit about the selection (issue a4203d22
            // part 1: "then mouse orbits and zooms"). Deliberately in the same
            // block as the pick, AFTER the gizmo has been submitted, so it
            // inherits ui.camera_input_allowed() unchanged — an orbit that
            // fights ImGuizmo or steals a drag from a panel is a regression,
            // and camera_input_allowed is the one place that arbitrates it.
            //
            // Additional gates beyond that: free-fly owns the mouse when the
            // cursor is captured; the issue reporter's region drag reads the
            // mouse straight off the IO outside any ImGui window; and the
            // cursor has to be inside the viewport rect (camera_input_allowed
            // only knows that ImGui does not want the mouse, not where it is).
            // IsMouseDragging's threshold is what keeps a click-to-select from
            // also nudging the camera.
            if (ui_frame_ready && camera_prefs.orbit_selection &&
                selection_pivot_valid && !camera_capture &&
                !viewer::issue_reporter_wants_mouse(issue_state) &&
                ui.camera_input_allowed()) {
                const ImGuiIO& io = ImGui::GetIO();
                const auto& vp = ui.viewport_rect();
                const bool over_viewport =
                    io.MousePos.x >= vp.x && io.MousePos.x < vp.x + vp.w &&
                    io.MousePos.y >= vp.y && io.MousePos.y < vp.y + vp.h;
                if (over_viewport) {
                    const bool dragging = ImGui::IsMouseDragging(
                        ImGuiMouseButton_Left, /*lock_threshold=*/4.0f);
                    viewer::orbit_camera_by_mouse(
                        camera, selection_pivot,
                        dragging ? io.MouseDelta.x : 0.0f,
                        dragging ? io.MouseDelta.y : 0.0f, io.MouseWheel,
                        camera_prefs.look_sensitivity,
                        camera_prefs.orbit_zoom_step);
                }
            }
        }

        selection_set.validate([&](const viewer::SelectedObject& obj) {
            if (obj.kind == viewer::SelectedObject::BakedRoot) {
                matter::InstanceInfo info;
                return session->instance_info_by_hash(obj.id, info);
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
        ui.update_sector_streaming(*session, frame_camera,
                                   !stats.freeze_stream_anchor);
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
        // Streaming backlog: sector publishes are ~1 ms each, so the fixed
        // 4 ms budget drained a 5,000-sector world fill at only a handful
        // per frame — minutes of "terrain still popping in". Widen the
        // budget while jobs are queued; quiet frames keep the old cost.
        session->pump_gpu_jobs(session->gpu_jobs_idle() ? 4.0f : 14.0f);
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
                // Layer 2 for render.fog, adopted BEFORE on_world_connected
                // snapshots the baseline below — that ordering is what makes
                // "Reset to World" restore what the world script authored
                // rather than a compiled default.
                if (bake_ready && apply_world_fog_after_bake) {
                    matter::FogSettings world_fog;
                    if (session->world_fog(world_fog)) {
                        stats.fog = world_fog;
                        fog_override_ready = true;
                        apply_world_fog_after_bake = false;
                    }
                }
                if (bake_ready && apply_world_volumetrics_after_bake) {
                    matter::VulkanVolumetricsSettings world_vol;
                    if (session->world_volumetrics(world_vol)) {
                        stats.volumetrics = world_vol;
                        apply_world_volumetrics_after_bake = false;
                    }
                }
                if (bake_ready && apply_world_atmosphere_after_bake) {
                    matter::AtmosphereSettings world_atmosphere;
                    if (session->world_atmosphere(world_atmosphere)) {
                        stats.atmosphere = world_atmosphere;
                        apply_world_atmosphere_after_bake = false;
                    }
                }
                if (bake_ready && apply_world_cloud_shadows_after_bake) {
                    matter::CloudShadowSettings world_cloud_shadows;
                    if (session->world_cloud_shadows(world_cloud_shadows)) {
                        stats.cloud_shadows = world_cloud_shadows;
                        apply_world_cloud_shadows_after_bake = false;
                    }
                }
                // Layer 2 for the sun half of render.lighting. Same ordering
                // rule as fog: strictly before on_world_connected below.
                //
                // The engine derived these angles from the same WorldSettings
                // it built the manifest from, so seeding them here leaves the
                // override EQUAL to the authored sun -- and the engine's
                // equality test then passes the authored Float3 through
                // without a round trip through trig. Dragging either slider
                // breaks the equality and the conversion takes over.
                if (bake_ready && apply_world_sun_after_bake) {
                    matter::SunAngles world_sun;
                    if (session->world_sun(world_sun)) {
                        stats.lighting.sun_azimuth_deg = world_sun.azimuth_deg;
                        stats.lighting.sun_elevation_deg =
                            world_sun.elevation_deg;
                        stats.lighting.sun_angular_diameter_deg =
                            world_sun.angular_diameter_deg;
                        sun_override_ready = true;
                        apply_world_sun_after_bake = false;
                    }
                }
                // Property connect seam (S4): layer 2 has now landed in the
                // bound structs, so snapshot it as the baseline and only then
                // apply the world override file and the env layer. Must stay
                // AFTER the authored-value adopters above.
                if (bake_ready && apply_world_props_after_bake) {
                    // The world's `static props` group is built by the connect
                    // and owned by the session; EditorProps binds it here and
                    // releases it again at the next set_world.
                    editor_props.on_world_connected(
                        session->world_props(), session->draw_overrides());
                    apply_world_props_after_bake = false;
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
                if (fifo_path) {
                    std::printf("viewer: bake ready\n");
                    std::fflush(stdout);
                }
            } else if (event.type == matter::EventType::BakeError) {
                std::printf("bake error [%s]: %s\n", event.module.c_str(),
                            event.message.c_str());
                console_log.push(viewer::LogSeverity::Error,
                                  "[" + event.module + "] " + event.message);
            }
        }
        matter::RenderOptions options;
        const bool native_rt_requested =
            fifo_render_path_override
                ? fifo_render_path == matter::RenderPath::Raytrace
                : editor_props.gpu_prefs().ray_tracing;
        const bool native_rt_enabled =
            vulkan->ray_tracing_available() && native_rt_requested;
        options.path = native_rt_enabled ? matter::RenderPath::Raytrace
                                         : matter::RenderPath::GpuDriven;
        stats.session_status.render_path =
            native_rt_enabled ? viewer::ViewerRenderPathStatus::NativeRt
                              : viewer::ViewerRenderPathStatus::Raster;
        // Geometry-stage views. Unlike 1-4 these are not composite modes, so
        // the remap below leaves composite_debug_view at 0 for them.
        //
        // Wireframe has TWO inputs on purpose (see resolve_wireframe_request):
        // combo index 6 is the persistable single int, and the checkbox is the
        // form that composes with index 5's LOD tint. Capability is enforced
        // here, once, so no caller can ask for lines the device cannot draw.
        options.wireframe = viewer::resolve_wireframe_request(
            stats.wireframe, stats.debug_view_mode, /*wireframe_view_index=*/6,
            stats.wireframe_available);
        options.impostor_parallax = stats.impostor_parallax;
        static const bool force_lod_tint =
            std::getenv("MATTER_FORCE_LOD_TINT") != nullptr;
        options.geometry_debug_view =
            (stats.debug_view_mode == 5 || force_lod_tint)
                ? matter::GeometryDebugView::LodTint
                : matter::GeometryDebugView::None;
        options.occlusion_draw_cull = stats.occlusion_draw_cull;
        // Frozen cull camera (M4). The renderer takes its own snapshot of the
        // planes and the eye on the rising edge; this side captures the POSE at
        // the same moment, purely so the viewport can outline that frustum.
        // Two snapshots of the same instant rather than one shared one, because
        // the renderer's is of values in its own space (planes, already
        // jittered) and reconstructing a drawable frustum from those would be
        // more machinery than redrawing the pinhole.
        if (stats.freeze_cull_camera && !stats.frozen_cull_camera_valid) {
            stats.frozen_cull_camera = frame_camera;
            stats.frozen_cull_camera_valid = true;
        } else if (!stats.freeze_cull_camera) {
            stats.frozen_cull_camera_valid = false;
        }
        options.freeze_cull_camera = stats.freeze_cull_camera;
        options.pixel_budget = stats.pixel_budget;
        options.min_projected_size = stats.min_projected_size;
        options.dlss_mode = selected_dlss_mode();
        options.vulkan_lighting = stats.lighting;
        // ViewerStats index -> composite.frag mode. The two numberings differ
        // and always have; keep this in step with kDebugViewLabels
        // (editor_props.cpp), which documents why the indices are append-only.
        // 1 -> 2.0 normals, 2 -> 3.0 packed linear depth, 3 -> 1.0 RT sun
        // visibility, 4 -> 4.0 raw GBuffer albedo (the horizon diagnostic).
        // 5 (LOD levels) and 6 (Wireframe) are geometry views and deliberately
        // fall through to 0.0 here -- both are already in the G-buffer albedo
        // this composites.
        options.vulkan_lighting.composite_debug_view =
            stats.debug_view_mode == 1   ? 2.0f
            : stats.debug_view_mode == 2 ? 3.0f
            : stats.debug_view_mode == 3 ? 1.0f
            : stats.debug_view_mode == 4 ? 4.0f
                                         : 0.0f;
        options.atmosphere = stats.atmosphere;
        options.volumetrics = stats.volumetrics;
        options.volumetrics.vol_debug_view =
            static_cast<float>(stats.vol_debug_view);
        options.cloud_shadows = stats.cloud_shadows;
        stats.requested_froxel = matter::resolve_froxel_grid(options.volumetrics);
        // The current allocator accepts this exact grid; retain a separate
        // field so a future fallback can report its effective dimensions
        // without changing the requested setting or the UI contract.
        stats.effective_froxel = stats.requested_froxel;
        stats.froxel_bytes = matter::estimate_froxel_bytes(
            stats.effective_froxel,
            matter::enhanced_cloud_lighting(options.volumetrics,
                                            options.cloud_shadows));
        stats.cloud_shadow_bytes =
            matter::estimate_cloud_shadow_bytes(options.cloud_shadows);
        // render.fog. Only once stats.fog actually holds this world's authored
        // fog (see fog_override_ready); until then — and for the whole of a
        // replay — the session keeps consuming its own authored_fog_. Cleared
        // again below for the Workbench's isolation session, which has its own
        // world and its own authored fog.
        options.use_fog_override = fog_override_ready;
        options.fog_override = stats.fog;
        // Sun aim/size. Only once the angles actually describe THIS world --
        // see sun_override_ready. The shadow-ray count rides along because it
        // is the knob that makes an enlarged sun visible in shadows at all
        // (rt_shadow.rgen collapses the cone to a hard ray at 1 sample).
        options.use_sun_override = sun_override_ready;
        options.vulkan_ray_tracing.samples =
            static_cast<uint32_t>(stats.lighting.sun_shadow_samples);
        options.vulkan_tileset_pom = stats.tileset_pom;
        options.vulkan_vt_near_band = stats.vt_near_band;
        // render.gpu.ray_tracing. The device capability is the hard gate and
        // the property is the preference; the renderer only ever sees the
        // conjunction, so ticking the box on a GPU without RT extensions
        // cannot ask for something impossible. Per-frame, which is what makes
        // the checkbox live — see the group definition in editor_props.cpp for
        // the trace proving the renderer tolerates the flip.
        options.vulkan_ray_tracing.enabled = native_rt_enabled;
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
        if (show_isolation) {
            bake_lab.workbench().apply_lod_inspector_options(options);
            // The production HUD's render.fog override belongs to the
            // production world; the isolation session keeps its own authored
            // fog, same reasoning as force_lod above but in the other
            // direction.
            options.use_fog_override = false;
            // Identical reasoning for the sun: the angles in stats.lighting
            // were seeded from the PRODUCTION world, and the isolation session
            // has its own world with its own authored sun_direction.
            options.use_sun_override = false;
        }
        // Bake Lab/Workbench per-frame work plus the session event drain.
        phase.lab = phase_split();
        if (!show_isolation)
            viewer::submit_selection_overlay_lines(selection_set, *session);
        else
            render_session->submit_overlay_lines(nullptr, 0);
        if (!render_session->render(render_camera, render_frame, options, error)) {
            std::fprintf(stderr, "FATAL: render: %s\n", error.c_str());
            fatal_error = true;
        } else if (!show_isolation) {
            camera_input_order.render_scene();
            matter::ResolvedAtmospherePresentationStatus committed{};
            if (session->resolved_atmosphere_status(committed)) {
                stats.atmosphere_status.generation_serial =
                    committed.generation_serial;
                stats.atmosphere_status.resolved_elevation_deg =
                    committed.resolved_elevation_deg;
                stats.atmosphere_status.atmospheric_direct_base_rgb =
                    committed.atmospheric_direct_base_rgb;
                stats.atmosphere_status.atmospheric_noon_direct_base_rgb =
                    committed.atmospheric_noon_direct_base_rgb;
                stats.atmosphere_status.direct_world_ratio =
                    committed.direct_world_ratio;
                stats.atmosphere_status.direct_base_rgb =
                    committed.direct_base_rgb;
                stats.atmosphere_status.direct_world_sun_rgb =
                    committed.direct_world_sun_rgb;
                stats.atmosphere_status.sky_ambient_ratio =
                    committed.sky_ambient_ratio;
                stats.atmosphere_status.sky_display_modifier_rgb =
                    committed.sky_display_modifier_rgb;
                stats.atmosphere_status.sky_irradiance_modifier_rgb =
                    committed.sky_irradiance_modifier_rgb;
            }
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
                // The frozen cull frustum (M4). Same guard and same rect: it
                // is drawn in the production session's world space.
                if (stats.freeze_cull_camera &&
                    stats.frozen_cull_camera_valid) {
                    viewer::draw_frozen_cull_frustum(
                        stats.frozen_cull_camera, frame_camera,
                        static_cast<int>(render_frame.extent.width),
                        static_cast<int>(render_frame.extent.height),
                        // Truncation depth: far enough to enclose the near
                        // bands of a streamed world, near enough that the shape
                        // still reads as a frustum rather than as two parallel
                        // lines running off the screen.
                        1000.0f, sel_vp.x, sel_vp.y);
                }
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
        // QA timeline: wait_idle / wait_event release checks. Here (frame_stats
        // just refreshed, and bake_ready reflects this frame's poll_event
        // drain above) so both waits observe up-to-date state once per frame;
        // wait_frames' own release is symmetric, after present, below.
        if (fifo_block == FifoBlockKind::WaitIdle) {
            const uint32_t resident = frame_stats.resident_sectors;
            const auto now = std::chrono::steady_clock::now();
            if (resident != fifo_wait_idle_last_resident) {
                fifo_wait_idle_last_resident = resident;
                fifo_wait_idle_last_change = now;
            }
            const double settled_for =
                std::chrono::duration<double>(now - fifo_wait_idle_last_change).count();
            if (bake_ready && settled_for >= fifo_wait_idle_seconds) {
                const double elapsed =
                    std::chrono::duration<double>(now - fifo_wait_idle_start).count();
                std::printf("idle: settled after %.1fs\n", elapsed);
                fifo_block = FifoBlockKind::None;
            }
        } else if (fifo_block == FifoBlockKind::WaitEvent) {
            if (fifo_wait_event_fired.load(std::memory_order_acquire)) {
                std::printf("event: %s\n", fifo_wait_event_name.c_str());
                fifo_wait_event_sub.reset();
                fifo_block = FifoBlockKind::None;
            } else if (fifo_wait_event_session_scoped &&
                       (binding.current_session_id() != fifo_wait_event_session_id ||
                        binding.current_generation() != fifo_wait_event_generation)) {
                // The session (and its hub) this subscription targeted is
                // gone -- study SessionBinding's epoch sequence: quiesce_bridge
                // only clears ITS OWN subs, not ours, and the old Hub is
                // destroyed with the old session, so the event we're waiting
                // for can never arrive. reset() is always safe (Subscription
                // holds no pointer back into Hub) even though the hub is dead.
                std::printf("event: %s aborted (session changed)\n",
                            fifo_wait_event_name.c_str());
                fifo_wait_event_sub.reset();
                fifo_block = FifoBlockKind::None;
            } else if (fifo_wait_event_timeout_s > 0.0 &&
                       std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                      fifo_wait_event_start)
                               .count() >= fifo_wait_event_timeout_s) {
                std::printf("event: %s timeout after %.1fs\n",
                            fifo_wait_event_name.c_str(), fifo_wait_event_timeout_s);
                fifo_wait_event_sub.reset();
                fifo_block = FifoBlockKind::None;
            }
        }
        // MATTER_SEAM_TRACE (declared above): app-thread poll of the seam
        // welder, change-only, stamped with the cam-path pose clock.
        if (seam_trace && bake_ready) {
            const matter::WorldSession::SeamWeldStatus s =
                session->seam_weld_status();
            const uint64_t label = viewer::lod_trace::frame_label(frame.serial);
            SeamTraceKey key;
            key.pairs = s.pairs;
            key.drawn_pairs = s.drawn_pairs;
            key.registered_parts = s.registered_parts;
            key.missing_landing = s.missing_landing;
            key.sign_conflicts = s.sign_conflicts;
            key.degenerate = s.degenerate;
            key.fine_side_incomplete = s.fine_side_incomplete;
            key.coarse_side_nulls = s.coarse_side_nulls;
            key.level_gap_pairs = s.level_gap_pairs;
            key.build_errors = s.build_errors;
            key.level_holds = s.level_holds;
            key.merge_coverage_holds = s.merge_coverage_holds;
            key.drawn_level_violations = s.drawn_level_violations;
            key.drawn_without_record = s.drawn_without_record;
            key.register_failures = s.register_failures;
            key.hash_collisions = s.hash_collisions;
            key.id_collisions = s.id_collisions;
            if (key != seam_last) {
                seam_last = key;
                std::printf(
                    "[seam] #f%llu eye=(%.0f,%.0f,%.0f) sect=%u pairs=%d "
                    "drawn=%d parts=%d tri=%llu dtri=%llu | cross=%d q=%d t=%d "
                    "miss=%d sign=%d degen=%d fine_inc=%d coarse_null=%d | "
                    "gap=%llu err=%llu holds=%llu mholds=%llu viol=%llu "
                    "norec=%llu "
                    "reg=%llu rel=%llu noop=%llu regfail=%llu hcol=%llu "
                    "icol=%llu peak=%d\n",
                    (unsigned long long)label, frame_camera.position.x,
                    frame_camera.position.y, frame_camera.position.z,
                    frame_stats.resident_sectors, s.pairs, s.drawn_pairs,
                    s.registered_parts, (unsigned long long)s.triangles,
                    (unsigned long long)s.drawn_triangles, s.crossings, s.quads,
                    s.tris, s.missing_landing, s.sign_conflicts, s.degenerate,
                    s.fine_side_incomplete, s.coarse_side_nulls,
                    (unsigned long long)s.level_gap_pairs,
                    (unsigned long long)s.build_errors,
                    (unsigned long long)s.level_holds,
                    (unsigned long long)s.merge_coverage_holds,
                    (unsigned long long)s.drawn_level_violations,
                    (unsigned long long)s.drawn_without_record,
                    (unsigned long long)s.parts_registered,
                    (unsigned long long)s.parts_released,
                    (unsigned long long)s.noop_rebuilds,
                    (unsigned long long)s.register_failures,
                    (unsigned long long)s.hash_collisions,
                    (unsigned long long)s.id_collisions, s.pairs_peak);
            }
            auto hi = [](auto& slot, auto value) { if (value > slot) slot = value; };
            hi(seam_run.peak_pairs, s.pairs);
            hi(seam_run.peak_drawn_pairs, s.drawn_pairs);
            hi(seam_run.peak_registered_parts, s.registered_parts);
            hi(seam_run.peak_missing_landing, s.missing_landing);
            hi(seam_run.peak_sign_conflicts, s.sign_conflicts);
            hi(seam_run.peak_degenerate, s.degenerate);
            hi(seam_run.peak_fine_side_incomplete, s.fine_side_incomplete);
            hi(seam_run.peak_coarse_side_nulls, s.coarse_side_nulls);
            hi(seam_run.peak_triangles, s.triangles);
            hi(seam_run.peak_drawn_triangles, s.drawn_triangles);
            if (s.crossings !=
                s.quads + s.tris + s.missing_landing + s.degenerate) {
                if (seam_run.first_identity_break < 0)
                    seam_run.first_identity_break = (int64_t)label;
                ++seam_run.identity_breaks;
            }
            if (s.drawn_pairs > s.pairs) ++seam_run.drawn_exceeds_pairs;
            if (seam_run.first_sign_conflict < 0 && s.sign_conflicts != 0)
                seam_run.first_sign_conflict = (int64_t)label;
            if (seam_run.first_level_gap < 0 && s.level_gap_pairs != 0)
                seam_run.first_level_gap = (int64_t)label;
            if (seam_run.first_build_error < 0 && s.build_errors != 0)
                seam_run.first_build_error = (int64_t)label;
            if (seam_run.first_id_collision < 0 && s.id_collisions != 0)
                seam_run.first_id_collision = (int64_t)label;
            if (seam_run.first_level_violation < 0 &&
                s.drawn_level_violations != 0)
                seam_run.first_level_violation = (int64_t)label;
            seam_run.last = s;
            ++seam_run.samples;
        }
        dlss_modes_supported = vulkan->dlss_available() &&
                               frame_stats.dlss_reason.empty();
        if (!frame_stats.dlss_reason.empty())
            last_dlss_reason = frame_stats.dlss_reason;
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
        stats.gpu_occlusion_culled = static_cast<int>(frame_stats.occlusion_culled);
        stats.resident_sectors = frame_stats.resident_sectors;
        stats.culled_clusters = stats.gpu_culled;
        stats.raster_tris = static_cast<int>(frame_stats.triangles);
        stats.raster_batches = static_cast<int>(frame_stats.draw_batches);
        stats.resident_impostors =
            static_cast<int>(frame_stats.resident_impostors);
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
        stats.gpu_rt_gi_ms           = frame_stats.gpu_rt_gi_ms;
        stats.gpu_denoise_ms         = frame_stats.gpu_denoise_ms;
        stats.gpu_dlss_ms            = frame_stats.gpu_dlss_ms;
        stats.gpu_composite_ms       = frame_stats.gpu_composite_ms;
        stats.gpu_vol_ms             = frame_stats.gpu_vol_ms;
        stats.gpu_atmosphere_ms      = frame_stats.gpu_atmosphere_ms;
        stats.gpu_cloud_shadows_ms   = frame_stats.gpu_cloud_shadows_ms;
        stats.gpu_vol_density_ms     = frame_stats.gpu_vol_density_ms;
        stats.gpu_vol_scatter_ms     = frame_stats.gpu_vol_scatter_ms;
        stats.gpu_vol_integrate_ms   = frame_stats.gpu_vol_integrate_ms;
        stats.effective_froxel = {frame_stats.vol_grid_w, frame_stats.vol_grid_h,
                                  frame_stats.vol_grid_d};
        stats.froxel_bytes = frame_stats.vol_memory_bytes;
        stats.cloud_shadow_bytes = frame_stats.cloud_shadow_memory_bytes;
        stats.last_volumetric_allocation_error = frame_stats.vol_allocation_error;
        if (frame_stats.vol_allocation_rejected &&
            frame_stats.vol_resource_generation !=
                last_rejected_froxel_generation) {
            last_rejected_froxel_generation = frame_stats.vol_resource_generation;
            if (matter::props::Binding* b = editor_props.volumetrics()) {
                const matter::props::Desc* xy = nullptr;
                const matter::props::Desc* depth = nullptr;
                for (uint32_t i = 0; i < b->schema().field_count; ++i) {
                    const matter::props::Desc& field = b->schema().fields[i];
                    if (std::strcmp(field.name, "froxel_xy_scale") == 0) xy = &field;
                    if (std::strcmp(field.name, "froxel_depth_slices") == 0) depth = &field;
                }
                if (xy && depth) {
                    matter::props::set_enum(b->instance(), *xy,
                        static_cast<int32_t>(frame_stats.vol_effective_xy_scale));
                    matter::props::set_enum(b->instance(), *depth,
                        static_cast<int32_t>(frame_stats.vol_effective_depth_slices));
                    b->set_dirty(false);
                }
            }
            std::fprintf(stderr,
                         "volumetric froxel allocation rejected: requested %ux%ux%u "
                         "%.2f MiB; effective %ux%ux%u; %s\n",
                         stats.requested_froxel.width, stats.requested_froxel.height,
                         stats.requested_froxel.depth,
                         static_cast<double>(matter::estimate_froxel_bytes(
                             stats.requested_froxel, false)) / (1024.0 * 1024.0),
                         stats.effective_froxel.width, stats.effective_froxel.height,
                         stats.effective_froxel.depth,
                         frame_stats.vol_allocation_error.c_str());
        }
        stats.vt_active              = frame_stats.vt_active;
        stats.vt_variants            = frame_stats.vt_variants;
        stats.vt_max_variants        = frame_stats.vt_max_variants;
        stats.vt_rejected_variants   = frame_stats.vt_rejected_variants;
        stats.vt_shared_refs_total    = frame_stats.vt_shared_refs_total;
        stats.vt_finer_rebuilds_total = frame_stats.vt_finer_rebuilds_total;
        stats.vt_pool_used           = frame_stats.vt_pool_used;
        stats.vt_pool_capacity       = frame_stats.vt_pool_capacity;
        stats.vt_pool_pinned         = frame_stats.vt_pool_pinned;
        stats.vt_mesh_bytes          = frame_stats.vt_mesh_bytes;
        stats.vt_mesh_budget_bytes   = frame_stats.vt_mesh_budget_bytes;
        stats.vt_pool_bytes          = frame_stats.vt_pool_bytes;
        stats.vt_indirection_bytes   = frame_stats.vt_indirection_bytes;
        stats.vt_indirection_capacity_bytes =
            frame_stats.vt_indirection_capacity_bytes;
        stats.impostor_atlas_bytes   = frame_stats.impostor_atlas_bytes;
        {
            const auto gpu = matter::gpu_memory_stats();
            stats.gpu_device_local_bytes = gpu.device_local_bytes;
            stats.gpu_host_visible_bytes = gpu.host_visible_bytes;
            stats.gpu_total_alloc_bytes  = gpu.total_bytes;
            stats.gpu_allocation_count   = gpu.allocation_count;
        }
        {
            const auto proc = matter::process_memory_stats();
            stats.process_working_set_bytes = proc.working_set_bytes;
            stats.process_peak_working_set_bytes = proc.peak_working_set_bytes;
        }

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
        bool fifo_immediate_capture = false;
        std::string capture_path;
        if (!screenshot_path.empty() && bake_ready && frame_stats.instances_drawn > 0 &&
            ++screenshot_settle >= settle_frames) {
            capture = true; capture_path = screenshot_path;
        } else if (shot_settle > 0 && frame_stats.instances_drawn > 0 &&
                   --shot_settle == 0) {
            capture = true; capture_path = shot_path;
        } else if (!fifo_present.pending_screenshot_path().empty()) {
            capture = true;
            fifo_immediate_capture = true;
            capture_path = fifo_present.pending_screenshot_path();
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
            else if (!fifo_immediate_capture) shot_settle = 2;
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
        const viewer::FifoPresentUpdate fifo_present_update =
            fifo_present.advance(frame_completed && frame_presented &&
                                     !fatal_error,
                                 !fifo_immediate_capture || capture);
        stats.session_status.presented_frame_serial =
            fifo_present.presented_frame_serial();
        for (const viewer::FifoCompletedWait& completed :
             fifo_present_update.completed_waits) {
            std::printf("wait_frames: complete %u frame_serial=%llu\n",
                        completed.count,
                        static_cast<unsigned long long>(
                            completed.frame_serial));
            // QA timeline: release consumption gated by this wait_frames.
            // Under the gated drain above at most one wait is ever in flight
            // here, so any completion observed while blocked is this one.
            if (fifo_block == FifoBlockKind::WaitFrames)
                fifo_block = FifoBlockKind::None;
        }
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

            // ---- ISSUE-REPORT FRAME HISTORY ----------------------------
            //
            // Pushed EVERY frame so a shot can carry the seconds leading up
            // to it. The reporter's deep state is read when the report is
            // SUBMITTED -- seconds after any hitch -- and three hitch reports
            // in a row recorded 29-31 ms while the user was describing 500 ms
            // stalls. A capture-time ring is the difference between an
            // instrument that can see the defect and one that cannot.
            //
            // HERE, not beside stats.frame_ms: `phase` is where the loop's
            // real per-frame render time lives, and it is not computed until
            // this point. Sampling the EMA earlier would have recorded a
            // smoothed number, which is exactly the averaging that hid the
            // spike in the first place.
            //
            // Fixed capacity, no allocation after the first frames, no locks:
            // this runs on the main thread inside the frame it measures.
            {
                viewer::IssueFrameSample sample;
                // RAW cadence, NOT hud_frame_ms. The HUD value is a 0.9/0.1
                // EMA, and the first capture proved why that matters: its
                // "peak frame" read 258 ms while the raw frames behind it were
                // 400 ms, and peak_render (which was already raw) came out
                // LARGER than peak_frame -- an impossibility that only makes
                // sense once you know one of the two was smoothed. Recording a
                // filtered signal in a spike recorder defeats the recorder.
                sample.frame_ms = static_cast<float>(perf_frame_cadence_ms);
                sample.render_ms = static_cast<float>(phase.render);
                sample.build_ms = frame_stats.build_ms;
                sample.gpu_ms = frame_stats.gpu_total_ms;
                sample.triangles = frame_stats.triangles;
                sample.instances_drawn = frame_stats.instances_drawn;
                sample.resolve_ms = frame_stats.resolve_ms;
                sample.draw_ms = frame_stats.draw_ms;
                sample.zone_vt_ms = frame_stats.draw_vt_requests_ms;
                sample.zone_cull_ms = frame_stats.draw_cull_render_ms;
                sample.zone_skin_ms = frame_stats.draw_skin_seal_ms;
                sample.zone_comp_ms = frame_stats.draw_composite_ms;
                if (issue_frame_history.size() < kIssueHistoryFrames)
                    issue_frame_history.push_back(sample);
                else
                    issue_frame_history[issue_history_cursor] = sample;
                issue_history_cursor =
                    (issue_history_cursor + 1u) % kIssueHistoryFrames;
            }
        }
        if (!frame_completed) {
            std::fprintf(stderr, "FATAL: end_frame: %s\n", error.c_str());
            fatal_error = true;
        } else {
            if (ui_frame_completed && !fatal_error) {
                camera_input_order.end_frame();
                // A region drag must not also fly the camera — the rubber band
                // reads the mouse straight off the IO, outside any ImGui window.
                // A running MATTER_CAM_PATH owns the camera outright: the
                // free-fly controller reads live keyboard/mouse state, which is
                // exactly the nondeterminism the gate exists to exclude.
                if ((camera_input_order.camera_update_allowed() ||
                     camera_capture) &&
                    !viewer::issue_reporter_wants_mouse(issue_state) &&
                    !cam_path_running) {
                    camera_controller.update(window, dt, camera, camera_prefs);
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
                        fill_shot_history(shot);
                        shot.instance_cache_expansions =
                            frame_stats.vk_instance_cache_expansions;
                        shot.command_layout_rebuilds =
                            frame_stats.vk_command_layout_rebuilds;
                        shot.immediate_submits = frame_stats.vk_immediate_submits;
                        shot.resident_sectors = frame_stats.resident_sectors;
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
            } else if (capture &&
                       (!fifo_immediate_capture ||
                        !fifo_present_update.screenshot_path.empty())) {
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
                    if (capture_path == shot_path || fifo_immediate_capture) {
                        const std::string done = capture_path + ".done";
                        if (FILE* file = std::fopen(done.c_str(), "w"))
                            std::fclose(file);
                    }
                    if (capture_path == screenshot_path) quit_requested = true;
                }
            }
        }

        // QA timeline: resolve a deferred `quit` only once no FIFO screenshot
        // capture is still in flight (see reg_fifo_quit above). shot_settle
        // was decremented earlier this frame (before end_frame) and any
        // capture it triggered has now been written above, so both checks
        // reflect this frame's true state.
        if (fifo_quit_pending && shot_settle == 0 &&
            fifo_present.pending_screenshot_path().empty()) {
            quit_requested = true;
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
            // APPEND-ONLY format (scripts parse by position). The established
            // row ends at vol_resource_generation. Task 14 appends five timing
            // lanes (ms) and cloud_shadow_memory_MiB after that stable prefix.
            // M4 appends three GPU-TIMESTAMP lanes (total, cull, gbuffer).
            // frame_ms is pinned to the refresh by VK_PRESENT_MODE_FIFO_KHR, so
            // it cannot see a sub-millisecond change on the GPU: an A/B that
            // reads it is measuring the display, not the renderer.
            std::printf("STATS,%s,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d"
                        ",%u,%u,%u,%.1f,%.1f,%u,%u,%u,%.2f,%llu"
                        ",%.3f,%.3f,%.3f,%.3f,%.3f,%.2f"
                        ",%.3f,%.3f,%.3f\n",
                        stats_label.c_str(), stats.frame_ms, stats.resolve_ms,
                        stats.build_ms, stats.draw_ms, stats.instances_active,
                        stats.raster_batches, stats.raster_tris,
                        stats.culled_clusters, stats.gpu_occlusion_culled,
                        frame_stats.vt_variants,
                        frame_stats.vt_rejected_variants,
                        frame_stats.vt_max_variants,
                        static_cast<double>(frame_stats.vt_mesh_bytes) /
                            (1024.0 * 1024.0),
                        static_cast<double>(
                            frame_stats.vt_mesh_budget_bytes) /
                            (1024.0 * 1024.0),
                        frame_stats.vol_grid_w, frame_stats.vol_grid_h,
                        frame_stats.vol_grid_d,
                        static_cast<double>(frame_stats.vol_memory_bytes) /
                            (1024.0 * 1024.0),
                        static_cast<unsigned long long>(
                            frame_stats.vol_resource_generation),
                        frame_stats.gpu_atmosphere_ms,
                        frame_stats.gpu_cloud_shadows_ms,
                        frame_stats.gpu_vol_density_ms,
                        frame_stats.gpu_vol_scatter_ms,
                        frame_stats.gpu_vol_integrate_ms,
                        static_cast<double>(frame_stats.cloud_shadow_memory_bytes) /
                            (1024.0 * 1024.0),
                        frame_stats.gpu_total_ms, frame_stats.gpu_cull_ms,
                        frame_stats.gpu_gbuffer_ms);
            std::fflush(stdout);
            // The VT census goes to STDERR, next to the rest of the [vk]/[vt]
            // diagnostics, and unbuffered: a streamed-world capture that dies
            // during shutdown must not lose the one line that says how much of
            // the world actually got virtual texturing.
            // APPEND-ONLY too: the trailing ind= field (buffer-indirection
            // bytes, live/capacity) was appended when the image-array
            // indirection and its 2048-layer wall were replaced.
            std::fprintf(stderr,
                        "STATSVT,%s,active=%d,variants=%u/%u,rejected=%u,"
                        "mesh=%.1f/%.1f MiB,pool=%u/%u,pinned=%u,queue=%u,"
                        "fills=%llu,evictions=%llu,ind=%.2f/%.0f MiB\n",
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
                            frame_stats.vt_evictions_total),
                        static_cast<double>(
                            frame_stats.vt_indirection_bytes) /
                            (1024.0 * 1024.0),
                        static_cast<double>(
                            frame_stats.vt_indirection_capacity_bytes) /
                            (1024.0 * 1024.0));
            std::fflush(stderr);
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
            apply_world_volumetrics_after_bake = !replay.valid;
            apply_world_atmosphere_after_bake = !replay.valid;
            apply_world_cloud_shadows_after_bake = !replay.valid;
            // prepare_world_reload below drops stats.fog to the compiled
            // default; until the reconnect re-seeds it, the session's own
            // authored fog is the only truthful source.
            apply_world_fog_after_bake = !replay.valid;
            fog_override_ready = false;
            // prepare_world_reload drops stats.lighting to the compiled
            // default too, so the same rule applies to the sun angles.
            apply_world_sun_after_bake = !replay.valid;
            sun_override_ready = false;
            // set_world BEFORE the reset: it flushes this world's edits by
            // diffing against the baseline, which the reset is about to erase.
            editor_props.set_world(worlds[stats.world_current].project_dir,
                                   worlds[stats.world_current].world_name);
            apply_world_props_after_bake = true;
            viewer::prepare_world_reload(stats);
            binding.reload();  // clears app models + session->reload()
        }
        const int pending_switch = binding.pending_switch();
        if (pending_switch >= 0 && pending_switch < static_cast<int>(worlds.size())) {
            binding.clear_pending_switch();
            const int selected = pending_switch;
            // BEFORE replace(), not after: set_world flushes the outgoing
            // world's edits (while its baseline is still intact) AND loads the
            // incoming world's RequiresReload groups, which open_world — called
            // from inside replace() — hands to the new session before its first
            // bake. Only the paths and those groups move here; the World-scope
            // structs are still reset by complete_world_switch below.
            editor_props.set_world(worlds[selected].project_dir,
                                   worlds[selected].world_name);
            // SessionBinding::replace runs the S I.13 close/quiesce/replace/
            // rebind/open/request epoch sequence; a failed open leaves the old
            // session + epoch fully intact.
            const bool ok = binding.replace([&]() { return open_world(worlds[selected]); });
            if (!ok) {
                // Point back at the world we are still in, or its later edits
                // would be written to the file of a world we never opened.
                editor_props.set_world(worlds[stats.world_current].project_dir,
                                       worlds[stats.world_current].world_name);
                // set_world released the (still perfectly valid) props group of
                // the session we never left, and no connect is coming to
                // restore it — rebind it by hand.
                editor_props.adopt_world_props(session->world_props());
                editor_props.adopt_draw_overrides(session->draw_overrides());
                viewer::complete_world_switch(stats, false);
            } else {
                apply_world_props_after_bake = true;
                viewer::complete_world_switch(stats, true);
                stats.world_current = selected;
                selected_world_reported = false;
                console_log.push(viewer::LogSeverity::Info,
                                 "Connected to " + worlds[selected].world_name);
                bake_ready = false;
                screenshot_settle = 0;
                apply_world_camera_after_bake = true;
                apply_world_volumetrics_after_bake = true;
                apply_world_atmosphere_after_bake = true;
                apply_world_cloud_shadows_after_bake = true;
                // complete_world_switch reset stats.fog; same reasoning as the
                // reload seam above.
                apply_world_fog_after_bake = true;
                fog_override_ready = false;
                apply_world_sun_after_bake = true;
                sun_override_ready = false;
                apply_world_resolver_defaults(worlds[selected].world_name,
                                              min_projected_size, stats);
            }
        }
    }

    // Idempotent: a completed MATTER_CAM_PATH already closed it. This covers a
    // run that ended some other way (window closed, fatal error) so the trace
    // still gets its summary line rather than being silently truncated.
    viewer::lod_trace::close();
    // Same reasoning for the seam summary: a run cut short by the window
    // closing still has to say what it saw. Idempotent.
    print_seam_summary("shutdown");

#ifndef _WIN32
    if (cmd_fd >= 0) close(cmd_fd);
    if (fifo_path) unlink(fifo_path);
#endif
    if (camera_capture) camera_controller.set_capture(window, false, false);
    // MATTER_PROFILE_TRACE=<path>: on exit, dump the profiler's FrameRecord tail
    // as a Chrome-trace (loads in chrome://tracing). Headless capture path for
    // the same data every issue report embeds as profile_tail.json.
    if (const char* trace_path = std::getenv("MATTER_PROFILE_TRACE")) {
        if (trace_path[0] != '\0') {
            if (matter::profile::dump_chrome_trace(trace_path))
                std::printf("profile: wrote trace to %s\n", trace_path);
            else
                std::fprintf(stderr, "profile: could not write trace to %s\n",
                             trace_path);
        }
    }
    // Flush a debounced User-scope autosave that the last frames did not reach.
    // World scope stays explicit-save (plus the automatic flush at every
    // world-change seam) — a save-on-exit prompt is Stage 3.
    editor_props.shutdown();
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
