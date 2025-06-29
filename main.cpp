extern "C" {
    #include "raylib.h"
    #include "rlgl.h"
}
#include "raymath.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>


#include "particle_system.h"

class ParticleDynamicsDemo {
public:
    ParticleDynamicsDemo(int width, int height, bool debug_mode = false) 
        : screen_width_(width), screen_height_(height),
          debug_mode_(debug_mode), debug_frame_count_(0),
          particle_system_(std::make_unique<ParticleSystem>()) {
        
        InitWindow(screen_width_, screen_height_, "Gravitational N-Body Simulation with Black Hole");
        SetTargetFPS(60);
        
        // Disable cursor for first person camera control
        DisableCursor();
        
        setup_scene();
        
        printf("=== Gravitational N-Body System Initialized ===\n");
        printf("Physics: Custom Structure of Arrays system\n");
    }
    
    ~ParticleDynamicsDemo() {
        cleanup();
        EnableCursor(); // Restore cursor functionality before closing
        CloseWindow();
    }
    
    void run() {
        // Main game loop
        while (!WindowShouldClose()) {
            PROFILE_FRAME_BEGIN();
            
            // Auto-quit for debug mode
            if (debug_mode_) {
                debug_frame_count_++;
                if (debug_frame_count_ >= 300) {
                    printf("DEBUG MODE: Auto-quitting after 300 frames\n");
                    break;
                }
            }
            
            {
                PROFILE_SECTION("Input & Update");
                update();
            }
            
            {
                PROFILE_SECTION("Rendering");
                render();
            }
            
            PROFILE_FRAME_END();
        }
        
        // Print final profiling stats before cleanup
        if (debug_mode_) {
            particle_system_->print_profiling_stats();
        }
        
        cleanup();
    }

private:
    void setup_scene() {
        printf("=== Setting Up Gravitational N-Body Simulation ===\n");
        
        // Initialize camera further out for orbital view
        camera_.position = {0.0f, 15.0f, 30.0f};
        camera_.target = {0.0f, 0.0f, 0.0f};
        camera_.up = {0.0f, 1.0f, 0.0f};
        camera_.fovy = 45.0f;
        camera_.projection = CAMERA_PERSPECTIVE;
        
        // Initialize particle system
        particle_system_->initialize();
        
        // Create particle types (1/10 the size)
        uint32_t light_type  = particle_system_->create_particle_type(0.01f, 0.008f, BLUE);
        uint32_t medium_type = particle_system_->create_particle_type(0.02f, 0.015f, GREEN);
        uint32_t heavy_type  = particle_system_->create_particle_type(0.04f, 0.025f, RED);
        
        // Create particles in orbital configuration around the black hole
        for (int i = 0; i < 500; i++) {  // More particles to test performance
            float angle = (float)i / 50.0f * 2.0f * PI;
            float radius = 8.0f + (float)(rand() % 100) / 100.0f * 10.0f;  // Random orbital distances
            float height = ((float)(rand() % 100) / 100.0f - 0.5f) * 2.0f;  // Reduced height variation
            
            float x = cosf(angle) * radius;
            float z = sinf(angle) * radius;
            float y = height;
            
            // Calculate proper orbital velocity for stable orbit: v = sqrt(GM/r)
            // Using gravitational constant 50.0 and black hole mass 100.0
            float orbital_speed = sqrtf(50.0f * 100.0f / radius);
            
            // Add small random variation (±10%) to orbital speed for interesting dynamics
            float speed_variation = 0.9f + (float)(rand() % 100) / 100.0f * 0.2f;  // 0.9 to 1.1 multiplier
            orbital_speed *= speed_variation;
            
            // Calculate orbital velocity vector (perpendicular to radial direction)
            float vel_x = -sinf(angle) * orbital_speed;
            float vel_z = cosf(angle) * orbital_speed;
            float vel_y = ((float)(rand() % 100) / 100.0f - 0.5f) * 1.0f;  // Small vertical velocity
            
            // Choose random particle type
            uint32_t type_id;
            if (i % 3 == 0) type_id = light_type;
            else if (i % 3 == 1) type_id = medium_type;
            else type_id = heavy_type;
            
            float temperature = 20.0f + (float)(rand() % 100) / 100.0f * 30.0f;  // Random initial temperature
            
            particle_system_->add_particle(type_id, Vector3{x, y, z}, Vector3{vel_x, vel_y, vel_z}, temperature);
        }
        
        printf("Orbital simulation setup complete!\n");
        printf("Black hole at center, particles should form orbital dynamics\n");
    }
    
    void update() {
        // Handle input
        handle_input();
        
        // Update physics (slowed down 10x for stability)
        float dt = GetFrameTime() * 0.05f;
        particle_system_->update(dt);
        
        // Update camera (DisableCursor handles mouse capture automatically)
        UpdateCamera(&camera_, CAMERA_FREE);
    }
    
    void handle_input() {
        // Toggle cursor mode
        if (IsKeyPressed(KEY_ESCAPE)) {
            cursor_disabled_ = !cursor_disabled_;
            if (cursor_disabled_) {
                DisableCursor();
                printf("Mouse captured for first person camera control\n");
            } else {
                EnableCursor();
                printf("Mouse cursor released\n");
            }
        }
        
        // Add particle with orbital velocity
        if (IsKeyPressed(KEY_SPACE)) {
            Vector3 pos = camera_.position;
            
            // Calculate orbital velocity around black hole
            float distance_to_center = Vector3Length(pos);
            if (distance_to_center > 1.0f) {
                // Cross product for orbital velocity direction
                Vector3 to_center = Vector3Normalize(Vector3Scale(pos, -1.0f));
                Vector3 up = {0, 1, 0};
                Vector3 orbital_dir = Vector3CrossProduct(to_center, up);
                
                // Calculate proper orbital speed: v = sqrt(GM/r)
                float orbital_speed = sqrtf(50.0f * 100.0f / distance_to_center);
                
                // Add small random variation (±10%)
                float speed_variation = 0.9f + (float)(rand() % 100) / 100.0f * 0.2f;
                orbital_speed *= speed_variation;
                
                Vector3 vel = Vector3Scale(orbital_dir, orbital_speed);
                
                // Use medium particle type (ID 1) 
                uint32_t type_id = 1;
                float temperature = 25.0f + (float)(rand() % 100) / 100.0f * 20.0f;
                particle_system_->add_particle(type_id, pos, vel, temperature);
                printf("Added particle with orbital velocity at distance %.2f\n", distance_to_center);
            }
        }
        
        // Reset simulation
        if (IsKeyPressed(KEY_R)) {
            printf("Resetting simulation...\n");
            particle_system_->reset();
            setup_scene();
        }
        
        // Add multiple particles with varied orbits
        if (IsKeyPressed(KEY_E)) {
            printf("Creating orbital particle cluster...\n");
            for (int i = 0; i < 100; i++) {  // More particles to test new system
                float angle = (float)rand() / RAND_MAX * 2.0f * PI;
                float radius = 5.0f + (float)rand() / RAND_MAX * 15.0f;
                float height = ((float)rand() / RAND_MAX - 0.5f) * 3.0f;  // Reduced height variation
                
                Vector3 pos = {
                    cosf(angle) * radius,
                    height,
                    sinf(angle) * radius
                };
                
                // Calculate proper orbital velocity: v = sqrt(GM/r)
                float orbital_speed = sqrtf(50.0f * 100.0f / radius);
                
                // Add small random variation (±10%) to match initial setup
                float speed_variation = 0.9f + (float)rand() / RAND_MAX * 0.2f;  // 0.9 to 1.1 multiplier
                orbital_speed *= speed_variation;
                
                Vector3 vel = {
                    -sinf(angle) * orbital_speed,
                    ((float)rand() / RAND_MAX - 0.5f) * 1.5f,  // Small vertical velocity
                    cosf(angle) * orbital_speed
                };
                
                // Random particle type
                uint32_t type_id = rand() % 3;  // 0, 1, or 2
                float temperature = 15.0f + (float)rand() / RAND_MAX * 40.0f;
                particle_system_->add_particle(type_id, pos, vel, temperature);
            }
        }
        
        // Toggle debug spatial visualization
        if (IsKeyPressed(KEY_D)) {
            bool debug_enabled = !particle_system_->get_debug_spatial_visualization();
            particle_system_->set_debug_spatial_visualization(debug_enabled);
            if (debug_enabled) {
                printf("Debug visualization ENABLED - showing gravity radii and spatial cells\n");
            } else {
                printf("Debug visualization DISABLED\n");
            }
        }
        
        // Toggle neighbor lines visualization
        if (IsKeyPressed(KEY_N)) {
            bool neighbor_lines_enabled = !particle_system_->get_debug_neighbor_lines();
            particle_system_->set_debug_neighbor_lines(neighbor_lines_enabled);
            if (neighbor_lines_enabled) {
                printf("Neighbor lines ENABLED - showing particle interaction connections\n");
            } else {
                printf("Neighbor lines DISABLED\n");
            }
        }
        
        // Print detailed profiling stats
        if (IsKeyPressed(KEY_P)) {
            printf("\n=== Detailed Performance Profiling ===\n");
            particle_system_->print_profiling_stats();
        }
        
        // Reset profiling stats
        if (IsKeyPressed(KEY_T)) {
            particle_system_->reset_profiling_stats();
            printf("Profiling statistics reset\n");
        }
        
        // Toggle rendering mode for performance comparison
        if (IsKeyPressed(KEY_I)) {
            particle_system_->cycle_rendering_mode();
        }
    }
    
    void render() {
        BeginDrawing();
        ClearBackground(BLACK);
        
        BeginMode3D(camera_);
        
        // Clean space background (no ground plane or grid)
        
        // Draw coordinate axes
        DrawLine3D(Vector3{0, 0, 0}, Vector3{5, 0, 0}, RED);
        DrawLine3D(Vector3{0, 0, 0}, Vector3{0, 5, 0}, GREEN);
        DrawLine3D(Vector3{0, 0, 0}, Vector3{0, 0, 5}, BLUE);
        
        // Render particles
        particle_system_->render();
        
        EndMode3D();
        
        render_ui();
        
        EndDrawing();
    }
    
    void render_ui() {
        // System info
        DrawText("SIMPLIFIED STRUCTURE OF ARRAYS N-BODY SIMULATION", 10, 10, 20, GREEN);
        DrawText(TextFormat("Particles: %d + 1 Black Hole", 
                 particle_system_->get_particle_count()), 10, 40, 16, WHITE);
        DrawText(TextFormat("FPS: %d", GetFPS()), 10, 60, 16, WHITE);
        
        // Controls
        DrawText("Controls:", 10, 100, 16, YELLOW);
        DrawText("SPACE - Add particle with orbital velocity", 10, 120, 14, LIGHTGRAY);
        DrawText("E - Add 100 particles (test performance!)", 10, 140, 14, LIGHTGRAY);
        DrawText("D - Toggle debug visualization (spatial cells + gravity)", 10, 160, 14, LIGHTGRAY);
        DrawText("N - Toggle neighbor lines (particle interactions)", 10, 180, 14, LIGHTGRAY);
        DrawText("P - Print detailed profiling stats", 10, 200, 14, LIGHTGRAY);
        DrawText("T - Reset profiling statistics", 10, 220, 14, LIGHTGRAY);
        DrawText("I - Toggle rendering mode (performance test)", 10, 240, 14, LIGHTGRAY);
        DrawText("R - Reset simulation", 10, 260, 14, LIGHTGRAY);
        DrawText("ESC - Toggle mouse capture", 10, 280, 14, LIGHTGRAY);
        DrawText("WASD/Mouse - Camera control", 10, 300, 14, LIGHTGRAY);
        
        // Physics info
        DrawText(TextFormat("Physics step: %.2f ms", particle_system_->get_physics_time_ms()), 10, 330, 14, LIME);
        DrawText("Physics: Stable orbits + inelastic collisions (10x slower)", 10, 350, 14, SKYBLUE);
        DrawText("Particle types: Blue(light), Green(medium), Red(heavy)", 10, 370, 14, SKYBLUE);
        DrawText("Particles merge when close (< 0.15 units)", 10, 390, 14, ORANGE);
        
        // Rendering info
        DrawText(TextFormat("Rendering Mode: %s", particle_system_->get_rendering_mode_name()), 10, 420, 14, YELLOW);
        
        // Profiling info
        DrawText("Profiling (averaged):", 10, 450, 14, SKYBLUE);
        DrawText(TextFormat("  Spatial Hash: %.2f ms", particle_system_->get_profiling_section_time("Populate Spatial Hash")), 10, 470, 12, WHITE);
        DrawText(TextFormat("  Gravity Forces: %.2f ms", particle_system_->get_profiling_section_time("Total Gravitational Forces")), 10, 490, 12, WHITE);
        DrawText(TextFormat("  Collision Det: %.2f ms", particle_system_->get_profiling_section_time("Collision Detection")), 10, 510, 12, WHITE);
        DrawText(TextFormat("  Integration: %.2f ms", particle_system_->get_profiling_section_time("Integrate Particles")), 10, 530, 12, WHITE);
        DrawText(TextFormat("  Rendering: %.2f ms", particle_system_->get_profiling_section_time("Particle Rendering")), 10, 550, 12, WHITE);
        
        // Debug info
        bool debug_vis = particle_system_->get_debug_spatial_visualization();
        bool neighbor_lines = particle_system_->get_debug_neighbor_lines();
        
        Color debug_color = debug_vis ? LIME : GRAY;
        Color neighbor_color = neighbor_lines ? LIME : GRAY;
        
        DrawText(TextFormat("Debug Visualization: %s", debug_vis ? "ON" : "OFF"), 10, 580, 14, debug_color);
        DrawText(TextFormat("Neighbor Lines: %s", neighbor_lines ? "ON" : "OFF"), 10, 600, 14, neighbor_color);
        
        if (debug_vis) {
            DrawText("Yellow cubes = spatial cells, Colored spheres = gravity influence", 10, 620, 12, YELLOW);
        }
        if (neighbor_lines) {
            DrawText("Colored lines = particle interactions (brighter = stronger force)", 10, 640, 12, LIGHTGRAY);
        }
        
        // Mouse control info
        if (cursor_disabled_) {
            DrawText("Mouse captured for camera control (ESC to release)", 10, screen_height_ - 30, 14, LIGHTGRAY);
        } else {
            DrawText("Mouse cursor free (ESC to capture for camera control)", 10, screen_height_ - 30, 14, YELLOW);
        }
    }
    
    void cleanup() {
        // Particle system cleanup is handled by destructor
    }
    
private:
    // Window settings
    int screen_width_;
    int screen_height_;
    
    // Debug mode
    bool debug_mode_;
    int debug_frame_count_;
    
    // Systems
    std::unique_ptr<ParticleSystem> particle_system_;
    
    // Rendering
    Camera camera_;
    bool cursor_disabled_ = true;
};

int main(int argc, char* argv[]) {
    printf("=== Particle Dynamics Demo with ODE Physics ===\n");
    
    bool debug_mode = false;
    
    // Check for debug flag
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0) {
            debug_mode = true;
            printf("DEBUG MODE ENABLED: Will auto-quit after 300 frames\n");
        }
    }
    
    try {
        ParticleDynamicsDemo demo(1280, 800, debug_mode);
        demo.run();
    } catch (const std::exception& e) {
        printf("Error: %s\n", e.what());
        return 1;
    }
    
    return 0;
} 