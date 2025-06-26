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
        
        setup_test_scene(current_test_scene_);
        
        blas_manager_->print_stats();
        tlas_manager_->print_stats();
    }
    
    void setup_test_scene(int test_number) {
        PROFILE_SECTION("Test Scene Setup");
        
        printf("Setting up test scene %d...\n", test_number);
        
        // Clear previous scene
        tlas_manager_->clear();
        
        switch (test_number) {
            case 1: {
                // Test 1: Single cube at origin
                tlas_manager_->load_identity();
                tlas_manager_->draw(cube_blas_, 0); // Red cube
                break;
            }
            
            case 2: {
                // Test 2: Single cube + ground plane
                tlas_manager_->load_identity();
                tlas_manager_->draw(cube_blas_, 0); // Red cube
                
                //tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -5.0f, 0.0f);
                tlas_manager_->draw(ground_blas_, 2); // Green ground
                

                break;
            }
            
            case 3: {
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -5.0f, 0.0f);
                tlas_manager_->draw(ground_blas_, 2); // Green ground

                tlas_manager_->load_identity();
                tlas_manager_->draw(cube_blas_, 0); // Red cube
                
                // tlas_manager_->load_identity();
                // tlas_manager_->translate(-2.0f, 0.0f, 0.0f);
                // tlas_manager_->draw(cube_blas_, 1); // Blue cube
                break;
            }
            
            case 4: {
                // Test 4: Four cubes in a square + ground
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -5.0f, 0.0f);
                tlas_manager_->draw(ground_blas_, 2);
                
                float positions[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f}};
                for (int i = 0; i < 4; i++) {
                    tlas_manager_->load_identity();
                    tlas_manager_->translate(positions[i][0], 0.0f, positions[i][1]);
                    tlas_manager_->draw(cube_blas_, i % 5);
                }
                break;
            }
            
            case 5: {
                // Test 5: Add a sphere to the mix
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -5.0f, 0.0f);
                tlas_manager_->draw(ground_blas_, 2);
                
                float positions[4][2] = {{-1.5f, -1.5f}, {1.5f, -1.5f}, {-1.5f, 1.5f}, {1.5f, 1.5f}};
                for (int i = 0; i < 4; i++) {
                    tlas_manager_->load_identity();
                    tlas_manager_->translate(positions[i][0], 0.0f, positions[i][1]);
                    tlas_manager_->draw(cube_blas_, i);
                }
                
                // Central sphere
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, 3.0f, 0.0f);
                tlas_manager_->draw(sphere_blas_, 4);
                break;
            }
            
            case 6: {
                // Test 6: Circle of cubes around central sphere
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -1.0f, 0.0f);
                tlas_manager_->draw(ground_blas_, 2);
                
                // Central sphere
                tlas_manager_->load_identity();
                tlas_manager_->draw(sphere_blas_, 4);
                
                // Circle of cubes
                SceneBuilder::create_circle(*tlas_manager_, cube_blas_, 6, 2.5f, 0);
                break;
            }
            
            case 7: {
                // Test 7: 3x3 grid of alternating cubes and spheres
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -1.0f, 0.0f);
                tlas_manager_->draw(ground_blas_, 2);
                
                for (int x = -1; x <= 1; x++) {
                    for (int z = -1; z <= 1; z++) {
                        tlas_manager_->load_identity();
                        tlas_manager_->translate(x * 2.0f, 0.0f, z * 2.0f);
                        
                        if ((x + z) % 2 == 0) {
                            tlas_manager_->draw(cube_blas_, (x + 1) + (z + 1) * 3);
                        } else {
                            tlas_manager_->draw(sphere_blas_, (x + 1) + (z + 1) * 3);
                        }
                    }
                }
                break;
            }
            
            case 8: {
                // Test 8: Multi-level scene with floating objects
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -1.0f, 0.0f);
                tlas_manager_->draw(ground_blas_, 2);
                
                // Ground level objects
                SceneBuilder::create_circle(*tlas_manager_, cube_blas_, 8, 3.0f, 0);
                
                // Mid level spheres
                for (int i = 0; i < 4; i++) {
                    float angle = i * M_PI / 2.0f;
                    tlas_manager_->load_identity();
                    tlas_manager_->translate(std::cos(angle) * 1.5f, 2.0f, std::sin(angle) * 1.5f);
                    tlas_manager_->draw(sphere_blas_, i + 1);
                }
                
                // Top level central cube
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, 4.0f, 0.0f);
                tlas_manager_->rotate_y(M_PI / 4.0f);
                tlas_manager_->draw(cube_blas_, 4);
                break;
            }
            
            case 9: {
                // Test 9: Complex scene with everything
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -1.0f, 0.0f);
                tlas_manager_->draw(ground_blas_, 2);
                
                // Central cluster
                tlas_manager_->load_identity();
                tlas_manager_->draw(cube_blas_, 0);
                
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, 2.0f, 0.0f);
                tlas_manager_->draw(sphere_blas_, 1);
                
                // Multiple circles at different heights and radii
                tlas_manager_->push_matrix();
                tlas_manager_->translate(0.0f, 0.0f, 0.0f);
                SceneBuilder::create_circle(*tlas_manager_, cube_blas_, 8, 2.0f, 0);
                tlas_manager_->pop_matrix();
                
                tlas_manager_->push_matrix();
                tlas_manager_->translate(0.0f, 1.5f, 0.0f);
                SceneBuilder::create_circle(*tlas_manager_, sphere_blas_, 6, 3.5f, 2);
                tlas_manager_->pop_matrix();
                
                // Grid of floating objects
                tlas_manager_->push_matrix();
                tlas_manager_->translate(-8.0f, 1.0f, -8.0f);
                SceneBuilder::create_grid(*tlas_manager_, cube_blas_, 4, 4, 2.0f, 3);
                tlas_manager_->pop_matrix();
                
                // Scattered spheres
                for (int i = 0; i < 6; i++) {
                    float angle = i * M_PI / 3.0f;
                    tlas_manager_->load_identity();
                    tlas_manager_->translate(std::cos(angle) * 6.0f, 3.0f + i * 0.5f, std::sin(angle) * 6.0f);
                    tlas_manager_->scale(0.5f + i * 0.1f);
                    tlas_manager_->draw(sphere_blas_, i % 5);
                }
                break;
            }
            
            default:
                // Fallback to test 1
                setup_test_scene(1);
                return;
        }
        
        // Build TLAS from recorded draw calls
        tlas_manager_->build(*blas_manager_);
        
        printf("Test scene %d built: %d nodes for %d instances\n", 
               test_number, tlas_manager_->get_node_count(), tlas_manager_->get_instance_count());
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
        
        // Test scene selection (1-9)
        for (int i = 1; i <= 9; i++) {
            if (IsKeyPressed(KEY_ONE + i - 1)) {
                if (current_test_scene_ != i) {
                    current_test_scene_ = i;
                    printf("Switching to test scene %d...\n", i);
                    setup_test_scene(current_test_scene_);
                    blas_manager_->print_stats();
                    tlas_manager_->print_stats();
                }
                break;
            }
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
        
        // Test scene indicator
        DrawText(TextFormat("Test Scene %d (Press 1-9 to change)", current_test_scene_), 10, 90, 16, LIGHTGRAY);
        
        // Performance info
        double frame_time = Performance::Profiler::instance().get_frame_time_ms();
        DrawText(TextFormat("Frame: %.2f ms (%.1f FPS)", frame_time, 1000.0 / frame_time), 10, 110, 16, LIME);
        
        // Scene stats
        int total_instances_ = tlas_manager_->get_instance_count();
        DrawText(TextFormat("Scene: %d instances, %d triangles", 
                 total_instances_, total_triangles_), 10, 130, 14, LIGHTGRAY);
        
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
    int current_test_scene_ = 1;
    
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