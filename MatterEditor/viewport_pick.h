#pragma once
#include <cstdint>
#include "matter/camera.h"
#include "selection_set.h"

namespace matter { class WorldSession; }

namespace viewer {

struct PickResult {
    bool hit = false;
    SelectedObject object;
    float distance = 0.0f;
};

// Cast a ray from screen-space cursor into the scene and find the nearest object.
// `cursor_x`, `cursor_y` are pixel coordinates; `fb_width`, `fb_height` the viewport size.
PickResult viewport_pick(float cursor_x, float cursor_y,
                         int fb_width, int fb_height,
                         const matter::CameraDesc& camera,
                         matter::WorldSession& session);

} // namespace viewer
