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
#include "include/cluster.h"
#include "include/cell.h"

class MatterSurfaceLibDemo {
public:
    MatterSurfaceLibDemo(int width, int height) 
        : screen_width_(width), screen_height_(height),
          blas_manager_(std::make_unique<BLASManager>()),
          tlas_manager_(std::make_unique<TLASManager>(50)),
          test_cluster_(std::make_unique<Cluster>(0, 2.0f)) {
        
        InitWindow(screen_width_, screen_height_, "MatterSurfaceLib - Cluster and Cell System");
        SetTargetFPS(60);
        
        DisableCursor();
        
        setup_scene();
        setup_rendering();
        setup_matter_system();
    }
    
    ~MatterSurfaceLibDemo() {
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
        create_example_scene();
    }
    
    void create_example_scene() {
        tlas_manager_->clear();
        
        // Add cluster meshes to TLAS for ray tracing
        if (test_cluster_) {
            test_cluster_->add_to_tlas(*tlas_manager_);
        }
        
        tlas_manager_->build(*blas_manager_);
    }
    
    void setup_matter_system() {
        // Create a cluster of particles to demonstrate the system
        printf("Setting up matter system with cluster and cells...\n");
        
        // Add particles in a roughly spherical distribution
        for (int i = 0; i < 100; ++i) {
            float angle1 = (float)i * 0.15f;
            float angle2 = (float)i * 0.25f;
            
            Vector3 position = {
                cosf(angle1) * sinf(angle2) * 5.0f,
                sinf(angle1) * sinf(angle2) * 5.0f,
                cosf(angle2) * 5.0f
            };
            
            uint32_t material = i % 3; // Cycle through materials
            test_cluster_->add_particle(position, 0.5f, material);
        }
        
        // Add some additional particles in a line
        for (int i = 0; i < 20; ++i) {
            Vector3 position = {(float)i - 10.0f, 0.0f, 0.0f};
            test_cluster_->add_particle(position, 0.8f, 1);
        }
        
        printf("Added %u particles to cluster\n", test_cluster_->get_particle_count());
        
        // Position cluster in world space
        test_cluster_->set_position({0.0f, 2.0f, 0.0f});
        
        // Force initial mesh rebuild
        test_cluster_->rebuild_dirty_cells(*blas_manager_);
        
        printf("Cluster has %u cells, %u dirty\n", 
               test_cluster_->get_cell_count(), test_cluster_->get_dirty_cell_count());
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
        
        // Toggle rendering modes
        if (IsKeyPressed(KEY_R)) {
            render_mode_ = (render_mode_ + 1) % 4; // Cycle through 4 modes
            printf("Render mode: %s\n", 
                   render_mode_ == 0 ? "Ray Tracing" : 
                   render_mode_ == 1 ? "Surface Meshes" : 
                   render_mode_ == 2 ? "Wireframe Meshes" : "Debug Bounds");
        }
        
        // Add dynamic particle movement
        if (IsKeyPressed(KEY_SPACE)) {
            // Move some particles randomly to test dirty region updates
            for (int i = 0; i < 10; ++i) {
                float x = (GetRandomValue(-50, 50) / 10.0f);
                float y = (GetRandomValue(-50, 50) / 10.0f);
                float z = (GetRandomValue(-50, 50) / 10.0f);
                
                Vector3 new_pos = {x, y, z};
                test_cluster_->add_particle(new_pos, 0.5f, GetRandomValue(0, 2));
            }
            
            test_cluster_->rebuild_dirty_cells(*blas_manager_);
            printf("Added 10 random particles. Cluster now has %u cells\n", 
                   test_cluster_->get_cell_count());
        }
        
        UpdateCamera(&camera_, CAMERA_FREE);
    }
    
    
    void render() {
        BeginDrawing();
        ClearBackground(BLACK);
        
        if (render_mode_ == 0 && raytracing_shader_.id != 0) {
            render_raytraced();
        } else {
            // 3D rendering mode for meshes
            BeginMode3D(camera_);
            
            
            if (render_mode_ == 1) {
                // Render solid surface meshes
                test_cluster_->render_cells(false);
            } else if (render_mode_ == 2) {
                // Render wireframe meshes
                test_cluster_->render_cells(true);
            } else if (render_mode_ == 3) {
                // Debug bounds mode
                test_cluster_->render_debug_bounds();
            }
            
            EndMode3D();
            
            // Draw UI
            DrawText(TextFormat("Render Mode: %s (Press R to cycle)", 
                               render_mode_ == 0 ? "Ray Tracing" : 
                               render_mode_ == 1 ? "Surface Meshes" : 
                               render_mode_ == 2 ? "Wireframe Meshes" : "Debug Bounds"), 
                     10, 10, 20, WHITE);
            DrawText("Press SPACE to add random particles", 10, 40, 20, WHITE);
            DrawText("Press ESC to toggle cursor", 10, 70, 20, WHITE);
            DrawText(TextFormat("Particles: %u, Cells: %u", 
                               test_cluster_->get_particle_count(),
                               test_cluster_->get_cell_count()), 
                     10, 100, 20, WHITE);
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
    std::unique_ptr<Cluster> test_cluster_;
    
    
    Camera camera_;
    Shader raytracing_shader_{};
    bool cursor_disabled_ = true;
    int render_mode_ = 1; // 0=raytracing, 1=solid_meshes, 2=wireframe_meshes, 3=debug
    
    int camera_pos_loc_;
    int camera_target_loc_;
    int camera_up_loc_;
    int camera_fovy_loc_;
    int screen_size_loc_;
    

};

int main() {
    try {
        MatterSurfaceLibDemo demo(1280, 800);
        demo.run();
    } catch (const std::exception& e) {
        printf("Error: %s\n", e.what());
        return 1;
    }
    
    return 0;
}