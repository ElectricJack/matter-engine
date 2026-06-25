#include "renderer.h"

#include "material_registry.h"   // MaterialRegistryPackForGPU/Count, MATERIAL_FLOATS_PER_DEF

#include "rlgl.h"   // rlDrawRenderBatchActive

extern "C" { void glFinish(void); }

#include <cstdio>

namespace viewer {

bool Renderer::init(const std::string& shader_fs_path, std::string& err) {
    shader_ = LoadShader(nullptr, shader_fs_path.c_str());
    if (shader_.id == 0) { err = "failed to load shader: " + shader_fs_path; return false; }

    loc_cam_pos_       = GetShaderLocation(shader_, "cameraPos");
    loc_cam_target_    = GetShaderLocation(shader_, "cameraTarget");
    loc_cam_up_        = GetShaderLocation(shader_, "cameraUp");
    loc_cam_fovy_      = GetShaderLocation(shader_, "cameraFovy");
    loc_screen_size_   = GetShaderLocation(shader_, "screenSize");
    loc_material_table_ = GetShaderLocation(shader_, "materialTable");
    loc_material_count_ = GetShaderLocation(shader_, "materialCount");

    camera_.position   = (Vector3){ 12.0f, 10.0f, -12.0f };
    camera_.target     = (Vector3){ 12.0f, 1.0f, 12.0f };
    camera_.up         = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera_.fovy       = 60.0f;
    camera_.projection = CAMERA_PERSPECTIVE;

    ready_ = true;
    return true;
}

void Renderer::shutdown() {
    if (ready_) UnloadShader(shader_);
    ready_ = false;
}

void Renderer::update_camera_free() { UpdateCamera(&camera_, CAMERA_FREE); }

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

    SetShaderValue(shader_, loc_cam_pos_,    &cp,   SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, loc_cam_target_, &ct,   SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, loc_cam_up_,     &cu,   SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, loc_cam_fovy_,   &fovy, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, loc_screen_size_, screen, SHADER_UNIFORM_VEC2);
    upload_material_table();

    blas.ensure_gpu_textures_ready();
    blas.bind_to_shader(shader_);
    tlas.bind_to_shader(shader_, blas);

    BeginShaderMode(shader_);
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
