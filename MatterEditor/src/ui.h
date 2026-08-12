#ifndef VIEWER_UI_H
#define VIEWER_UI_H

#include <cstdint>
#include "matter/props.h"

#ifndef VIEWER_UI_STATUS_ONLY
#include <functional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "matter/camera.h"
#include "matter/world_session.h"
#include "camera_controller.h"   // CameraPrefs (the camera.prefs group)
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
#include "property_editor.h"
#include "streaming_lod_prefs.h"
#endif

#ifndef VIEWER_UI_STATUS_ONLY
struct GLFWwindow;
namespace matter { class VulkanDevice; struct VulkanFrame; namespace evt { class Hub; } }
#endif

namespace viewer {

#ifndef VIEWER_UI_STATUS_ONLY
struct ViewportRect { float x = 0, y = 0, w = 0, h = 0; };

// One available world for the runtime picker. Populated by scan_worlds at
// startup; consumed by the Assets panel's per-world Load button
// (asset_browser.cpp) and the main-loop switch handler.
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
#endif

enum class ViewerRenderPathStatus : int32_t {
    Raster = 0, NativeRt = 1, NativeRtUnavailable = 2
};
struct ViewerSessionStatus {
    ViewerRenderPathStatus render_path = ViewerRenderPathStatus::Raster;
    uint64_t presented_frame_serial = 0;
    bool native_rt_available = false;
};
struct ViewerAtmosphereStatus {
    uint64_t generation_serial = 0;
    float resolved_elevation_deg = 0.0f;
    matter::Float3 atmospheric_direct_base_rgb{};
    matter::Float3 atmospheric_noon_direct_base_rgb{};
    float direct_world_ratio = 0.0f;
    matter::Float3 direct_base_rgb{};
    matter::Float3 direct_world_sun_rgb{};
    float sky_ambient_ratio = 0.0f;
    matter::Float3 sky_display_modifier_rgb{};
    matter::Float3 sky_irradiance_modifier_rgb{};
};

inline const matter::props::Group& viewer_session_status_group() {
    static const char* const render_path_labels[] = {
        "raster", "native_rt", "native_rt_unavailable"};
    using S = ViewerSessionStatus;
    static const auto definition = matter::props::group<S>(
        "viewer.session", "Viewer Session Status",
        matter::props::prop(&S::render_path, "render_path")
            .enums(render_path_labels, 3).read_only().no_serialize(),
        matter::props::prop(&S::presented_frame_serial,
                            "presented_frame_serial")
            .read_only().no_serialize(),
        matter::props::prop(&S::native_rt_available, "native_rt_available")
            .read_only().no_serialize());
    return definition.group();
}

inline const matter::props::Group& viewer_atmosphere_status_group() {
    using S = ViewerAtmosphereStatus;
    static const auto definition = matter::props::group<S>(
        "viewer.atmosphere_status", "Viewer Atmosphere Status",
        matter::props::prop(&S::generation_serial, "generation_serial")
            .read_only().no_serialize(),
        matter::props::prop(&S::resolved_elevation_deg,
                            "resolved_elevation_deg")
            .read_only().no_serialize(),
        matter::props::prop(&S::atmospheric_direct_base_rgb,
                            "atmospheric_direct_base_rgb")
            .read_only().no_serialize(),
        matter::props::prop(&S::atmospheric_noon_direct_base_rgb,
                            "atmospheric_noon_direct_base_rgb")
            .read_only().no_serialize(),
        matter::props::prop(&S::direct_world_ratio, "direct_world_ratio")
            .read_only().no_serialize(),
        matter::props::prop(&S::direct_base_rgb, "direct_base_rgb")
            .read_only().no_serialize(),
        matter::props::prop(&S::direct_world_sun_rgb,
                            "direct_world_sun_rgb")
            .read_only().no_serialize(),
        matter::props::prop(&S::sky_ambient_ratio, "sky_ambient_ratio")
            .read_only().no_serialize(),
        matter::props::prop(&S::sky_display_modifier_rgb,
                            "sky_display_modifier_rgb")
            .read_only().no_serialize(),
        matter::props::prop(&S::sky_irradiance_modifier_rgb,
                            "sky_irradiance_modifier_rgb")
            .read_only().no_serialize());
    return definition.group();
}

#ifndef VIEWER_UI_STATUS_ONLY
// Read-only stats the HUD displays each frame; the resolver selector is the one
// field the panel writes back. Everything else is filled by main/composer/provider.
struct ViewerStats {
    ViewerSessionStatus session_status{};
    ViewerAtmosphereStatus atmosphere_status{};
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
    // GPU-path counters. The Vulkan GPU-driven path reports raster_batches (live
    // indirect draw buckets) and raster_tris from the cull shader stats SSBO.
    // batch_cache_hit remains legacy/always-false (no per-frame batch cache).
    int      raster_batches = 0;
    int      raster_tris = 0;
    int      culled_clusters = 0;
    // M2.5 terminal impostors currently holding an atlas slot.
    int      resident_impostors = 0;
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
    // 2026-07-30 tuning pass (3.64 -> 2.01, itself up from 1.0 on 2026-07-29):
    // holding cluster detail as far as 3.64 did was affordable in triangles
    // once distant terrain became the heightfield ladder, but not in the
    // per-publish instance work it kept alive. 2.01 is the settled dial.
    float    pixel_budget = 2.01f;
    // World picker: main sets `world_current` after each connect. The panel no
    // longer writes a switch-request flag — it issues ViewerCommands::switch_world
    // (index into the enumerated worlds list) instead.
    int      world_current = 0;
    // Includes independent visible-sky, post-9SH ambient, world-sun and disc
    // presentation controls; reset/reload always replaces the whole snapshot.
    matter::VulkanLightingOverrides lighting{};
    matter::AtmosphereSettings atmosphere{};
    matter::VulkanVolumetricsSettings volumetrics{};
    matter::CloudShadowSettings cloud_shadows{};
    // Allocation/cost readouts for the Lighting panel. main.cpp updates the
    // requested values from the live registered settings before submitting a
    // frame; the renderer may later replace effective_froxel/error when an
    // allocation fallback is needed.
    matter::FroxelGridDimensions requested_froxel{};
    matter::FroxelGridDimensions effective_froxel{};
    uint64_t froxel_bytes = 0;
    uint64_t cloud_shadow_bytes = 0;
    std::string last_volumetric_allocation_error;
    // Live override of the world-authored fog (property group "render.fog",
    // Scope::World). Seeded from WorldSession::world_fog at every connect —
    // which is what makes the group's layer-2 baseline the authored values —
    // and pushed back through RenderOptions::fog_override every frame. The
    // volumetrics multipliers above modulate whatever lands here.
    matter::FogSettings fog{};
    int vol_debug_view = 0;
    matter::TilesetPomSettings tileset_pom{};
    // Phase 0 (property group "render.vt", Scope::World). Used to be derived
    // from tileset_pom's distance pair inside gbuffer.frag; see
    // matter::VtNearBandSettings.
    matter::VtNearBandSettings vt_near_band{};
    // GPU-side per-pass timings (ms), smoothed EMA. Values are 0 when the
    // zone did not execute or GPU timers are unsupported.
    float gpu_total_ms          = 0.0f;
    float gpu_cull_ms           = 0.0f;
    float gpu_gbuffer_ms        = 0.0f;
    float gpu_blas_ms           = 0.0f;
    float gpu_tlas_ms           = 0.0f;
    float gpu_rt_ms             = 0.0f;  // primary/shadow trace only
    float gpu_rt_gi_ms          = 0.0f;  // GI/reflection trace (0 when GI off)
    float gpu_denoise_ms        = 0.0f;
    float gpu_dlss_ms           = 0.0f;
    float gpu_composite_ms      = 0.0f;
    float gpu_vol_ms            = 0.0f;
    float gpu_atmosphere_ms     = 0.0f;
    float gpu_cloud_shadows_ms  = 0.0f;
    float gpu_vol_density_ms    = 0.0f;
    float gpu_vol_scatter_ms    = 0.0f;
    float gpu_vol_integrate_ms  = 0.0f;
    bool  gpu_timers_supported  = false;
    int   debug_view_mode       = 0;
    // Wireframe debug view. `wireframe` is the session-owned request (the
    // Debug View checkbox and the FIFO `wireframe` verbs both write it);
    // `wireframe_available` is seeded once from the device and is NOT a
    // preference -- it is why the control is disabled when it is. The reason
    // string is shown rather than swallowed, because a wireframe view that
    // silently renders solid is worse than an absent one.
    bool  wireframe             = false;
    // Intra-cell impostor parallax (Debug View panel). Default ON = the
    // shipped look; off is a measurement aid, not a fix.
    bool  impostor_parallax     = true;
    // Freeze the STREAMING ANCHOR at wherever it is, so the camera stops
    // dragging the terrain's level-of-detail around with it. Inspection aid:
    // it lets you fly up to a seam and look at the exact geometry that was
    // selected from a distance, instead of watching the tiles refine out from
    // under you as you approach.
    //
    // Gated at the update_sector_streaming call site rather than by clearing
    // StreamingAnchorState::follow_editor_camera, because that flag also
    // decides whether the anchor gizmo is draggable
    // (streaming_anchor_controller.cpp: gizmo_translation_allowed) -- freezing
    // the view should not silently hand the anchor to the gizmo.
    //
    // What this does NOT freeze: the virtual texture (pages stay demand-driven
    // off the real camera, which is what you want -- you are flying in to LOOK
    // at the surface), and the GPU cull's per-cluster rung, which still comes
    // from projected size. Force that separately with the draw.overrides
    // per-module LOD if a part's own ladder is what you are chasing.
    bool  freeze_stream_anchor  = false;
    // Freeze the CULL camera (RenderOptions::freeze_cull_camera). The frame
    // keeps being drawn from the live camera; only the frustum planes and the
    // eye the cull dispatch tests against are pinned. Fly out afterwards and
    // what is left on screen is exactly what the cull pass kept for the frozen
    // view -- which is the only way to look at a culling decision, since an
    // over-aggressive cull and a correct one are identical from inside the
    // frustum until something pops.
    //
    // Distinct from freeze_stream_anchor above, and usually wanted with it: the
    // anchor freeze holds which tiles EXIST and at what rung, this holds which
    // of them are DRAWN. Freezing only the cull leaves the streamer refining
    // the world around a camera the cull cannot see.
    bool  freeze_cull_camera    = false;
    // The camera pose captured when freeze_cull_camera last went on, kept so
    // the viewport overlay can draw that frustum. Reconstructed by the editor
    // rather than read back from the renderer: the overlay is a sketch of where
    // the frozen eye was, and a debug line that needs the renderer's exact
    // jittered projection to be worth drawing would be a worse debug line.
    matter::CameraDesc frozen_cull_camera{};
    bool  frozen_cull_camera_valid = false;
    // Sub-pixel floor cull (RenderOptions::min_projected_size). A part whose
    // projected size falls below this is floor-culled at RESOLVE time, so its
    // instances are never created -- they never reach cull.instances, the
    // temporal mirror, update_instances or the RT lane. 0 = off, which is the
    // StreamMountain default; Meadow ships 0.0015. Seeded per world by
    // apply_world_resolver_defaults, then live-overridable here.
    float min_projected_size    = 0.0f;
    // (No active_radius and no resolver_choice. The engine has one resolver,
    // and its activation radius is derived from the world's outermost terrain
    // LOD band -- see RenderOptions in matter/world_session.h. Edit the band
    // table to move it.)
    bool  wireframe_available   = false;
    std::string wireframe_unavailable_reason;
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
    // Chart-space virtual texturing census. vt_rejected_variants != 0 means
    // some part of the world was refused a VT variant and is rendering the
    // legacy path — i.e. it IGNORES its authored surfaces() classification.
    // That degradation is otherwise invisible in-app, so the viewport shows a
    // warning banner while the count is nonzero.
    bool     vt_active = false;
    uint32_t vt_variants = 0;
    uint32_t vt_max_variants = 0;
    uint32_t vt_rejected_variants = 0;
    // M6: rungs that reused an existing layer with the same parameterisation
    // (i.e. duplicate page sets NOT fetched), and rebuilds a finer rung forced.
    uint64_t vt_shared_refs_total = 0;
    uint64_t vt_finer_rebuilds_total = 0;
    uint32_t vt_pool_used = 0;
    uint32_t vt_pool_capacity = 0;
    uint32_t vt_pool_pinned = 0;
    uint64_t vt_mesh_bytes = 0;
    uint64_t vt_mesh_budget_bytes = 0;
};

// StreamingLodPrefs (the persisted, schema-described override) -> the engine
// struct set_streaming_lod_overrides consumes. Empty ring strings decode to
// empty lists, which the engine documents as "keep the world's own values", so
// a default-constructed prefs is a no-op override.
matter::WorldSession::StreamingLodConfig streaming_config_from(
    const StreamingLodPrefs& prefs);

void reset_lighting_controls(ViewerStats& stats);
// Every Scope::World property group's backing struct dropped to its compiled
// default (layer 1), so the incoming world's authored values are the only
// layer 2 the baseline capture can see. Called at the reload / switch seams.
void reset_world_scope_controls(ViewerStats& stats);
void prepare_world_reload(ViewerStats& stats);
void complete_world_switch(ViewerStats& stats, bool succeeded);

class Ui {
public:
    bool setup(GLFWwindow* window, matter::VulkanDevice& vulkan,
               std::string& error);
    void shutdown();
    bool begin_frame(const matter::VulkanFrame& frame, std::string& error);
    bool end_frame(const matter::VulkanFrame& frame, std::string& error);
    // `props` supplies the registered lighting / volumetrics / POM /
    // pixel-budget groups; the hand-written sliders for those were deleted in
    // favor of the generic renderer (property-system design S6.2).
    void draw_debug_panel(ViewerStats& stats, const ViewerCommands& commands,
                          EditorProps& props);
    // Tunables window: the catch-all panel every registered property group
    // appears in. Same Begin/End split as draw_console_panel.
    void draw_tunables_panel(EditorProps& props);

    // Lighting window: render.lighting (incl. the sun/sky tints) and
    // render.volumetrics, plus their shared "Reset to World". Docked with
    // Properties/Tunables. Body lives in property_editor.cpp
    // (draw_lighting_contents) — it is pure group drawing with no panel state.
    void draw_lighting_panel(EditorProps& props);

    // Performance window (replaces the former "LOD Settings"): every knob that
    // trades quality for frame time, in one place —
    //   viewer.budget, render.pom, vt.residency  (live)
    //   camera.prefs                             (live: far plane, fly speed)
    //   stream.lod                               (RequiresReload draft: scatter
    //                                             rings, terrain LOD bands,
    //                                             heightfield ladder)
    // plus the hand-written LOD transition bars, which edit the SAME props
    // draft the schema fields do and are committed by the generic Apply &
    // Reload bar. `camera` supplies only the transition bars' axis scale.
    void draw_performance_panel(matter::WorldSession* session,
                                EditorProps& props,
                                const ViewerCommands& commands,
                                const matter::CameraDesc& camera);

    // Engine profiler window (libs/ProfileLib). Self-contained: reads the
    // global profiler state, needs no session/props. Shows last-frame wall
    // stats, jitter/smoothness, a frame-time graph, a per-zone breakdown
    // averaged over the resident window, and a "dump trace" button.
    void draw_profiler_panel(const ViewerStats& stats);
    // Viewport banner shown while vt_rejected_variants != 0. Drawn on the
    // foreground draw list (like the simulation border tint) so it is visible
    // even when the Viewer Debug window is collapsed or buried — a rejected
    // variant renders plausible-looking legacy shading, which is exactly why
    // an author tuning a surfaces() tape needs an unmissable signal.
    void draw_vt_warning_banner(const ViewerStats& stats);
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
    // MSL-style move/turn/orbit/zoom controls: navigate the view without locking
    // the cursor or using WASD (works over remote desktop). Mutates the camera
    // in place. `prefs` supplies the move/turn/orbit/zoom steps and the "Orbit
    // selection" toggle (camera.prefs); the orbit DISTANCE is deliberately not a
    // pref — it is derived from the live camera's position/target every frame,
    // so there is nothing to persist. `prefs` is non-const because the panel's
    // own toggle writes back into it.
    //
    // `pivot_valid`/`pivot` are the current selection's focus point, computed by
    // the caller (main.cpp already owns the selection, the field accessors and
    // the session-backed baked-bounds provider). Passing the answer in rather
    // than the three inputs keeps Ui free of session plumbing, and lets the same
    // point drive the viewport drag-orbit in main.cpp without computing it twice.
    void draw_camera_panel(matter::CameraDesc& cam, CameraPrefs& prefs,
                           EditorProps& props, bool pivot_valid,
                           const matter::Float3& pivot);
    ToolbarActions draw_toolbar(matter::scene::SimulationMode mode);
    // Simulation rate, owned by the toolbar slider. Readable (and seedable)
    // even when the UI is hidden, so a headless capture can run in slow motion.
    float sim_time_scale() const { return toolbar_state_.time_scale; }
    void set_sim_time_scale(float value) { toolbar_state_.time_scale = value; }
    // The two panel-state structs EditorProps binds as property groups
    // ("sim.time" and "console.filters"). Exposed rather than duplicated: the
    // toolbar slider and the console checkboxes keep editing these exact
    // fields, and the groups add Tunables visibility, the FIFO `set` path and
    // (for the console) User-scope persistence on top of them.
    ToolbarState& toolbar_state() { return toolbar_state_; }
    ConsolePanelState& console_state() { return console_state_; }
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
    // `props` is only used to note_panel_home("console.filters", "Console")
    // for Tunables' de-duplication checkbox — console.filters is edited
    // directly through console_state_, never through a Binding.
    void draw_console_panel(ConsoleLog& log, EditorProps& props);
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
    // `follow == false` (ViewerStats::freeze_stream_anchor) leaves the anchor
    // where it is; the anchor's validation still runs.
    void update_sector_streaming(matter::WorldSession& session,
                                 const matter::CameraDesc& camera,
                                 bool follow);
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
    // Streaming-LOD section state (Performance panel). The DRAFT
    // (matter::props) is now the source of
    // truth for the override; this is only the decoded ring view the
    // hand-written widgets manipulate — rebuilt from the draft (or, where the
    // draft holds no override, from the session's active profile) each frame,
    // and re-encoded into the draft whenever a widget changes it.
    struct LodSettingsState {
        matter::WorldSession::StreamingLodConfig edit;
        bool have = false;
        // Active drag handle per transition bar (-1 = none).
        int drag_scatter = -1;
        int drag_bands = -1;
    };
    LodSettingsState lod_settings_;

    // The streaming half of the Performance panel: camera.prefs + stream.lod +
    // the transition bars. Split out only so the panel's early-outs ("no
    // session yet") do not have to unwind an ImGui::Begin.
    void draw_streaming_lod_section(matter::WorldSession* session,
                                    EditorProps& props,
                                    const ViewerCommands& commands,
                                    const matter::CameraDesc& camera);

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
    // Did the 3D pass render into the offscreen viewport target THIS frame?
    // Distinct from has_viewport_target(), which only says the target object
    // exists: viewport_render_frame hands back the swapchain frame unchanged
    // when the UI is hidden or when ensure_viewport_target fails, and in both
    // cases a stale rt_image_ from an earlier frame can still be alive. This
    // is the flag end_frame's load-op must key on -- see the note there.
    bool rendered_to_viewport_target_ = false;
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
    TunablesPanelState tunables_state_;
};
#endif

} // namespace viewer

#endif // VIEWER_UI_H
