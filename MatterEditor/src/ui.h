#ifndef VIEWER_UI_H
#define VIEWER_UI_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "matter/camera.h"
#include "matter/world_session.h"
#include "streaming_anchor_controller.h"
#include "editor_model.h"
#include "scene_tree_panel.h"
#include "console_panel.h"
#include "toolbar_panel.h"
#include "properties_registry.h"
#include "properties_panel.h"
#include "selection_set.h"
#include "gizmo.h"
#include "animation_debug_overlay.h"
#include "bake_lab.h"
#include "asset_browser.h"

struct GLFWwindow;
namespace matter { class VulkanDevice; struct VulkanFrame; namespace evt { class Hub; } }

namespace viewer {

struct ViewportRect { float x = 0, y = 0, w = 0, h = 0; };

// One available world for the runtime picker. Populated by scan_worlds at
// startup; consumed by draw_worlds_panel and the main-loop switch handler.
struct WorldEntry {
    std::string label;        // display name (world .js filename stem)
    std::string project_dir;  // project containing objects/ and worlds/
    std::string world_name;   // e.g. "Demo"
};

// Scan a root like "../projects" for projects containing objects/ + worlds/.
// Every regular worlds/*.js file contributes one entry, sorted by label.
std::vector<WorldEntry> scan_worlds(const std::string& examples_root);

// ViewerCommands — the plain-std::function bridge by which UI panels ISSUE the
// viewer's registered commands (event-system.md S I.10/S I.11, E4b). Same idiom
// as SceneCommands / FieldCommands: main.cpp populates each closure to call
// registry.execute(<typed command>) on the app lane (synchronous, same-thread
// UI trigger), so command.h stays off this header's wide include chain. These
// replace the deleted polled request flags (reload_requested /
// world_switch_requested) and the WorkbenchHandoff struct.
struct ViewerCommands {
    std::function<void()> reload;                     // viewer.reload
    std::function<void(int world_index)> switch_world;  // viewer.switch_world{index}
    // workbench.open_part{project,module} + lab.focus_tab{Workbench}: opens the
    // part in the isolation session and selects/raises the Workbench tab.
    std::function<void(const std::string& project, const std::string& module)>
        open_in_workbench;
    // viewer.reveal_part{module}: select the module's baked root in the ACTIVE
    // world (Scene-tree highlight + selection outline) and aim the camera at
    // it; reports to the console when the module isn't loaded here.
    std::function<void(const std::string& module)> reveal_part;
};

// Read-only stats the HUD displays each frame; the resolver selector is the one
// field the panel writes back. Everything else is filled by main/composer/provider.
struct ViewerStats {
    float    fps = 0.0f;
    float    frame_ms = 0.0f;
    float    cam_pos[3] = {0,0,0};
    int      instances_total = 0;
    int      instances_active = 0;
    int      occupied_sectors = 0;
    bool     connected = false;
    int      parts_baked = 0;       // last connect: cache misses
    int      cache_hits = 0;        // last connect: cache hits
    int      last_want_count = 0;   // last reconcile want-list size
    // Writable: 0 = PassThrough, 1 = SectorLod. Panel sets this; main swaps resolver.
    int      resolver_choice = 0;
    // GPU-path counters. The Vulkan GPU-driven path reports raster_batches (live
    // indirect draw buckets) and raster_tris from the cull shader stats SSBO.
    // batch_cache_hit remains legacy/always-false (no per-frame batch cache).
    int      raster_batches = 0;
    int      raster_tris = 0;
    int      culled_clusters = 0;
    bool     batch_cache_hit = false;
    // Raster-path CPU timing split (ms) — Stage 0 of the frame-time package.
    float    resolve_ms = 0.0f;   // SectorResolver::resolve
    float    build_ms   = 0.0f;   // GpuCuller::cull (upload + dispatch, no readback)
    float    draw_ms    = 0.0f;   // RasterComposer::draw_gpu_driven (CPU submit side)
    // GPU cull HUD (Task 7): active only when MATTER_GPU_CULL=1 + GL 4.6 ok.
    bool     gpu_cull_active = false;
    int      gpu_emitted = 0;   // clusters that passed the cull this frame
    int      gpu_culled  = 0;   // clusters rejected by the frustum cull this frame
    int      gpu_culled_hiz = 0;   // clusters rejected by HiZ occlusion this frame
    // Writable: HiZ occlusion toggle (Task 10). Default OFF: the previous-frame
    // pyramid causes false-positive occlusion culls at freehand camera angles
    // (terrain / tree segments disappear). Correct fix needs a same-frame
    // conservative depth or scissor-refined redraw — filed as ROADMAP follow-up.
    // HUD checkbox / FIFO `hiz on|off` / MATTER_HIZ=1 opt in.
    bool     hiz_enabled = false;
    // Writable: runtime LOD quality/speed dial. main propagates it to the
    // resolver + composer each frame; also settable via FIFO `budget <f>`.
    // 2026-07-29 alpine tuning pass default (was 1.0): hold cluster detail
    // much farther, affordable now that distant terrain is the heightfield
    // ladder.
    float    pixel_budget = 3.64f;
    // World picker: main sets `world_current` after each connect. The panel no
    // longer writes a switch-request flag — it issues ViewerCommands::switch_world
    // (index into the enumerated worlds list) instead.
    int      world_current = 0;
    matter::VulkanLightingOverrides lighting{};
    matter::VulkanVolumetricsSettings volumetrics{};
    int vol_debug_view = 0;
    matter::TilesetPomSettings tileset_pom{};
    // GPU-side per-pass timings (ms), smoothed EMA. Values are 0 when the
    // zone did not execute or GPU timers are unsupported.
    float gpu_total_ms          = 0.0f;
    float gpu_cull_ms           = 0.0f;
    float gpu_gbuffer_ms        = 0.0f;
    float gpu_blas_ms           = 0.0f;
    float gpu_tlas_ms           = 0.0f;
    float gpu_rt_ms             = 0.0f;
    float gpu_denoise_ms        = 0.0f;
    float gpu_dlss_ms           = 0.0f;
    float gpu_composite_ms      = 0.0f;
    float gpu_vol_ms            = 0.0f;
    bool  gpu_timers_supported  = false;
    int   debug_view_mode       = 0;
    // Viewer-local diagnostic toggles.  These are deliberately not part of
    // the animation service or checkpoint state.
    AnimationDebugOverlayOptions animation_overlay{};
    uint32_t animation_debug_instances = 0;
    bool animation_debug_query_ok = true;
    // Main-loop phase timings (ms), smoothed EMA. These partition the SAME
    // wall-clock window that produces `frame_ms` (perf_frame_start .. after
    // end_frame), so `frame_ms` minus their sum is genuinely unmeasured code
    // rather than a gap between two different clocks. Added because
    // resolve/build/draw accounted for under a third of the frame and we had
    // no way to attribute the rest.
    float loop_poll_ms     = 0.0f;  // events + input, up to begin_frame
    float loop_acquire_ms  = 0.0f;  // vulkan->begin_frame: fence wait + swapchain acquire
    float loop_ui_ms       = 0.0f;  // ImGui panel building
    float loop_tick_ms     = 0.0f;  // session->tick (ECS, physics, transforms)
    float loop_pump_ms     = 0.0f;  // session->pump_gpu_jobs (sector publish lands here)
    float loop_lab_ms      = 0.0f;  // Bake Lab / Workbench per-frame work
    float loop_render_ms   = 0.0f;  // session->render (contains resolve/build/draw)
    float loop_present_ms  = 0.0f;  // vulkan->end_frame: submit + present
    // Peak-hold over a decaying window. Publish/bake stalls are spiky and an
    // EMA averages them away — the peak is what actually breaks frame cadence.
    float loop_peak_pump_ms    = 0.0f;
    float loop_peak_acquire_ms = 0.0f;
};

void reset_lighting_controls(ViewerStats& stats);
void prepare_world_reload(ViewerStats& stats);
void complete_world_switch(ViewerStats& stats, bool succeeded);

class Ui {
public:
    bool setup(GLFWwindow* window, matter::VulkanDevice& vulkan,
               std::string& error);
    void shutdown();
    bool begin_frame(const matter::VulkanFrame& frame, std::string& error);
    bool end_frame(const matter::VulkanFrame& frame, std::string& error);
    void draw_debug_panel(ViewerStats& stats, const ViewerCommands& commands);

    // LOD Settings window: everything level-of-detail / draw-distance in one
    // place, split into live controls (pixel budget, camera far plane) and
    // reload-required streaming config (scatter rings, terrain LOD bands,
    // heightfield ladder toggle) with an Apply & Reload button.
    void draw_lod_settings_panel(matter::WorldSession* session,
                                 ViewerStats& stats,
                                 const ViewerCommands& commands,
                                 matter::CameraDesc& camera);
    // Bake Lab shell (bake-lab.md §II.5): "Bake Lab" window wrapping
    // BakeLab::draw_contents(), same Begin/End split as draw_console_panel.
    // No-op while lab.visible is false (window close button clears it).
    // `worlds` is threaded to the Workbench tab's part picker (W2). `handoff`
    // is the pending "Open in Workbench" request written by the standalone
    // Asset Browser pane (draw_asset_browser_panel below); a pending request
    // both consumes into PartWorkbench::open_part (inside draw_contents) and,
    // here, raises/focuses the Bake Lab window itself.
    void draw_bake_lab_panel(BakeLab& lab, matter::evt::Hub* app_hub,
                             matter::WorldSession* session,
                             const std::vector<WorldEntry>& worlds,
                             ViewerStats& stats);
    // Standalone Assets pane (promoted out of Bake Lab's former "Assets" tab
    // so it's usable during any workflow, not just baking). Owns no state
    // itself — `browser` is a loop-scope AssetBrowser instance main.cpp owns
    // next to `bake_lab`. worlds/stats/shared_lib_root are exactly what
    // AssetBrowser::draw always needed; `handoff` is where "Open in
    // Workbench" clicks issue ViewerCommands::open_in_workbench, and "Load"
    // issues ViewerCommands::switch_world.
    void draw_asset_browser_panel(AssetBrowser& browser,
                                  const std::vector<WorldEntry>& worlds, ViewerStats& stats,
                                  const std::string& shared_lib_root,
                                  const ViewerCommands& commands);
    // MSL-style orbit/zoom controls: navigate the view without locking the cursor
    // or using WASD (works over remote desktop). Mutates the camera in place.
    void draw_camera_panel(matter::CameraDesc& cam);
    // Standalone panel listing available worlds as buttons. Clicking a non-current
    // world issues ViewerCommands::switch_world (viewer.switch_world command).
    void draw_worlds_panel(const std::vector<WorldEntry>& worlds, ViewerStats& stats,
                           const ViewerCommands& commands);
    ToolbarActions draw_toolbar(matter::scene::SimulationMode mode);
    // Simulation rate, owned by the toolbar slider. Readable (and seedable)
    // even when the UI is hidden, so a headless capture can run in slow motion.
    float sim_time_scale() const { return toolbar_state_.time_scale; }
    void set_sim_time_scale(float value) { toolbar_state_.time_scale = value; }
    void prepare_viewport_rect();
    void draw_viewport_window();
    const ViewportRect& viewport_rect() const { return viewport_rect_; }
    void set_hide_ui(bool hide) { hide_ui_ = hide; }
    matter::VulkanFrame viewport_render_frame(const matter::VulkanFrame& frame,
                                               std::string& error);
    void transition_viewport_for_sampling(VkCommandBuffer cmd);
    bool has_viewport_target() const { return rt_image_ != VK_NULL_HANDLE; }
    // Task 13: `commands` drives the row context menus (Add Child Entity,
    // Duplicate, Delete, reparent-on-add-child); `mode` disables the
    // destructive items during Play; `camera`/`fields` drive the Focus
    // action; `selection` is kept in sync with context-menu-driven selection
    // changes so the Properties panel/gizmo follow along; `console_log`
    // receives error messages from failed mutations. All pointer params are
    // nullable — passing null simply disables/no-ops the features that need
    // them (see scene_tree_panel.h).
    void draw_scene_panel(EditorModel& editor, matter::WorldSession* session,
                          SceneCommands* commands, matter::scene::SimulationMode mode,
                          matter::CameraDesc* camera, SelectionSet* selection,
                          const FieldCommands* fields, ConsoleLog* console_log,
                          const std::unordered_set<uint64_t>* authored_entity_ids = nullptr);
    void draw_properties_panel(const SelectionSet& selection, EditorModel& editor,
                               const PropertiesRegistry& registry,
                               const FieldCommands& fields,
                               const ComponentCommands& components,
                               matter::scene::SimulationMode mode,
                               const part_graph_snapshot::Snapshot* snapshot,
                               SpecializedEditors& specialized,
                               const matter::Float3& camera_position);
    void draw_console_panel(ConsoleLog& log);
    // Draws the ImGuizmo transform gizmo for the primary selection (Task 10).
    // No-op outside Edit/Pause or when nothing selectable is chosen. Sets
    // gizmo_submitted_ so camera_input_allowed() can suppress camera input
    // while the gizmo is hovered/dragged. Call once per frame, after
    // draw_properties_panel and before camera_input_order.decide_capture().
    void draw_gizmo(const SelectionSet& selection, const FieldCommands& fields,
                    const matter::CameraDesc& camera,
                    matter::scene::SimulationMode mode, float viewport_x,
                    float viewport_y, float viewport_w, float viewport_h);
    // Forwards to viewer::update_gizmo_hotkeys(gizmo_state_) — call only when
    // !io.WantTextInput && !io.WantCaptureKeyboard.
    void update_gizmo_hotkeys();
    // World-kind (streaming) sessions draw geometry ONLY from streamed
    // sectors, and the engine refuses to construct a SectorStreamer until an
    // ECS entity carrying matter::streaming::SectorStreaming exists
    // (sector_streaming_coordinator.cpp gates on worker_owner_ + worker_anchor_,
    // both of which originate from that component's OnAdd/OnSet observer).
    // Retiring the sector panel removed the only production code that created
    // one, so streaming worlds rendered nothing. Call this once the session is
    // Ready (after BakeFinished); it creates + attaches the anchor with
    // follow_editor_camera enabled so update_sector_streaming below keeps it on
    // the camera. Idempotent, and a no-op for closed-world sessions (detected
    // via WorldSession::sea_level, which only succeeds for world-kind).
    // Returns true only on the call that actually created the anchor.
    bool ensure_streaming_anchor(matter::WorldSession& session);
    void update_sector_streaming(matter::WorldSession& session,
                                 const matter::CameraDesc& camera);
    // draw_sector_streaming_panel retired in Phase 4 Task 12: sector streaming
    // editing now lives in the Properties panel via SpecializedEditors
    // (see MatterEditor/src/specialized_editors.h). update_sector_streaming above
    // (the per-frame anchor/follow logic) is unaffected and stays here.
    bool camera_input_allowed() const;
    void reset_scene_tree_cache();
    // viewer.reveal_part: record the hash a Scene-tree baked-root click would
    // record, so the revealed row highlights on the next draw.
    void select_baked_root(uint64_t resolved_hash);

private:
    // LOD Settings panel state: a live mirror of the session's active
    // streaming profile until the user edits (dirty), then a held draft
    // until Apply & Reload or Reset.
    struct LodSettingsState {
        matter::WorldSession::StreamingLodConfig edit;
        bool have = false;
        bool dirty = false;
        // Active drag handle per transition bar (-1 = none).
        int drag_scatter = -1;
        int drag_bands = -1;
    };
    LodSettingsState lod_settings_;

    void build_dockspace();
    bool ensure_viewport_target(uint32_t width, uint32_t height,
                                VkFormat format, std::string& error);
    void destroy_viewport_target();
    bool initialize_vulkan_backend(VkFormat format, std::uint32_t image_count,
                                   std::string& error);
    bool prepare_vulkan_backend(const matter::VulkanFrame& frame,
                                std::string& error);
    matter::VulkanDevice* vulkan_ = nullptr;
    std::uint64_t descriptor_pool_ = 0;
    std::uint32_t image_count_ = 0;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    bool imgui_context_initialized_ = false;
    bool glfw_backend_initialized_ = false;
    bool vulkan_backend_initialized_ = false;
    bool gizmo_submitted_ = false;
    bool viewport_hovered_ = false;
    bool hide_ui_ = false;
    ViewportRect viewport_rect_{};
    VkImage rt_image_ = VK_NULL_HANDLE;
    VkImageView rt_view_ = VK_NULL_HANDLE;
    VkDeviceMemory rt_memory_ = VK_NULL_HANDLE;
    VkDescriptorSet rt_descriptor_ = VK_NULL_HANDLE;
    VkSampler rt_sampler_ = VK_NULL_HANDLE;
    uint32_t rt_width_ = 0;
    uint32_t rt_height_ = 0;
    uint32_t pending_rt_w_ = 0;
    uint32_t pending_rt_h_ = 0;
    int pending_rt_frames_ = 0;
    matter_viewer::StreamingAnchorState streaming_anchor_{};
    std::uint64_t anchor_id_input_ = 0;
    std::uint64_t streaming_seed_ = 0;
    GizmoState gizmo_state_;
    SceneTreeState scene_tree_state_;
    ToolbarState toolbar_state_;
    ConsolePanelState console_state_;
    PropertiesPanelState properties_state_;
};

} // namespace viewer

#endif // VIEWER_UI_H
