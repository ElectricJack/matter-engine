// MatterEngine3 Vulkan world viewer. The production path creates a GLFW
// NO_API window and presents genuine WorldSession data through VkSceneRenderer.
// MATTER_CAM, MATTER_WORLD, MATTER_SCREENSHOT and FIFO commands are retained
// from the legacy viewer. MATTER_HIZ is recognised but IGNORED — the Hi-Z
// buffer it selected is gone; see the note at its getenv below.
//
// MatterEditor/src/main.cpp
//
// The editor's whole entry point: `main()` plus a file-local anonymous
// namespace of helpers. There is no App/Editor class — every long-lived
// object (the GLFW window, VulkanDevice, EngineContext, WorldSession, Ui,
// the panels, the command registry) is a LOCAL of `main()`, and their
// declaration order is their destruction order. Several teardown comments at
// the bottom of the file depend on that.
//
// Vulkan-only. The GL/raylib render and windowing path was deleted in Phase
// 5a: the window is created with GLFW_CLIENT_API = GLFW_NO_API and every
// pixel goes through matter::VulkanDevice + VkSceneRenderer.
//
// ---------------------------------------------------------------------------
// Startup sequence
// ---------------------------------------------------------------------------
// In order, because most steps depend on the one before:
//
//   1. `process_start_time` is stamped first, so the device-fault auto-filer
//      at the very end can tell a vulkan_device_fault.log written by THIS run
//      from a stale one left by an earlier crash.
//   2. stdout is unbuffered when MATTER_CMD_FIFO is set — FIFO automation
//      consumes exact acknowledgement lines as its synchronization.
//   3. read_perf_run_config() (MATTER_PERF_*), then glfwInit().
//   4. viewer::load_replay_from_env() runs BEFORE the window exists: a
//      MATTER_REPLAY shot records its framebuffer size, and the window has to
//      be created at that size for the same geometry to land on the same
//      pixels.
//   5. glfwCreateWindow -> matter::VulkanDevice::create -> EngineContext::create.
//   6. viewer::Ui::setup (Dear ImGui + its Vulkan backend). For a replay the
//      recorded imgui.ini layout is restored and IniFilename is cleared, so
//      the panel layout — and therefore the viewport rect — is reproduced too.
//   7. viewer::scan_worlds(examples_root()) builds the world list; MATTER_WORLD
//      (or, failing that, the replay's own world) picks `initial_world`.
//   8. EditorProps::init() binds the property registry BEFORE anything writes
//      the tunable structs, so bind() captures the compiled defaults.
//   9. open_world() -> matter::WorldSession, then SessionBinding::initialize()
//      builds the app<->session bridge and opens the first command epoch
//      BEFORE requesting the initial bake, so no bake.started can precede its
//      subscribers.
//  10. Command handlers are registered on the app lane; the frame loop starts.
//
// ---------------------------------------------------------------------------
// What one iteration of the frame loop does
// ---------------------------------------------------------------------------
//   glfwPollEvents -> retire preview textures queued on earlier frames ->
//   hotkeys (TAB mouse capture, F8 DLSS, F9/F10 issue capture, F11
//   presentation mode) -> read MATTER_CMD_FIFO bytes and dispatch as many
//   queued lines as no blocking wait forbids -> registry.pump(app_lane) ->
//   shot/issue-capture deadman checks -> VulkanDevice::begin_frame (fence
//   wait + swapchain acquire) -> Ui::begin_frame and all panel drawing ->
//   MATTER_CAM_PATH pose -> the `frame_camera` snapshot -> viewport pick and
//   orbit -> streaming anchor update -> WorldSession::tick ->
//   PropertyScheduler::flush_dirty -> pump_gpu_jobs -> Bake Lab / Workbench
//   tick -> poll_event drain (bake events, world-authored value adoption) ->
//   RenderOptions assembly -> WorldSession::render -> selection/frustum/
//   animation overlays -> FrameStats mirrored into ViewerStats ->
//   Ui::end_frame -> optional swapchain readback (screenshot or issue shot)
//   -> VulkanDevice::end_frame -> phase timing + the issue frame-history ring
//   -> FIFO block releases and the deferred `quit` -> perf sampling -> the
//   STATS line -> post-frame seam (reload / world switch).
//
//   Two timing rules the rest of the loop relies on:
//   - `frame_camera` is taken after the UI and after the cam-path pose, and is
//     const for the remainder of the frame, so streaming, tick, render, the
//     overlays and the pick all agree on exactly one camera.
//   - Heavy session operations (reload, world switch) NEVER run mid-ImGui
//     draw. Their command handlers only RECORD intent on SessionBinding; the
//     actual session destroy/recreate happens at the post-frame seam at the
//     bottom of the loop.
//
// ---------------------------------------------------------------------------
// Environment control surface
// ---------------------------------------------------------------------------
// Full reference: docs/agent/control-surface.md. Read directly by THIS file:
//
//   MATTER_WORLD              world to open, by name, case-insensitive; a name
//                             not in the scanned list is fatal
//   MATTER_CAM                "ex,ey,ez,tx,ty,tz" initial camera
//   MATTER_CAM_PATH           file of one `eye target` pose per line, consumed
//                             ONE POSE PER RENDERED FRAME — frame-indexed, not
//                             wall-clock, which is what makes it a determinism
//                             gate. `#` and blank lines are ignored.
//   MATTER_CAM_PATH_WARMUP    frames held at pose 0 once drawable (default 30)
//   MATTER_CAM_PATH_SETTLE    SECONDS of unchanged resident_sectors required
//                             before the path starts (0 = off)
//   MATTER_CAM_PATH_EXIT      quit once the path plus its drain tail ends
//   MATTER_SCREENSHOT         capture-then-quit to this PNG path
//   MATTER_SCREENSHOT_SETTLE  frames to hold before that capture (default 3;
//                             a streamed world needs far more)
//   MATTER_REPLAY             reproduce a recorded issue shot (shot_replay.h)
//   MATTER_REPLAY_OUT         where the replay PNG goes (default replay.png)
//   MATTER_REPLAY_SETTLE      replay settle frames (default 90 — RT worlds
//                             accumulate through a temporal denoiser)
//   MATTER_REPLAY_STRICT      make a non-comparable replay a fatal error
//   MATTER_CMD_FIFO           command stream: a real FIFO on POSIX, an
//                             append-only polled file on Windows
//   MATTER_HIDE_UI            hide every panel; the 3D view then renders
//                             straight to the swapchain image
//   MATTER_TIME_SCALE         initial simulation time scale (clamped to the
//                             toolbar range, otherwise ignored with a message)
//   MATTER_LIVE_EDIT          enable world-script live edit on the session
//   MATTER_CACHE_ROOT         engine cache root, canonicalized to absolute
//   MATTER_VK_VALIDATION      request Vulkan validation layers — opt-in,
//                             because they are a Vulkan-SDK dependency the
//                             shipped exe must not require
//   MATTER_PERF_OUTPUT        with MATTER_PERF_WARMUP_SECONDS and
//                             MATTER_PERF_SAMPLE_SECONDS: a timed perf run
//                             that writes one JSON object and quits. All three
//                             must be set together or startup fails.
//   MATTER_PROFILE_TRACE      dump the ProfileLib tail as a Chrome trace on exit
//   MATTER_SEAM_TRACE         per-frame seam-welder poll, printed on change,
//                             plus an end-of-run PASS/FAIL verdict
//   MATTER_FORCE_LOD_TINT     force the LOD-tint geometry debug view
//   MATTER_TEST_RESIZE        resize the window once, after the bake
//   MATTER_CAPTURE_LIGHTING_UI focus the Lighting tab during a capture run
//   MATTER_HIZ                recognised but IGNORED (the HZB is gone)
//
// Reached through the property registry's own env layer rather than a getenv
// in this file: MATTER_DISABLE_VK_RT, MATTER_DLSS_MODE, the MATTER_SUN_*
// angles, and every other `.env()`-bound tunable (see editor_props.cpp).
// MATTER_VK_SMOKE_MODE is not handled here at all — the Vulkan smoke suite is
// a separate binary (MatterEngine3/tests/vulkan_smoke_tests.cpp).
//
// ---------------------------------------------------------------------------
// The FIFO / QA timeline
// ---------------------------------------------------------------------------
// Lines arriving on MATTER_CMD_FIFO are split into `fifo_pending_lines` and
// popped one at a time. A blocking verb (wait_frames / wait_idle / wait_event
// / shot / shot_now / `issue capture`) sets `fifo_block` and STOPS the pop
// loop until it releases — that is what turns a pre-written command file into
// a timeline instead of "every buffered line lands in one frame". Reading more
// bytes off the file is never gated; only dispatch is. Everything else parses
// into a typed viewer::Fifo* command and goes through registry.dispatch(), so
// each external submission is named, traced and explicitly completed.
//
// ---------------------------------------------------------------------------
// Sharp edges
// ---------------------------------------------------------------------------
// - Teardown order is load-bearing and partly MANUAL, because C++ would
//   otherwise get it wrong: session.reset(), issue_previews.shutdown() and
//   bake_lab.workbench().close() are all called by hand before vulkan.reset(),
//   since those objects hold GPU resources but are stack locals that would be
//   destroyed only when main() returns — after the device was gone.
// - The process returns 1 when vulkan->validation_error_count() is non-zero at
//   shutdown, independently of `fatal_error`.
// - `fatal_error_reason` is set only by mark_device_fatal(), i.e. only at the
//   Vulkan/device-surfacing failure sites. That is how the post-loop auto-filer
//   tells a device fault from any other fatal exit without matching on error
//   text — a non-device fatal exits with no report, exactly as before.
// - Most world-authored settings (camera, fog, sun, volumetrics, atmosphere,
//   cloud shadows, the world's `static props`) adopt through a one-shot
//   `apply_world_*_after_bake` flag consumed at the first successful bake, and
//   are re-armed at both the reload seam and the world-switch seam. The ORDER
//   inside that block matters: the authored values must land before
//   EditorProps::on_world_connected() snapshots the "Reset to World" baseline.
// - A replay deliberately neither persists nor adopts: EditorProps::init() is
//   passed persist=false and ImGui's IniFilename is cleared, so a headless
//   capture can neither inherit nor overwrite an interactive session's files.
#include "matter/engine_context.h"
#include "matter/vulkan_device.h"
#include "matter/world_session.h"
#include "matter/ecs.h"
#include "matter/physics.h"
#include "matter/character.h"  // walking mode (M3)
#include "matter/scene.h"
#include "matter/streaming.h"
#include "ecs/simulation_control.h"
#include "ecs/scene_registry.h"
#include "scene/scene_service.h"  // E5c: session->scene_service() (world_session.h fwd-decls it)
#include "camera_controller.h"
#include "character_walk_controller.h"
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
#include "selection_commands.h"
#include "selection_outline.h"
#include "selection_set.h"
#include "toolbar_panel.h"
#include "console_panel.h"
#include "matter/log.h"
#include "ui.h"
#include "wireframe_controls.h"
#include "session_binding.h"
#include "scene_model_adapter.h"
#include "scene_inventory.h"
#include "regen_jobs.h"
#include "procedural_parameters.h"
#include "viewer_commands.h"
#include "matter/event/event_hub.h"
#include "matter/event/command.h"
#include "matter/event/property.h"
#include "matter/windows_compat.h"
// QA timeline `wait_event`: subscribes directly against session->events()
// (bake./stream.) and app_hub (cmd.*) by name, so the typed event structs
// must be visible here (world_session.h only pulls in the legacy events.h).
#include "matter/events/bake_events.h"
#include "matter/events/stream_events.h"
#include "viewport_capture.h"
#include "viewport_pick.h"
#include "viewport_pick_command.h"
#include "dsl_bindings.h"

#include "imgui.h"
#include "quickjs.h"

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
#endif

namespace {

// regen_jobs.h mirrors matter::BakeErrorCode by VALUE so the job ledger can
// stay engine-free and still report the engine's own classification. This is
// the one place the two are held together: adding a code to matter/events.h
// without adding it there is a compile error here, not a silently mislabelled
// diagnostic in an agent's result.
static_assert(static_cast<int>(matter::BakeErrorCode::None) ==
                  static_cast<int>(viewer::jobs::ErrorCode::None) &&
              static_cast<int>(matter::BakeErrorCode::Cancelled) ==
                  static_cast<int>(viewer::jobs::ErrorCode::Cancelled) &&
              static_cast<int>(matter::BakeErrorCode::OutOfMemory) ==
                  static_cast<int>(viewer::jobs::ErrorCode::OutOfMemory) &&
              static_cast<int>(matter::BakeErrorCode::ScriptError) ==
                  static_cast<int>(viewer::jobs::ErrorCode::ScriptError) &&
              static_cast<int>(matter::BakeErrorCode::GpuError) ==
                  static_cast<int>(viewer::jobs::ErrorCode::GpuError) &&
              static_cast<int>(matter::BakeErrorCode::IoError) ==
                  static_cast<int>(viewer::jobs::ErrorCode::IoError) &&
              static_cast<int>(matter::BakeErrorCode::Internal) ==
                  static_cast<int>(viewer::jobs::ErrorCode::Internal),
              "viewer::jobs::ErrorCode must mirror matter::BakeErrorCode");

// ---------------------------------------------------------------------------
// Console log sink. Bridges the engine-wide matter::log facility into the
// editor's Console panel: every MATTER_LOG* line (from the engine, its
// libraries, or the editor itself) is mirrored into the ConsoleLog ring buffer
// so the panel shows the same diagnostics that go to stderr. Fires on whatever
// thread logged; ConsoleLog::push is thread-safe by design.
void console_log_sink(matter::log::Level level, const char* tag,
                      const char* message, void* user) {
    auto* log = static_cast<viewer::ConsoleLog*>(user);
    if (!log) return;
    viewer::LogSeverity severity;
    switch (level) {
        case matter::log::Level::Warn:  severity = viewer::LogSeverity::Warning; break;
        case matter::log::Level::Error: severity = viewer::LogSeverity::Error; break;
        default:                        severity = viewer::LogSeverity::Info; break;
    }
    // Reproduce the tee's "[tag] message" shape so a copied console line reads
    // the same as the terminal output it mirrors.
    if (tag && tag[0] != '\0') {
        log->push(severity, std::string("[") + tag + "] " + message);
    } else {
        log->push(severity, message);
    }
}

// RAII guard: registers the console sink for its lifetime. Declared just AFTER
// the ConsoleLog it targets so it destructs FIRST (locals unwind in reverse),
// removing the sink before the ConsoleLog it points at is torn down -- no
// worker thread can log into a half-destroyed buffer.
struct ConsoleLogSinkGuard {
    explicit ConsoleLogSinkGuard(viewer::ConsoleLog& log) : log_(&log) {
        matter::log::add_sink(&console_log_sink, log_);
    }
    ~ConsoleLogSinkGuard() { matter::log::remove_sink(&console_log_sink, log_); }
    ConsoleLogSinkGuard(const ConsoleLogSinkGuard&) = delete;
    ConsoleLogSinkGuard& operator=(const ConsoleLogSinkGuard&) = delete;
    viewer::ConsoleLog* log_;
};

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

// Resolve a stable authored SceneEntityId to the live flecs entity carrying it.
//
// O(entities): a full each() scan with no early exit, run once per Properties
// field get/set, once per specialized-editor action, and once per selection
// validate callback. Entity selections are keyed by SceneEntityId rather than
// by flecs entity id on purpose, so a selection survives across frames for
// entities the ECS creates dynamically.
//
// Returns a default-constructed (`is_valid() == false`) entity when nothing
// matches; every caller treats that as "not found", not as an error.
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
        case ComponentKind::CharacterController:
            return fetch_component_copy<matter::character::CharacterController>(e, out);
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
        case ComponentKind::CharacterController: {
            const auto& controller = *static_cast<const matter::character::CharacterController*>(in);
            return viewer::store_character_component(e, controller);
        }
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

// The one non-field entry in FieldCommands: the parent's accumulated
// WorldTransform, which is what the transform gizmo needs to place a child
// entity's handle where the renderer actually draws it.
//
// `out` is ALWAYS written — identity when the entity is a root, or when the
// parent exists but has not been propagated yet (the transform systems only
// write WorldTransform for entities they have visited). A false return means
// the entity id itself did not resolve, and `out` is identity in that case too,
// so a caller that ignores the result still gets the old parentless behaviour
// rather than garbage.
bool field_get_parent_world_matrix(matter::WorldSession* session,
                                   matter::scene::SceneEntityId id,
                                   matter::Mat4f& out) {
    out = matter::Mat4f{};
    out.m[0] = 1.0f; out.m[5] = 1.0f; out.m[10] = 1.0f; out.m[15] = 1.0f;
    if (!session) return false;
    flecs::entity e = find_scene_entity(session->ecs(), id);
    if (!e.is_valid()) return false;
    flecs::entity parent = e.parent();
    if (!parent.is_valid()) return true;
    const matter::ecs::WorldTransform* wt =
        parent.try_get<matter::ecs::WorldTransform>();
    if (!wt) return true;
    out = wt->matrix;
    return true;
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
    else if (!std::strcmp(component_name, "CharacterController")) {
        matter::character::CharacterController controller;
        std::string error;
        if (!matter::scene::validate_character_component(e, controller, error))
            return SceneEditResult{SceneEditError::InvalidTarget, {}};
        e.set<matter::character::CharacterController>(controller);
    }
    else return SceneEditResult{SceneEditError::InvalidTarget, {}};

    return SceneEditResult{SceneEditError::None, id};
}

// The remove half of component_add above, and the same shape: a strcmp ladder
// over component NAMES, because flecs remove<T>() is typed on the C++ type.
// Transform is absent from both ladders — every scene entity has one — so
// asking for "Transform" here returns InvalidTarget rather than removing it.
// Removing a component the entity does not have is a no-op in flecs and still
// reports success.
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
    else if (!std::strcmp(component_name, "CharacterController")) e.remove<matter::character::CharacterController>();
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

// Rising-edge key test: true only on the frame the key goes down. `previous`
// is an in/out latch OWNED BY THE CALLER — one bool per key (see the
// tab_down/f8_down/f9_down/f10_down/f11_down locals in main) — and is updated
// to the current down-state on every call, so each key must be polled once per
// frame or its latch goes stale.
bool key_pressed(GLFWwindow* window, int key, bool& previous) {
    const bool down = glfwGetKey(window, key) == GLFW_PRESS;
    const bool pressed = down && !previous;
    previous = down;
    return pressed;
}

// Write tightly-packed RGBA8 (4 bytes/texel, no row padding) to `path` as a
// PNG, creating any missing parent directories first. Returns false when
// `rgba` is not exactly width*height*4 bytes — a caller bug, not an I/O
// failure — or when stb's encoder fails. Never throws; directory creation
// takes the std::error_code overload and its result is deliberately ignored,
// since stbi_write_png reports the real outcome.
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

// A timed performance run, configured entirely by MATTER_PERF_OUTPUT /
// MATTER_PERF_WARMUP_SECONDS / MATTER_PERF_SAMPLE_SECONDS — all three or none
// (read_perf_run_config below rejects a partial set). The run waits for the
// bake to finish and the world to actually draw, warms for `warmup_seconds`,
// samples end-to-end frame cadence for `sample_seconds`, writes one JSON
// object to `output_path`, and then requests quit. See the PerfPhase state
// machine in the frame loop.
struct PerfRunConfig {
    bool enabled = false;
    std::string output_path;
    double warmup_seconds = 0.0;   // seconds; may be 0
    double sample_seconds = 0.0;   // seconds; must be > 0
};

// Monotonic engine counters snapshotted at the start and at the end of the
// sampling window. The JSON reports the DELTAS, not these absolutes, which is
// what makes "a static scene must stop uploading" an assertable property.
struct PerfCounters {
    uint64_t vertex_uploads = 0;
    uint64_t cluster_uploads = 0;
    uint64_t instance_uploads = 0;
    uint64_t immediate_submits = 0;
    uint64_t water_animation_uploads = 0;
    uint64_t water_animation_decode_dispatches = 0;
    uint64_t water_animation_steady_state_allocations = 0;
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
            stats.vk_instance_uploads, stats.vk_immediate_submits,
            stats.water_animation_uploads,
            stats.water_animation_decode_dispatches,
            stats.water_animation_steady_state_allocations};
}

double median_of_sorted(const std::vector<double>& sorted) {
    const size_t middle = sorted.size() / 2;
    return (sorted.size() & 1) != 0
               ? sorted[middle]
               : (sorted[middle - 1] + sorted[middle]) * 0.5;
}

// Escape `value` for embedding inside a JSON string. Returns the escaped INNER
// text only — the surrounding double quotes are written by the call sites in
// write_perf_result — so the result is not a complete JSON literal on its own.
// Control characters below 0x20 become \u00xx; bytes >= 0x80 pass through
// unchanged (the inputs here are ASCII diagnostic strings).
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

std::vector<std::string> quickjs_global_property_names(JSContext* context,
                                                       std::string& error) {
    JSValue global = JS_GetGlobalObject(context);
    JSPropertyEnum* properties = nullptr;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(context, &properties, &count, global,
                               JS_GPN_STRING_MASK) != 0) {
        JS_FreeValue(context, global);
        error = "QuickJS failed to enumerate global properties";
        return {};
    }

    std::vector<std::string> names;
    names.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const char* name = JS_AtomToCString(context, properties[index].atom);
        if (name) {
            names.emplace_back(name);
            JS_FreeCString(context, name);
        }
        JS_FreeAtom(context, properties[index].atom);
    }
    js_free(context, properties);
    JS_FreeValue(context, global);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::vector<std::string> runtime_dsl_binding_names(std::string& error) {
    JSRuntime* runtime = JS_NewRuntime();
    if (!runtime) {
        error = "QuickJS failed to create a runtime for the registration census";
        return {};
    }
    JSContext* context = JS_NewContext(runtime);
    if (!context) {
        JS_FreeRuntime(runtime);
        error = "QuickJS failed to create a context for the registration census";
        return {};
    }

    const std::vector<std::string> before =
        quickjs_global_property_names(context, error);
    std::vector<std::string> after;
    if (error.empty()) {
        dsl::install_bindings(context);
        after = quickjs_global_property_names(context, error);
    }

    std::vector<std::string> installed;
    if (error.empty()) {
        std::set_difference(after.begin(), after.end(), before.begin(),
                            before.end(), std::back_inserter(installed));
    }
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return installed;
}

void append_json_string_array(std::ostringstream& stream,
                              const std::vector<std::string>& values) {
    stream << '[';
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) stream << ',';
        stream << '"' << json_string(values[index]) << '"';
    }
    stream << ']';
}

void emit_registration_census(
    const std::vector<viewer::WorldEntry>& worlds,
    const matter::props::Registry& properties,
    const matter::evt::CommandRegistry& commands) {
    std::vector<std::string> world_names;
    world_names.reserve(worlds.size());
    for (const viewer::WorldEntry& world : worlds) {
        world_names.push_back(world.world_name);
    }
    std::sort(world_names.begin(), world_names.end());
    world_names.erase(std::unique(world_names.begin(), world_names.end()),
                      world_names.end());

    std::vector<std::string> property_names;
    property_names.reserve(properties.size());
    for (size_t index = 0; index < properties.size(); ++index) {
        const char* path = properties.at(index).schema().path;
        if (path && path[0] != '\0') property_names.emplace_back(path);
    }
    std::sort(property_names.begin(), property_names.end());
    property_names.erase(
        std::unique(property_names.begin(), property_names.end()),
        property_names.end());

    std::string dsl_error;
    const std::vector<std::string> dsl_names =
        runtime_dsl_binding_names(dsl_error);
    if (!dsl_error.empty()) {
        MATTER_LOGE("registration-census", "FATAL: %s\n", dsl_error.c_str());
        return;
    }

    std::ostringstream json;
    json << '{';
    json << "\"world\":";
    append_json_string_array(json, world_names);
    json << ",\"dsl\":";
    append_json_string_array(json, dsl_names);
    json << ",\"property\":";
    append_json_string_array(json, property_names);
    json << ",\"editor\":";
    append_json_string_array(json, commands.registered_handler_names());
    json << '}';
    std::printf("MATTER_REGISTRATION_CENSUS_JSON=%s\n", json.str().c_str());
}

// Write the perf run's single-line JSON result; returns false with `error` set
// on an empty sample set or an output path that cannot be written.
//
// `frame_times` is taken BY VALUE and sorted in place (milliseconds per frame,
// end-to-end loop cadence). The median and p95 come out of that sorted vector;
// p95 is element ceil(0.95 * n) - 1, so a one-frame run reports that frame for
// both.
//
// Two different time bases live in the output and mixing them up is the usual
// mistake: the `*_delta` fields are end-minus-start over the whole sampling
// window, while every gpu_*_ms / cpu_*_ms / loop_*_ms field is the LAST
// SAMPLED FRAME only. The inline comments in the body say why each group was
// added.
bool write_perf_result(const PerfRunConfig& config, const std::string& world,
                       std::vector<double> frame_times,
                       std::vector<double> water_animation_times,
                       const PerfCounters& start,
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
    if (water_animation_times.size() != frame_times.size()) {
        error = "water-animation GPU samples do not match performance frames";
        return false;
    }
    std::sort(water_animation_times.begin(), water_animation_times.end());
    const double median_water_animation_ms =
        median_of_sorted(water_animation_times);
    const double p95_water_animation_ms = water_animation_times[p95_index];
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
           << ",\"water_animation_gpu_median_ms\":"
           << median_water_animation_ms
           << ",\"water_animation_gpu_p95_ms\":"
           << p95_water_animation_ms
           << ",\"water_animation_upload_delta\":"
           << (finish.water_animation_uploads -
               start.water_animation_uploads)
           << ",\"water_animation_decode_dispatch_delta\":"
           << (finish.water_animation_decode_dispatches -
               start.water_animation_decode_dispatches)
           << ",\"water_animation_steady_state_allocation_delta\":"
           << (finish.water_animation_steady_state_allocations -
               start.water_animation_steady_state_allocations)
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
           << ",\"gpu_water_animation_ms\":"
           << frame_stats.gpu_water_animation_ms;
    matter::append_water_forward_perf_json(output, frame_stats);
    // CPU render-thread split (last sampled frame).
    output << ",\"cpu_resolve_ms\":" << frame_stats.resolve_ms
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

// ---------------------------------------------------------------------------
// main() — startup
// ---------------------------------------------------------------------------
// Every long-lived object below is a local of this function, and declaration
// order is destruction order (see the Shutdown section at the bottom, which
// has to undo part of that by hand). The full startup sequence is in the file
// header. Each early-failure path unwinds only what it has already created,
// which is why the teardown calls repeat with a growing prefix.
int main() {
    // Stamped before anything else so the device-fault auto-filer (see the
    // post-loop seam near the end of main) can tell a vulkan_device_fault.log
    // that THIS run just wrote from a stale one left by an earlier crash.
    const std::filesystem::file_time_type process_start_time =
        std::filesystem::file_time_type::clock::now();
    // Native Windows stdout is fully buffered when the harness redirects it;
    // MSYS `stdbuf` cannot alter the MSVCRT stream. FIFO automation consumes
    // exact acknowledgements as synchronization, so publish them immediately.
    if (std::getenv("MATTER_CMD_FIFO"))
        std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool registration_census_mode =
        std::getenv("MATTER_REGISTRATION_CENSUS") != nullptr;
    if (registration_census_mode) std::setvbuf(stdout, nullptr, _IONBF, 0);
    PerfRunConfig perf;
    std::string perf_error;
    if (!read_perf_run_config(perf, perf_error)) {
        MATTER_LOGE("perf", "FATAL: %s\n", perf_error.c_str());
        return 1;
    }
    if (!glfwInit()) {
        MATTER_LOGE("editor", "FATAL: glfwInit failed\n");
        return 1;
    }
    // Replay run (shot_replay.h): reproduce a recorded issue shot and exit.
    // Loaded before the window exists because the recorded framebuffer size is
    // what makes the same geometry land on the same pixels — resizing after the
    // fact would reflow the docked panels and move the viewport.
    const viewer::ShotReplay replay = viewer::load_replay_from_env();
    if (!replay.valid && !replay.error.empty()) {
        MATTER_LOGE("replay", "FATAL: MATTER_REPLAY: %s\n", replay.error.c_str());
        glfwTerminate();
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    // Perf mode is an automated GPU measurement, not an interactive editor
    // session. Keeping its window hidden prevents the Windows desktop manager
    // from clipping oversized acceptance resolutions to the work area or
    // throttling an occluded surface to roughly one present per second.
    if (registration_census_mode || perf.enabled)
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    if (perf.enabled)
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    int initial_window_width = replay.valid && replay.frame_width > 0
        ? static_cast<int>(replay.frame_width) : 1280;
    int initial_window_height = replay.valid && replay.frame_height > 0
        ? static_cast<int>(replay.frame_height) : 720;
    if (!replay.valid) {
        const char* width_env = std::getenv("MATTER_WINDOW_WIDTH");
        const char* height_env = std::getenv("MATTER_WINDOW_HEIGHT");
        if ((width_env == nullptr) != (height_env == nullptr)) {
            MATTER_LOGE(
                "editor",
                "FATAL: MATTER_WINDOW_WIDTH and MATTER_WINDOW_HEIGHT must be set together\n");
            glfwTerminate();
            return 1;
        }
        if (width_env && height_env) {
            char* width_end = nullptr;
            char* height_end = nullptr;
            const long width = std::strtol(width_env, &width_end, 10);
            const long height = std::strtol(height_env, &height_end, 10);
            if (!width_end || *width_end != '\0' || !height_end ||
                *height_end != '\0' || width < 320 || width > 16384 ||
                height < 240 || height > 16384) {
                MATTER_LOGE(
                    "editor",
                    "FATAL: MATTER_WINDOW_WIDTH/HEIGHT must be integers in [320,16384]x[240,16384]\n");
                glfwTerminate();
                return 1;
            }
            initial_window_width = static_cast<int>(width);
            initial_window_height = static_cast<int>(height);
        }
    }
    GLFWwindow* window = glfwCreateWindow(
        initial_window_width, initial_window_height,
        "MatterEngine3 World Viewer", nullptr, nullptr);
    if (!window) {
        MATTER_LOGE("editor", "FATAL: glfwCreateWindow failed\n");
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
        MATTER_LOGE("editor", "FATAL: %s\n", error.c_str());
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
        MATTER_LOGE("editor", "FATAL: %s\n", error.c_str());
        vulkan.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    viewer::Ui ui;
    if (!ui.setup(window, *vulkan, error)) {
        MATTER_LOGE("editor", "FATAL: %s\n", error.c_str());
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
        MATTER_LOGE("editor", "FATAL: no worlds found under %s\n",
                     examples_root().c_str());
        ui.shutdown();
        engine.reset();
        vulkan.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // -----------------------------------------------------------------------
    // Camera and scripted camera paths
    // -----------------------------------------------------------------------
    // Layered, later layers overwriting earlier ones: the compiled default
    // (init_camera), then a replay's recorded projection, then MATTER_CAM.
    // The world's own authored camera is adopted later still — at the first
    // successful bake, via apply_world_camera_after_bake — and only when
    // neither a replay nor MATTER_CAM has already fixed the pose.
    // -----------------------------------------------------------------------
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
            MATTER_LOGE("editor", "FATAL: MATTER_CAM_PATH: cannot open %s\n",
                         value);
            ui.shutdown();
            engine.reset();
            vulkan.reset();
            glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
        if (cam_path.empty()) {
            MATTER_LOGE("editor", "FATAL: MATTER_CAM_PATH: %s has no poses\n",
                         value);
            ui.shutdown();
            engine.reset();
            vulkan.reset();
            glfwDestroyWindow(window);
            glfwTerminate();
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

    // -----------------------------------------------------------------------
    // World selection, ViewerStats, and the property registry
    // -----------------------------------------------------------------------
    int initial_world = 0;
    // MATTER_WORLD still wins, so a replay can be re-aimed at another world
    // deliberately; absent that, the shot's own world is authoritative.
    const char* world_env = std::getenv("MATTER_WORLD");
    const std::string replay_world = replay.valid ? replay.world : std::string();
    if (!world_env && !replay_world.empty()) world_env = replay_world.c_str();
    if (const char* value = world_env) {
        std::string wanted(value);
        std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        bool found = false;
        for (size_t i = 0; i < worlds.size(); ++i) {
            std::string candidate = worlds[i].world_name;
            std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            if (candidate == wanted) {
                initial_world = static_cast<int>(i);
                found = true;
                break;
            }
        }
        if (!found) {
            MATTER_LOGE("editor",
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
    // MATTER_HIZ is recognised only so old scripts get an answer instead of
    // silence: the Hi-Z occlusion buffer it selected no longer exists (it could
    // not work on tile-sized clusters) and the FIFO `hiz` verb prints the same
    // kind of notice, pointing at viewer.debug.occlusion_draw_cull.
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
                      ui.console_state(),
                      !replay.valid && !registration_census_mode);
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

    // -----------------------------------------------------------------------
    // Session lifecycle
    // -----------------------------------------------------------------------
    // open_world() is the ONE place a matter::WorldSession is created — used
    // both for the initial open here and for every world switch at the
    // post-frame seam, so the two can never drift. It deliberately does not
    // request a bake: SessionBinding owns bake ordering.
    // -----------------------------------------------------------------------
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
            MATTER_LOGE("open-world", "open_world: %s\n", world_error.c_str());
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

    // -----------------------------------------------------------------------
    // App-side models, panel state, and the ECS/scene bridges
    // -----------------------------------------------------------------------
    // Everything from here to the command-registry section is wiring: the
    // observable EditorModel and its scheduler, the selection set, simulation
    // transport, the console log, and the std::function bridges
    // (FieldCommands / ComponentCommands / SpecializedEditors / SceneCommands)
    // that let UI code in viewer:: mutate world state without knowing about
    // flecs or WorldSession. Several of those closures capture `session` by
    // reference, so they follow a world switch automatically.
    // -----------------------------------------------------------------------
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
    viewer::CharacterWalkController character_walk;
    viewer::CharacterJumpEdge character_jump;
    viewer::ConsoleLog console_log;
    // Mirror the engine-wide matter::log stream into this panel for as long as
    // console_log lives (guard removes the sink before console_log destructs).
    ConsoleLogSinkGuard console_log_sink_guard(console_log);
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
    // Not a field: the parent's world matrix, so the transform gizmo can place
    // its handle where a CHILD entity is actually drawn (see gizmo.cpp).
    field_commands.get_parent_world_matrix = [&session](matter::scene::SceneEntityId id, matter::Mat4f& out) {
        return field_get_parent_world_matrix(session.get(), id, out);
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
    specialized_editors.part_commands().current_part_hash =
        [&session](matter::scene::SceneEntityId id, uint64_t& out_hash) {
            out_hash = 0;
            if (!session) return false;
            flecs::entity e = find_scene_entity(session->ecs(), id);
            if (!e.is_valid() || !e.has<matter::scene::PartInstance>()) return false;
            out_hash = e.get<matter::scene::PartInstance>().part_hash;
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
    // set_follow_camera and regenerate are deliberately left NULL rather than
    // assigned a do-nothing lambda. There is nothing behind either one yet —
    // per-anchor follow-camera is not wired to
    // matter_viewer::StreamingAnchorState (that controller tracks a single
    // global anchor, not a per-entity flag; follow-camera behaviour today
    // comes from Ui::update_sector_streaming / streaming_anchor_controller),
    // and no reseed entry point is exposed by
    // matter::streaming::SectorStreaming / sector_streamer.cpp. A null command
    // is what the Properties panel greys the control out on, so the user is
    // told the action is unavailable instead of clicking into a no-op.

    // -----------------------------------------------------------------------
    // Frame-loop state
    // -----------------------------------------------------------------------
    // Per-key rising-edge latches for key_pressed(), the saved windowed rect
    // for F11 presentation mode, and the `reported_*` mirrors that make the
    // DLSS and RT reporters below print only when something actually changed
    // (a per-frame print would drown a several-thousand-frame soak).
    // -----------------------------------------------------------------------
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
            MATTER_LOGW("issue", "issue preview: %s\n", preview_error.c_str());
    };
    // Part Workbench (part-workbench.md W2): private isolation session, see
    // part_workbench.h's architecture note. cache/lab-scratch is entirely
    // separate from production worlds' <project>/.cache/<world> roots.
    bake_lab.workbench().configure(vulkan.get(), examples_root(), shared_lib);
    // -----------------------------------------------------------------------
    // Capture control: MATTER_SCREENSHOT / MATTER_REPLAY
    // -----------------------------------------------------------------------
    // A replay run IS a screenshot run — it reuses the settle/readback/quit
    // path wholesale and differs only in cropping the result to the recorded
    // rect. The `apply_world_*_after_bake` / `*_override_ready` flags declared
    // just below are the one-shot adoption latches described in the file
    // header's "sharp edges".
    // -----------------------------------------------------------------------
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
            MATTER_LOGW("editor", "MATTER_TIME_SCALE ignored: '%s' outside [%.2f, %.2f]\n",
                         scale, viewer::kToolbarMinTimeScale,
                         viewer::kToolbarMaxTimeScale);
    }
    bool resize_exercised = false;
    if (hide_ui) {
        std::printf("viewer: UI hidden by MATTER_HIDE_UI\n");
        ui.set_hide_ui(true);
    }

    // -----------------------------------------------------------------------
    // MATTER_CMD_FIFO command stream
    // -----------------------------------------------------------------------
    // POSIX: a real named FIFO, created here and opened O_RDWR|O_NONBLOCK so
    // the read side never blocks and never sees EOF between writers; it is
    // unlinked at shutdown.
    // Windows: there is no POSIX FIFO, so the same path names an append-only
    // file that the loop polls by size, remembering its own read offset
    // (`cmd_offset`) and rewinding to 0 if the file shrinks — i.e. if a driver
    // truncated or replaced it.
    // Either way the bytes land in the bounded `cmd_lines` framer, then into
    // `fifo_pending_lines`, and are dispatched under the `fifo_block` gate.
    // -----------------------------------------------------------------------
#ifndef _WIN32
    int cmd_fd = -1;
#else
    HANDLE cmd_handle = INVALID_HANDLE_VALUE;
    LARGE_INTEGER cmd_offset{};
#endif
    viewer::agent::LineBuffer cmd_lines;
    const char* fifo_path = std::getenv("MATTER_CMD_FIFO");
    const char* agent_result_path = std::getenv("MATTER_AGENT_RESULT_FILE");
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
    if (agent_result_path) {
        std::printf("MATTER_AGENT_RESULT_FILE: writing JSONL to %s\n",
                    agent_result_path);
        std::fflush(stdout);
    }
    std::string shot_path;
    std::string stats_label;
    int shot_settle = 0;
    viewer::FifoPresentSequencer fifo_present;
    // Agent viewport coordinates name a PRESENTED image. Keep the exact
    // production camera that produced that image beside its serial rather than
    // letting a newly queued `cam` command ray-cast against last frame's ID
    // buffer with tomorrow's pose.
    matter::CameraDesc presented_camera = camera;
    bool presented_camera_available = false;
    bool presented_production_view = false;
    uint64_t presented_session_generation = 0;
    // viewport.capture's one in-flight request. A screenshot is the only agent
    // command whose answer needs a PRESENTED frame, so its terminal result is
    // emitted from the frame loop's capture seam (or from one of the three
    // failure seams beside it), not from the app-lane handler. At most one is
    // armed at a time -- see viewport_capture.h.
    viewer::capture::Tracker agent_capture;
    // The regeneration job ledger (regen_jobs.h) and its bounded waits. The
    // ledger is written at exactly two places: the post-frame seam, which is
    // the only point a reload/regenerate may actually run, and the poll_event
    // drain below, which is where the engine's own bake events arrive. Nothing
    // else may transition a job -- a state derived anywhere else would be a
    // guess, and guessing is what the ledger exists to replace.
    viewer::jobs::Registry regen_jobs;
    viewer::jobs::WaitList regen_waits;
    // D-03: wall-clock deadman for a `shot`/`shot_now` that can never
    // complete (a world where presents never succeed, or where
    // instances_drawn never goes positive so shot_settle can never reach
    // 0). Reset to "now" at every arm (reg_fifo_shot / reg_fifo_shot_now
    // below); checked once per loop iteration right after registry.pump,
    // BEFORE begin_frame, so it still fires even on iterations that never
    // reach the end-of-frame write/quit resolution (the `continue` on a
    // zero-sized-window begin_frame failure skips straight past that code).
    // 30s comfortably exceeds the multi-frame settle window even under a
    // slow bake.
    constexpr double kFifoShotTimeoutSeconds = 30.0;
    std::chrono::steady_clock::time_point fifo_shot_wait_start{};
    // Same deadman shape for FIFO `issue capture` (see the dispatch-loop
    // handler and the AwaitingCapture deadman check beside the shot one,
    // both below): a world where readback never succeeds would otherwise
    // leave issue_state stuck in AwaitingCapture and hang the timeline.
    constexpr double kFifoIssueCaptureTimeoutSeconds = 30.0;
    std::chrono::steady_clock::time_point fifo_issue_capture_wait_start{};
    // True only while the CURRENT AwaitingCapture readback was armed by the
    // FIFO `issue capture` verb (as opposed to an interactive F10 press) --
    // gates the `issue: captured`/`issue: capture failed` prints below so
    // interactive F10 use keeps its existing (unprefixed) log lines.
    bool fifo_issue_capture_active = false;
    // FIFO `issue file <note>`: parsed in the dispatch loop (which runs
    // before begin_frame, too early for write_issue_report's IssueContext --
    // frame.extent, camera, etc. are only settled later), so the parse just
    // stages the note into issue_state.note and sets this flag; the actual
    // write_issue_report call happens at the flag's consumption site further
    // down (deliberately NOT nested in the `!hide_ui` block the "File
    // report" button lives in -- a headless/MATTER_HIDE_UI run must be able
    // to file too).
    bool fifo_issue_file_pending = false;

    // ---- QA timeline state (event-system.md-style FIFO blocking waits) ------
    // Lines already split out by cmd_lines but not yet dispatched. Blocking
    // waits (wait_frames / wait_idle / wait_event / shot / shot_now) pause
    // POPPING this queue -- reading more bytes/lines off the file above is
    // always fine -- which is what turns a pre-written command file into a
    // timeline script instead of "every buffered line lands in one frame"
    // (the old drain-loop behavior). One-command-at-a-time external writers
    // (tools/viewer_shots.sh) see identical behavior: nothing here holds a
    // line back unless it, or one before it, is itself a blocking wait.
    std::deque<std::string> fifo_pending_lines;
    // D-02: `shot` blocks like the other waits below -- shot_settle > 0 or
    // a queued fifo_present screenshot IS the in-flight condition, so this
    // arm carries no extra state of its own (see the release check beside
    // fifo_quit_pending's, after end_frame).
    enum class FifoBlockKind {
        None, WaitFrames, WaitIdle, WaitEvent, Shot, IssueCapture
    };
    FifoBlockKind fifo_block = FifoBlockKind::None;
    // wait_idle <seconds> [timeout_seconds]: released once resident_sectors
    // has held steady for `seconds` of wall-clock time AND the bake is
    // ready. Mirrors the MATTER_CAM_PATH_SETTLE plateau logic above (wall
    // time, not frame count, for the same reason: bake progress is
    // rate-limited in wall time, not frames). D-04: the optional timeout
    // mirrors wait_event's -- the script continues past an expired
    // wait_idle, it does not quit.
    double fifo_wait_idle_seconds = 0.0;
    std::chrono::steady_clock::time_point fifo_wait_idle_start{};
    std::chrono::steady_clock::time_point fifo_wait_idle_last_change{};
    uint32_t fifo_wait_idle_last_resident = UINT32_MAX;
    double fifo_wait_idle_timeout_s = 0.0;
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
    // D-07: an arm generation, bumped on every fifo_begin_wait_event() call.
    // Each subscribed callback below captures its own arm's stamp and only
    // sets fifo_wait_event_fired while that stamp still matches the live
    // generation. bake./stream. events fire on the bake worker thread, and
    // fifo_wait_event_sub.reset() (in the release checks below) does not
    // guarantee a concurrently-in-flight invocation of the PREVIOUS arm's
    // callback observes the unsubscribe before it runs -- without this
    // check, that stale callback can set `fired` for the wrong (later,
    // still-arming-or-armed) wait_event.
    std::atomic<uint64_t> fifo_wait_event_live_gen{0};
    // `quit` is deferred until no FIFO screenshot capture is still in flight
    // -- see the fifo_quit_pending resolution after end_frame below.
    bool fifo_quit_pending = false;

    matter::RenderPath fifo_render_path = matter::RenderPath::GpuDriven;
    bool fifo_render_path_override = false;
    stats.session_status.native_rt_available =
        vulkan->ray_tracing_available();
    bool quit_requested = false;
    bool fatal_error = false;
    // Captured only at the Vulkan/device-surfacing fatal sites (render(),
    // end_frame(), the ImGui Vulkan backend prepare/end_frame calls, and
    // exhausted screenshot-readback retries) -- see mark_device_fatal below.
    // Left empty by non-device fatal exits (MATTER_REPLAY_STRICT mismatch,
    // perf validation-error count), which is how the post-loop auto-filer
    // (near the end of main) tells "a device fault" apart from "some other
    // fatal condition" without string-matching error text.
    std::string fatal_error_reason;
    auto mark_device_fatal = [&](const std::string& reason) {
        fatal_error = true;
        if (fatal_error_reason.empty()) fatal_error_reason = reason;
    };
    enum class PerfPhase { WaitingForBake, Warming, Sampling, Complete };
    constexpr std::uint32_t kPerfStaticStableFrames = 30u;
    PerfPhase perf_phase = PerfPhase::WaitingForBake;
    std::chrono::steady_clock::time_point perf_phase_start{};
    std::uint64_t perf_last_static_vertex_uploads = 0u;
    std::uint64_t perf_last_static_cluster_uploads = 0u;
    std::uint32_t perf_static_stable_frames = 0u;
    bool perf_observed_static_uploads = false;
    PerfCounters perf_start_counters{};
    uint64_t perf_start_dlss_resets = 0;
    std::vector<double> perf_frame_times;
    std::vector<double> perf_water_animation_times;
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

    auto reset_character_walk = [&]() {
        const bool was_walking = character_walk.enabled();
        character_walk.reset(session->ecs());
        character_jump.reset();
        if (was_walking) {
            camera_capture = false;
            camera_controller.set_capture(window, false, false);
        }
    };
    // Clears app-side models referencing a dead/reloaded world. The E5 scene
    // adapter resnapshots here; E4b clears selection + editor selection + sim.
    auto clear_app_models = [&]() {
        reset_character_walk();
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

    // Versioned agent JSONL rides the existing FIFO and dispatches these
    // metadata operations through this same registry. Results go to a separate
    // append-only file so human logs on stdout/stderr can never corrupt JSON.
    viewer::agent::OutputSink agent_output;
    if (agent_result_path)
        agent_output = viewer::agent::jsonl_file_sink(agent_result_path);
    // ONE identity snapshot, read at two different moments: the protocol takes
    // it when a terminal record is written, and the viewport.capture resolver
    // takes it on the frame the PNG was read back from. Those differ whenever a
    // later frame presented in between, which is exactly when a caller must not
    // use the newer numbers -- so the two must not be two hand-copied bodies.
    auto agent_context_snapshot = [&]() {
        viewer::agent::Context context;
        context.scene_ready = bake_ready;
        context.session_id = binding.current_session_id();
        context.session_generation = binding.current_generation();
        context.scene_generation = session ? session->graph_generation() : 0;
        context.scene_revision = editor_model.revision();
        context.selection_revision = selection_set.revision();
        // The presented-frame serial is both the exact framebuffer identity
        // and this first protocol version's conservative view identity. A
        // later frame is stale even when its camera happens to compare equal.
        context.frame_id = fifo_present.presented_frame_serial();
        context.view_id = fifo_present.presented_frame_serial();
        return context;
    };
    viewer::agent::Protocol agent_protocol(
        agent_result_path != nullptr, std::move(agent_output),
        agent_context_snapshot);
    agent_protocol.add_command({
        "agent.commands", "List commands and current availability", {}, "object",
        false, {}});
    agent_protocol.add_command({
        "agent.help", "Describe one command and its arguments",
        {{"command", "string", true, "Command name returned by agent.commands"}},
        "object", false, {}});
    agent_protocol.add_command({
        "agent.schema", "Return the v1 envelopes, limits and one command schema",
        {{"command", "string", true, "Command name returned by agent.commands"}},
        "object", false, {}});
    agent_protocol.add_command({
        "agent.subscribe", "Reserved event streaming operation", {}, "object", false,
        [](const viewer::agent::Context&) {
            return viewer::agent::Availability{
                false, viewer::agent::Status::UnsupportedCommand,
                "protocol v1 is request/result only; event subscription is unsupported"};
        }});
    // The two scene reads (scene_inventory.h). Both stay available before the
    // first bake finishes: `context.scene.ready` already tells a caller the
    // world is still filling in, and answering "nothing yet" is more useful
    // than refusing. Argument TYPES are checked by the descriptor; ranges and
    // enum spellings are checked at dispatch so they report invalid_input.
    agent_protocol.add_command({
        "scene.list_objects",
        "List scene objects with typed ids, ordered by (kind, id)",
        {{"kinds", "array", false,
          "Subset of [\"entity\",\"baked_root\"]; omitted means both"},
         {"name_contains", "string", false,
          "Case-insensitive substring of the object name; ids are never searched"},
         {"offset", "integer", false, "Rows to skip within the matched set"},
         {"limit", "integer", false, "Rows to return, 1 through 200 (default 100)"}},
        "object", false, {}});
    agent_protocol.add_command({
        "scene.get_object",
        "Inspect one object by typed id: naming, placement, visibility, "
        "selection, provenance availability and supported operations",
        {{"object", "object_id", true,
          "{kind,id} pair from scene.list_objects; entity and baked_root ids "
          "are separate namespaces"}},
        "object", false, {}});
    agent_protocol.add_command({
        "scene.trace_provenance",
        "Trace an inspected object to recorded procedural modules and inputs",
        {{"object", "object_id", true,
          "{kind,id} pair from scene.list_objects; entity and baked_root ids "
          "are separate namespaces"},
         {"max_depth", "integer", false, "Graph hops, 0 through 8 (default 3)"},
         {"max_nodes", "integer", false,
          "Returned graph nodes, 1 through 100 (default 64)"}},
        "object", false, {}});
    agent_protocol.add_command({
        "selection.replace", "Replace selection with typed scene objects",
        {{"objects", "array", true,
          "Non-empty unique {kind,id} array; last object becomes primary"}},
        "object", false, {}});
    agent_protocol.add_command({
        "selection.add", "Add typed scene objects to selection",
        {{"objects", "array", true,
          "Non-empty unique {kind,id} array; last newly added object becomes primary"}},
        "object", false, {}});
    agent_protocol.add_command({
        "selection.remove", "Remove typed scene objects from selection",
        {{"objects", "array", true,
          "Non-empty unique {kind,id} array; removed primary promotes last survivor"}},
        "object", false, {}});
    agent_protocol.add_command({
        "selection.toggle", "Toggle typed scene objects in selection",
        {{"objects", "array", true,
          "Non-empty unique {kind,id} array; last newly added object becomes primary"}},
        "object", false, {}});
    agent_protocol.add_command({
        "selection.clear", "Clear the shared editor selection", {}, "object", false,
        {}});
    agent_protocol.add_command({
        "selection.list", "List selected typed objects and the primary", {}, "object",
        false, {}});
    agent_protocol.add_command({
        "viewport.pick",
        "Read the object at viewport-local logical pixels without changing selection",
        {{"x", "number", true, "Viewport-local logical X pixel (left = 0)"},
         {"y", "number", true, "Viewport-local logical Y pixel (top = 0)"}},
        "object", false, {}});
    agent_protocol.add_command({
        "viewport.pick_select",
        "Pick at viewport-local logical pixels and update the shared selection",
        {{"x", "number", true, "Viewport-local logical X pixel (left = 0)"},
         {"y", "number", true, "Viewport-local logical Y pixel (top = 0)"},
         {"mode", "string", false,
          "replace (default), add, or toggle; a replace miss clears selection"}},
        "object", false, {}});
    agent_protocol.add_command({
        "viewport.capture",
        "Capture the next presented frame to a PNG and return its geometry, "
        "captured revisions and optional selection annotations",
        {{"path", "string", true,
          "Absolute .png path; the same safety rules the shot_now FIFO verb applies"},
         {"annotate_selection", "boolean", false,
          "Also return each selected object's typed id and projected image rectangle"}},
        "object", false, {}});
    agent_protocol.add_command({
        "view.focus",
        "Frame the camera on the selection, or on one named object without "
        "changing the selection",
        {{"object", "object_id", false,
          "{kind,id} pair to frame; omitted frames the current selection"}},
        "object", false, {}});
    // Regeneration job control (regen_jobs.h). `seed` and `job_id` are decimal
    // STRINGS for the same reason every other id here is: a 64-bit seed does
    // not survive an IEEE-754 double. Ranges and the per-operation seed rules
    // are checked at dispatch so they report invalid_input.
    agent_protocol.add_command({
        "job.start",
        "Queue a world reload or seeded regeneration and return its job id",
        {{"operation", "string", true,
          "\"reload\" (rebake the world as authored) or \"regenerate\" "
          "(rebake with a worldSeed override)"},
         {"seed", "string", false,
          "Unsigned 64-bit decimal string; required for \"regenerate\" and "
          "refused for \"reload\""}},
        "object", false, {}});
    agent_protocol.add_command({
        "job.status",
        "Read one regeneration job: state, progress, diagnostics and result",
        {{"job_id", "string", true, "Decimal job id returned by job.start"}},
        "object", false, {}});
    agent_protocol.add_command({
        "job.wait",
        "Wait, bounded by this request's timeout_ms, for one job to reach a "
        "terminal state",
        {{"job_id", "string", true, "Decimal job id returned by job.start"}},
        "object", false, {}});
    agent_protocol.add_command({
        "job.cancel",
        "Cancel a job that is still queued; report unsupported for one the "
        "engine is already running",
        {{"job_id", "string", true, "Decimal job id returned by job.start"}},
        "object", false, {}});
    agent_protocol.add_command({
        "job.list", "List the most recent regeneration jobs, oldest first",
        {{"limit", "integer", false, "Rows to return, 1 through 64 (default 16)"}},
        "object", false, {}});
    agent_protocol.add_command({
        "procedural.parameters",
        "Describe one baked root's typed effective procedural parameters",
        {{"object", "object_id", true,
          "baked_root identity returned by scene.list_objects"}},
        "object", false, {}});
    agent_protocol.add_command({
        "procedural.update",
        "Validate or atomically apply typed root parameter changes without rewriting JS",
        {{"object", "object_id", true,
          "baked_root identity returned by scene.list_objects"},
         {"changes", "object", true,
          "Non-empty object of declared parameter values"},
         {"dry_run", "boolean", false, "Validate only; default false"}},
        "object", false, {}});

    auto reg_agent_commands =
        registry.must_register_handler<viewer::AgentCommands>(
            matter::evt::CommandScope::App, app_lane,
            [&](const viewer::AgentCommands&) {
                return viewer::AgentCommands::Result::succeeded(
                    agent_protocol.discovery());
            });
    auto reg_agent_help = registry.must_register_handler<viewer::AgentHelp>(
        matter::evt::CommandScope::App, app_lane,
        [&](const viewer::AgentHelp& command) {
            return viewer::AgentHelp::Result::succeeded(
                agent_protocol.help(command.command));
        });
    auto reg_agent_schema = registry.must_register_handler<viewer::AgentSchema>(
        matter::evt::CommandScope::App, app_lane,
        [&](const viewer::AgentSchema& command) {
            return viewer::AgentSchema::Result::succeeded(
                agent_protocol.schema(command.command));
        });

    // --- scene.list_objects / scene.get_object (scene_inventory.h) ----------
    // The editor names two populations that are NOT one list: authored
    // entities (EditorModel rows, authored-id hashes or runtime ids) and baked roots
    // (part-graph roots, content-hash ids). This snapshot joins them without
    // merging their id namespaces, and is rebuilt per request on the app lane
    // — caching it would let a rebake or a world switch be answered from a
    // dead copy, which is exactly the failure the typed ids exist to prevent.
    // all_rows() rather than rows(): the outliner's search box must not
    // silently narrow a programmatic listing.
    auto build_scene_inventory = [&]() {
        std::vector<viewer::inventory::EntityRow> entity_rows;
        entity_rows.reserve(editor_model.all_rows().size());
        for (const viewer::HierarchyRow& row : editor_model.all_rows()) {
            viewer::inventory::EntityRow out;
            out.id = row.id.value;
            out.parent_id = row.parent_id.value;
            out.name = row.name;
            out.depth = row.depth;
            out.child_count = row.child_count;
            out.component_names = row.component_names;
            if (!session) {
                out.part_instance.reason = "no world session is open";
            } else {
                const flecs::entity entity = find_scene_entity(
                    session->ecs(), matter::scene::SceneEntityId{row.id.value});
                if (!entity.is_valid() ||
                    !entity.has<matter::scene::PartInstance>()) {
                    out.part_instance.reason =
                        "this entity has no PartInstance component in the live ECS";
                } else {
                    out.part_instance.available = true;
                    out.part_hash =
                        entity.get<matter::scene::PartInstance>().part_hash;
                }
            }
            entity_rows.push_back(std::move(out));
        }

        std::vector<viewer::inventory::RootRow> root_rows;
        if (session) {
            part_graph_snapshot::Snapshot graph;
            // False while the current session has published nothing; an empty
            // root list is then the honest answer, not a stale one.
            if (session->graph_snapshot(graph)) {
                for (const auto& [module_name, node] : graph.nodes) {
                    // Non-root nodes only exist composed inside other parts and
                    // have no world instance to name — the same rule
                    // scene_tree_panel.cpp and reveal_part.cpp apply.
                    if (!node.is_root) continue;
                    viewer::inventory::RootRow out;
                    out.resolved_hash = node.resolved_hash;
                    out.module = node.module.empty() ? module_name : node.module;
                    out.source_path = node.source_path;
                    out.params_json = node.params_json;
                    root_rows.push_back(std::move(out));
                }
            }
        }

        viewer::inventory::SelectionInput selection;
        const viewer::SelectedObject* primary = selection_set.primary();
        for (const viewer::SelectedObject& item : selection_set.items()) {
            viewer::agent::ObjectIdentity identity;
            identity.kind = item.kind == viewer::SelectedObject::BakedRoot
                                ? viewer::agent::ObjectIdentity::Kind::BakedRoot
                                : viewer::agent::ObjectIdentity::Kind::Entity;
            identity.id = item.id;
            if (primary && *primary == item)
                selection.primary_index = static_cast<int>(selection.items.size());
            selection.items.push_back(identity);
        }

        return viewer::inventory::build_snapshot(entity_rows, root_rows, selection,
                                                 editor_model.revision());
    };

    // Convert the session's locked deep copy into editor-owned plain data
    // before serializing a trace.  No response retains worker-owned pointers.
    auto build_provenance_graph = [&]() {
        std::vector<viewer::inventory::ProvenanceNode> nodes;
        if (!session) return nodes;
        part_graph_snapshot::Snapshot graph;
        if (!session->graph_snapshot(graph)) return nodes;
        nodes.reserve(graph.nodes.size());
        for (const auto& [module_name, node] : graph.nodes) {
            viewer::inventory::ProvenanceNode out;
            out.module = node.module.empty() ? module_name : node.module;
            out.source_path = node.source_path;
            out.params_json = node.params_json;
            out.children = node.children;
            out.shared_imports = node.shared_imports;
            out.shared_source_paths = node.shared_source_paths;
            out.resolved_hash = node.resolved_hash;
            out.is_root = node.is_root;
            nodes.push_back(std::move(out));
        }
        return nodes;
    };

    // The published root snapshot is the authoritative bridge from a typed
    // baked-root identity to its module and effective canonical parameters.
    auto procedural_root_for = [&](const viewer::agent::ObjectIdentity& object,
                                   viewer::procedural::Root& out) {
        if (!session || object.kind !=
                             viewer::agent::ObjectIdentity::Kind::BakedRoot)
            return false;
        part_graph_snapshot::Snapshot graph;
        if (!session->graph_snapshot(graph)) return false;
        for (const auto& entry : graph.nodes) {
            const auto& node = entry.second;
            if (!node.is_root || node.resolved_hash != object.id) continue;
            out.object = object;
            out.module = node.module.empty() ? entry.first : node.module;
            out.source_path = node.source_path;
            out.params_json = node.params_json;
            return true;
        }
        return false;
    };
    auto procedural_root_module_count = [&](const std::string& module) {
        if (!session) return std::size_t{0};
        part_graph_snapshot::Snapshot graph;
        if (!session->graph_snapshot(graph)) return std::size_t{0};
        std::size_t count = 0;
        for (const auto& entry : graph.nodes) {
            const auto& node = entry.second;
            if (node.is_root && (node.module.empty() ? entry.first : node.module) == module)
                ++count;
        }
        return count;
    };

    auto reg_scene_list_objects =
        registry.must_register_handler<viewer::SceneListObjects>(
            matter::evt::CommandScope::App, app_lane,
            [&](const viewer::SceneListObjects& command) {
                viewer::AgentPayload payload;
                viewer::inventory::ListQuery query;
                std::string error;
                // Re-parsed rather than trusted: the dispatch site validated an
                // identical copy, and a handler that assumes its input was
                // checked elsewhere is one refactor from not being.
                if (!viewer::inventory::parse_list_query(command.arguments, query,
                                                         error)) {
                    payload.status = viewer::agent::Status::InvalidInput;
                    payload.message = error;
                    return viewer::SceneListObjects::Result::succeeded(
                        std::move(payload));
                }
                const viewer::inventory::Snapshot snapshot = build_scene_inventory();
                payload.value = viewer::inventory::list_result_json(
                    snapshot, query,
                    viewer::inventory::list_objects(snapshot, query));
                return viewer::SceneListObjects::Result::succeeded(std::move(payload));
            });

    auto reg_scene_get_object =
        registry.must_register_handler<viewer::SceneGetObject>(
            matter::evt::CommandScope::App, app_lane,
            [&](const viewer::SceneGetObject& command) {
                const bool is_entity =
                    command.object.kind ==
                    viewer::agent::ObjectIdentity::Kind::Entity;
                const viewer::inventory::Snapshot snapshot = build_scene_inventory();
                viewer::AgentPayload payload;
                const viewer::inventory::Entry* entry =
                    viewer::inventory::find_object(snapshot, command.object);
                if (!entry) {
                    // A deleted entity, a root a rebake replaced, or an id that
                    // is only valid in the OTHER kind's namespace. All three are
                    // ordinary answers, and all three name the revision they are
                    // true at so a caller can tell them from a stale question.
                    payload.status = viewer::agent::Status::NotFound;
                    payload.message = "no such object at this scene revision";
                    payload.value = viewer::inventory::missing_result_json(
                        snapshot, command.object,
                        is_entity
                            ? "no authored entity carries this id in the current "
                              "session"
                            : "no baked root with this content hash is in the "
                              "current part graph");
                    return viewer::SceneGetObject::Result::succeeded(
                        std::move(payload));
                }

                viewer::inventory::Detail detail;
                detail.entry = *entry;
                detail.operations = viewer::inventory::default_operations(
                    command.object.kind, entry->source_path.available);

                if (!session) {
                    detail.placement.reason = "no world session is open";
                    detail.visibility.reason = "no world session is open";
                } else {
                    // One shared implementation with the selection outline and
                    // the pick raycast (selection_bounds.h), so the box an agent
                    // reads is the box the user sees.
                    const viewer::SelectedObject target{
                        is_entity ? viewer::SelectedObject::Entity
                                  : viewer::SelectedObject::BakedRoot,
                        command.object.id};
                    viewer::SelectionBounds bounds{};
                    bool resolved = false;
                    viewer::bounds_for_objects(&target, 1, *session, &bounds,
                                               &resolved);
                    if (resolved) {
                        detail.placement.available = true;
                        std::copy(bounds.world_matrix, bounds.world_matrix + 16,
                                  detail.world_matrix);
                        std::copy(bounds.local_min, bounds.local_min + 3,
                                  detail.local_min);
                        std::copy(bounds.local_max, bounds.local_max + 3,
                                  detail.local_max);
                    } else {
                        detail.placement.reason =
                            is_entity
                                ? "this entity is in the scene rows but has no "
                                  "resolvable transform in the live ECS"
                                : "this baked root is in the part graph but has "
                                  "no placed instance in the current world";
                    }

                    if (!is_entity) {
                        detail.visibility.reason =
                            "baked roots carry no authored visibility flag; "
                            "on-screen presence is decided by LOD and culling";
                    } else {
                        const flecs::entity e = find_scene_entity(
                            session->ecs(),
                            matter::scene::SceneEntityId{command.object.id});
                        if (e.is_valid() && e.has<matter::scene::PartInstance>()) {
                            detail.visibility.available = true;
                            detail.visible =
                                e.get<matter::scene::PartInstance>().visible;
                        } else {
                            detail.visibility.reason =
                                "this entity places no part, so it carries no "
                                "authored visibility flag";
                        }
                    }
                }

                payload.value =
                    viewer::inventory::detail_result_json(snapshot, detail);
                return viewer::SceneGetObject::Result::succeeded(std::move(payload));
            });

    auto reg_scene_trace_provenance =
        registry.must_register_handler<viewer::SceneTraceProvenance>(
            matter::evt::CommandScope::App, app_lane,
            [&](const viewer::SceneTraceProvenance& command) {
                viewer::AgentPayload payload;
                viewer::inventory::TraceQuery query;
                std::string error;
                if (!viewer::inventory::parse_trace_query(command.arguments, query,
                                                          error)) {
                    payload.status = viewer::agent::Status::InvalidInput;
                    payload.message = error;
                    return viewer::SceneTraceProvenance::Result::succeeded(
                        std::move(payload));
                }
                const viewer::inventory::Snapshot snapshot = build_scene_inventory();
                if (!viewer::inventory::find_object(snapshot, query.object)) {
                    payload.status = viewer::agent::Status::NotFound;
                    payload.message = "no such object at this scene revision";
                    payload.value = viewer::inventory::missing_result_json(
                        snapshot, query.object,
                        "this typed object was deleted or replaced before its "
                        "provenance could be traced");
                    return viewer::SceneTraceProvenance::Result::succeeded(
                        std::move(payload));
                }
                payload.value = viewer::inventory::trace_result_json(
                    snapshot, build_provenance_graph(), query);
                return viewer::SceneTraceProvenance::Result::succeeded(
                    std::move(payload));
            });

    // Selection commands share exactly the SelectionSet read by picking,
    // outlines and the gizmo.  The auxiliary Scene-tree highlight is only a
    // projection of its primary item, never a second selection authority.
    auto prune_selection_to_inventory = [&]() {
        viewer::inventory::Snapshot snapshot = build_scene_inventory();
        selection_set.validate([&](const viewer::SelectedObject& item) {
            const viewer::agent::ObjectIdentity object{
                item.kind == viewer::SelectedObject::BakedRoot
                    ? viewer::agent::ObjectIdentity::Kind::BakedRoot
                    : viewer::agent::ObjectIdentity::Kind::Entity,
                item.id};
            return viewer::inventory::find_object(snapshot, object) != nullptr;
        });
        // build_scene_inventory embeds SelectionSet state, so refresh it after
        // pruning before returning it to a command or a response serializer.
        return build_scene_inventory();
    };
    auto mirror_selection_primary = [&]() {
        const viewer::SelectedObject* primary = selection_set.primary();
        if (primary && primary->kind == viewer::SelectedObject::Entity) {
            editor_model.select(matter::scene::SceneEntityId{primary->id});
            ui.select_baked_root(0);
        } else if (primary) {
            editor_model.clear_selection();
            ui.select_baked_root(primary->id);
        } else {
            editor_model.clear_selection();
            ui.select_baked_root(0);
        }
    };
    auto selection_change = [&](viewer::selection_command::Operation operation,
                                const std::vector<viewer::agent::ObjectIdentity>& objects) {
        viewer::AgentPayload payload;
        const uint64_t selection_revision_before = selection_set.revision();
        viewer::inventory::Snapshot snapshot = prune_selection_to_inventory();
        const viewer::selection_command::ApplyResult applied =
            viewer::selection_command::apply(selection_set, snapshot, operation, objects);
        payload.status = applied.status;
        payload.message = applied.message;
        const bool pruned = selection_set.revision() != selection_revision_before &&
                            !applied.changed;
        if (pruned || applied.changed) mirror_selection_primary();
        if (applied.status != viewer::agent::Status::Ok) return payload;
        snapshot = build_scene_inventory();
        payload.value = viewer::selection_command::selection_json(
            snapshot, selection_set, operation, applied.changed);
        return payload;
    };
    auto reg_selection_replace =
        registry.must_register_handler<viewer::SelectionReplace>(
            matter::evt::CommandScope::App, app_lane,
            [&](const viewer::SelectionReplace& command) {
                return viewer::SelectionReplace::Result::succeeded(
                    selection_change(viewer::selection_command::Operation::Replace,
                                     command.objects));
            });
    auto reg_selection_add = registry.must_register_handler<viewer::SelectionAdd>(
        matter::evt::CommandScope::App, app_lane,
        [&](const viewer::SelectionAdd& command) {
            return viewer::SelectionAdd::Result::succeeded(
                selection_change(viewer::selection_command::Operation::Add,
                                 command.objects));
        });
    auto reg_selection_remove =
        registry.must_register_handler<viewer::SelectionRemove>(
            matter::evt::CommandScope::App, app_lane,
            [&](const viewer::SelectionRemove& command) {
                return viewer::SelectionRemove::Result::succeeded(
                    selection_change(viewer::selection_command::Operation::Remove,
                                     command.objects));
            });
    auto reg_selection_toggle =
        registry.must_register_handler<viewer::SelectionToggle>(
            matter::evt::CommandScope::App, app_lane,
            [&](const viewer::SelectionToggle& command) {
                return viewer::SelectionToggle::Result::succeeded(
                    selection_change(viewer::selection_command::Operation::Toggle,
                                     command.objects));
            });
    auto reg_selection_clear =
        registry.must_register_handler<viewer::SelectionClear>(
            matter::evt::CommandScope::App, app_lane,
            [&](const viewer::SelectionClear&) {
                return viewer::SelectionClear::Result::succeeded(
                    selection_change(viewer::selection_command::Operation::Clear, {}));
            });
    auto reg_selection_list = registry.must_register_handler<viewer::SelectionList>(
        matter::evt::CommandScope::App, app_lane, [&](const viewer::SelectionList&) {
            const uint64_t selection_revision_before = selection_set.revision();
            const viewer::inventory::Snapshot snapshot = prune_selection_to_inventory();
            if (selection_set.revision() != selection_revision_before)
                mirror_selection_primary();
            viewer::AgentPayload payload;
            payload.value = viewer::selection_command::selection_json(
                snapshot, selection_set, viewer::selection_command::Operation::List,
                false);
            return viewer::SelectionList::Result::succeeded(std::move(payload));
        });

    // Viewport agent commands use the exact same picker as the interactive
    // click below: GPU identity first (the only path that sees streamed
    // geometry), then its established CPU OBB fallback. The external x/y pair
    // is local to the measured ImGui viewport in LOGICAL pixels. The response
    // records both that rectangle and the GLFW framebuffer scale so a caller
    // never has to guess whether it should send screenshot pixels.
    auto viewport_pick_result_json = [&](const viewer::PickResult& pick,
                                         const viewer::ViewportRect& vp,
                                         const matter::CameraDesc& pick_camera,
                                         float x, float y) {
        // agent_protocol.h's shared value constructors -- one spelling of
        // "a JSON number" for every hand-built payload in this file.
        using JsonValue = matter::jsondoc::Value;
        using viewer::agent::json::boolean;
        using viewer::agent::json::number;
        using viewer::agent::json::object;
        using viewer::agent::json::string;
        using viewer::agent::json::unavailable;
        using viewer::agent::json::vec3;

        int framebuffer_width = 0;
        int framebuffer_height = 0;
        glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float scale_x = display.x > 0.0f
                                  ? static_cast<float>(framebuffer_width) / display.x
                                  : 1.0f;
        const float scale_y = display.y > 0.0f
                                  ? static_cast<float>(framebuffer_height) / display.y
                                  : 1.0f;

        JsonValue result = object();
        JsonValue viewport = object();
        JsonValue logical = object();
        logical.set("x", number(vp.x));
        logical.set("y", number(vp.y));
        logical.set("width", number(vp.w));
        logical.set("height", number(vp.h));
        viewport.set("logical", std::move(logical));
        JsonValue framebuffer = object();
        framebuffer.set("x", number(vp.x * scale_x));
        framebuffer.set("y", number(vp.y * scale_y));
        framebuffer.set("width", number(vp.w * scale_x));
        framebuffer.set("height", number(vp.h * scale_y));
        viewport.set("framebuffer", std::move(framebuffer));
        JsonValue scale = object();
        scale.set("x", number(scale_x));
        scale.set("y", number(scale_y));
        viewport.set("framebuffer_scale", std::move(scale));
        result.set("viewport", std::move(viewport));

        JsonValue coordinate = object();
        coordinate.set("x", number(x));
        coordinate.set("y", number(y));
        coordinate.set("space", string("viewport_local_logical_pixels"));
        JsonValue framebuffer_coordinate = object();
        framebuffer_coordinate.set("x", number(x * scale_x));
        framebuffer_coordinate.set("y", number(y * scale_y));
        coordinate.set("framebuffer", std::move(framebuffer_coordinate));
        result.set("coordinate", std::move(coordinate));

        JsonValue camera_json = object();
        camera_json.set("position", vec3(pick_camera.position.x, pick_camera.position.y,
                                             pick_camera.position.z));
        camera_json.set("target", vec3(pick_camera.target.x, pick_camera.target.y,
                                           pick_camera.target.z));
        camera_json.set("up", vec3(pick_camera.up.x, pick_camera.up.y,
                                      pick_camera.up.z));
        camera_json.set("vertical_fov_radians",
                        number(pick_camera.vertical_fov_radians));
        camera_json.set("near_plane", number(pick_camera.near_plane));
        camera_json.set("far_plane", number(pick_camera.far_plane));
        result.set("camera", std::move(camera_json));
        JsonValue frame = object();
        frame.set("id", string(std::to_string(fifo_present.presented_frame_serial())));
        frame.set("view_id", string(std::to_string(fifo_present.presented_frame_serial())));
        result.set("presented", std::move(frame));

        result.set("hit", boolean(pick.hit));
        if (pick.hit) {
            result.set("object", viewer::agent::object_identity_json(
                {pick.object.kind == viewer::SelectedObject::BakedRoot
                     ? viewer::agent::ObjectIdentity::Kind::BakedRoot
                     : viewer::agent::ObjectIdentity::Kind::Entity,
                 pick.object.id}));
        } else {
            result.set("object", JsonValue{});
        }
        if (pick.geometry == viewer::PickGeometry::GpuIdentity) {
            JsonValue geometry = object();
            geometry.set("source", string("gpu_identity"));
            geometry.set("world_position", unavailable(
                "GPU identity picking exposes no depth or world position"));
            geometry.set("distance_meters", unavailable(
                "GPU identity picking exposes no depth or ray distance"));
            result.set("geometry", std::move(geometry));
        } else if (pick.geometry == viewer::PickGeometry::CpuObbFallback) {
            JsonValue geometry = object();
            geometry.set("source", string("cpu_obb_fallback"));
            JsonValue world_position = object();
            world_position.set("available", boolean(true));
            world_position.set("value", vec3(pick.world_position.x,
                                                pick.world_position.y,
                                                pick.world_position.z));
            geometry.set("world_position", std::move(world_position));
            JsonValue distance = object();
            distance.set("available", boolean(true));
            distance.set("value", number(pick.distance));
            geometry.set("distance_meters", std::move(distance));
            result.set("geometry", std::move(geometry));
        } else {
            JsonValue geometry = object();
            geometry.set("source", string("none"));
            geometry.set("world_position", unavailable("no object was hit"));
            geometry.set("distance_meters", unavailable("no object was hit"));
            result.set("geometry", std::move(geometry));
        }
        return result;
    };
    auto perform_viewport_pick = [&](float x, float y,
                                     const viewer::ViewportPickSelect::Mode* mode) {
        viewer::AgentPayload payload;
        const viewer::ViewportRect vp = ui.viewport_rect();
        if (vp.w <= 0.0f || vp.h <= 0.0f) {
            payload.status = viewer::agent::Status::NotReady;
            payload.message = "the viewport has no drawable logical-pixel rectangle";
            return payload;
        }
        if (!presented_camera_available) {
            payload.status = viewer::agent::Status::NotReady;
            payload.message = "no viewport frame has been presented yet";
            return payload;
        }
        if (!presented_production_view) {
            payload.status = viewer::agent::Status::NotReady;
            payload.message = "the Part Workbench isolation view owns the viewport";
            return payload;
        }
        if (presented_session_generation != binding.current_generation()) {
            payload.status = viewer::agent::Status::NotReady;
            payload.message = "the active world session has not presented a viewport frame";
            return payload;
        }
        if (x >= vp.w || y >= vp.h) {
            payload.status = viewer::agent::Status::InvalidInput;
            payload.message = "viewport-local logical coordinates are outside the current viewport";
            return payload;
        }
        const viewer::PickResult pick = viewer::viewport_pick(
            x, y, static_cast<int>(vp.w), static_cast<int>(vp.h), presented_camera,
            *session);
        payload.value = viewport_pick_result_json(pick, vp, presented_camera, x, y);
        if (!mode) return payload;

        viewer::selection_command::Operation operation =
            viewer::selection_command::Operation::Replace;
        if (*mode == viewer::ViewportPickSelect::Mode::Add)
            operation = viewer::selection_command::Operation::Add;
        else if (*mode == viewer::ViewportPickSelect::Mode::Toggle)
            operation = viewer::selection_command::Operation::Toggle;

        // A replace miss matches an ordinary GUI click on empty space: clear
        // selection. Add/toggle misses leave it untouched, matching modifier
        // gesture expectations while still reporting the unambiguous miss.
        viewer::AgentPayload selected;
        if (pick.hit) {
            const viewer::agent::ObjectIdentity object{
                pick.object.kind == viewer::SelectedObject::BakedRoot
                    ? viewer::agent::ObjectIdentity::Kind::BakedRoot
                    : viewer::agent::ObjectIdentity::Kind::Entity,
                pick.object.id};
            selected = selection_change(operation, {object});
        } else if (*mode == viewer::ViewportPickSelect::Mode::Replace) {
            selected = selection_change(viewer::selection_command::Operation::Clear, {});
        } else {
            const uint64_t before = selection_set.revision();
            const viewer::inventory::Snapshot snapshot = prune_selection_to_inventory();
            if (selection_set.revision() != before) mirror_selection_primary();
            selected.value = viewer::selection_command::selection_json(
                snapshot, selection_set, operation, false);
        }
        payload.status = selected.status;
        payload.message = selected.message;
        if (selected.value.kind == matter::jsondoc::Value::Kind::Object)
            payload.value.set("selection", std::move(selected.value));
        else {
            const viewer::inventory::Snapshot snapshot = prune_selection_to_inventory();
            payload.value.set("selection", viewer::selection_command::selection_json(
                snapshot, selection_set, operation, false));
        }
        return payload;
    };
    auto reg_viewport_pick = registry.must_register_handler<viewer::ViewportPick>(
        matter::evt::CommandScope::App, app_lane, [&](const viewer::ViewportPick& command) {
            return viewer::ViewportPick::Result::succeeded(
                perform_viewport_pick(command.x, command.y, nullptr));
        });
    auto reg_viewport_pick_select =
        registry.must_register_handler<viewer::ViewportPickSelect>(
            matter::evt::CommandScope::App, app_lane,
            [&](const viewer::ViewportPickSelect& command) {
                return viewer::ViewportPickSelect::Result::succeeded(
                    perform_viewport_pick(command.x, command.y, &command.mode));
            });

    // The one baked-root bounds provider for camera framing. camera_focus.h is
    // session-free by design, so every framing site has to supply this; sharing
    // one lambda is what keeps `viewer.reveal_part`, the viewport orbit pivot
    // and `view.focus` framing the SAME box.
    auto baked_root_bounds = [&](uint64_t part_hash, viewer::SelectionBounds& out) {
        viewer::SelectedObject obj{viewer::SelectedObject::BakedRoot, part_hash};
        return viewer::bounds_for_object(obj, *session, out);
    };

    // view.focus — the F key's framing, addressable. `args.object` frames one
    // named object WITHOUT selecting it: framing is a view operation, and an
    // agent that wanted the selection changed has selection.replace.
    auto reg_view_focus = registry.must_register_handler<viewer::ViewFocus>(
        matter::evt::CommandScope::App, app_lane,
        [&](const viewer::ViewFocus& command) {
            using JsonValue = matter::jsondoc::Value;
            using viewer::agent::json::number;
            using viewer::agent::json::object;
            using viewer::agent::json::string;
            using viewer::agent::json::vec3;
            const auto camera_json = [&](const matter::CameraDesc& desc) {
                JsonValue value = object();
                value.set("position",
                          vec3(desc.position.x, desc.position.y, desc.position.z));
                value.set("target",
                          vec3(desc.target.x, desc.target.y, desc.target.z));
                value.set("up", vec3(desc.up.x, desc.up.y, desc.up.z));
                value.set("vertical_fov_radians", number(desc.vertical_fov_radians));
                value.set("near_plane", number(desc.near_plane));
                value.set("far_plane", number(desc.far_plane));
                return value;
            };

            viewer::AgentPayload payload;
            if (!session) {
                payload.status = viewer::agent::Status::NotReady;
                payload.message = "no world session is loaded";
                return viewer::ViewFocus::Result::succeeded(std::move(payload));
            }
            const uint64_t selection_revision_before = selection_set.revision();
            const viewer::inventory::Snapshot snapshot = prune_selection_to_inventory();
            if (selection_set.revision() != selection_revision_before)
                mirror_selection_primary();

            // A scratch set for the object form. It never touches the shared
            // SelectionSet, so `view.focus {object}` cannot move the gizmo,
            // the outline or the Scene tree highlight.
            viewer::SelectionSet scratch;
            const viewer::SelectionSet* framed = &selection_set;
            if (command.has_object) {
                if (!viewer::inventory::find_object(snapshot, command.object)) {
                    payload.status = viewer::agent::Status::NotFound;
                    payload.message =
                        "no object with that typed id exists at this scene revision";
                    return viewer::ViewFocus::Result::succeeded(std::move(payload));
                }
                scratch.replace(viewer::selection_command::selected(command.object));
                framed = &scratch;
            } else if (selection_set.empty()) {
                payload.status = viewer::agent::Status::NotFound;
                payload.message =
                    "the editor selection is empty; pass args.object to frame "
                    "one object";
                return viewer::ViewFocus::Result::succeeded(std::move(payload));
            }

            // Asked first, and separately, so "nothing here resolves to bounds
            // yet" is an explicit not_ready rather than a silent no-op camera.
            // focus_camera_on_selection leaves the camera alone in exactly this
            // case, which is indistinguishable from success from outside.
            matter::Float3 center{};
            float radius = 0.0f;
            if (!viewer::selection_focus_point(*framed, field_commands,
                                               baked_root_bounds, center, radius)) {
                payload.status = viewer::agent::Status::NotReady;
                payload.message =
                    "nothing in the focus target resolves to bounds in this "
                    "world yet";
                return viewer::ViewFocus::Result::succeeded(std::move(payload));
            }
            const matter::CameraDesc before = camera;
            viewer::focus_camera_on_selection(camera, *framed, field_commands,
                                              baked_root_bounds);

            JsonValue result = object();
            JsonValue target = object();
            target.set("mode", string(command.has_object ? "object" : "selection"));
            if (command.has_object)
                target.set("object", viewer::agent::object_identity_json(command.object));
            else
                target.set("object", JsonValue{});
            JsonValue framed_objects = viewer::agent::json::array();
            for (const viewer::SelectedObject& item : framed->items()) {
                framed_objects.arr.push_back(viewer::agent::object_identity_json(
                    {item.kind == viewer::SelectedObject::BakedRoot
                         ? viewer::agent::ObjectIdentity::Kind::BakedRoot
                         : viewer::agent::ObjectIdentity::Kind::Entity,
                     item.id}));
            }
            target.set("objects", std::move(framed_objects));
            result.set("target", std::move(target));
            JsonValue focus = object();
            focus.set("center", vec3(center.x, center.y, center.z));
            focus.set("radius_meters", number(radius));
            result.set("focus", std::move(focus));
            JsonValue camera_block = object();
            camera_block.set("before", camera_json(before));
            camera_block.set("after", camera_json(camera));
            result.set("camera", std::move(camera_block));
            // The camera moves at the NEXT presented frame, so the frame the
            // caller can pick against is not this one. Say so rather than let
            // an agent assume the shot it took a moment ago still matches.
            result.set("applies_at", string("next_presented_frame"));
            result.set("scene_revision",
                       viewer::agent::json::decimal(snapshot.scene_revision));
            payload.value = std::move(result);
            return viewer::ViewFocus::Result::succeeded(std::move(payload));
        });

    // --- regeneration job control (regen_jobs.h) ---------------------------
    // job.start ACCEPTS: it records a ledger entry and returns. The heavy
    // session operation runs at the post-frame seam below, because destroying
    // and rebuilding world content mid-draw is exactly what SessionBinding
    // exists to prevent. Everything else here is a read of that ledger.
    auto reg_job_start = registry.must_register_handler<viewer::JobStart>(
        matter::evt::CommandScope::App, app_lane,
        [&](const viewer::JobStart& command) {
            viewer::AgentPayload payload;
            if (!session) {
                payload.status = viewer::agent::Status::NotReady;
                payload.message = "no world session is open to regenerate";
                return viewer::JobStart::Result::succeeded(std::move(payload));
            }
            viewer::jobs::StartRequest request = command.request;
            // The world is the editor's, not the caller's: naming it in the
            // record is what makes a job id readable after a world switch.
            request.world = worlds[stats.world_current].world_name;
            request.project = worlds[stats.world_current].project_dir;
            const uint64_t id = regen_jobs.accept(
                std::move(request), editor_model.revision(),
                session->graph_generation(), viewer::jobs::Clock::now());
            const viewer::jobs::Job* job = regen_jobs.find(id);
            payload.value = viewer::jobs::start_result_json(
                *job, viewer::jobs::Clock::now());
            return viewer::JobStart::Result::succeeded(std::move(payload));
        });

    auto reg_procedural_parameters =
        registry.must_register_handler<viewer::ProceduralParameters>(
            matter::evt::CommandScope::App, app_lane,
            [&](const viewer::ProceduralParameters& command) {
                viewer::AgentPayload payload;
                viewer::procedural::Root root;
                if (!procedural_root_for(command.object, root)) {
                    payload.status = viewer::agent::Status::NotFound;
                    payload.message = "no published baked root matches this typed identity";
                } else {
                    payload.value = viewer::procedural::describe_json(root);
                }
                return viewer::ProceduralParameters::Result::succeeded(
                    std::move(payload));
            });

    auto reg_procedural_update =
        registry.must_register_handler<viewer::ProceduralUpdate>(
            matter::evt::CommandScope::App, app_lane,
            [&](const viewer::ProceduralUpdate& command) {
                viewer::AgentPayload payload;
                viewer::procedural::Root root;
                if (!procedural_root_for(command.object, root)) {
                    payload.status = viewer::agent::Status::NotFound;
                    payload.message = "no published baked root matches this typed identity";
                    return viewer::ProceduralUpdate::Result::succeeded(
                        std::move(payload));
                }
                if (procedural_root_module_count(root.module) != 1) {
                    payload.status = viewer::agent::Status::UnsupportedCommand;
                    payload.message =
                        "this module is published as multiple roots; a module-wide "
                        "override would not identify one owner";
                    return viewer::ProceduralUpdate::Result::succeeded(
                        std::move(payload));
                }
                viewer::procedural::Plan plan;
                std::string error;
                if (!viewer::procedural::make_plan(root, command.changes, plan, error)) {
                    payload.status = viewer::agent::Status::InvalidInput;
                    payload.message = error;
                    return viewer::ProceduralUpdate::Result::succeeded(
                        std::move(payload));
                }
                payload.value = viewer::procedural::plan_json(plan, command.dry_run);
                if (!command.dry_run && !plan.changed.empty()) {
                    viewer::jobs::StartRequest request;
                    request.kind = viewer::jobs::Kind::Parameters;
                    request.parameter_module = root.module;
                    request.parameters_json = matter::jsondoc::write_json(plan.after);
                    request.world = worlds[stats.world_current].world_name;
                    request.project = worlds[stats.world_current].project_dir;
                    const uint64_t id = regen_jobs.accept(
                        std::move(request), editor_model.revision(),
                        session->graph_generation(), viewer::jobs::Clock::now());
                    const viewer::jobs::Job* job = regen_jobs.find(id);
                    payload.value.set("job", viewer::jobs::start_result_json(
                        *job, viewer::jobs::Clock::now()));
                }
                return viewer::ProceduralUpdate::Result::succeeded(
                    std::move(payload));
            });

    auto reg_job_status = registry.must_register_handler<viewer::JobStatus>(
        matter::evt::CommandScope::App, app_lane,
        [&](const viewer::JobStatus& command) {
            viewer::AgentPayload payload;
            const viewer::jobs::Job* job = regen_jobs.find(command.job_id);
            if (!job) {
                // A read of a job that is gone is not_found with the window
                // that IS answerable, so "aged out" and "never existed" stay
                // distinguishable.
                payload.status = viewer::agent::Status::NotFound;
                payload.message = "no such regeneration job at this id";
                payload.value =
                    viewer::jobs::missing_result_json(regen_jobs, command.job_id);
                return viewer::JobStatus::Result::succeeded(std::move(payload));
            }
            // A status READ succeeds even when the job it describes failed:
            // the failure is in `job.state`, not in the query.
            payload.value =
                viewer::jobs::status_result_json(*job, viewer::jobs::Clock::now());
            return viewer::JobStatus::Result::succeeded(std::move(payload));
        });

    auto reg_job_list = registry.must_register_handler<viewer::JobList>(
        matter::evt::CommandScope::App, app_lane,
        [&](const viewer::JobList& command) {
            viewer::AgentPayload payload;
            payload.value = viewer::jobs::list_result_json(
                regen_jobs, command.limit, viewer::jobs::Clock::now());
            return viewer::JobList::Result::succeeded(std::move(payload));
        });

    auto reg_job_cancel = registry.must_register_handler<viewer::JobCancel>(
        matter::evt::CommandScope::App, app_lane,
        [&](const viewer::JobCancel& command) {
            viewer::AgentPayload payload;
            const viewer::jobs::CancelOutcome outcome =
                regen_jobs.cancel(command.job_id, viewer::jobs::Clock::now());
            if (outcome == viewer::jobs::CancelOutcome::NotFound) {
                payload.status = viewer::agent::Status::NotFound;
                payload.message = "no such regeneration job at this id";
                payload.value =
                    viewer::jobs::missing_result_json(regen_jobs, command.job_id);
                return viewer::JobCancel::Result::succeeded(std::move(payload));
            }
            const viewer::jobs::Job* job = regen_jobs.find(command.job_id);
            if (outcome == viewer::jobs::CancelOutcome::UnsupportedRunning) {
                // WorldSession has no cancel entry point -- supersession is
                // the only mechanism the backend has. Saying so with the
                // protocol's own "known but unavailable" code is the honest
                // answer; reporting ok would claim a stop that never happened.
                payload.status = viewer::agent::Status::UnsupportedCommand;
                payload.message =
                    "this job is already running and the engine exposes no "
                    "cancel; start a newer job to supersede it";
            }
            payload.value = viewer::jobs::cancel_result_json(
                *job, outcome, viewer::jobs::Clock::now());
            return viewer::JobCancel::Result::succeeded(std::move(payload));
        });

    // job.wait is the second command (after viewport.capture) whose answer is
    // not knowable on the app lane. The handler decides only whether a wait can
    // be ARMED -- an already-terminal job answers here and now, a missing one
    // is not_found -- and the frame loop below emits the one terminal record
    // when the job ends or the deadline passes.
    auto reg_job_wait = registry.must_register_handler<viewer::JobWait>(
        matter::evt::CommandScope::App, app_lane,
        [&](const viewer::JobWait& command) {
            viewer::AgentPayload payload;
            const viewer::jobs::Job* job = regen_jobs.find(command.job_id);
            if (!job) {
                payload.status = viewer::agent::Status::NotFound;
                payload.message = "no such regeneration job at this id";
                payload.value =
                    viewer::jobs::missing_result_json(regen_jobs, command.job_id);
            } else if (regen_waits.size() >= viewer::jobs::kMaxWaiters) {
                payload.status = viewer::agent::Status::NotReady;
                payload.message = "too many bounded waits are already pending";
            }
            return viewer::JobWait::Result::succeeded(std::move(payload));
        });

    // The ONE place a released bounded wait becomes a terminal result. Three
    // callers: the frame-loop sweep, the shutdown drain after the loop, and the
    // dispatch bridge's immediate answer for a job that was already terminal.
    // Completing a request the protocol has already expired is a no-op, which
    // is what keeps "exactly one terminal record" true here too.
    auto resolve_regen_wait = [&](const viewer::jobs::Waiter& waiter,
                                  bool timed_out) {
        const viewer::jobs::Job* job = regen_jobs.find(waiter.job_id);
        const viewer::jobs::Clock::time_point now = viewer::jobs::Clock::now();
        if (!job) {
            // The job aged out of the retained window while the wait was open.
            // There is no outcome to report, so this is a timeout rather than
            // an invented completion.
            agent_protocol.complete(
                waiter.request_id, waiter.ticket_id, viewer::agent::Status::Timeout,
                viewer::jobs::missing_result_json(regen_jobs, waiter.job_id),
                "the job aged out of the retained history before this wait ended");
            return;
        }
        const viewer::agent::Status status =
            timed_out ? viewer::agent::Status::Timeout
                      : viewer::jobs::wait_status(job->state);
        const std::string message =
            timed_out ? "the bounded wait expired with the job in state '" +
                            std::string(viewer::jobs::to_string(job->state)) + "'"
                      : viewer::jobs::wait_message(*job);
        agent_protocol.complete(
            waiter.request_id, waiter.ticket_id, status,
            viewer::jobs::wait_result_json(*job, timed_out, now), message);
    };

    // FNV-1a over the published part-graph ROOTS, sorted, so "the same seed
    // produced the same world" is answerable. It is the roots and their
    // resolved content hashes -- the same identities scene.list_objects hands
    // out as baked_root ids -- and not a render or a screenshot, so it is
    // decided by what was baked rather than by what a frame happened to draw.
    //
    // An EMPTY root set is reported as unavailable rather than digested. A
    // world-kind (streamed) session installs sector assets and publishes no
    // graph roots at all, so a digest over zero roots is the same constant for
    // every such world and for every seed -- offering it as a content identity
    // would be a signal that cannot distinguish anything.
    auto regen_content_digest = [&](bool& available) -> uint64_t {
        available = false;
        if (!session) return 0;
        part_graph_snapshot::Snapshot graph;
        if (!session->graph_snapshot(graph)) return 0;
        std::vector<viewer::jobs::RootDigest> roots;
        for (const auto& [module_name, node] : graph.nodes) {
            if (!node.is_root) continue;
            roots.push_back(viewer::jobs::RootDigest{
                node.module.empty() ? module_name : node.module,
                node.resolved_hash});
        }
        if (roots.empty()) return 0;
        available = true;
        return viewer::jobs::content_digest(std::move(roots));
    };

    // The ONE place a pending viewport.capture becomes a terminal result. Four
    // frame-loop seams call it: the successful PNG write, the readback/write
    // failure beside it, the `shot` deadman that abandons a capture the editor
    // can no longer present, and the request's own deadline. Completing a
    // request the protocol has already expired is a no-op (complete() returns
    // false), which is what keeps "exactly one terminal record" true without
    // this having to know which of the two deadlines fired first.
    auto resolve_agent_capture =
        [&](viewer::capture::Resolution resolution,
            const viewer::capture::Geometry* geometry,
            const matter::CameraDesc* captured_camera,
            const std::vector<viewer::capture::AnnotationInput>* annotation_inputs) {
            if (!agent_capture.armed()) return;
            const viewer::capture::Request request = agent_capture.release();
            const viewer::capture::Terminal terminal =
                viewer::capture::terminal_for(resolution);
            matter::jsondoc::Value payload;
            if (resolution == viewer::capture::Resolution::Captured && geometry &&
                captured_camera) {
                std::vector<viewer::capture::Annotation> annotations;
                if (request.annotate && annotation_inputs)
                    annotations = viewer::capture::project_annotations(
                        *annotation_inputs, *captured_camera, *geometry);
                payload = viewer::capture::capture_result_json(
                    request, *geometry, *captured_camera, agent_context_snapshot(),
                    request.annotate ? &annotations : nullptr);
            }
            agent_protocol.complete(request.request_id, request.ticket_id,
                                    terminal.status, std::move(payload),
                                    terminal.message);
        };

    // viewport.capture: the handler decides only whether a capture can be
    // ARMED right now. The arming itself, and the single terminal result, are
    // owned by the dispatch bridge and the frame loop -- a screenshot is not
    // true until a frame has presented and its readback has been written, and
    // the app lane cannot wait for that.
    auto reg_viewport_capture =
        registry.must_register_handler<viewer::ViewportCapture>(
            matter::evt::CommandScope::App, app_lane,
            [&](const viewer::ViewportCapture&) {
                viewer::AgentPayload payload;
                const viewer::ViewportRect vp = ui.viewport_rect();
                if (vp.w <= 0.0f || vp.h <= 0.0f) {
                    payload.status = viewer::agent::Status::NotReady;
                    payload.message =
                        "the viewport has no drawable logical-pixel rectangle";
                } else if (!presented_camera_available) {
                    payload.status = viewer::agent::Status::NotReady;
                    payload.message = "no viewport frame has been presented yet";
                } else if (agent_capture.armed()) {
                    payload.status = viewer::agent::Status::NotReady;
                    payload.message =
                        "another viewport.capture is still in flight; captures "
                        "are not queued";
                }
                return viewer::ViewportCapture::Result::succeeded(std::move(payload));
            });

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
            viewer::focus_camera_on_selection(camera, selection_set,
                                              field_commands, baked_root_bounds);
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
            // D-03: (re)start the deadman clock on every arm, not just the
            // first, so a slow-but-eventually-fine shot doesn't inherit a
            // stale deadline from an earlier one.
            fifo_shot_wait_start = std::chrono::steady_clock::now();
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
                // D-03: same deadman clock `shot` uses -- see reg_fifo_shot.
                fifo_shot_wait_start = std::chrono::steady_clock::now();
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
                MATTER_LOGE("sim", "sim transport: %s\n", sim_err.c_str());
                return viewer::FifoSimTransport::Result::failed(sim_err);
            }
            if (cmd.action == Action::Stop) {
                reset_character_walk();
                selection_set.clear();
                editor_model.clear_selection();
            }
            return viewer::FifoSimTransport::Result::succeeded(true);
        });

    // This command owns authored-player input, so queued commands may never
    // cross a SessionBinding epoch. G executes this same typed policy.
    auto reg_fifo_character = registry.must_register_handler<viewer::FifoCharacter>(
        matter::evt::CommandScope::ActiveSession, app_lane,
        [&](const viewer::FifoCharacter& cmd) {
            using Action = viewer::FifoCharacter::Action;
            std::string character_error;
            bool ok = false;
            switch (cmd.action) {
                case Action::Walk: {
                    const auto* state = session->ecs().try_get<matter::ecs::WorldRuntimeState>();
                    const auto collision = session->terrain_collision_status().state;
                    const bool ready = state && state->status == matter::ecs::WorldStatus::Ready &&
                        (collision == matter::TerrainCollisionState::Installed ||
                         collision == matter::TerrainCollisionState::Disabled) &&
                        !binding.pending_reload() && binding.pending_switch() < 0;
                    ok = character_walk.set_enabled(session->ecs(), sim_control,
                                                    cmd.enabled, ready, character_error);
                    if (ok) {
                        character_jump.reset();
                        camera_capture = character_walk.enabled();
                        camera_controller.set_capture(window, camera_capture, camera_prefs.raw_mouse_motion);
                    }
                    break;
                }
                case Action::Intent:
                    ok = character_walk.set_intent(session->ecs(), cmd.direction, cmd.sprint, character_error);
                    // Make same-pump status observe the accepted persistent
                    // direction; sample never consumes a pending jump.
                    if (ok) character_walk.sample(session->ecs(), sim_control.mode(), {});
                    break;
                case Action::ClearIntent:
                    character_walk.clear_intent(session->ecs());
                    character_jump.reset();
                    ok = true;
                    break;
                case Action::Jump:
                    ok = character_walk.latch_jump(session->ecs(), sim_control.mode(), character_error);
                    break;
                case Action::Status: {
                    std::string json;
                    ok = character_walk.status_json(session->ecs(), sim_control.mode(), cmd.label, json, character_error);
                    if (ok) std::printf("character_status %s\n", json.c_str());
                    break;
                }
            }
            if (!ok) {
                std::printf("character: failed %s\n", character_error.c_str());
                return viewer::FifoCharacter::Result::failed(character_error);
            }
            return viewer::FifoCharacter::Result::succeeded(true);
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

    // The diagnostic reaches the same live registries and actual production
    // handler registrations as normal startup, but exits before
    // SessionBinding::initialize() requests the first (expensive) world bake.
    // It is intentionally machine-readable and has no source/string inventory
    // fallback: registration handles must have executed and still be live.
    if (registration_census_mode) {
        emit_registration_census(worlds, editor_props.registry(), registry);
        editor_props.shutdown();
        ui.shutdown();
        return 0;
    }

    // Startup bind-then-request: builds the bridge (snapshot-primes the scene
    // model) + opens the first command epoch BEFORE requesting the initial bake.
    binding.initialize();

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
        fifo_wait_event_name = name;
        fifo_wait_event_timeout_s = timeout_s;
        fifo_wait_event_start = std::chrono::steady_clock::now();
        fifo_wait_event_session_scoped = false;
        std::atomic<bool>* fired = &fifo_wait_event_fired;
        // D-07: this arm's stamp. A callback only sets `fired` while its
        // captured stamp still matches the live generation (see the member
        // comment above fifo_wait_event_live_gen) -- guards against a
        // callback from a PREVIOUS arm that was already mid-invocation (on
        // the bake worker thread, for bake./stream. names) when this arm's
        // fifo_wait_event_sub.reset() ran, and so did not observe the
        // unsubscribe before delivering to the old closure.
        std::atomic<uint64_t>* live_gen = &fifo_wait_event_live_gen;
        const uint64_t my_gen =
            fifo_wait_event_live_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
        // Clear `fired` only AFTER the generation bump: a straggler callback
        // from the previous arm that already passed its generation check can
        // no longer land a store(true) that this arm would mistake for its
        // own event.
        fifo_wait_event_fired.store(false, std::memory_order_release);
        const std::string sub_name =
            "qa.wait_event." + std::to_string(fifo_wait_event_seq++) + "." + name;
        if (name == "bake.started") {
            fifo_wait_event_sub = session->events().must_subscribe<matter::events::BakeStarted>(
                sub_name.c_str(), matter::evt::immediate,
                [fired, live_gen, my_gen](const matter::events::BakeStarted&) {
                    if (live_gen->load(std::memory_order_acquire) == my_gen)
                        fired->store(true, std::memory_order_release);
                });
        } else if (name == "bake.finished") {
            fifo_wait_event_sub = session->events().must_subscribe<matter::events::BakeFinished>(
                sub_name.c_str(), matter::evt::immediate,
                [fired, live_gen, my_gen](const matter::events::BakeFinished&) {
                    if (live_gen->load(std::memory_order_acquire) == my_gen)
                        fired->store(true, std::memory_order_release);
                });
        } else if (name == "bake.part_done") {
            fifo_wait_event_sub = session->events().must_subscribe<matter::events::BakePartDone>(
                sub_name.c_str(), matter::evt::immediate,
                [fired, live_gen, my_gen](const matter::events::BakePartDone&) {
                    if (live_gen->load(std::memory_order_acquire) == my_gen)
                        fired->store(true, std::memory_order_release);
                });
        } else if (name == "bake.error") {
            fifo_wait_event_sub = session->events().must_subscribe<matter::events::BakeError>(
                sub_name.c_str(), matter::evt::immediate,
                [fired, live_gen, my_gen](const matter::events::BakeError&) {
                    if (live_gen->load(std::memory_order_acquire) == my_gen)
                        fired->store(true, std::memory_order_release);
                });
        } else if (name == "bake.aborted") {
            fifo_wait_event_sub = session->events().must_subscribe<matter::events::BakeAborted>(
                sub_name.c_str(), matter::evt::immediate,
                [fired, live_gen, my_gen](const matter::events::BakeAborted&) {
                    if (live_gen->load(std::memory_order_acquire) == my_gen)
                        fired->store(true, std::memory_order_release);
                });
        } else if (name == "stream.refine_tile") {
            fifo_wait_event_sub =
                session->events().must_subscribe<matter::events::RefineTileDone>(
                    sub_name.c_str(), matter::evt::immediate,
                    [fired, live_gen, my_gen](const matter::events::RefineTileDone&) {
                        if (live_gen->load(std::memory_order_acquire) == my_gen)
                            fired->store(true, std::memory_order_release);
                    });
        } else if (name == "cmd.completed") {
            fifo_wait_event_sub = app_hub.must_subscribe<matter::evt::CommandCompleted>(
                sub_name.c_str(), matter::evt::immediate,
                [fired, live_gen, my_gen](const matter::evt::CommandCompleted&) {
                    if (live_gen->load(std::memory_order_acquire) == my_gen)
                        fired->store(true, std::memory_order_release);
                });
        } else if (name == "cmd.failed") {
            fifo_wait_event_sub = app_hub.must_subscribe<matter::evt::CommandFailed>(
                sub_name.c_str(), matter::evt::immediate,
                [fired, live_gen, my_gen](const matter::evt::CommandFailed&) {
                    if (live_gen->load(std::memory_order_acquire) == my_gen)
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

    // =======================================================================
    // Frame loop
    // =======================================================================
    // Exits on: the window's close button, `quit_requested` (a MATTER_SCREENSHOT
    // capture landing, a FIFO `quit` once nothing is in flight, a finished perf
    // run, or MATTER_CAM_PATH_EXIT), or `fatal_error`. The per-frame ordering
    // — and the two timing rules it exists to preserve — are laid out in the
    // file header.
    //
    // `phase_split()` below is a rolling split timer: each call returns the ms
    // since the previous split, so the recorded phases exactly partition
    // perf_frame_start..end_frame and sum to the frame time they decompose.
    // =======================================================================
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
        // Also disarm while minimized: begin_frame may skip the later input
        // sample, and holding Space through refocus must not create a press.
        if (glfwGetWindowAttrib(window, GLFW_FOCUSED) != GLFW_TRUE)
            character_jump.reset();
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
        // ---- FIFO: drain bytes, split into lines, dispatch under the gate ---
        // Reading is unconditional every frame; only DISPATCH is gated by
        // `fifo_block`. See the file header's "FIFO / QA timeline" note.
#ifndef _WIN32
        if (cmd_fd >= 0) {
            char bytes[512];
            ssize_t count = 0;
            while ((count = read(cmd_fd, bytes, sizeof(bytes))) > 0)
                cmd_lines.append(bytes, static_cast<size_t>(count));
        }
#else
        if (cmd_handle != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size{};
            if (GetFileSizeEx(cmd_handle, &size) &&
                size.QuadPart < cmd_offset.QuadPart) {
                cmd_offset.QuadPart = 0;
                // A replacement/truncation is a transport reconnect. Drop an
                // incomplete old line so it cannot join a new JSON request.
                cmd_lines.reset();
            }
            if (size.QuadPart > cmd_offset.QuadPart) {
                SetFilePointerEx(cmd_handle, cmd_offset, nullptr, FILE_BEGIN);
                char bytes[512];
                DWORD count = 0;
                while (ReadFile(cmd_handle, bytes, sizeof(bytes), &count,
                                nullptr) && count > 0) {
                    cmd_lines.append(bytes, static_cast<size_t>(count));
                    cmd_offset.QuadPart += count;
                    if (count < sizeof(bytes)) break;
                }
            }
        }
#endif
        {
            viewer::agent::LineBuffer::Line framed;
            while (cmd_lines.pop(framed)) {
                if (framed.oversized) {
                    // No bounded parser can reliably recover an id from a line
                    // it intentionally discarded. Emit a request-less terminal
                    // error when the JSON result channel exists, plus a log.
                    agent_protocol.begin(std::string(
                        viewer::agent::kMaxRequestBytes + 1, 'x'));
                    std::fprintf(stderr,
                                 "MATTER_CMD_FIFO: dropped line over %zu bytes\n",
                                 viewer::agent::kMaxRequestBytes);
                    continue;
                }
                fifo_pending_lines.push_back(std::move(framed.text));
            }
        }
        // Timeline semantics (QA timeline feature): a blocking wait
        // (wait_frames / wait_idle / wait_event) pauses POPPING fifo_pending_lines
        // until it releases, so a `set`/`shot`/etc. line written after a
        // `wait_frames` in the file cannot dispatch before the wait completes.
        // Reading more bytes into cmd_lines above still happens every frame
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
                const size_t fifo_token_end = line.find_first_of(" \t");
                if (line.substr(0, fifo_token_end) == "agent") {
                    size_t json_start = fifo_token_end;
                    while (json_start != std::string::npos &&
                           json_start < line.size() &&
                           (line[json_start] == ' ' || line[json_start] == '\t'))
                        ++json_start;
                    const std::string json =
                        json_start == std::string::npos ? std::string()
                                                        : line.substr(json_start);
                    viewer::agent::BeginResult begun = agent_protocol.begin(json);
                    if (!begun.accepted) continue;

                    const std::string request_id = begun.request.request_id;
                    // One ticket->terminal bridge for every agent command. The
                    // registry only distinguishes Success / StaleScope / failed,
                    // which is the whole protocol status for the metadata
                    // commands but not for a scene READ, where "no such object"
                    // is a successful query with a not_found answer. The
                    // projection is what each command contributes on Success.
                    auto attach_agent_ticket =
                        [&](auto ticket, auto to_terminal) {
                            const uint64_t ticket_id = ticket.id();
                            if (!agent_protocol.attach_ticket(request_id, ticket_id)) {
                                agent_protocol.reject_accepted(
                                    request_id,
                                    viewer::agent::Status::ExecutionFailure,
                                    "could not attach CommandRegistry ticket");
                                return;
                            }
                            ticket.then(
                                app_lane,
                                [&, request_id, ticket_id, to_terminal](
                                    const auto& result) {
                                    viewer::agent::Status status =
                                        viewer::agent::Status::ExecutionFailure;
                                    matter::jsondoc::Value payload;
                                    std::string message = result.error;
                                    if (result.status ==
                                        matter::evt::CommandStatus::Success)
                                        to_terminal(result, status, payload, message);
                                    else if (result.status ==
                                             matter::evt::CommandStatus::StaleScope)
                                        status = viewer::agent::Status::StaleRevision;
                                    agent_protocol.complete(
                                        request_id, ticket_id, status,
                                        std::move(payload), message);
                                });
                        };
                    // agent.commands / agent.help / agent.schema: the handler
                    // answered, so the answer is ok.
                    const auto metadata_terminal =
                        [](const auto& result, viewer::agent::Status& status,
                           matter::jsondoc::Value& payload, std::string&) {
                            status = viewer::agent::Status::Ok;
                            if (result.value) payload = *result.value;
                        };
                    // scene.list_objects / scene.get_object: the handler carries
                    // its own protocol status back in the AgentPayload.
                    const auto payload_terminal =
                        [](const auto& result, viewer::agent::Status& status,
                           matter::jsondoc::Value& payload, std::string& message) {
                            if (!result.value) return;  // stays execution_failure
                            status = result.value->status;
                            payload = result.value->value;
                            message = result.value->message;
                        };

                    if (begun.request.command == "agent.commands") {
                        attach_agent_ticket(registry.dispatch(viewer::AgentCommands{}),
                                            metadata_terminal);
                    } else if (begun.request.command == "scene.list_objects") {
                        // Range/enum validation happens HERE so a bad limit or
                        // an unknown kind is invalid_input rather than a handler
                        // failure; the descriptor only checked JSON types.
                        viewer::inventory::ListQuery query;
                        std::string query_error;
                        if (!viewer::inventory::parse_list_query(
                                begun.request.arguments, query, query_error)) {
                            agent_protocol.reject_accepted(
                                request_id, viewer::agent::Status::InvalidInput,
                                query_error);
                        } else {
                            viewer::SceneListObjects command;
                            command.arguments = begun.request.arguments;
                            attach_agent_ticket(
                                registry.dispatch(std::move(command)),
                                payload_terminal);
                        }
                    } else if (begun.request.command == "scene.get_object") {
                        // `object` is a REQUIRED object_id, so the descriptor has
                        // already validated its shape (and rejected a duplicated
                        // key as missing); this re-parse is what turns it into
                        // the typed identity the command carries.
                        const matter::jsondoc::Value* requested =
                            begun.request.arguments.find("object");
                        viewer::agent::ObjectIdentity object;
                        std::string object_error;
                        if (!requested ||
                            !viewer::agent::parse_object_identity(
                                *requested, object, object_error)) {
                            agent_protocol.reject_accepted(
                                request_id, viewer::agent::Status::InvalidInput,
                                object_error.empty()
                                    ? "object must be {\"kind\",\"id\"}"
                                    : object_error);
                        } else {
                            viewer::SceneGetObject command;
                            command.object = object;
                            attach_agent_ticket(
                                registry.dispatch(std::move(command)),
                                payload_terminal);
                        }
                    } else if (begun.request.command == "scene.trace_provenance") {
                        // Keep the boundary validation identical to the handler:
                        // malformed typed ids and traversal bounds are protocol
                        // input errors, not a failed app-lane command.
                        viewer::inventory::TraceQuery query;
                        std::string query_error;
                        if (!viewer::inventory::parse_trace_query(
                                begun.request.arguments, query, query_error)) {
                            agent_protocol.reject_accepted(
                                request_id, viewer::agent::Status::InvalidInput,
                                query_error);
                        } else {
                            viewer::SceneTraceProvenance command;
                            command.arguments = begun.request.arguments;
                            attach_agent_ticket(
                                registry.dispatch(std::move(command)),
                                payload_terminal);
                        }
                    } else if (begun.request.command == "procedural.parameters" ||
                               begun.request.command == "procedural.update") {
                        const matter::jsondoc::Value* requested =
                            begun.request.arguments.find("object");
                        viewer::agent::ObjectIdentity object;
                        std::string object_error;
                        if (!requested || !viewer::agent::parse_object_identity(
                                              *requested, object, object_error)) {
                            agent_protocol.reject_accepted(
                                request_id, viewer::agent::Status::InvalidInput,
                                object_error.empty() ? "object must be {kind,id}"
                                                     : object_error);
                        } else if (object.kind !=
                                   viewer::agent::ObjectIdentity::Kind::BakedRoot) {
                            agent_protocol.reject_accepted(
                                request_id, viewer::agent::Status::InvalidInput,
                                "procedural parameters are owned by baked_root objects");
                        } else if (begun.request.command == "procedural.parameters") {
                            viewer::ProceduralParameters command;
                            command.object = object;
                            attach_agent_ticket(registry.dispatch(std::move(command)),
                                                payload_terminal);
                        } else {
                            const matter::jsondoc::Value* changes =
                                begun.request.arguments.find("changes");
                            if (!changes || changes->kind !=
                                                matter::jsondoc::Value::Kind::Object) {
                                agent_protocol.reject_accepted(
                                    request_id, viewer::agent::Status::InvalidInput,
                                    "changes must be an object");
                            } else {
                                viewer::ProceduralUpdate command;
                                command.object = object;
                                command.changes = *changes;
                                if (const matter::jsondoc::Value* dry =
                                        begun.request.arguments.find("dry_run"))
                                    command.dry_run = dry->b;
                                attach_agent_ticket(
                                    registry.dispatch(std::move(command)),
                                    payload_terminal);
                            }
                        }
                    } else if (begun.request.command == "viewport.pick" ||
                               begun.request.command == "viewport.pick_select") {
                        viewer::viewport_pick_command::Coordinates coordinates;
                        std::string coordinate_error;
                        if (!viewer::viewport_pick_command::parse_coordinates(
                                begun.request.arguments, coordinates, coordinate_error)) {
                            agent_protocol.reject_accepted(
                                request_id, viewer::agent::Status::InvalidInput,
                                coordinate_error);
                        } else if (begun.request.command == "viewport.pick") {
                            viewer::ViewportPick command;
                            command.x = coordinates.x;
                            command.y = coordinates.y;
                            attach_agent_ticket(registry.dispatch(std::move(command)),
                                                payload_terminal);
                        } else {
                            viewer::viewport_pick_command::SelectionMode mode;
                            std::string mode_error;
                            if (!viewer::viewport_pick_command::parse_selection_mode(
                                    begun.request.arguments, mode, mode_error)) {
                                agent_protocol.reject_accepted(
                                    request_id, viewer::agent::Status::InvalidInput,
                                    mode_error);
                            } else {
                                viewer::ViewportPickSelect command;
                                command.x = coordinates.x;
                                command.y = coordinates.y;
                                switch (mode) {
                                    case viewer::viewport_pick_command::SelectionMode::Replace:
                                        command.mode = viewer::ViewportPickSelect::Mode::Replace;
                                        break;
                                    case viewer::viewport_pick_command::SelectionMode::Add:
                                        command.mode = viewer::ViewportPickSelect::Mode::Add;
                                        break;
                                    case viewer::viewport_pick_command::SelectionMode::Toggle:
                                        command.mode = viewer::ViewportPickSelect::Mode::Toggle;
                                        break;
                                }
                                attach_agent_ticket(registry.dispatch(std::move(command)),
                                                    payload_terminal);
                            }
                        }
                    } else if (begun.request.command == "viewport.capture") {
                        // The path policy is the shot_now verb's, applied here
                        // rather than in viewport_capture.h so the FIFO and the
                        // agent protocol can never disagree about what a safe
                        // capture path is.
                        viewer::capture::Arguments arguments;
                        std::string argument_error;
                        if (!viewer::capture::parse_arguments(
                                begun.request.arguments, arguments, argument_error)) {
                            agent_protocol.reject_accepted(
                                request_id, viewer::agent::Status::InvalidInput,
                                argument_error);
                        } else if (!viewer::fifo_safe_absolute_png_path(arguments.path)) {
                            agent_protocol.reject_accepted(
                                request_id, viewer::agent::Status::InvalidInput,
                                "path must be a safe absolute .png path, the same "
                                "rule the shot_now FIFO verb applies");
                        } else {
                            viewer::ViewportCapture command;
                            command.path = arguments.path;
                            command.annotate = arguments.annotate;
                            // The deadline the tracker enforces is the request's
                            // own, recomputed here because Protocol keeps its
                            // copy private. The two can differ by the microseconds
                            // between begin() and this line; whichever fires
                            // first emits the ONE terminal timeout record, and
                            // Protocol::complete's pending map is what makes that
                            // exactly-once rather than a race.
                            const auto deadline =
                                viewer::capture::Clock::now() +
                                std::chrono::milliseconds(begun.request.timeout_ms);
                            const bool annotate = arguments.annotate;
                            const std::string path = arguments.path;
                            auto ticket = registry.dispatch(std::move(command));
                            const uint64_t ticket_id = ticket.id();
                            if (!agent_protocol.attach_ticket(request_id, ticket_id)) {
                                agent_protocol.reject_accepted(
                                    request_id,
                                    viewer::agent::Status::ExecutionFailure,
                                    "could not attach CommandRegistry ticket");
                            } else {
                                ticket.then(
                                    app_lane,
                                    [&, request_id, ticket_id, path, annotate,
                                     deadline](const auto& result) {
                                        // Unlike every other agent command, a
                                        // Success here is NOT a terminal answer:
                                        // it means "armable". Only the refusals
                                        // complete from this continuation.
                                        if (result.status !=
                                            matter::evt::CommandStatus::Success) {
                                            agent_protocol.complete(
                                                request_id, ticket_id,
                                                result.status ==
                                                        matter::evt::CommandStatus::StaleScope
                                                    ? viewer::agent::Status::StaleRevision
                                                    : viewer::agent::Status::ExecutionFailure,
                                                matter::jsondoc::Value{},
                                                result.error);
                                            return;
                                        }
                                        if (result.value &&
                                            result.value->status !=
                                                viewer::agent::Status::Ok) {
                                            agent_protocol.complete(
                                                request_id, ticket_id,
                                                result.value->status,
                                                matter::jsondoc::Value{},
                                                result.value->message);
                                            return;
                                        }
                                        viewer::capture::Request pending;
                                        pending.request_id = request_id;
                                        pending.ticket_id = ticket_id;
                                        pending.path = path;
                                        pending.annotate = annotate;
                                        pending.deadline = deadline;
                                        if (!agent_capture.arm(std::move(pending))) {
                                            agent_protocol.complete(
                                                request_id, ticket_id,
                                                viewer::agent::Status::NotReady,
                                                matter::jsondoc::Value{},
                                                "another viewport.capture armed "
                                                "first; captures are not queued");
                                            return;
                                        }
                                        // Same queue, same presented-frame drain
                                        // and same PNG writer as `shot_now` --
                                        // including the D-03 deadman, whose
                                        // in-flight condition is the queue
                                        // itself, so arming re-arms it here the
                                        // way the FIFO verbs do.
                                        fifo_present.queue_screenshot(path);
                                        fifo_shot_wait_start =
                                            std::chrono::steady_clock::now();
                                    });
                            }
                        }
                    } else if (begun.request.command == "view.focus") {
                        // `object` is OPTIONAL here, so unlike scene.get_object
                        // the descriptor has not already validated its shape --
                        // an absent key is the selection form, a malformed one
                        // is invalid_input.
                        viewer::ViewFocus command;
                        std::string object_error;
                        const matter::jsondoc::Value* requested =
                            begun.request.arguments.find("object");
                        if (requested &&
                            !viewer::agent::parse_object_identity(
                                *requested, command.object, object_error)) {
                            agent_protocol.reject_accepted(
                                request_id, viewer::agent::Status::InvalidInput,
                                object_error.empty()
                                    ? "object must be {\"kind\",\"id\"}"
                                    : object_error);
                        } else {
                            command.has_object = requested != nullptr;
                            attach_agent_ticket(registry.dispatch(std::move(command)),
                                                payload_terminal);
                        }
                    } else if (begun.request.command == "selection.replace" ||
                               begun.request.command == "selection.add" ||
                               begun.request.command == "selection.remove" ||
                               begun.request.command == "selection.toggle") {
                        const matter::jsondoc::Value* requested =
                            begun.request.arguments.find("objects");
                        std::vector<viewer::agent::ObjectIdentity> objects;
                        std::string objects_error;
                        if (!requested || !viewer::selection_command::parse_objects(
                                              *requested, objects, objects_error)) {
                            agent_protocol.reject_accepted(
                                request_id, viewer::agent::Status::InvalidInput,
                                objects_error.empty()
                                    ? "objects must be a non-empty array of object identities"
                                    : objects_error);
                        } else if (begun.request.command == "selection.replace") {
                            viewer::SelectionReplace command;
                            command.objects = std::move(objects);
                            attach_agent_ticket(registry.dispatch(std::move(command)),
                                                payload_terminal);
                        } else if (begun.request.command == "selection.add") {
                            viewer::SelectionAdd command;
                            command.objects = std::move(objects);
                            attach_agent_ticket(registry.dispatch(std::move(command)),
                                                payload_terminal);
                        } else if (begun.request.command == "selection.remove") {
                            viewer::SelectionRemove command;
                            command.objects = std::move(objects);
                            attach_agent_ticket(registry.dispatch(std::move(command)),
                                                payload_terminal);
                        } else {
                            viewer::SelectionToggle command;
                            command.objects = std::move(objects);
                            attach_agent_ticket(registry.dispatch(std::move(command)),
                                                payload_terminal);
                        }
                    } else if (begun.request.command == "selection.clear") {
                        attach_agent_ticket(registry.dispatch(viewer::SelectionClear{}),
                                            payload_terminal);
                    } else if (begun.request.command == "selection.list") {
                        attach_agent_ticket(registry.dispatch(viewer::SelectionList{}),
                                            payload_terminal);
                    } else if (begun.request.command == "job.start") {
                        // The operation enum and the per-operation seed rules
                        // are checked HERE so a seed on a reload is
                        // invalid_input rather than a handler failure; the
                        // descriptor only checked JSON types.
                        viewer::jobs::StartRequest request;
                        std::string request_error;
                        if (!viewer::jobs::parse_start_arguments(
                                begun.request.arguments, request, request_error)) {
                            agent_protocol.reject_accepted(
                                request_id, viewer::agent::Status::InvalidInput,
                                request_error);
                        } else {
                            viewer::JobStart command;
                            command.request = std::move(request);
                            attach_agent_ticket(registry.dispatch(std::move(command)),
                                                payload_terminal);
                        }
                    } else if (begun.request.command == "job.status" ||
                               begun.request.command == "job.cancel" ||
                               begun.request.command == "job.wait") {
                        uint64_t job_id = 0;
                        std::string job_error;
                        if (!viewer::jobs::parse_job_id(begun.request.arguments,
                                                        job_id, job_error)) {
                            agent_protocol.reject_accepted(
                                request_id, viewer::agent::Status::InvalidInput,
                                job_error);
                        } else if (begun.request.command == "job.status") {
                            viewer::JobStatus command;
                            command.job_id = job_id;
                            attach_agent_ticket(registry.dispatch(std::move(command)),
                                                payload_terminal);
                        } else if (begun.request.command == "job.cancel") {
                            viewer::JobCancel command;
                            command.job_id = job_id;
                            attach_agent_ticket(registry.dispatch(std::move(command)),
                                                payload_terminal);
                        } else {
                            // job.wait: like viewport.capture, a Success from
                            // the handler means "waitable", not "finished".
                            // The deadline is the request's own, recomputed
                            // here because Protocol keeps its copy private;
                            // whichever of the two fires first emits the ONE
                            // terminal record.
                            const auto deadline =
                                viewer::jobs::Clock::now() +
                                std::chrono::milliseconds(begun.request.timeout_ms);
                            viewer::JobWait command;
                            command.job_id = job_id;
                            auto ticket = registry.dispatch(std::move(command));
                            const uint64_t ticket_id = ticket.id();
                            if (!agent_protocol.attach_ticket(request_id, ticket_id)) {
                                agent_protocol.reject_accepted(
                                    request_id,
                                    viewer::agent::Status::ExecutionFailure,
                                    "could not attach CommandRegistry ticket");
                            } else {
                                ticket.then(
                                    app_lane,
                                    [&, request_id, ticket_id, job_id, deadline](
                                        const auto& result) {
                                        if (result.status !=
                                            matter::evt::CommandStatus::Success) {
                                            agent_protocol.complete(
                                                request_id, ticket_id,
                                                result.status ==
                                                        matter::evt::CommandStatus::StaleScope
                                                    ? viewer::agent::Status::StaleRevision
                                                    : viewer::agent::Status::ExecutionFailure,
                                                matter::jsondoc::Value{},
                                                result.error);
                                            return;
                                        }
                                        if (result.value &&
                                            result.value->status !=
                                                viewer::agent::Status::Ok) {
                                            agent_protocol.complete(
                                                request_id, ticket_id,
                                                result.value->status,
                                                result.value->value,
                                                result.value->message);
                                            return;
                                        }
                                        viewer::jobs::Waiter waiter;
                                        waiter.request_id = request_id;
                                        waiter.ticket_id = ticket_id;
                                        waiter.job_id = job_id;
                                        waiter.deadline = deadline;
                                        // A job that is ALREADY terminal is
                                        // answered now rather than parked for
                                        // a frame -- the wait has nothing left
                                        // to wait for.
                                        const viewer::jobs::Job* job =
                                            regen_jobs.find(job_id);
                                        if (job && viewer::jobs::is_terminal(job->state)) {
                                            resolve_regen_wait(waiter, false);
                                            return;
                                        }
                                        if (!regen_waits.add(std::move(waiter))) {
                                            agent_protocol.complete(
                                                request_id, ticket_id,
                                                viewer::agent::Status::NotReady,
                                                matter::jsondoc::Value{},
                                                "too many bounded waits are "
                                                "already pending");
                                        }
                                    });
                            }
                        }
                    } else if (begun.request.command == "job.list") {
                        std::size_t limit = 0;
                        std::string limit_error;
                        if (!viewer::jobs::parse_list_limit(begun.request.arguments,
                                                            limit, limit_error)) {
                            agent_protocol.reject_accepted(
                                request_id, viewer::agent::Status::InvalidInput,
                                limit_error);
                        } else {
                            viewer::JobList command;
                            command.limit = limit;
                            attach_agent_ticket(registry.dispatch(std::move(command)),
                                                payload_terminal);
                        }
                    } else {
                        const matter::jsondoc::Value* requested =
                            begun.request.arguments.find("command");
                        const std::string target = requested ? requested->str
                                                             : std::string();
                        if (!agent_protocol.find_command(target)) {
                            agent_protocol.reject_accepted(
                                request_id, viewer::agent::Status::NotFound,
                                "requested command is not registered");
                        } else if (begun.request.command == "agent.help") {
                            viewer::AgentHelp command;
                            command.command = target;
                            attach_agent_ticket(registry.dispatch(std::move(command)),
                                                metadata_terminal);
                        } else if (begun.request.command == "agent.schema") {
                            viewer::AgentSchema command;
                            command.command = target;
                            attach_agent_ticket(registry.dispatch(std::move(command)),
                                                metadata_terminal);
                        } else {
                            // Available descriptors must have a typed registry
                            // route. Treat a missing route as an implementation
                            // failure rather than silently accepting it.
                            agent_protocol.reject_accepted(
                                request_id,
                                viewer::agent::Status::ExecutionFailure,
                                "available command has no CommandRegistry route");
                        }
                    }
                    continue;
                }
                const viewer::FifoParseResult presentation_command =
                    viewer::parse_fifo_line(line);
                if (presentation_command.recognized) {
                    if (!presentation_command.success) {
                        std::printf("%s\n",
                                    presentation_command.error.c_str());
                    } else if (const auto* render_path_command =
                                   std::get_if<viewer::FifoRenderPath>(
                                       &presentation_command.command)) {
                        registry.dispatch(*render_path_command);
                    } else if (const auto* history_reset_command =
                                   std::get_if<viewer::FifoHistoryReset>(
                                       &presentation_command.command)) {
                        registry.dispatch(*history_reset_command);
                    } else if (const auto* character_command =
                                   std::get_if<viewer::FifoCharacter>(
                                       &presentation_command.command)) {
                        const auto ticket = registry.dispatch(*character_command);
                        if (ticket.ready() && ticket.status() != matter::evt::CommandStatus::Success)
                            std::printf("character: dispatch failed (%s)\n", matter::evt::to_string(ticket.status()));
                    } else if (const auto* wait_frames_command =
                                   std::get_if<viewer::FifoWaitFrames>(
                                       &presentation_command.command)) {
                        const auto ticket = registry.dispatch(*wait_frames_command);
                        // D-06: dispatch() can finalize the ticket
                        // SYNCHRONOUSLY as a rejection (no handler / registry
                        // shut down / queue full) before pump() ever runs the
                        // handler -- ready() is non-blocking and only true in
                        // that synchronous case. Arming the block
                        // unconditionally there is a permanent hang: nothing
                        // will ever satisfy a WaitFrames that was never
                        // actually queued. A still-pending (queued-for-pump)
                        // ticket reads not-ready here and is trusted exactly
                        // as before.
                        if (ticket.ready() &&
                            ticket.status() != matter::evt::CommandStatus::Success) {
                            std::printf("wait_frames: dispatch failed (%s)\n",
                                        matter::evt::to_string(ticket.status()));
                        } else {
                            // Semantics upgrade: wait_frames now blocks every
                            // later line, not just screenshots (the present-count
                            // itself is still FifoPresentSequencer's, released
                            // below in the completed_waits loop after advance()).
                            fifo_block = FifoBlockKind::WaitFrames;
                        }
                    } else if (const auto* screenshot_now_command =
                                   std::get_if<viewer::FifoScreenshotNow>(
                                       &presentation_command.command)) {
                        registry.dispatch(*screenshot_now_command);
                        // D-02: shot_now blocks like `shot` -- see the release
                        // check beside fifo_quit_pending's, after end_frame.
                        // Recommended for consistency (control-surface.md):
                        // neither spelling needs a trailing wait_frames to be
                        // safe to follow with another timeline line.
                        fifo_block = FifoBlockKind::Shot;
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
                    // D-02: block subsequent timeline lines until this
                    // shot's settle completes and its PNG + .done sidecar are
                    // written (see the release check beside
                    // fifo_quit_pending's, after end_frame) -- without this, a
                    // `shot a.png` / `set X` / `shot b.png` sequence with no
                    // intervening wait dispatches `set` before a.png is
                    // captured, poisoning a.png with the post-set state.
                    fifo_block = FifoBlockKind::Shot;
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
                    // wait_idle <seconds> [timeout_seconds]: blocks until
                    // resident_sectors has held steady for <seconds> of
                    // wall-clock time AND the bake is ready, or (D-04) until
                    // the optional deadline expires -- mirrors wait_event's
                    // timeout semantics: the script continues past an
                    // expired wait_idle, it does not quit. Released in the
                    // frame_stats-driven check below (mirrors
                    // MATTER_CAM_PATH_SETTLE's plateau logic).
                    std::istringstream input(line);
                    std::string verb, seconds_text, timeout_text, extra;
                    input >> verb >> seconds_text >> timeout_text;
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
                    const bool has_timeout = !timeout_text.empty();
                    double timeout_s = 0.0;
                    if (parse_ok && has_timeout) {
                        try {
                            size_t consumed = 0;
                            timeout_s = std::stod(timeout_text, &consumed);
                            parse_ok = consumed == timeout_text.size() && timeout_s > 0.0;
                        } catch (...) {
                            parse_ok = false;
                        }
                    }
                    if (input >> extra) parse_ok = false;
                    if (!parse_ok) {
                        std::printf(
                            "wait_idle: expected `wait_idle <seconds> "
                            "[timeout_seconds]`\n");
                    } else if (seconds <= 0.0) {
                        std::printf("wait_idle: <seconds> must be > 0\n");
                    } else {
                        fifo_wait_idle_seconds = seconds;
                        fifo_wait_idle_start = std::chrono::steady_clock::now();
                        fifo_wait_idle_last_change = fifo_wait_idle_start;
                        fifo_wait_idle_last_resident = UINT32_MAX;  // force resync below
                        fifo_wait_idle_timeout_s = has_timeout ? timeout_s : 0.0;
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
                } else if (line.substr(0, line.find_first_of(" \t")) == "world" &&
                           std::sscanf(line.c_str(), "world %255s", word) == 1) {
                    // D-08: the verb-token check above (same
                    // find_first_of(" \t") idiom parse_fifo_line uses for its
                    // strict verbs, e.g. "wait_frames") is required because
                    // sscanf's "world %255s" only requires the literal
                    // "world" followed by >= 0 whitespace -- "worldfoo bar"
                    // would otherwise match with word="foo".
                    //
                    // Same case-insensitive resolution as MATTER_WORLD, and
                    // the same intent-recording path `reload` uses: the
                    // actual session destroy/recreate runs at the post-frame
                    // seam (SessionBinding::replace), never mid-parse. Not
                    // itself a blocking wait -- pair it with wait_idle for a
                    // multi-world sweep.
                    std::string wanted(word);
                    std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                                   [](unsigned char ch) {
                                       return static_cast<char>(std::tolower(ch));
                                   });
                    int world_index = -1;
                    for (size_t i = 0; i < worlds.size(); ++i) {
                        std::string candidate = worlds[i].world_name;
                        std::transform(candidate.begin(), candidate.end(),
                                       candidate.begin(),
                                       [](unsigned char ch) {
                                           return static_cast<char>(std::tolower(ch));
                                       });
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
                } else if (line == "issue capture") {
                    // FIFO `issue capture`: reuses F10's viewport-capture
                    // handshake (begin_viewport_capture + the AwaitingCapture
                    // readback resolved after end_frame below), but blocking
                    // -- FifoBlockKind::IssueCapture holds every later
                    // timeline line until record_shot has actually run (see
                    // the release check beside fifo_quit_pending's). Draft
                    // shots accumulate exactly as F9/F10 do: this does not
                    // reset issue_state, it only adds to whatever draft is
                    // already open.
                    viewer::begin_viewport_capture(issue_state);
                    fifo_issue_capture_active = true;
                    fifo_issue_capture_wait_start = std::chrono::steady_clock::now();
                    fifo_block = FifoBlockKind::IssueCapture;
                } else if (line.compare(0, 11, "issue file ") == 0 ||
                           line == "issue file") {
                    // `issue file <note>` -- the rest of the line, verbatim
                    // (same idiom as `set`'s value), so the note can contain
                    // spaces/punctuation. May be empty only when the draft
                    // already has a shot -- issue_reporter_panel.cpp's
                    // "File report" button enforces the identical rule
                    // (can_file) in the UI layer; write_issue_report itself
                    // has no such guard, so the FIFO path enforces it here.
                    const std::string note =
                        line.size() > 11 ? line.substr(11) : std::string();
                    if (note.empty() && issue_state.shots.empty()) {
                        std::printf("issue: file failed (empty draft)\n");
                    } else {
                        std::snprintf(issue_state.note, sizeof(issue_state.note),
                                      "%s", note.c_str());
                        fifo_issue_file_pending = true;
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
        // The armed capture's deadline, checked BEFORE the protocol's generic
        // expiry so the capture-specific timeout message is the one that lands
        // in the ordinary case. Both deadlines are the same request's, so
        // whichever wins still emits exactly one `timeout` record.
        if (agent_capture.expired(std::chrono::steady_clock::now())) {
            // Drop the queued capture only when it is still the FRONT of the
            // present queue: cancel_pending_screenshot pops the front, and a
            // `shot_now` queued ahead of it is not ours to discard.
            if (fifo_present.pending_screenshot_path() == agent_capture.request().path)
                fifo_present.cancel_pending_screenshot();
            resolve_agent_capture(viewer::capture::Resolution::TimedOut, nullptr,
                                  nullptr, nullptr);
        }
        // Bounded job waits, swept beside the capture deadline and before the
        // protocol's generic expiry so the job-specific timeout message is the
        // one that lands in the ordinary case. Both deadlines belong to the
        // same request, so whichever wins still emits exactly one record.
        for (const viewer::jobs::WaitList::Ready& released :
             regen_waits.collect(regen_jobs, viewer::jobs::Clock::now()))
            resolve_regen_wait(released.waiter, released.timed_out);
        agent_protocol.expire();
        if (const std::string agent_error = agent_protocol.take_io_error();
            !agent_error.empty())
            std::fprintf(stderr, "agent protocol: %s\n", agent_error.c_str());

        // D-03: shot deadman. Checked every iteration (not gated on
        // begin_frame succeeding) so a world where presents never succeed
        // cannot hide a stuck shot from it -- the begin_frame failure branch
        // below `continue`s straight past the end-of-frame write/quit
        // resolution that would otherwise be the only place this clears.
        if (const bool shot_in_flight =
                shot_settle > 0 || !fifo_present.pending_screenshot_path().empty();
            shot_in_flight &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          fifo_shot_wait_start)
                    .count() >= kFifoShotTimeoutSeconds) {
            const std::string abandoned_path =
                shot_settle > 0 ? shot_path : fifo_present.pending_screenshot_path();
            std::printf("shot: timeout, abandoned %s\n", abandoned_path.c_str());
            if (agent_capture.owns(abandoned_path))
                resolve_agent_capture(viewer::capture::Resolution::Abandoned,
                                      nullptr, nullptr, nullptr);
            shot_settle = 0;
            fifo_present.cancel_pending_screenshot();
            if (fifo_block == FifoBlockKind::Shot) fifo_block = FifoBlockKind::None;
        }

        // Same deadman shape for FIFO `issue capture`: a world where the
        // AwaitingCapture readback (below, after end_frame) never resolves
        // -- readback keeps failing and re-arming capture_settle -- would
        // otherwise hang the timeline (and any `quit` after it) forever.
        // Checked every iteration for the same reason the shot deadman is.
        if (fifo_block == FifoBlockKind::IssueCapture &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          fifo_issue_capture_wait_start)
                    .count() >= kFifoIssueCaptureTimeoutSeconds) {
            std::printf("issue: capture timeout, abandoned\n");
            // Abandon the stuck attempt so it cannot wedge a later F9/F10 or
            // `issue capture` behind an AwaitingCapture that will never
            // resolve on its own.
            issue_state.phase = viewer::ReporterPhase::Idle;
            fifo_issue_capture_active = false;
            fifo_block = FifoBlockKind::None;
        }

        if (test_resize && bake_ready && !resize_exercised) {
            glfwSetWindowSize(window, 960, 540);
            glfwPollEvents();
            screenshot_settle = 0;
            resize_exercised = true;
        }

        phase.poll = phase_split();   // events + input, everything up to acquire
        // ---- Frame begin: fence wait + swapchain acquire --------------------
        // A "zero-sized" failure is the minimized-window case: wait briefly on
        // events and retry, without treating it as an error. Any other failure
        // breaks the loop. begin_frame also rebuilds an out-of-date swapchain,
        // which is why a window resize (F11 presentation mode,
        // MATTER_TEST_RESIZE, a user drag) needs no handling of its own here.
        //
        // NOTE: the `continue` on the zero-sized path skips the whole rest of
        // the iteration, including the end-of-frame capture/quit resolution.
        // That is why the shot and issue-capture deadman checks sit ABOVE this
        // point rather than below it.
        matter::VulkanFrame frame{};
        if (!vulkan->begin_frame(frame, error)) {
            if (error.find("zero-sized") != std::string::npos) {
                glfwWaitEventsTimeout(0.05);
                continue;
            }
            MATTER_LOGE("editor", "FATAL: begin_frame: %s\n", error.c_str());
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
                selection_set, field_commands, baked_root_bounds,
                selection_pivot, pivot_radius);
        }

        // ---- UI pass --------------------------------------------------------
        // `frame` is the swapchain frame; `render_frame` is what the 3D scene
        // renders into — normally the offscreen viewport image that ImGui then
        // samples, but when the UI is hidden (MATTER_HIDE_UI or F11
        // presentation mode) viewport_render_frame hands back the swapchain
        // frame directly, so the scene goes straight to the screen.
        // A false `ui_frame_ready` is a device-surfacing failure and is
        // reported through mark_device_fatal; the rest of the frame still runs
        // its non-UI work.
        const bool ui_frame_ready = ui.begin_frame(frame, error);
        matter::VulkanFrame render_frame = frame;
        // Reset before the Bake Lab tab bar draws so wants_viewport() below
        // reflects whether the Workbench tab is actually focused THIS frame
        // (see part_workbench.h's modal-isolation note).
        bake_lab.workbench().begin_frame();
        if (!ui_frame_ready) {
            MATTER_LOGE("editor", "FATAL: ImGui Vulkan prepare: %s\n",
                         error.c_str());
            mark_device_fatal(error);
        } else {
            camera_input_order.begin_ui();
            // E5c: no per-frame editor_model.refresh() — the scene tree is
            // delta-driven by the SessionBinding scene adapter, and its flattened
            // rows are re-derived at property_scheduler.flush_dirty() (after tick)
            // only on ticks that actually changed rows.
            // G toggles authored-player walking; T/R/S retain gizmo control
            // when not walking. Walking's S must not also select gizmo scale.
            // Only when ImGui isn't capturing keyboard/text input, so typing
            // in a Properties field doesn't retarget the gizmo.
            {
                const ImGuiIO& io = ImGui::GetIO();
                if (!io.WantTextInput && !io.WantCaptureKeyboard &&
                    glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE) {
                    const bool walk_toggle = ImGui::IsKeyPressed(ImGuiKey_G, false);
                    if (walk_toggle) {
                        viewer::FifoCharacter command;
                        command.action = viewer::FifoCharacter::Action::Walk;
                        command.enabled = !character_walk.enabled();
                        registry.execute(command);
                    }
                    if (!character_walk.enabled() && !walk_toggle) ui.update_gizmo_hotkeys();
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
                MATTER_LOGE("editor", "viewport target: %s\n", error.c_str());
                error.clear();
            }
            if (!hide_ui) {
                const viewer::ToolbarActions toolbar =
                    ui.draw_toolbar(sim_control.mode());
                if (toolbar.play_clicked) {
                    std::string sim_err;
                    if (!sim_control.play(session->ecs(), sim_err))
                        MATTER_LOGE("sim", "play: %s\n", sim_err.c_str());
                }
                if (toolbar.pause_clicked) {
                    std::string sim_err;
                    if (!sim_control.pause(sim_err))
                        MATTER_LOGE("sim", "pause: %s\n", sim_err.c_str());
                }
                if (toolbar.step_clicked) {
                    std::string sim_err;
                    if (!sim_control.step(sim_err))
                        MATTER_LOGE("sim", "step: %s\n", sim_err.c_str());
                }
                if (toolbar.stop_clicked) {
                    std::string sim_err;
                    if (!sim_control.stop(session->ecs(), sim_err)) {
                        MATTER_LOGE("sim", "stop: %s\n", sim_err.c_str());
                    } else {
                        reset_character_walk();
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

        // FIFO `issue file <note>`: same filing path the "File report"
        // button uses (write_issue_report with the full IssueContext), but
        // deliberately NOT nested inside the `if (!hide_ui)` block above --
        // draw_issue_reporter() and its `file_issue` trigger only run in
        // there, so a MATTER_HIDE_UI run would never reach the button path
        // at all. Field-for-field the same IssueContext construction as the
        // button's call site.
        if (ui_frame_ready && fifo_issue_file_pending) {
            fifo_issue_file_pending = false;
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
                std::printf("issue: file failed (%s)\n",
                            issue_state.status.c_str());
                console_log.push(viewer::LogSeverity::Error,
                                 "Issue report: " + issue_state.status);
            } else {
                std::printf("issue: filed %s\n", filed.c_str());
                console_log.push(viewer::LogSeverity::Info,
                                 "Issue report written to " + filed);
                // Reset for the next one, same as the button flow.
                reset_issue_state();
            }
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
        // this pose through streaming/picking. Walking refreshes its eye after
        // the fixed tick below, before render, without changing yaw/pitch.
        matter::CameraDesc frame_camera = camera;

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
        viewer::CharacterWalkInput walk_input{};
        const bool accepts_walk_keyboard = character_walk.enabled() && camera_capture && ui_frame_ready &&
            glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE &&
            !ImGui::GetIO().WantCaptureKeyboard && !ImGui::GetIO().WantTextInput &&
            !viewer::issue_reporter_wants_mouse(issue_state) && !cam_path_running;
        if (accepts_walk_keyboard) {
            const float forward = (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS ? 1.0f : 0.0f) -
                                  (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS ? 1.0f : 0.0f);
            const float right = (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS ? 1.0f : 0.0f) -
                                (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS ? 1.0f : 0.0f);
            float fx = camera.target.x - camera.position.x;
            float fz = camera.target.z - camera.position.z;
            const float yaw_length = std::sqrt(fx * fx + fz * fz);
            if (yaw_length > 1e-6f) { fx /= yaw_length; fz /= yaw_length; }
            else { fx = 0; fz = -1; }
            walk_input.world_direction = {fx * forward - fz * right, 0, fz * forward + fx * right};
            const float length = std::sqrt(walk_input.world_direction.x * walk_input.world_direction.x +
                                           walk_input.world_direction.z * walk_input.world_direction.z);
            if (length > 1) {
                walk_input.world_direction.x /= length;
                walk_input.world_direction.z /= length;
            }
            walk_input.sprint = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                                glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        }
        walk_input.jump_pressed = character_jump.update(
            glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS, accepts_walk_keyboard);
        const bool was_walking = character_walk.enabled();
        character_walk.sample(session->ecs(), sim_control.mode(), walk_input);
        if (was_walking && !character_walk.enabled()) {
            character_jump.reset();
            camera_capture = false;
            camera_controller.set_capture(window, false, false);
        }
        const matter::TickDesc tick = viewer::make_editor_tick(sim_control, dt, ui.sim_time_scale());
        phase.ui = phase_split();   // ImGui panel building
        session->tick(tick);
        matter::Float3 character_eye{};
        if (character_walk.eye_position(session->ecs(), character_eye)) {
            camera.target.x += character_eye.x - camera.position.x;
            camera.target.y += character_eye.y - camera.position.y;
            camera.target.z += character_eye.z - camera.position.z;
            camera.position = character_eye;
            frame_camera = camera;
        }
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
            // Regeneration-job ingestion (regen_jobs.h). This drain is the ONE
            // place the engine's bake lifecycle is observed, so it is also the
            // one place a job may progress or finish. Every call below applies
            // to the job the post-frame seam marked running and to nothing
            // else; an event arriving with no running job (the startup bake, a
            // world switch's own bake, the deferred tileset phase that follows
            // a BakeFinished) is dropped rather than attributed to whichever
            // job happens to be newest.
            if (event.type == matter::EventType::BakeStarted)
                regen_jobs.on_bake_started(viewer::jobs::Clock::now());
            else if (event.type == matter::EventType::BakePartDone)
                regen_jobs.on_part_done(event.module, event.phase, event.done,
                                        event.total);
            else if (event.type == matter::EventType::BakeError)
                regen_jobs.on_bake_error(
                    event.module, event.phase,
                    static_cast<viewer::jobs::ErrorCode>(event.code),
                    event.message);
            if (event.type == matter::EventType::BakeFinished &&
                regen_jobs.running_id() != 0) {
                bool digest_available = false;
                const uint64_t digest = regen_content_digest(digest_available);
                regen_jobs.on_bake_finished(
                    event.errors, editor_model.revision(),
                    session ? session->graph_generation() : 0, digest_available,
                    digest, viewer::jobs::Clock::now());
            }
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
            } else if (event.type == matter::EventType::BakeAborted) {
                // smart-dune.7: the terminal event for a bake that gave up.
                // Without it the log just stopped after the last bake error
                // while `bake_ready` stayed false, which reads identically to a
                // bake that is merely slow. `bake_ready` is deliberately left
                // alone here — the world on screen is still the last good one.
                std::printf("bake aborted [%s]: %s (%d error%s, gen %llu)\n",
                            event.phase.c_str(), event.message.c_str(),
                            event.errors, event.errors == 1 ? "" : "s",
                            (unsigned long long)event.bake_generation);
                std::fflush(stdout);
                console_log.push(viewer::LogSeverity::Error,
                                  "bake aborted in " + event.phase + ": " +
                                      event.message);
            }
        }
        // ---- RenderOptions assembly -----------------------------------------
        // Rebuilt from scratch every frame out of ViewerStats + EditorProps, so
        // every control is live with no separate "apply" step. Device
        // capability is ANDed in HERE, once (RT, wireframe), so no caller can
        // ask the renderer for something the GPU cannot do. The `use_*_override`
        // flags are set only once the matching stats field actually holds THIS
        // world's authored value — see fog_override_ready / sun_override_ready
        // — and are cleared again for the Part Workbench's isolation session,
        // which has its own world and its own authored fog and sun.
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
            MATTER_LOGE("editor", "FATAL: render: %s\n", error.c_str());
            mark_device_fatal(error);
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
        // ---- Post-render: QA waits, seam trace, stats mirror -----------------
        // `frame_stats` binds the session's own FrameStats and is read
        // throughout the rest of the iteration. Note it is always the
        // PRODUCTION session's, even on a frame where the Part Workbench's
        // isolation session owned the viewport.
        //
        // The long assignment block further down copies it field-by-field into
        // ViewerStats, which is what the HUD and every panel actually read.
        const matter::FrameStats& frame_stats = session->frame_stats();
        // QA timeline: wait_idle / wait_event release checks. Here (frame_stats
        // just refreshed, and bake_ready reflects this frame's poll_event
        // drain above) so both waits observe up-to-date state once per frame;
        // wait_frames' own release is symmetric, after present, below.
        if (fifo_block == FifoBlockKind::WaitIdle) {
            const uint32_t resident = frame_stats.resident_sectors;
            const auto idle_now = std::chrono::steady_clock::now();
            if (resident != fifo_wait_idle_last_resident) {
                fifo_wait_idle_last_resident = resident;
                fifo_wait_idle_last_change = idle_now;
            }
            const double settled_for =
                std::chrono::duration<double>(idle_now - fifo_wait_idle_last_change).count();
            if (bake_ready && settled_for >= fifo_wait_idle_seconds) {
                const double elapsed =
                    std::chrono::duration<double>(idle_now - fifo_wait_idle_start).count();
                std::printf("idle: settled after %.1fs\n", elapsed);
                fifo_block = FifoBlockKind::None;
            } else if (fifo_wait_idle_timeout_s > 0.0 &&
                       std::chrono::duration<double>(idle_now - fifo_wait_idle_start)
                               .count() >= fifo_wait_idle_timeout_s) {
                // D-04: explicit deadline expired without ever settling --
                // mirrors wait_event's timeout branch below: print and
                // release, the script continues rather than hanging.
                std::printf("idle: timeout after %.1fs\n", fifo_wait_idle_timeout_s);
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
            MATTER_LOGW("volumetric",
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
                MATTER_LOGE("editor", "FATAL: ImGui Vulkan backend: %s\n",
                             error.c_str());
                mark_device_fatal(error);
            }
        }

        // ---- Swapchain readback: screenshots and issue shots -----------------
        // At most ONE capture per frame, chosen by the priority of the
        // if/else-if chain below: MATTER_SCREENSHOT (which also quits once its
        // PNG lands), then a settled FIFO `shot`, then a FIFO `shot_now` via
        // fifo_present, then an F9/F10/`issue capture` readback. The issue
        // readback is deliberately NOT gated on instances_drawn — "the world
        // renders nothing" is exactly the kind of defect worth photographing.
        //
        // The readback runs BEFORE VulkanDevice::end_frame, i.e. before
        // present. A failure re-arms the relevant settle counter so the next
        // frame retries; five consecutive failures is treated as a device
        // fatal (mark_device_fatal), on the theory that it is a plausible
        // device-loss symptom.
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
            MATTER_LOGW("screenshot", "screenshot readback retry %d/5: %s\n",
                         screenshot_failures, error.c_str());
            capture = false;
            if (issue_capture) { issue_capture = false; issue_state.capture_settle = 2; }
            else if (capture_path == screenshot_path) screenshot_settle = 1;
            else if (!fifo_immediate_capture) shot_settle = 2;
            if (screenshot_failures >= 5) {
                MATTER_LOGE("screenshot", "FATAL: screenshot readback exhausted retries\n");
                // `error` still holds the last readback failure's text (the
                // retry loop above overwrites it each attempt) -- five
                // consecutive readback failures is a plausible device-loss
                // symptom, so this counts as a device fatal too.
                mark_device_fatal(error);
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
        if (frame_completed && frame_presented && !fatal_error) {
            // frame_camera is the production pose the interactive picker uses.
            presented_camera = frame_camera;
            presented_camera_available = true;
            presented_production_view = !show_isolation;
            presented_session_generation = binding.current_generation();
        }
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
            MATTER_LOGE("editor", "FATAL: end_frame: %s\n", error.c_str());
            // The primary device-fault seam: MATTER_VK_TEST_END_FRAME_FAULT
            // (record/submit) and a real VK_ERROR_DEVICE_LOST from
            // vkQueueSubmit2 both surface here.
            mark_device_fatal(error);
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
                    !cam_path_running && glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE) {
                    auto effective_camera_prefs = camera_prefs;
                    if (character_walk.enabled()) effective_camera_prefs.move_speed = 0;
                    camera_controller.update(window, dt, camera, effective_camera_prefs);
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
                        MATTER_LOGW("issue", "issue freeze preview: %s\n",
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
                        if (fifo_issue_capture_active)
                            std::printf("issue: capture failed (%s)\n",
                                        issue_state.status.c_str());
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
                        if (fifo_issue_capture_active)
                            std::printf("issue: captured %s\n", path.c_str());
                    }
                    issue_state.phase = viewer::ReporterPhase::Editing;
                    issue_state.window_open = true;
                } else {
                    console_log.push(viewer::LogSeverity::Error,
                                     "Issue shot: " + issue_state.status);
                    if (fifo_issue_capture_active)
                        std::printf("issue: capture failed (%s)\n",
                                    issue_state.status.c_str());
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
                // Where the written image starts inside the framebuffer. Only a
                // replay crop moves it; viewport.capture reports it so an agent
                // can map an image pixel back to a viewport-local one without
                // assuming the PNG is the whole swapchain.
                double out_origin_x = 0.0;
                double out_origin_y = 0.0;
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
                        MATTER_LOGW("replay",
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
                        MATTER_LOGW("replay",
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
                            MATTER_LOGI("replay",
                                         "replay: crop covered the 3D view; remapped "
                                         "through viewport_uv to %dx%d at (%d,%d)\n",
                                         rect.w, rect.h, rect.x, rect.y);
                        } else {
                            MATTER_LOGI("replay",
                                         "replay: crop is outside the 3D view (UI only); "
                                         "keeping absolute rect. Panel CONTENT at these "
                                         "pixels depends on the layout\n");
                        }
                    }
                    if (!comparable && std::getenv("MATTER_REPLAY_STRICT")) {
                        MATTER_LOGE("replay",
                                     "FATAL: MATTER_REPLAY_STRICT set and this replay "
                                     "cannot reproduce the recorded shot\n");
                        fatal_error = true;
                    }
                    out_rgba = viewer::crop_rgba(rgba, frame.extent.width,
                                                 frame.extent.height, rect);
                    out_w = static_cast<uint32_t>(rect.w);
                    out_h = static_cast<uint32_t>(rect.h);
                    out_origin_x = rect.x;
                    out_origin_y = rect.y;
                }
                if (!write_png(capture_path, out_rgba, out_w, out_h)) {
                    MATTER_LOGE("screenshot", "screenshot FAILED %s\n",
                                 capture_path.c_str());
                    if (agent_capture.owns(capture_path))
                        resolve_agent_capture(
                            viewer::capture::Resolution::WriteFailed, nullptr,
                            nullptr, nullptr);
                    fatal_error = true;
                } else {
                    bool completion_written = true;
                    if (capture_path == shot_path || fifo_immediate_capture) {
                        const std::string done = capture_path + ".done";
                        completion_written =
                            viewer::write_screenshot_completion_marker(done);
                    }
                    if (!completion_written) {
                        MATTER_LOGE(
                            "screenshot",
                            "screenshot completion marker FAILED %s.done\n",
                            capture_path.c_str());
                        if (agent_capture.owns(capture_path))
                            resolve_agent_capture(
                                viewer::capture::Resolution::WriteFailed, nullptr,
                                nullptr, nullptr);
                        fatal_error = true;
                    } else {
                        screenshot_failures = 0;
                        std::printf("screenshot written to %s\n",
                                    capture_path.c_str());
                        if (agent_capture.owns(capture_path)) {
                            // Every number here is measured on THIS frame -- the
                            // one that presented and was read back -- so a resize
                            // between arming and capturing is reported as the
                            // size the PNG really is, not the size it was asked
                            // for.
                            viewer::capture::Geometry geometry;
                            const viewer::ViewportRect& capture_vp = ui.viewport_rect();
                            const ImVec2 capture_display = ImGui::GetIO().DisplaySize;
                            geometry.viewport_logical =
                                viewer::capture::Rect{capture_vp.x, capture_vp.y,
                                                      capture_vp.w, capture_vp.h};
                            geometry.framebuffer_scale_x =
                                capture_display.x > 0.0f
                                    ? frame.extent.width / capture_display.x
                                    : 1.0;
                            geometry.framebuffer_scale_y =
                                capture_display.y > 0.0f
                                    ? frame.extent.height / capture_display.y
                                    : 1.0;
                            geometry.image_width = out_w;
                            geometry.image_height = out_h;
                            geometry.image_origin_x = out_origin_x;
                            geometry.image_origin_y = out_origin_y;
                            geometry.production_view = !show_isolation;
                            // The same batched bounds the selection overlay
                            // draws from, so an annotation rectangle and the
                            // box on screen describe one box.
                            std::vector<viewer::capture::AnnotationInput> annotations;
                            const std::vector<viewer::SelectedObject>& selected =
                                selection_set.items();
                            if (agent_capture.request().annotate && !selected.empty()) {
                                std::vector<viewer::SelectionBounds> bounds(selected.size());
                                std::unique_ptr<bool[]> resolved(new bool[selected.size()]);
                                // The selection names production-world objects,
                                // so an isolation frame gets the rows without
                                // the O(entities) scan; project_annotations
                                // reports every one of them unavailable anyway.
                                if (show_isolation)
                                    std::fill(resolved.get(),
                                              resolved.get() + selected.size(), false);
                                else
                                    viewer::bounds_for_objects(selected.data(),
                                                               selected.size(), *session,
                                                               bounds.data(), resolved.get());
                                const viewer::SelectedObject* primary =
                                    selection_set.primary();
                                annotations.reserve(selected.size());
                                for (size_t i = 0; i < selected.size(); ++i) {
                                    viewer::capture::AnnotationInput input;
                                    input.object = {
                                        selected[i].kind == viewer::SelectedObject::BakedRoot
                                            ? viewer::agent::ObjectIdentity::Kind::BakedRoot
                                            : viewer::agent::ObjectIdentity::Kind::Entity,
                                        selected[i].id};
                                    input.primary = primary && *primary == selected[i];
                                    input.resolved = resolved[i];
                                    if (resolved[i]) input.bounds = bounds[i];
                                    annotations.push_back(input);
                                }
                            }
                            // render_camera, not frame_camera: an isolation
                            // frame was drawn with the workbench's pose, and
                            // reporting the production one would describe a
                            // picture nobody took.
                            resolve_agent_capture(
                                viewer::capture::Resolution::Captured, &geometry,
                                &render_camera, &annotations);
                        }
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
        //
        // D-02: the SAME condition also releases a `shot`/`shot_now` FIFO
        // block -- quit was already waiting on exactly this in-flight state,
        // so blocking timeline lines on it too costs nothing new.
        const bool fifo_shot_settled =
            shot_settle == 0 && fifo_present.pending_screenshot_path().empty();
        if (fifo_block == FifoBlockKind::Shot && fifo_shot_settled)
            fifo_block = FifoBlockKind::None;
        if (fifo_quit_pending && fifo_shot_settled) quit_requested = true;

        // FIFO `issue capture` release: the AwaitingCapture readback handshake
        // above (F10-shaped: ensure_report_dir + crop + write + record_shot,
        // or a failure branch) always resolves the phase away from
        // AwaitingCapture by the end of the frame it fires in -- either to
        // Editing (success or a logged failure) or, via the deadman above, to
        // Idle. Checked here (same seam as the shot/quit release above) so a
        // capture armed and settled within one frame iteration cannot race.
        if (fifo_block == FifoBlockKind::IssueCapture &&
            issue_state.phase != viewer::ReporterPhase::AwaitingCapture) {
            fifo_block = FifoBlockKind::None;
            fifo_issue_capture_active = false;
        }

        if (perf.enabled && perf_phase != PerfPhase::Complete && !fatal_error) {
            const auto perf_now = std::chrono::steady_clock::now();
            if (perf_phase == PerfPhase::WaitingForBake) {
                if (bake_ready && frame_stats.instances_drawn > 0) {
                    const bool static_uploads_unchanged =
                        perf_observed_static_uploads &&
                        frame_stats.vk_vertex_uploads ==
                            perf_last_static_vertex_uploads &&
                        frame_stats.vk_cluster_uploads ==
                            perf_last_static_cluster_uploads;
                    perf_static_stable_frames = static_uploads_unchanged
                        ? perf_static_stable_frames + 1u : 0u;
                    perf_last_static_vertex_uploads =
                        frame_stats.vk_vertex_uploads;
                    perf_last_static_cluster_uploads =
                        frame_stats.vk_cluster_uploads;
                    perf_observed_static_uploads = true;
                    if (perf_static_stable_frames >= kPerfStaticStableFrames) {
                        perf_phase = PerfPhase::Warming;
                        perf_phase_start = perf_now;
                        std::printf(
                            "perf: static geometry stable for %u frames; warming for %.3f seconds\n",
                            kPerfStaticStableFrames, perf.warmup_seconds);
                    }
                }
            } else if (perf_phase == PerfPhase::Warming &&
                       std::chrono::duration<double>(perf_now - perf_phase_start)
                               .count() >= perf.warmup_seconds) {
                perf_phase = PerfPhase::Sampling;
                perf_phase_start = perf_now;
                perf_start_counters = capture_perf_counters(frame_stats);
                perf_start_dlss_resets = frame_stats.dlss_reset_count;
                perf_frame_times.clear();
                perf_water_animation_times.clear();
                std::printf("perf: sampling for %.3f seconds\n",
                            perf.sample_seconds);
            } else if (perf_phase == PerfPhase::Sampling) {
                perf_frame_times.push_back(perf_frame_cadence_ms);
                perf_water_animation_times.push_back(
                    frame_stats.gpu_water_animation_ms);
                if (std::chrono::duration<double>(perf_now - perf_phase_start)
                        .count() >= perf.sample_seconds) {
                    const PerfCounters perf_finish_counters =
                        capture_perf_counters(frame_stats);
                    const uint32_t validation_errors =
                        vulkan->validation_error_count();
                    if (!write_perf_result(
                            perf, worlds[stats.world_current].world_name,
                            perf_frame_times,
                            perf_water_animation_times,
                            perf_start_counters,
                            perf_finish_counters, frame_stats, stats,
                            perf_start_dlss_resets,
                            validation_errors, perf_error)) {
                        MATTER_LOGE("perf", "FATAL: perf: %s\n",
                                     perf_error.c_str());
                        fatal_error = true;
                    } else if (validation_errors != 0) {
                        MATTER_LOGE("perf",
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
        // The world-reload prologue, shared by the viewer.reload path and the
        // job ledger's own reload/regenerate. It is the SAME prologue in both
        // cases by construction: a seeded regeneration is a reload that also
        // carries a worldSeed override, and letting the two drift would mean a
        // job-driven reroll quietly kept the previous world's fog or sun.
        auto prepare_reload_seam = [&]() {
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
        };
        if (binding.pending_reload()) {
            binding.clear_pending_reload();
            // A reload that did not come from the job queue (the toolbar
            // button, the `reload` FIFO verb) still supersedes a running job.
            // Recorded here rather than inferred from the engine's cancellation
            // BakeError, which names no job.
            regen_jobs.note_external_restart(
                "an editor reload restarted the world while this job was running",
                viewer::jobs::Clock::now());
            prepare_reload_seam();
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
                // The switch tore down the session a running job was baking in
                // and requested a bake on a different world. Recorded only on
                // the success path: a FAILED open leaves the old session and
                // its in-flight bake completely intact, so nothing was
                // superseded there.
                regen_jobs.note_external_restart(
                    "the editor switched worlds while this job was running",
                    viewer::jobs::Clock::now());
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
        // Queued regeneration jobs (regen_jobs.h) are applied LAST and only
        // when nothing else already restarted the world this frame: a job that
        // began here would otherwise be superseded by the very reload or
        // switch that ran a few lines above, before the engine ever saw it.
        // One job per seam, which is what makes `superseded` a recorded
        // decision -- begin_next() ends the previous job in the same call.
        if (!binding.pending_reload() && binding.pending_switch() < 0 &&
            regen_jobs.has_queued()) {
            viewer::jobs::Job started;
            if (regen_jobs.begin_next(viewer::jobs::Clock::now(), started)) {
                prepare_reload_seam();
                if (started.request.kind == viewer::jobs::Kind::Regenerate) {
                    binding.regenerate(started.request.seed);
                    console_log.push(viewer::LogSeverity::Info,
                                     "Job " + std::to_string(started.id) +
                                         ": regenerating with seed " +
                                         std::to_string(started.request.seed));
                } else if (started.request.kind == viewer::jobs::Kind::Parameters) {
                    binding.regenerate_parameters(started.request.parameter_module,
                                                  started.request.parameters_json);
                    console_log.push(viewer::LogSeverity::Info,
                                     "Job " + std::to_string(started.id) +
                                         ": regenerating " +
                                         started.request.parameter_module +
                                         " with typed parameter override");
                } else {
                    binding.reload();
                    console_log.push(viewer::LogSeverity::Info,
                                     "Job " + std::to_string(started.id) +
                                         ": reloading the world");
                }
            }
        }
    }

    // ---- Regeneration jobs: shutdown is a terminal outcome ----------------
    // The frame loop is over, so no queued job will ever run and no running
    // bake will ever report. Every live job fails HERE with that as its
    // reason, and every open bounded wait gets its one terminal record, so an
    // agent that was waiting learns the editor stopped rather than reading
    // "running" as the last word and timing out against a dead process.
    // Placed before any teardown: the protocol, its result-file sink and the
    // ledger are all still alive at this point.
    {
        const viewer::jobs::Clock::time_point shutdown_at = viewer::jobs::Clock::now();
        std::vector<viewer::jobs::Waiter> abandoned = regen_waits.drain();
        regen_jobs.shutdown("the editor shut down before this job finished",
                            shutdown_at);
        for (const viewer::jobs::Waiter& waiter : abandoned) {
            const viewer::jobs::Job* job = regen_jobs.find(waiter.job_id);
            agent_protocol.complete(
                waiter.request_id, waiter.ticket_id,
                viewer::agent::Status::ExecutionFailure,
                job ? viewer::jobs::wait_result_json(*job, false, shutdown_at)
                    : viewer::jobs::missing_result_json(regen_jobs, waiter.job_id),
                "the editor shut down before this job finished");
        }
        if (const std::string agent_error = agent_protocol.take_io_error();
            !agent_error.empty())
            std::fprintf(stderr, "agent protocol: %s\n", agent_error.c_str());
    }

    // ---- Auto-file an issue report on a fatal device/Vulkan error ---------
    // docs/agent/issue-system.md says "there is no automatic filing" -- that
    // stops being quite true here: a device fault means the user can no
    // longer press F9/F10 themselves (the process is already on its way
    // down), so this is the one path that files on its own. Narrowly scoped:
    // fatal_error_reason is only ever set by mark_device_fatal (see its
    // definition near `bool fatal_error`), which is wired to the Vulkan/
    // device-surfacing fatal sites (render(), end_frame(), the ImGui Vulkan
    // backend prepare/end_frame calls, and exhausted screenshot-readback
    // retries) and deliberately NOT to the non-device fatal exits
    // (MATTER_REPLAY_STRICT mismatch, a perf run's validation-error count) --
    // those keep exiting exactly as before, without a report.
    //
    // Single seam, main thread only: this runs after the loop has already
    // decided to terminate (fatal_error true), before ANY teardown below
    // (session/vulkan/ui are all still alive), so every field the writer
    // touches is exactly as valid as it is on every other frame's normal
    // filing path. A background-thread device loss (e.g. inside a bake
    // worker's own Vulkan calls, if it ever made any) would NOT reach this
    // seam -- there is no clean rendezvous for that here, so it is out of
    // scope; the only fault path this covers is one that surfaces through
    // the main thread's own render/present calls, which is what
    // MATTER_VK_TEST_END_FRAME_FAULT and a real VK_ERROR_DEVICE_LOST from
    // vkQueueSubmit2 both do.
    //
    // Best-effort by construction: every filesystem/engine call below is
    // wrapped in one try/catch, and nothing here can change `fatal_error` or
    // the exit code decided further down. A failure while trying to record
    // the original fault is swallowed, never escalated into a second one.
    if (fatal_error && !fatal_error_reason.empty()) {
        try {
            viewer::IssueReporterState auto_state;
            std::snprintf(auto_state.note, sizeof(auto_state.note),
                          "auto-filed: device fault (%s)",
                          fatal_error_reason.c_str());
            if (viewer::ensure_report_dir(auto_state)) {
                int fb_w = 0, fb_h = 0;
                glfwGetFramebufferSize(window, &fb_w, &fb_h);
                viewer::IssueContext context;
                const int world_idx = stats.world_current;
                if (world_idx >= 0 &&
                    static_cast<size_t>(world_idx) < worlds.size()) {
                    context.world = worlds[world_idx].world_name;
                    context.project_dir = worlds[world_idx].project_dir;
                }
                context.camera = camera;
                context.sim_mode = sim_control.mode();
                context.time_scale = ui.sim_time_scale();
                context.frame_width =
                    fb_w > 0 ? static_cast<uint32_t>(fb_w) : 0;
                context.frame_height =
                    fb_h > 0 ? static_cast<uint32_t>(fb_h) : 0;
                context.props = &editor_props.registry();
                // No shots: a post-loss readback would either hang against a
                // dead device or hand back garbage, and the whole point of
                // this path is that nobody is left to press F9/F10.
                const std::string filed = viewer::write_issue_report(
                    auto_state, context, stats, session->frame_stats(),
                    console_log);
                if (!filed.empty()) {
                    std::fprintf(
                        stderr, "issue: auto-filed device fault report to %s\n",
                        filed.c_str());
                    // vulkan_device_fault.log: written by vk_context.cpp's
                    // log_device_fault() only on a REAL VK_ERROR_DEVICE_LOST
                    // -- the injected end_frame faults
                    // MATTER_VK_TEST_END_FRAME_FAULT exercises never reach
                    // that call (see the #ifdef guard around end_frame's
                    // fault injection: it substitutes a different VkResult
                    // before submit, it does not put the device itself into
                    // the lost state vkGetDeviceFaultInfoEXT requires). Copy
                    // the log in only if it exists AND was written during
                    // THIS run, so a stale log left by an earlier crash is
                    // never misattributed to this one.
                    std::error_code fault_ec;
                    const std::filesystem::path fault_log(
                        "vulkan_device_fault.log");
                    if (std::filesystem::exists(fault_log, fault_ec) &&
                        !fault_ec) {
                        const auto mtime = std::filesystem::last_write_time(
                            fault_log, fault_ec);
                        if (!fault_ec && mtime >= process_start_time) {
                            std::filesystem::copy_file(
                                fault_log,
                                std::filesystem::path(filed) /
                                    "vulkan_device_fault.log",
                                std::filesystem::copy_options::overwrite_existing,
                                fault_ec);
                        }
                    }
                } else {
                    MATTER_LOGW("issue", "issue: auto-file failed (%s)\n",
                                 auto_state.status.c_str());
                }
            }
        } catch (...) {
            // Best-effort: the auto-filer must never become a second fatal
            // error on top of the one it exists to record.
            MATTER_LOGW("issue", "issue: auto-file threw while recording a "
                                 "device fault; ignored\n");
        }
    }

    // =======================================================================
    // Shutdown
    // =======================================================================
    // Order here is load-bearing and partly MANUAL. GPU-owning objects that are
    // stack locals of main() — issue_previews, the Bake Lab workbench's
    // isolation session, and the WorldSession itself — would otherwise be
    // destroyed only when main() returns, i.e. after vulkan.reset() had already
    // killed the device. Each is therefore released explicitly below, and
    // ui.shutdown() runs after issue_previews.shutdown() because the preview
    // handles are ImGui descriptor sets that need the ImGui Vulkan backend
    // alive to remove.
    //
    // Exit code: 1 if the device counted any Vulkan validation error over the
    // run, otherwise 1 if `fatal_error`, otherwise 0.
    // =======================================================================
    // Idempotent: a completed MATTER_CAM_PATH already closed it. This covers a
    // run that ended some other way (window closed, fatal error) so the trace
    // still gets its summary line rather than being silently truncated.
    viewer::lod_trace::close();
    // Same reasoning for the seam summary: a run cut short by the window
    // closing still has to say what it saw. Idempotent.
    print_seam_summary("shutdown");

#ifndef _WIN32
    // Close once and disarm: the symmetric teardown further down (the partner
    // of the Windows CloseHandle) also closes cmd_fd, and closing the same
    // descriptor twice can take out an unrelated fd that the runtime handed
    // out for the same number in between.
    if (cmd_fd >= 0) {
        close(cmd_fd);
        cmd_fd = -1;
    }
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
                MATTER_LOGW("profile", "profile: could not write trace to %s\n",
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
        MATTER_LOGE("editor", "FATAL: Vulkan validation errors: %u\n",
                     validation_errors);
        return 1;
    }
    return fatal_error ? 1 : 0;
}
