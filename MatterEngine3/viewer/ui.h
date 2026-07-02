#ifndef VIEWER_UI_H
#define VIEWER_UI_H

#include <cstdint>

#include "raylib.h"   // Camera3D for the orbit panel

namespace viewer {

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
    // Raster-path counters (zero in RT mode)
    int      raster_batches = 0;
    int      raster_tris = 0;
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
    void draw_camera_panel(Camera3D& cam);
};

} // namespace viewer

#endif // VIEWER_UI_H
