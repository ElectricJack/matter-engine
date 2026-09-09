#pragma once

// MatterEditor/src/viewport_pick.h
//
// Click-to-select in the 3D viewport: turn a cursor position into either a
// baked static instance (by part hash) or a live ECS entity.
//
// Used by MatterEditor/src/main.cpp on a viewport click; the result feeds
// SelectionSet (and, for entities, EditorModel), which in turn drives the
// Properties panel, the gizmo and the selection outline.
//
// Two mechanisms behind one call, tried in order — see viewport_pick.cpp:
// the renderer's GPU identity buffer first (pixel-exact, and the only way to
// hit streamed/baked geometry, which has no CPU-side collider), then a CPU
// ray-vs-oriented-box test over ECS entities as a fallback for anything not
// yet in the raster pipeline.
//
// Main thread only, and only meaningful after the renderer has completed at
// least one frame — the GPU stage reads last frame's identity buffer.

#include <cstdint>
#include "matter/camera.h"
#include "selection_set.h"

namespace matter { class WorldSession; }

namespace viewer {

// What was under the cursor. `hit == false` means "nothing", which for a
// viewport click is an ordinary outcome (empty sky) and the caller treats as
// "clear the selection", not as an error.
//
// `object.kind` distinguishes a baked static instance (id = part hash) from an
// ECS entity (id = SceneEntityId), and consumers must branch on it — the two
// id spaces are unrelated.
//
// `distance` is metres along the view ray, but ONLY from the CPU fallback
// path: the GPU identity pick has no depth to report (matter::PickIdentity
// carries kind + ids and nothing else) and leaves it 0. Do not treat 0 as "on
// the camera" — and note that NOTHING reads this field today, so filling it
// properly means first adding a depth to the engine's pick result.
struct PickResult {
    bool hit = false;
    SelectedObject object;
    float distance = 0.0f;
};

// Cast a ray from screen-space cursor into the scene and find the nearest object.
// `cursor_x`, `cursor_y` are pixel coordinates; `fb_width`, `fb_height` the viewport size.
//
// To be precise about the coordinate space, because the parameter names read
// like the window's: all four are VIEWPORT-relative. The caller subtracts the
// viewport rect's origin from the cursor and passes the rect's own width and
// height (main.cpp does exactly that), so (0,0) is the top-left of the 3D
// view, not of the window. Passing window coordinates picks the wrong pixel
// wherever the viewport is not at the window origin — i.e. always, with the
// UI shown.
//
// COST: the GPU stage is O(1). The CPU fallback runs only when the GPU pick
// misses (background, or before the first completed frame) and then walks
// every ECS entity with a transform, so clicking empty space is the expensive
// case, not clicking an object.
PickResult viewport_pick(float cursor_x, float cursor_y,
                         int fb_width, int fb_height,
                         const matter::CameraDesc& camera,
                         matter::WorldSession& session);

} // namespace viewer
