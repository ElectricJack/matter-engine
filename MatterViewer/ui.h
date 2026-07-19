#ifndef VIEWER_UI_H
#define VIEWER_UI_H

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "matter/camera.h"
#include "matter/world_session.h"
#include "streaming_anchor_controller.h"

struct GLFWwindow;
namespace matter { class VulkanDevice; struct VulkanFrame; }

namespace viewer {

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
    float    pixel_budget = 1.0f;
    // World picker: main sets `world_current` after each connect; panel writes
    // `world_switch_requested` (index into the enumerated worlds list, -1 = none).
    int      world_current = 0;
    int      world_switch_requested = -1;
    matter::VulkanLightingOverrides lighting{};
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
    bool  gpu_timers_supported  = false;
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
    void draw_debug_panel(ViewerStats& stats);
    // MSL-style orbit/zoom controls: navigate the view without locking the cursor
    // or using WASD (works over remote desktop). Mutates the camera in place.
    void draw_camera_panel(matter::CameraDesc& cam);
    // Standalone panel listing available worlds as buttons. Clicking a non-current
    // world sets stats.world_switch_requested; main handles the swap next frame.
    void draw_worlds_panel(const std::vector<WorldEntry>& worlds, ViewerStats& stats);
    void update_sector_streaming(matter::WorldSession& session,
                                 const matter::CameraDesc& camera);
    // draw_sector_streaming_panel retired in Phase 4 Task 12: sector streaming
    // editing now lives in the Properties panel via SpecializedEditors
    // (see MatterViewer/specialized_editors.h). update_sector_streaming above
    // (the per-frame anchor/follow logic) is unaffected and stays here.
    bool camera_input_allowed() const;

private:
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
    matter_viewer::StreamingAnchorState streaming_anchor_{};
    std::uint64_t anchor_id_input_ = 0;
    std::uint64_t streaming_seed_ = 0;
};

} // namespace viewer

#endif // VIEWER_UI_H
