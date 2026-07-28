#ifndef VIEWER_SHOT_REPLAY_H
#define VIEWER_SHOT_REPLAY_H

// shot_replay.h — take a recorded screenshot again.
//
// Every shot in an issue report (issue_reporter.h) records the state it was
// captured in: world, camera, viewport, crop, and the render toggles that
// change pixels. This reads one of those back so the editor can reproduce it
// headlessly:
//
//   MATTER_REPLAY=../issues/<guid>/state.json
//   MATTER_REPLAY_SHOT=2            (1-based; defaults to 1)
//   MATTER_REPLAY_OUT=/tmp/after.png
//   ...all three in the exe's own env prefix, then run editor.exe
//
// The point is verification, not archaeology. An agent fixing a visual defect
// replays the shot before its change and after, and diffs the two PNGs with
// MatterEngine3/tools/img_diff.py. That turns "does it look right now?" — which
// is otherwise settled by argument — into a check with an exit code.
//
// REPLAY IS BIT-EXACT once the layout is pinned. Measured on CornellBox, a
// 360x300 crop taken entirely inside the 3D viewport: three replays of one
// descriptor, two of them from different working directories, produced
// identical images — 0 differing pixels, max channel delta 0.
//
// That only holds because the replay restores the recorded layout AND stops
// ImGui touching imgui.ini (IniFilename = nullptr). Before that, consecutive
// replays of the same descriptor differed by ~27% of pixels: each run saved a
// slightly different layout on exit and the next run loaded it, changing the
// viewport size and therefore the whole render. It looked exactly like
// irreducible ray-tracing noise, and it was not.
//
// The one parameter that must match between two images you intend to compare is
// the settle count, because the denoiser accumulates: the same descriptor at 30
// vs 90 settle frames differs by 27.8% of pixels. Replays default to 90; only
// override MATTER_REPLAY_SETTLE for both sides of a comparison, never one.
// The ordinary MATTER_SCREENSHOT path keeps its 3, which is what the existing
// capture scripts expect.
//
// Two earlier claims in this comment were wrong, in opposite directions, and
// both came from measuring without controlling the environment: first that
// replay was bit-exact (the crop was mostly static panels), then that RT was
// irreducibly noisy (the layout was drifting between runs). Verify a visual
// threshold with a crop INSIDE the viewport and a pinned layout, or it is
// measuring something else.
//
// UI CROPS behave differently from viewport crops, and better in one way:
// ImGui draws deterministically, so a crop of static panels is BIT-EXACT
// replay to replay (measured: 0 differing pixels over a 410x560 left-column
// crop). The exception is live telemetry — the HUD's fps/ms/counter text
// changes every run; a 430x560 crop of that column differed in 0.07% of pixels
// with a max channel delta of 186, all of it on the twelve text rows.
//
// But UI crops do NOT survive a window resize. Panels keep their widths while
// the viewport absorbs the extra space (1280x720 -> 1600x900 grew the viewport
// from 400x340 to 720x520), and panel HEIGHTS track the window, so content
// shifts vertically: the same left-column crop differed in 24.7% of pixels
// between those two window sizes. A UI crop is therefore only comparable at the
// window size and layout it was taken at.
//
// Because of that, a crop lying entirely outside the viewport is NEVER remapped
// through viewport_uv — it has no meaningful viewport-relative position, and
// scaling it by a resized viewport sends it somewhere arbitrary (a left-column
// crop once remapped to x = -331 and came out 407x810 instead of 410x560).
// Such crops keep their absolute rect and get a warning instead.
//
// A window larger than the display is clamped by the window manager: asking for
// 5000x3000 on a 3440x1440 monitor yields 3444x1421, which the framebuffer
// check reports. MATTER_REPLAY_STRICT=1 turns any of these mismatches into exit
// 1 — use it in automated checks, where a plausible-looking image framed
// differently is far worse than a failure.
//
// ALSO NOT REPRODUCIBLE:
//   - DLSS is temporal AND resolution-dependent. Replay forces Native and says
//     so when the shot used something else.
//   - Play-mode shots depend on where the simulation had got to. The descriptor
//     records the transport state, not a tick count, so an animated subject
//     lands at a different phase. Pause before capturing anything you intend to
//     diff.
//   - Streaming worlds depend on which sectors are resident.
//   - Bakes are not bit-deterministic (see CLAUDE.md).

#include <cstdint>
#include <string>

#include "issue_reporter.h"  // ShotRect

namespace viewer {

struct ShotReplay {
    bool valid = false;
    std::string error;

    std::string world;
    float eye[3] = {0, 0, 0};
    float target[3] = {0, 0, 0};
    float up[3] = {0, 1, 0};
    float fov_radians = 0.0f;   // 0 = leave at the engine default
    float near_plane = 0.0f;
    float far_plane = 0.0f;

    uint32_t frame_width = 0, frame_height = 0;
    ShotRect rect{};      // crop to apply before writing the PNG
    ShotRect viewport{};  // where the 3D view was, in framebuffer pixels
    // The crop as a fraction of the viewport (u0, v0, u1, v1). Present only
    // when the shot recorded a viewport. If the replay's viewport does not
    // match the recorded one — a different panel layout will do that — the crop
    // is remapped through this so the same CONTENT stays in frame, even though
    // the pixels are then no longer comparable to the original shot.
    bool has_viewport_uv = false;
    float viewport_uv[4] = {0, 0, 1, 1};

    // Contents of the shot's ImGui layout sidecar, already read off disk
    // (resolved relative to the descriptor). Empty when the shot predates
    // layout capture, in which case the replay falls back to ImGui defaults and
    // says so. Restoring this is what makes the viewport land where it did.
    std::string layout_ini;

    std::string sim_mode = "Edit";
    float time_scale = 1.0f;

    std::string dlss_mode = "native";
    float pixel_budget = 1.0f;
    int resolver_choice = 0;
    int debug_view_mode = 0;
    bool ui_visible = true;
};

// Reads shot `shot_index` (1-based) out of a JSON document containing a
// top-level "shots" array — an issue report's state.json, or any file with the
// same shape. Returns a ShotReplay with valid=false and `error` set on failure.
ShotReplay load_shot_replay(const std::string& path, int shot_index);

// Resolves MATTER_REPLAY / MATTER_REPLAY_SHOT. Returns valid=false with an
// empty error when MATTER_REPLAY is unset (i.e. "not a replay run").
ShotReplay load_replay_from_env();

} // namespace viewer

#endif // VIEWER_SHOT_REPLAY_H
