#ifndef VIEWER_ISSUE_REPORTER_H
#define VIEWER_ISSUE_REPORTER_H

// issue_reporter.h — capture a defect the instant you see it, describe it in
// one box, move on.
//
// The flow is capture-first, on purpose. Opening a form and *then* asking the
// user to line up a shot loses the moment: by the time the dialog is up the
// frame has moved on, and every field you have to fill before capturing is a
// reason not to bother. So:
//
//   F9  — freezes the screen immediately, then you drag a box over the part
//         that is wrong. The crop comes out of the frozen frame, so an
//         animating scene cannot slide out from under the selection.
//   F10 — grabs the active viewport as-is, no drag. For "just take the shot".
//
// Either way the shot lands in the open report and the report window comes up.
// Keep hitting F9/F10 to add more; they accumulate until you file.
//
// There is no title, kind, severity or area field. Classifying a defect is
// work the ingestion pass can do by reading the report, and asking for it here
// only buys a slower capture and worse-labelled data (nobody picks a careful
// severity mid-flight). What the reporter collects is what *only* the person
// at the keyboard knows: the pixels, the machine state, and a sentence.
//
// Output is one directory per report under $MATTER_ISSUE_DIR (default
// ../issues — repo-relative, since the editor is always launched from
// MatterEditor/ and asset lookup is cwd-relative too):
//
//   issues/<guid>/
//     issue.md       the report; its Repro section is a runnable command
//     state.json     per-shot camera/counts + deep engine state at file time
//     log-tail.txt   the last console lines before filing
//     shot-1.png ... one per capture, cropped as selected
//
// The directory name is a bare GUID: naming costs thought at exactly the wrong
// moment. Ordering lives in the frontmatter (`reported:`), and ingestion
// renames the directory once it has read the report.
//
// The split across two TUs mirrors console_log.cpp / console_panel.cpp: this
// header's writer half is ImGui-free (issue_reporter.cpp) so the schema and
// pixel math can be exercised headlessly; the dialog lives in
// issue_reporter_panel.cpp.

#include <cstdint>
#include <string>
#include <vector>

#include "matter/camera.h"
#include "matter/scene.h"

namespace matter { struct FrameStats; }

namespace viewer {

class ConsoleLog;
struct ViewerStats;
struct ViewportRect;

// What part of the frame a shot kept.
enum class ShotRegion : uint8_t {
    FullWindow,  // whole swapchain, panels included
    Viewport,    // the 3D viewport rect only (F10)
    Custom       // drag-selected rectangle (F9)
};

// A framebuffer sub-rectangle in swapchain pixels. Empty (w or h == 0) means
// the whole frame.
struct ShotRect {
    int32_t x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

// One captured frame, plus everything needed to take it again. This is the
// difference between a report that is evidence and a report that is a test: an
// agent can replay the shot (MATTER_REPLAY, see shot_replay.h), fix, replay
// again, and diff the two PNGs instead of arguing about a description.
//
// World and the render toggles are PER SHOT, not per report: a world switch or
// a DLSS change between two shots would otherwise be recorded wrong for one of
// them, and both change what the pixels look like.
struct IssueShot {
    std::string file;     // "shot-1.png", relative to the report directory
    std::string caption;  // editable in the report window, after the fact
    std::string world;
    ShotRegion region = ShotRegion::FullWindow;
    ShotRect rect{};      // crop, in framebuffer pixels
    // The 3D viewport at capture time, also in framebuffer pixels. Lets a
    // reader tell whether a crop covers the scene, the panels, or straddles
    // both — and lets a replay at a different resolution crop the equivalent
    // slice of the view rather than the same absolute pixels.
    ShotRect viewport{};
    uint32_t frame_width = 0, frame_height = 0;  // whole framebuffer
    matter::CameraDesc camera{};
    matter::scene::SimulationMode sim_mode = matter::scene::SimulationMode::Edit;
    float time_scale = 1.0f;
    // Render state that changes pixels. Without these a replay can match the
    // framing and still not match the image.
    std::string dlss_mode = "native";
    float pixel_budget = 1.0f;
    int resolver_choice = 0;
    int debug_view_mode = 0;
    bool ui_visible = true;
    // The ImGui layout at capture time (SaveIniSettingsToMemory). The viewport
    // rect is entirely a function of the docked layout, so recording the window
    // size without this reproduces neither the viewport nor any panel position.
    // Written beside the shot as shot-N.layout.ini and reloaded on replay.
    std::string layout_ini;
    uint32_t width = 0, height = 0;  // written PNG dimensions
    // ImTextureID for the thumbnail (VkDescriptorSet under the Vulkan
    // backend). void* keeps imgui.h off this header; null if the upload failed,
    // which costs a preview and nothing else.
    void* preview = nullptr;
    uint32_t preview_width = 0, preview_height = 0;
    // Enough per-shot telemetry to tell two shots apart at a glance; the deep
    // state lives once at report level.
    float frame_ms = 0.0f;
    uint32_t instances_drawn = 0;
    uint32_t triangles = 0;
    uint32_t draw_batches = 0;
};

// What the reporter is doing right now. The capture is a two-frame handshake
// with main.cpp's readback: arm, then consume.
enum class ReporterPhase : uint8_t {
    Idle,
    AwaitingCapture,   // readback armed; the window hides itself
    SelectingRegion,   // frozen frame on screen, dragging a box over it
    Editing            // report window up
};

struct IssueReporterState {
    ReporterPhase phase = ReporterPhase::Idle;
    bool window_open = false;

    // The one thing we ask for. Sized for a paragraph, not an essay.
    char note[4096] = {};

    // --- open-report accumulation ------------------------------------------
    // Non-empty once the first shot lands: the report directory is created
    // lazily so a capture you abandon leaves nothing behind.
    std::string dir;
    std::string id;
    std::vector<IssueShot> shots;
    // Monotonic, NOT shots.size()+1: removing a shot from the middle of the
    // list would otherwise make the next capture reuse a filename that is still
    // on disk and still referenced by an earlier entry.
    int next_shot_index = 1;

    // --- pending capture ----------------------------------------------------
    // Frames are counted down so the reporter window is gone from the frame
    // that is actually read back — otherwise every UI defect report would have
    // this dialog sitting on top of the evidence.
    int capture_settle = 0;
    bool capture_is_region_pick = false;  // F9 (drag) vs F10 (straight to shot)
    ShotRegion pending_region = ShotRegion::FullWindow;
    ShotRect pending_rect{};

    // --- frozen frame being selected over (F9) -------------------------------
    std::vector<uint8_t> frozen;          // RGBA8 of the whole swapchain
    uint32_t frozen_width = 0, frozen_height = 0;
    void* frozen_preview = nullptr;       // ImTextureID for the backdrop
    // Set when the overlay resolves a selection; main.cpp then crops the frozen
    // buffer, writes the PNG and clears it. The panel cannot do that itself —
    // file IO and texture upload are main's, not the draw pass's.
    bool selection_committed = false;
    bool drag_active = false;
    float drag_start[2] = {0, 0};
    float drag_end[2] = {0, 0};

    std::string status;
    bool status_is_error = false;
};

// Where reports are written. Precedence: $MATTER_ISSUE_DIR, then whatever
// set_issues_dir() was given, then "../issues".
//
// That last fallback is only correct when the editor is launched from
// MatterEditor/, and it is routinely launched from build/windows/ or from a
// packaged dist folder instead — which silently buried reports under
// MatterEditor/build/. main.cpp resolves the real location the same way it
// resolves asset roots (walking up from the executable, then the working
// directory) and hands it over via set_issues_dir at startup.
std::string issues_dir();
void set_issues_dir(const std::string& dir);

// Creates the report directory if it does not exist yet, storing the path and
// id on `state`. Returns false (and sets state.status) if it cannot.
bool ensure_report_dir(IssueReporterState& state);

// Crops `rgba` (tightly packed RGBA8, src_w x src_h) to `rect`, clamped to the
// source bounds. Returns the cropped pixels and writes back the resolved rect.
// A rect that is empty or covers everything returns the source unchanged.
std::vector<uint8_t> crop_rgba(const std::vector<uint8_t>& rgba, uint32_t src_w,
                               uint32_t src_h, ShotRect& rect);

// Box-filters RGBA8 down so the longest edge is at most `max_edge`, for
// thumbnails. Returns the source unchanged when it already fits.
std::vector<uint8_t> downscale_rgba(const std::vector<uint8_t>& rgba,
                                    uint32_t src_w, uint32_t src_h,
                                    uint32_t max_edge, uint32_t& out_w,
                                    uint32_t& out_h);

// Frame-time facts main.cpp knows and the writer cannot look up itself.
struct IssueContext {
    std::string world;
    std::string project_dir;
    matter::CameraDesc camera{};
    matter::scene::SimulationMode sim_mode = matter::scene::SimulationMode::Edit;
    float time_scale = 1.0f;
    uint32_t frame_width = 0;
    uint32_t frame_height = 0;
};

// Records a shot main.cpp has just written to disk.
void record_shot(IssueReporterState& state, const IssueShot& shot);

// Writes issue.md / state.json / log-tail.txt into the report directory.
// Returns the directory path; on failure returns "" and sets state.status.
std::string write_issue_report(IssueReporterState& state,
                               const IssueContext& context,
                               const ViewerStats& stats,
                               const matter::FrameStats& frame_stats,
                               const ConsoleLog& log);

// --- dialog (issue_reporter_panel.cpp, ImGui) --------------------------------

// F9: freeze the screen, then drag a region out of it.
void begin_region_capture(IssueReporterState& state);
// F10: grab the active viewport as-is.
void begin_viewport_capture(IssueReporterState& state);

// True while a region drag is in progress: main.cpp suppresses camera input so
// dragging a box does not also fly the camera.
bool issue_reporter_wants_mouse(const IssueReporterState& state);

// Draws the frozen-frame selection overlay and/or the report window.
// Returns true on the frame "File report" was pressed.
bool draw_issue_reporter(IssueReporterState& state, uint32_t frame_width,
                         uint32_t frame_height);

} // namespace viewer

#endif // VIEWER_ISSUE_REPORTER_H
