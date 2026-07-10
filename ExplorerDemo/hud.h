#pragma once
#include "raylib.h"
#include "matter/events.h"
#include "matter/world_session.h"

#include <string>
#include <deque>

// Hud — bottom-strip loading HUD for ExplorerDemo.
//
// During initial bake:
//   - shows a progress bar (Option C: total may grow; recompute fraction per event,
//     bar may step backward; total=0 = indeterminate, shows spinner dots)
//   - shows current bake phase and done/total text
// After BakeFinished:
//   - subtle corner readout: "refined X/Y tiles near you"
// BakeError:
//   - non-fatal 6-second toast queue (module + message); never triggers exit.
//
// Stats readout (replaces old draw_hud free function) is drawn in the top-left corner.
struct Hud {
    // Feed an event from the world session event stream.
    void feed(const matter::Event& ev);

    // Draw HUD elements. Call between BeginDrawing/EndDrawing each frame.
    // w, h = framebuffer dimensions.
    // fs = current frame stats (for top-corner stats text).
    // fps = current frames per second.
    void draw(int w, int h, const matter::FrameStats& fs, float fps, float now);

private:
    // --- Bake progress state ---
    bool bake_started_  = false;
    bool bake_finished_ = false;
    int  bake_done_     = 0;
    int  bake_total_    = 0;     // 0 = indeterminate
    std::string bake_phase_;     // "install", "compose", "parts", "gl", etc.
    std::string bake_module_;    // last module name from BakePartDone
    int  bake_errors_   = 0;     // BakeFinished error count

    // --- Refine counters (RefineTileDone) ---
    int  refine_done_  = 0;
    int  refine_total_ = 0;

    // --- Toast queue ---
    struct Toast {
        std::string module;
        std::string message;
        float       expire_time = 0.0f;  // absolute time when this toast disappears
    };
    std::deque<Toast> toasts_;

    // --- Internal helpers ---
    void draw_bottom_strip(int w, int h, float now) const;
    void draw_stats_corner(int w, int h, const matter::FrameStats& fs, float fps) const;
    void draw_toasts(int w, int h, float now);
    void draw_refine_corner(int w, int h) const;
};
