#ifndef VIEWER_UI_H
#define VIEWER_UI_H

#include <cstdint>
#include <string>
#include <vector>

#include "matter/camera.h"
#include "matter/world_session.h"
#include "editor_model.h"
#include "scene_tree_panel.h"
#include "console_panel.h"
#include "toolbar_panel.h"
#include "properties_registry.h"
#include "properties_panel.h"
#include "selection_set.h"
#include "gizmo.h"

namespace viewer {

struct ViewportRect { float x = 0, y = 0, w = 0, h = 0; };

// One available world for the runtime picker. Populated by scan_worlds at
// startup; consumed by draw_worlds_panel and the main-loop switch handler.
struct WorldEntry {
    std::string label;        // display name (world .js filename stem)
    std::string project_dir;  // project containing objects/ and worlds/
    std::string world_name;   // e.g. "Demo"
};

// Scan a root like "../examples" for projects containing objects/ + worlds/.
// Every regular worlds/*.js file contributes one entry, sorted by label.
std::vector<WorldEntry> scan_worlds(const std::string& examples_root);

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
    bool     reload_requested = false;   // panel sets; main clears after handling
    // Raster-path counters (zero in RT mode). raster_batches / batch_cache_hit
    // are legacy fields kept in the struct so the HUD layout stays put; the
    // GPU-driven path always reports 0/false (there's no per-frame batch cache).
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
    float    pixel_budget = 1.0f;
    // World picker: main sets `world_current` after each connect; panel writes
    // `world_switch_requested` (index into the enumerated worlds list, -1 = none).
    int      world_current = 0;
    int      world_switch_requested = -1;
};

class Ui {
public:
    void setup();        // after InitWindow
    void shutdown();
    void begin_frame();
    void end_frame();
    void draw_debug_panel(ViewerStats& stats);
    // MSL-style orbit/zoom controls: navigate the view without locking the cursor
    // or using WASD (works over remote desktop). Mutates the camera in place.
    void draw_camera_panel(matter::CameraDesc& cam);
    // Standalone panel listing available worlds as buttons. Clicking a non-current
    // world sets stats.world_switch_requested; main handles the swap next frame.
    void draw_worlds_panel(const std::vector<WorldEntry>& worlds, ViewerStats& stats);
    ToolbarActions draw_toolbar(matter::scene::SimulationMode mode);
    void draw_viewport_window();
    const ViewportRect& viewport_rect() const { return viewport_rect_; }
    void set_hide_ui(bool hide) { hide_ui_ = hide; }
    // Task 13: see ui.h (Vulkan viewer) for the fuller doc comment; behavior
    // mirrors that build. All pointer params are nullable.
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
    // See ui.h (Vulkan viewer) for the fuller doc comment; behavior mirrors
    // that build. Sets gizmo_submitted_ so camera_input_allowed() can
    // suppress camera input while the gizmo is hovered/dragged.
    void draw_gizmo(const SelectionSet& selection, const FieldCommands& fields,
                    const matter::CameraDesc& camera,
                    matter::scene::SimulationMode mode, float viewport_x,
                    float viewport_y, float viewport_w, float viewport_h);
    // Forwards to viewer::update_gizmo_hotkeys(gizmo_state_) — call only when
    // !io.WantTextInput && !io.WantCaptureKeyboard.
    void update_gizmo_hotkeys();
    // True unless ImGui or the gizmo wants input this frame (mirrors the
    // Vulkan viewer's Ui::camera_input_allowed()).
    bool camera_input_allowed() const;

private:
    void build_dockspace();
    bool gizmo_submitted_ = false;
    bool viewport_hovered_ = false;
    bool hide_ui_ = false;
    ViewportRect viewport_rect_{};
    GizmoState gizmo_state_;
    SceneTreeState scene_tree_state_;
    ToolbarState toolbar_state_;
    ConsolePanelState console_state_;
    PropertiesPanelState properties_state_;
};

} // namespace viewer

#endif // VIEWER_UI_H
