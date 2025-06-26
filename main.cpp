extern "C" {
    #include "raylib.h"
    #include "rlgl.h"
    #include "include/bvh.h"
}

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "include/blas_manager.hpp"
#include "include/tlas_manager.hpp"
#include "include/profiler.hpp"

class RayTracingDemo {
public:
    RayTracingDemo(int width, int height) 
        : screen_width_(width), screen_height_(height),
          blas_manager_(std::make_unique<BLASManager>()),
          tlas_manager_(std::make_unique<TLASManager>(50)) {
        
        PROFILE_SECTION("Demo Initialization");
        
        InitWindow(screen_width_, screen_height_, "C++ Modular BLAS/TLAS with Performance Profiling");
        SetTargetFPS(60);
        
        setup_scene();
        setup_rendering();
        
        printf("=== C++ Modular BLAS/TLAS System Initialized ===\n");
    }
    
    ~RayTracingDemo() {
        cleanup();
        CloseWindow();
    }
    
    void run() {
        int frame_count = 0;
        
        while (!WindowShouldClose()) {
            PROFILE_FRAME_BEGIN();
            
            frame_count++;
            
            // Print simple frame count to check if we're making progress
            if (frame_count % 30 == 0) {
                printf("Frame %d...\n", frame_count);
            }
            
            // Print performance stats every 60 frames (roughly every second at 60 FPS)
            if (frame_count % 60 == 0) {
                PROFILE_PRINT();
            }
            
            // Reset stats every 5 seconds to show current performance
            if (frame_count % 300 == 0) {
                printf("\n--- Performance Reset ---\n");
                PROFILE_RESET();
            }
            
            update();
            render();
            
            PROFILE_FRAME_END();
        }
        
        // Final stats
        printf("\n=== Final Performance Statistics ===\n");
        PROFILE_PRINT();
    }

private:
    void setup_scene() {
        PROFILE_SECTION("Scene Setup");
        
        // Register different geometry types using factory functions
        cube_blas_ = BLASFactory::register_cube(*blas_manager_, 1.0f);
        sphere_blas_ = BLASFactory::register_sphere(*blas_manager_, 0.5f, 32, 16);
        ground_blas_ = BLASFactory::register_plane(*blas_manager_, 20.0f, 20.0f);
        
        printf("Registered BLAS handles: cube=%u, sphere=%u, ground=%u\n", 
               cube_blas_, sphere_blas_, ground_blas_);
        
        setup_static_scene();
        
        blas_manager_->print_stats();
        tlas_manager_->print_stats();
    }
    
    void setup_static_scene() {
        PROFILE_SECTION("Static Scene Setup");
        
        // Clear and setup full static scene (only called once during initialization)
        tlas_manager_->clear();
        
        // Ground plane
        tlas_manager_->load_identity();
        tlas_manager_->translate(0.0f, -1.0f, 0.0f);
        tlas_manager_->draw(ground_blas_, 2);
        
        // Central objects
        tlas_manager_->load_identity();
        tlas_manager_->draw(cube_blas_, 0); // Red cube at origin
        
        tlas_manager_->load_identity();
        tlas_manager_->translate(-3.0f, 0.0f, 0.0f);
        tlas_manager_->rotate_y(static_cast<float>(M_PI) / 3.0f);
        tlas_manager_->scale(0.8f, 1.2f, 0.8f);
        tlas_manager_->draw(cube_blas_, 1); // Blue cube
        
        tlas_manager_->load_identity();
        tlas_manager_->translate(3.0f, 2.0f, 0.0f);
        tlas_manager_->rotate_x(static_cast<float>(M_PI) / 6.0f);
        tlas_manager_->rotate_y(static_cast<float>(M_PI) / 4.0f);
        tlas_manager_->scale(0.6f);
        tlas_manager_->draw(cube_blas_, 3); // Yellow cube
        
        // Large sphere
        tlas_manager_->load_identity();
        tlas_manager_->translate(0.0f, 3.0f, -2.0f);
        tlas_manager_->scale(1.5f);
        tlas_manager_->draw(sphere_blas_, 1);
        
        // Multiple smaller spheres
        for (int i = 0; i < 3; i++) {
            tlas_manager_->push_matrix();
            tlas_manager_->load_identity();
            float x = -2.0f + i * 2.0f;
            tlas_manager_->translate(x, 1.0f, 2.0f);
            tlas_manager_->scale(0.8f);
            tlas_manager_->draw(sphere_blas_, static_cast<uint32_t>(i));
            tlas_manager_->pop_matrix();
        }
        
        // Use scene builder utilities for complex arrangements
        {
            tlas_manager_->push_matrix();
            tlas_manager_->translate(6.0f, 0.0f, 0.0f);
            SceneBuilder::create_circle(*tlas_manager_, cube_blas_, 8, 2.0f, 4);
            tlas_manager_->pop_matrix();
        }
        
        {
            tlas_manager_->push_matrix();
            tlas_manager_->translate(-6.0f, 0.0f, 0.0f);
            SceneBuilder::create_grid(*tlas_manager_, sphere_blas_, 3, 3, 1.0f, 2);
            tlas_manager_->pop_matrix();
        }
        
        // Add some static floating objects (no animation for performance)
        add_static_floating_objects();
        
        // Build TLAS from recorded draw calls (only once for static setup)
        tlas_manager_->build(*blas_manager_);
        
        printf("TLAS built: %d nodes for %d instances\n", 
               tlas_manager_->get_node_count(), tlas_manager_->get_instance_count());
    }
    
    void add_static_floating_objects() {
        // Add static floating objects (no animation for better performance)
        tlas_manager_->load_identity();
        tlas_manager_->translate(-4.0f, 4.0f, -3.0f);
        tlas_manager_->rotate_axis({1.0f, 1.0f, 0.0f}, 0.5f);
        tlas_manager_->scale(0.4f);
        tlas_manager_->draw(cube_blas_, 4);
        
        tlas_manager_->load_identity();
        tlas_manager_->translate(4.0f, 4.0f, -3.0f);
        tlas_manager_->rotate_y(1.0f);
        tlas_manager_->scale(0.6f);
        tlas_manager_->draw(sphere_blas_, 3);
    }
    
    
    void setup_rendering() {
        PROFILE_SECTION("Rendering Setup");
        
        // Load raytracing shader
        raytracing_shader_ = LoadShader(nullptr, "shaders/raytrace_tlas_blas_processed.fs");
        use_raytracing_ = (raytracing_shader_.id != 0);
        
        // Start in raytracing mode
        if (use_raytracing_) {
            printf("Starting in raytracing mode\n");
        }
        
        if (raytracing_shader_.id == 0) {
            printf("Failed to load raytracing shader, using rasterization\n");
        } else {
            printf("Raytracing shader loaded successfully\n");
            setup_shader_uniforms();
        }
        
        // Initialize camera
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
        PROFILE_SECTION("Frame Update");
        
        // Handle input
        handle_input();
        
        // Update camera
        {
            PROFILE_SECTION("Camera Update");
            UpdateCamera(&camera_, CAMERA_FREE);
        }
    }
    
    void handle_input() {
        // Toggle rendering mode
        if (IsKeyPressed(KEY_SPACE)) {
            use_raytracing_ = !use_raytracing_ && (raytracing_shader_.id != 0);
            printf("Switched to %s mode\n", use_raytracing_ ? "raytracing" : "rasterization");
        }
        
        // Performance controls
        if (IsKeyPressed(KEY_P)) {
            PROFILE_PRINT();
        }
        if (IsKeyPressed(KEY_R)) {
            printf("Resetting performance statistics...\n");
            PROFILE_RESET();
        }
    }
    
    void render() {
        PROFILE_SECTION("Frame Render");
        
        {
            PROFILE_SECTION("Begin Drawing");
            BeginDrawing();
        }
        
        {
            PROFILE_SECTION("Clear Screen");
            ClearBackground(BLACK);
        }
        
        if (use_raytracing_ && raytracing_shader_.id != 0) {
            render_raytraced();
        } else {
            render_rasterized();
        }
        
        render_ui();
        
        {
            PROFILE_SECTION("End Drawing");
            EndDrawing();
        }
    }
    
    void render_raytraced() {
        PROFILE_SECTION("Raytraced Rendering");
        
        BeginShaderMode(raytracing_shader_);
        
        // Set shader uniforms
        {
            PROFILE_SECTION("Shader Uniforms");
            
            Vector2 screen_size = {static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())};
            
            // Set camera uniforms
            SetShaderValue(raytracing_shader_, camera_pos_loc_, &camera_.position, SHADER_UNIFORM_VEC3);
            SetShaderValue(raytracing_shader_, camera_target_loc_, &camera_.target, SHADER_UNIFORM_VEC3);
            SetShaderValue(raytracing_shader_, camera_up_loc_, &camera_.up, SHADER_UNIFORM_VEC3);
            SetShaderValue(raytracing_shader_, camera_fovy_loc_, &camera_.fovy, SHADER_UNIFORM_FLOAT);
            SetShaderValue(raytracing_shader_, screen_size_loc_, &screen_size, SHADER_UNIFORM_VEC2);
            
            // Let managers handle their own shader binding and texture management
            blas_manager_->bind_to_shader(raytracing_shader_);
            tlas_manager_->bind_to_shader(raytracing_shader_, *blas_manager_);
        }
        
        // Draw fullscreen rectangle
        {
            PROFILE_SECTION("Fullscreen Quad");
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), WHITE);
        }
        
        EndShaderMode();
    }
    
    void render_rasterized() {
        PROFILE_SECTION("Rasterized Rendering");
        
        BeginMode3D(camera_);
        
        // Draw simplified scene representation
        DrawCube({0.0f, 0.0f, 0.0f}, 1.0f, 1.0f, 1.0f, RED);
        DrawCube({-3.0f, 0.0f, 0.0f}, 1.0f, 1.0f, 1.0f, BLUE);
        DrawCube({3.0f, 2.0f, 0.0f}, 0.6f, 0.6f, 0.6f, YELLOW);
        DrawSphere({0.0f, 3.0f, -2.0f}, 0.75f, GREEN);
        DrawPlane({0.0f, -1.0f, 0.0f}, {20.0f, 20.0f}, DARKGREEN);
        
        // Animated objects
        float time = static_cast<float>(GetTime());
        DrawCube({-4.0f + std::sin(time) * 2.0f, 4.0f + std::cos(time * 0.7f), -3.0f}, 0.4f, 0.4f, 0.4f, PURPLE);
        DrawSphere({4.0f + std::cos(time * 1.3f) * 1.5f, 4.0f, -3.0f + std::sin(time * 0.9f)}, 0.6f, ORANGE);
        
        DrawGrid(10, 1.0f);
        
        EndMode3D();
    }
    
    void render_ui() {
        PROFILE_SECTION("UI Rendering");

        // Scene statistics
        int total_triangles_  = blas_manager_->get_total_triangle_count();
        //int total_blas_nodes_ = blas_manager_->get_total_node_count();
        //int total_tlas_nodes_ = tlas_manager_->get_node_count();
        //int total_instances_  = tlas_manager_->get_instance_count(); 
        
        // Mode indicator
        if (use_raytracing_) {
            DrawText("C++ RAYTRACING MODE", 10, 40, 20, GREEN);
            DrawText("Press SPACE to toggle rasterization", 10, 70, 16, LIGHTGRAY);
        } else {
            DrawText("C++ RASTERIZATION MODE", 10, 40, 20, YELLOW);
            if (raytracing_shader_.id != 0) {
                DrawText("Press SPACE to toggle raytracing", 10, 70, 16, LIGHTGRAY);
            } else {
                DrawText("Raytracing shader failed to load", 10, 70, 16, RED);
            }
        }
        
        // Performance info
        double frame_time = Performance::Profiler::instance().get_frame_time_ms();
        DrawText(TextFormat("Frame: %.2f ms (%.1f FPS)", frame_time, 1000.0 / frame_time), 10, 100, 16, LIME);
        
        // Scene stats
        int total_instances_ = tlas_manager_->get_instance_count();
        DrawText(TextFormat("Scene: %d instances, %d triangles", 
                 total_instances_, total_triangles_), 10, 120, 14, LIGHTGRAY);
        
        // Performance controls
        DrawText("Press P for performance stats, R to reset", 10, screen_height_ - 50, 14, LIGHTGRAY);
        
        // System info
        DrawText("C++ Modular BLAS/TLAS System", 10, screen_height_ - 30, 16, LIGHTGRAY);
        
        DrawFPS(10, 10);
    }
    
    void cleanup() {
        PROFILE_SECTION("Cleanup");
        
        if (raytracing_shader_.id != 0) UnloadShader(raytracing_shader_);
        // Managers clean up their own textures in destructors
    }
    
private:
    // Window settings
    int screen_width_;
    int screen_height_;
    
    // Managers
    std::unique_ptr<BLASManager> blas_manager_;
    std::unique_ptr<TLASManager> tlas_manager_;
    
    // BLAS handles
    BLASHandle cube_blas_;
    BLASHandle sphere_blas_;
    BLASHandle ground_blas_;
    
    // Rendering
    Camera camera_;
    Shader raytracing_shader_{};
    bool use_raytracing_ = false;
    
    // GPU textures are now managed by the managers themselves
    
    // Shader uniform locations (camera and scene-level only)
    int camera_pos_loc_;
    int camera_target_loc_;
    int camera_up_loc_;
    int camera_fovy_loc_;
    int screen_size_loc_;
    

};

int main() {
    printf("=== C++ Modular BLAS/TLAS System with Performance Profiling ===\n");
    
    try {
        RayTracingDemo demo(800, 600);
        demo.run();
    } catch (const std::exception& e) {
        printf("Error: %s\n", e.what());
        return 1;
    }
    
    return 0;
}