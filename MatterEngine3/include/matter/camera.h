#pragma once

// MatterEngine3/include/matter/camera.h
//
// The engine's one camera description: where the view is and what it can see.
// Plain data with no behaviour — everything that moves a camera does it by
// mutating one of these.
//
// WHO OWNS IT. MatterEditor holds the live instance (created in
// MatterEditor/src/main.cpp) and hands it by reference to the pieces that move
// it: camera_controller.{h,cpp} (fly/WASD input, and it pushes far_plane in
// each frame), camera_orbit, camera_focus, and the gizmo. It is also captured
// by value into issue reports (MatterEditor/src/issue_reporter.h) so a filed
// issue can be replayed from the same viewpoint. Copy it freely; it owns
// nothing.
//
// UNITS AND CONVENTIONS. `position`, `target` and the near/far planes are in
// world METRES; the engine is Y-up, which is why `up` defaults to +Y.
// `vertical_fov_radians` is in RADIANS — an exception in a codebase that
// otherwise states angles in degrees (compare matter/sun_angles.h). The
// default is pi/4, i.e. a 45-degree vertical field of view.
//
// The defaults describe the framing a world gets before anything positions
// the camera, not a value any particular world depends on.

#include "matter/math_types.h"

namespace matter {

struct CameraDesc {
    Float3 position{20.0f, 16.0f, 34.0f};
    Float3 target{0.0f, 9.0f, 0.0f};
    Float3 up{0.0f, 1.0f, 0.0f};
    float vertical_fov_radians = 0.78539816339f;
    float near_plane = 1.0f;
    float far_plane = 5000.0f;
};

} // namespace matter
