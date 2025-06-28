extern "C" {
    #include "raylib.h"
    #include "rlgl.h"
}

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "include/blas_manager.hpp"
#include "include/tlas_manager.hpp"

class RayTracingDemo {
public:
    RayTracingDemo(int width, int height) 
        : screen_width_(width), screen_height_(height),
          blas_manager_(std::make_unique<BLASManager>()),
          tlas_manager_(std::make_unique<TLASManager>(50)) {
        
        InitWindow(screen_width_, screen_height_, "MatterSurfaceLib - Ray Tracing");
        SetTargetFPS(60);
        
        DisableCursor();
        
        setup_scene();
        setup_rendering();
    }
    
    ~RayTracingDemo() {
        cleanup();
        EnableCursor();
        CloseWindow();
    }
    
    void run() {
        while (!WindowShouldClose()) {
            update();
            render();
        }
    }

private:
    void setup_scene() {
        cube_blas_ = BLASFactory::register_cube(*blas_manager_, 1.0f);
        sphere_blas_ = BLASFactory::register_sphere(*blas_manager_, 0.5f, 32, 16);
        ground_blas_ = BLASFactory::register_plane(*blas_manager_, 200.0f, 200.0f);
        
        create_example_scene();
    }
    
    void create_example_scene() {
        tlas_manager_->clear();
        
        // Ground plane
        tlas_manager_->load_identity();
        tlas_manager_->translate(0.0f, -2.0f, 0.0f);
        tlas_manager_->scale(10.0f, 0.1f, 10.0f);
        tlas_manager_->draw(cube_blas_, 2);

        // Central cube
        tlas_manager_->load_identity();
        tlas_manager_->translate(0.0f, 0.0f, 0.0f);
        tlas_manager_->draw(cube_blas_, 0);
        
        // A few spheres
        tlas_manager_->load_identity();
        tlas_manager_->translate(-2.0f, 0.0f, 0.0f);
        tlas_manager_->draw(sphere_blas_, 1);
        
        tlas_manager_->load_identity();
        tlas_manager_->translate(2.0f, 0.0f, 0.0f);
        tlas_manager_->draw(sphere_blas_, 1);
        
        tlas_manager_->build(*blas_manager_);
    }
    
    
    
    
    void setup_rendering() {
        raytracing_shader_ = LoadShader(nullptr, "shaders/raytrace_tlas_blas_processed.fs");
        
        if (raytracing_shader_.id != 0) {
            setup_shader_uniforms();
        }
        
        camera_.position = {3.0f, 2.0f, 5.0f};
        camera_.target = {0.0f, 0.0f, 0.0f};
        camera_.up = {0.0f, 1.0f, 0.0f};
        camera_.fovy = 45.0f;
        camera_.projection = CAMERA_PERSPECTIVE;
    }

    
    
    void setup_shader_uniforms() {
        // Get camera and scene-level shader uniform locations
        camera_pos_loc_    = GetShaderLocation(raytracing_shader_, "cameraPos");
        camera_target_loc_ = GetShaderLocation(raytracing_shader_, "cameraTarget");
        camera_up_loc_     = GetShaderLocation(raytracing_shader_, "cameraUp");
        camera_fovy_loc_   = GetShaderLocation(raytracing_shader_, "cameraFovy");
        screen_size_loc_   = GetShaderLocation(raytracing_shader_, "screenSize");
        
        // BLAS/TLAS uniforms are now handled by their respective managers
    }
    
    void update() {
        if (IsKeyPressed(KEY_ESCAPE)) {
            cursor_disabled_ = !cursor_disabled_;
            if (cursor_disabled_) {
                DisableCursor();
            } else {
                EnableCursor();
            }
        }
        
        UpdateCamera(&camera_, CAMERA_FREE);
    }
    
    
    void render() {
        BeginDrawing();
        ClearBackground(BLACK);
        
        if (raytracing_shader_.id != 0) {
            render_raytraced();
        }
        
        EndDrawing();
    }
    
    void render_raytraced() {
        BeginShaderMode(raytracing_shader_);
        
        Vector2 screen_size = {static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())};
        
        SetShaderValue(raytracing_shader_, camera_pos_loc_, &camera_.position, SHADER_UNIFORM_VEC3);
        SetShaderValue(raytracing_shader_, camera_target_loc_, &camera_.target, SHADER_UNIFORM_VEC3);
        SetShaderValue(raytracing_shader_, camera_up_loc_, &camera_.up, SHADER_UNIFORM_VEC3);
        SetShaderValue(raytracing_shader_, camera_fovy_loc_, &camera_.fovy, SHADER_UNIFORM_FLOAT);
        SetShaderValue(raytracing_shader_, screen_size_loc_, &screen_size, SHADER_UNIFORM_VEC2);
        
        blas_manager_->bind_to_shader(raytracing_shader_);
        tlas_manager_->bind_to_shader(raytracing_shader_, *blas_manager_);
        
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), WHITE);
        
        EndShaderMode();
    }
    
    
    void cleanup() {
        if (raytracing_shader_.id != 0) UnloadShader(raytracing_shader_);
        // Managers clean up their own textures in destructors
    }
    
private:
    int screen_width_;
    int screen_height_;
    
    std::unique_ptr<BLASManager> blas_manager_;
    std::unique_ptr<TLASManager> tlas_manager_;
    
    BLASHandle cube_blas_;
    BLASHandle sphere_blas_;
    BLASHandle ground_blas_;
    
    Camera camera_;
    Shader raytracing_shader_{};
    bool cursor_disabled_ = true;
    
    int camera_pos_loc_;
    int camera_target_loc_;
    int camera_up_loc_;
    int camera_fovy_loc_;
    int screen_size_loc_;
    

};

int main() {
    try {
        RayTracingDemo demo(1280, 800);
        demo.run();
    } catch (const std::exception& e) {
        printf("Error: %s\n", e.what());
        return 1;
    }
    
    return 0;
}