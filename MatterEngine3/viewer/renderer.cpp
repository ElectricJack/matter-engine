#include "renderer.h"

#include "material_registry.h"   // MaterialRegistryPackForGPU/Count, MATERIAL_FLOATS_PER_DEF
// tileset_provider.h includes gl46.h -> glad.h, which declares glFinish via
// GLAD's function-pointer mechanism (glFinish == glad_glFinish macro).  Include
// it before rlgl.h so glad is the canonical GL declaration source; the manual
// extern "C" glFinish that was here would conflict with glad's pointer decl.
#include "tileset_provider.h"    // bind_all_to_shader for Wang atlas samplers

#include "rlgl.h"   // rlDrawRenderBatchActive

#include <cstdio>

namespace viewer {

void Renderer::init_camera() {
    // Single tree sits at the origin; frame it slightly above the base so the
    // canopy is centered. Orbit/zoom from the Camera panel pivots on the target.
    camera_.position   = (Vector3){ 20.0f, 16.0f, 34.0f };
    camera_.target     = (Vector3){ 0.0f, 9.0f, 0.0f };
    camera_.up         = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera_.fovy       = 45.0f;
    camera_.projection = CAMERA_PERSPECTIVE;
}

bool Renderer::init_shader(const std::string& shader_fs_path, std::string& err) {
    shader_ = LoadShader(nullptr, shader_fs_path.c_str());
    if (shader_.id == 0) { err = "failed to load shader: " + shader_fs_path; return false; }

    loc_cam_pos_       = GetShaderLocation(shader_, "cameraPos");
    loc_cam_target_    = GetShaderLocation(shader_, "cameraTarget");
    loc_cam_up_        = GetShaderLocation(shader_, "cameraUp");
    loc_cam_fovy_      = GetShaderLocation(shader_, "cameraFovy");
    loc_screen_size_   = GetShaderLocation(shader_, "screenSize");
    loc_material_table_ = GetShaderLocation(shader_, "materialTable");
    loc_material_count_ = GetShaderLocation(shader_, "materialCount");
    loc_gi_strength_     = GetShaderLocation(shader_, "giStrength");
    loc_shadow_strength_ = GetShaderLocation(shader_, "shadowStrength");
    loc_ao_enabled_      = GetShaderLocation(shader_, "aoEnabled");
    loc_debug_tri_       = GetShaderLocation(shader_, "debugTriangleTests");
    loc_wl_sun_dir_      = GetShaderLocation(shader_, "wlSunDir");
    loc_wl_sun_color_    = GetShaderLocation(shader_, "wlSunColor");
    loc_wl_sky_color_    = GetShaderLocation(shader_, "wlSkyColor");

    ready_ = true;
    return true;
}

void Renderer::shutdown() {
    if (ready_) UnloadShader(shader_);
    ready_ = false;
}

void Renderer::update_camera_free() { UpdateCamera(&camera_, CAMERA_FREE); }

void Renderer::set_lights(const world_lights::WorldLights& lights) {
    if (!ready_) return;
    // Must set uniforms with the shader active (raylib uploads them into the
    // currently bound program). Use BeginShaderMode/EndShaderMode just like draw().
    BeginShaderMode(shader_);
    viewer::tileset_provider::bind_all_to_shader((GLuint)shader_.id);
    if (loc_wl_sun_dir_   != -1) SetShaderValue(shader_, loc_wl_sun_dir_,   lights.sun_dir,   SHADER_UNIFORM_VEC3);
    if (loc_wl_sun_color_ != -1) SetShaderValue(shader_, loc_wl_sun_color_, lights.sun_color, SHADER_UNIFORM_VEC3);
    if (loc_wl_sky_color_ != -1) SetShaderValue(shader_, loc_wl_sky_color_, lights.sky_color, SHADER_UNIFORM_VEC3);
    EndShaderMode();
}

void Renderer::upload_material_table() {
    float table[64 * MATERIAL_FLOATS_PER_DEF] = {0};
    MaterialRegistryPackForGPU(table);
    int count = MaterialRegistryCount();
    SetShaderValueV(shader_, loc_material_table_, table, SHADER_UNIFORM_FLOAT,
                    count * MATERIAL_FLOATS_PER_DEF);
    SetShaderValue(shader_, loc_material_count_, &count, SHADER_UNIFORM_INT);
}

void Renderer::draw(BLASManager& blas, TLASManager& tlas) {
    Vector3 cp = camera_.position, ct = camera_.target, cu = camera_.up;
    float fovy = camera_.fovy;
    float screen[2] = { (float)GetScreenWidth(), (float)GetScreenHeight() };

    // All uniform/texture binding must happen with the shader active: raylib
    // stages sampler textures into the active program, and BeginShaderMode
    // flushes the batch, so binding the BVH textures before it would lose them
    // and every ray would miss (blank sky). Mirrors MSL main.cpp's render order.
    blas.ensure_gpu_textures_ready();
    BeginShaderMode(shader_);
        viewer::tileset_provider::bind_all_to_shader((GLuint)shader_.id);
        SetShaderValue(shader_, loc_cam_pos_,    &cp,   SHADER_UNIFORM_VEC3);
        SetShaderValue(shader_, loc_cam_target_, &ct,   SHADER_UNIFORM_VEC3);
        SetShaderValue(shader_, loc_cam_up_,     &cu,   SHADER_UNIFORM_VEC3);
        SetShaderValue(shader_, loc_cam_fovy_,   &fovy, SHADER_UNIFORM_FLOAT);
        SetShaderValue(shader_, loc_screen_size_, screen, SHADER_UNIFORM_VEC2);
        upload_material_table();

        float gi = 1.0f, shadow = 0.5f;
        int ao = 1, debug_tri = 0;
        if (loc_gi_strength_     != -1) SetShaderValue(shader_, loc_gi_strength_,     &gi,        SHADER_UNIFORM_FLOAT);
        if (loc_shadow_strength_ != -1) SetShaderValue(shader_, loc_shadow_strength_, &shadow,    SHADER_UNIFORM_FLOAT);
        if (loc_ao_enabled_      != -1) SetShaderValue(shader_, loc_ao_enabled_,      &ao,        SHADER_UNIFORM_INT);
        if (loc_debug_tri_       != -1) SetShaderValue(shader_, loc_debug_tri_,       &debug_tri, SHADER_UNIFORM_INT);

        blas.bind_to_shader(shader_);
        tlas.bind_to_shader(shader_, blas);

        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), WHITE);
    EndShaderMode();
}

void Renderer::warm_up(BLASManager& blas, TLASManager& tlas) {
    if (shader_.id == 0) return;
    printf("Warming up raytrace shader (one-time GPU compile)...\n");
    double t0 = GetTime();
    RenderTexture2D warm = LoadRenderTexture(1, 1);
    BeginTextureMode(warm);
    ClearBackground(BLACK);
    BeginShaderMode(shader_);
    viewer::tileset_provider::bind_all_to_shader((GLuint)shader_.id);
    Vector2 screen_size = {1.0f, 1.0f};
    if (loc_screen_size_ != -1)
        SetShaderValue(shader_, loc_screen_size_, &screen_size, SHADER_UNIFORM_VEC2);
    // Bind real BVH data so the texelFetch traversal paths are part of the compiled program.
    blas.ensure_gpu_textures_ready();
    blas.bind_to_shader(shader_);
    tlas.bind_to_shader(shader_, blas);
    DrawRectangle(0, 0, 1, 1, WHITE);
    EndShaderMode();
    EndTextureMode();
    rlDrawRenderBatchActive();   // flush raylib's batch so the draw is issued
    glFinish();                  // block until the driver actually compiles+runs it
    UnloadRenderTexture(warm);
    printf("Raytrace shader warm-up done in %.2fs\n", GetTime() - t0);
}

} // namespace viewer
