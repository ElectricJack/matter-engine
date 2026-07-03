#ifndef VIEWER_RENDERER_H
#define VIEWER_RENDERER_H

#include "raylib.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <string>

namespace viewer {

// Owns the raytrace shader + camera and draws a fullscreen traced frame.
// Mirrors MatterSurfaceLib/main.cpp's render path.
class Renderer {
public:
    // init_camera sets camera defaults (always called, both modes).
    void init_camera();
    // init_shader loads the raytrace shader (call only when use_rt=true).
    bool init_shader(const std::string& shader_fs_path, std::string& err);
    void shutdown();

    Camera3D& camera() { return camera_; }
    void update_camera_free();                  // UpdateCamera(CAMERA_FREE)

    // Bind BLAS/TLAS + camera + material uniforms and draw the fullscreen pass.
    void draw(BLASManager& blas, TLASManager& tlas);

    // One-time GPU compile of the raytrace shader via a 1x1 offscreen draw with
    // real BVH data bound, so the first real frame doesn't stall. Call once after
    // the world is composed (TLAS populated). Mirrors MSL warm_up_raytracing_shader.
    void warm_up(BLASManager& blas, TLASManager& tlas);

private:
    void upload_material_table();

    Shader   shader_{};
    Camera3D camera_{};
    int      loc_cam_pos_ = -1, loc_cam_target_ = -1, loc_cam_up_ = -1;
    int      loc_cam_fovy_ = -1, loc_screen_size_ = -1;
    int      loc_material_table_ = -1, loc_material_count_ = -1;
    int      loc_gi_strength_ = -1, loc_shadow_strength_ = -1;
    int      loc_ao_enabled_ = -1, loc_debug_tri_ = -1;
    bool     ready_ = false;
};

} // namespace viewer

#endif // VIEWER_RENDERER_H
