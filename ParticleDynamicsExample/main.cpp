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
#include "material_manager.h"
#include "cluster_manager.h"
#include "demo_interface.h"
#include "solar_system_demo.h"
#include "material_sandbox_demo.h"

/**
 * Demo Manager - handles multiple particle system demos
 * Supports switching between demos and manages shared resources
 */
class DemoManager {
public:
    DemoManager(int width, int height, bool debug_mode = false) 
        : screen_width_(width), screen_height_(height),
          debug_mode_(debug_mode), debug_frame_count_(0),
          current_demo_index_(0), current_demo_(nullptr),
          material_manager_(std::make_unique<MaterialManager>()),
          particle_system_(std::make_shared<ParticleSystem>(*material_manager_)),
          cluster_manager_(std::make_unique<ClusterManager>(*material_manager_)),
          camera_(), cursor_disabled_(true) {
        
        printf("DEBUG: Starting DemoManager initialization...\n");
        
        printf("DEBUG: Initializing window...\n");
        InitWindow(screen_width_, screen_height_, "Matter Engine 2 - Particle Dynamics Demos");
        SetTargetFPS(60);
        printf("DEBUG: Window initialized successfully\n");
        
        printf("DEBUG: Setting up cursor and camera...\n");
        // Disable cursor for first person camera control
        DisableCursor();
        
        // Set up camera
        camera_.position = {0.0f, 15.0f, 30.0f};
        camera_.target = {0.0f, 0.0f, 0.0f};
        camera_.up = {0.0f, 1.0f, 0.0f};
        camera_.fovy = 45.0f;
        camera_.projection = CAMERA_PERSPECTIVE;
        printf("DEBUG: Camera setup complete\n");
        
        printf("DEBUG: Initializing particle system...\n");
        particle_system_->initialize();
        printf("DEBUG: Particle system initialized\n");
        
        printf("DEBUG: Registering demos...\n");
        // Register demos
        register_demos();
        printf("DEBUG: Demos registered, count: %zu\n", demos_.size());
        
        printf("DEBUG: Initializing first demo...\n");
        // Initialize the first demo
        if (!demos_.empty()) {
            current_demo_ = demos_[current_demo_index_].get();
            printf("DEBUG: About to initialize demo: %s\n", current_demo_->get_name());
            current_demo_->initialize(particle_system_, camera_);
            printf("DEBUG: Demo initialized successfully\n");
        }
        
        printf("=== Matter Engine 2 Demo Manager Initialized ===\n");
        printf("Available demos: %zu\n", demos_.size());
        if (current_demo_) {
            printf("Current demo: %s\n", current_demo_->get_name());
        }
    }
    
    ~DemoManager() {
        cleanup();
        EnableCursor(); // Restore cursor functionality before closing
        CloseWindow();
    }
    
    void run() {
        printf("DEBUG: Starting main game loop...\n");
        
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
        
        printf("DEBUG: Exited main game loop\n");
        
        // Print final profiling stats before cleanup
        if (debug_mode_ && current_demo_) {
            particle_system_->print_profiling_stats();
        }
        
        cleanup();
        printf("DEBUG: Cleanup complete\n");
    }

private:
    void register_demos() {
        // Add all available demos here - Material Sandbox is now primary
        demos_.push_back(std::make_unique<MaterialSandboxDemo>());
        demos_.push_back(std::make_unique<SolarSystemDemo>());
        
        // Future demos can be added here:
        // demos_.push_back(std::make_unique<FluidSimulationDemo>());
        // demos_.push_back(std::make_unique<CollisionDemo>());
        // demos_.push_back(std::make_unique<GalaxyFormationDemo>());
    }
    
    void switch_demo(int new_index) {
        if (new_index < 0 || new_index >= static_cast<int>(demos_.size())) {
            return; // Invalid index
        }
        
        if (new_index == current_demo_index_) {
            return; // Already on this demo
        }
        
        printf("=== Switching Demos ===\n");
        
        // Cleanup current demo
        if (current_demo_) {
            printf("Cleaning up: %s\n", current_demo_->get_name());
            current_demo_->cleanup();
        }
        
        // Reset the particle system to clear all particles from previous demo
        printf("Resetting particle system...\n");
        particle_system_->reset();
        
        // Switch to new demo
        current_demo_index_ = new_index;
        current_demo_ = demos_[current_demo_index_].get();
        
        printf("Initializing: %s\n", current_demo_->get_name());
        printf("Description: %s\n", current_demo_->get_description());
        
        // Initialize new demo
        current_demo_->initialize(particle_system_, camera_);
        
        // Set cursor visibility based on demo preference
        cursor_disabled_ = !current_demo_->should_show_cursor();
        if (cursor_disabled_) {
            DisableCursor();
        } else {
            EnableCursor();
        }
        
        printf("Demo switch complete! Cursor %s\n", cursor_disabled_ ? "disabled" : "enabled");
    }
    
    void next_demo() {
        int next_index = (current_demo_index_ + 1) % static_cast<int>(demos_.size());
        switch_demo(next_index);
    }
    
    void previous_demo() {
        int prev_index = current_demo_index_ - 1;
        if (prev_index < 0) {
            prev_index = static_cast<int>(demos_.size()) - 1;
        }
        switch_demo(prev_index);
    }
    
    void update() {
        // Handle global input (demo switching, cursor control, etc.)
        handle_global_input();
        
        // Update current demo
        if (current_demo_) {
            // Update physics with demo-specific timestep multiplier
            float dt = GetFrameTime() * current_demo_->get_timestep_multiplier();
            particle_system_->update(dt);
            
            // Update clusters (pass particle types from particle system)
            // TODO: Add getter for particle types in ParticleSystem
            std::vector<ParticleType> empty_types; // Placeholder until we add proper getter
            cluster_manager_->update_clusters(dt, empty_types);
            
            // Let the demo handle its specific input
            current_demo_->handle_input(camera_, particle_system_);
            
            // Let the demo update its logic
            current_demo_->update(dt, camera_);
        }
    }
    
    void handle_global_input() {
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
        
        // Demo switching
        if (IsKeyPressed(KEY_TAB)) {
            if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
                previous_demo();
            } else {
                next_demo();
            }
        }
        
        // Reset current demo
        if (IsKeyPressed(KEY_R)) {
            if (current_demo_) {
                printf("Resetting current demo...\n");
                current_demo_->reset(particle_system_, camera_);
            }
        }
        
        // Quick demo selection with number keys
        for (int i = 0; i < static_cast<int>(demos_.size()) && i < 10; i++) {
            if (IsKeyPressed(KEY_ONE + i)) {
                switch_demo(i);
                break;
            }
        }
    }
    
    void render() {
        BeginDrawing();
        ClearBackground(BLACK);
        
        BeginMode3D(camera_);
        
        // Let the current demo render its 3D elements
        if (current_demo_) {
            current_demo_->render_3d(particle_system_);
        }
        
        // Render particles
        particle_system_->render();
        
        // Render clusters
        cluster_manager_->render_clusters();
        cluster_manager_->render_cluster_bounds();
        
        EndMode3D();
        
        render_ui();
        
        EndDrawing();
    }
    
    void render_ui() {
        // Global demo manager UI
        render_global_ui();
        
        // Let the current demo render its UI
        if (current_demo_) {
            current_demo_->render_ui(screen_width_, screen_height_, particle_system_);
        }
        
        // Mouse control info (always show at bottom)
        if (cursor_disabled_) {
            DrawText("Mouse captured for camera control (ESC to release)", 10, screen_height_ - 30, 14, LIGHTGRAY);
        } else {
            DrawText("Mouse cursor free (ESC to capture for camera control)", 10, screen_height_ - 30, 14, YELLOW);
        }
    }
    
    void render_global_ui() {
        // Demo manager header
        DrawRectangle(0, 0, screen_width_, 100, ColorAlpha(BLACK, 0.7f));
        DrawText("MATTER ENGINE 2 - PARTICLE DYNAMICS DEMOS", 10, 10, 20, LIME);
        
        if (current_demo_) {
            DrawText(TextFormat("Current Demo: %s (%d/%d)", 
                     current_demo_->get_name(),
                     current_demo_index_ + 1,
                     static_cast<int>(demos_.size())), 10, 40, 16, WHITE);
            DrawText(current_demo_->get_description(), 10, 60, 14, LIGHTGRAY);
        }
        
        // Global controls (always available)
        int control_y = screen_height_ - 120;
        DrawRectangle(0, control_y, screen_width_, 90, ColorAlpha(BLACK, 0.7f));
        DrawText("Global Controls:", 10, control_y + 5, 14, YELLOW);
        DrawText("TAB - Next demo | SHIFT+TAB - Previous demo | 1-9 - Quick demo select", 10, control_y + 25, 12, LIGHTGRAY);
        DrawText("R - Reset current demo | ESC - Toggle mouse capture", 10, control_y + 45, 12, LIGHTGRAY);
        DrawText("WASD/Mouse - Camera control", 10, control_y + 65, 12, LIGHTGRAY);
        
        // Demo list (right side)
        int demo_list_x = screen_width_ - 250;
        DrawRectangle(demo_list_x, 0, 250, 30 + static_cast<int>(demos_.size()) * 20, ColorAlpha(BLACK, 0.7f));
        DrawText("Available Demos:", demo_list_x + 10, 10, 14, YELLOW);
        
        for (size_t i = 0; i < demos_.size(); i++) {
            Color demo_color = (i == static_cast<size_t>(current_demo_index_)) ? LIME : LIGHTGRAY;
            const char* demo_name = demos_[i]->get_name();
            
            DrawText(TextFormat("%d. %s", static_cast<int>(i + 1), demo_name), 
                     demo_list_x + 15, 30 + static_cast<int>(i) * 20, 12, demo_color);
        }
    }
    
    void cleanup() {
        // Cleanup current demo
        if (current_demo_) {
            current_demo_->cleanup();
            current_demo_ = nullptr;
        }
        
        // Cleanup all demos
        demos_.clear();
        
        // Particle system cleanup is handled by destructor
    }
    
private:
    // Window settings
    int screen_width_;
    int screen_height_;
    
    // Debug mode
    bool debug_mode_;
    int debug_frame_count_;
    
    // Demo management
    std::vector<std::unique_ptr<DemoInterface>> demos_;
    int current_demo_index_;
    DemoInterface* current_demo_ = nullptr;
    
    // Shared systems
    std::unique_ptr<MaterialManager> material_manager_;
    std::shared_ptr<ParticleSystem> particle_system_;
    std::unique_ptr<ClusterManager> cluster_manager_;
    
    // Rendering
    Camera camera_;
    bool cursor_disabled_;
};

int main(int argc, char* argv[]) {
    printf("=== Matter Engine 2 - Particle Dynamics Demo System ===\n");
    printf("DEBUG: Main function started\n");
    
    bool debug_mode = false;
    
    // Check for debug flag
    printf("DEBUG: Checking command line arguments\n");
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0) {
            debug_mode = true;
            printf("DEBUG MODE ENABLED: Will auto-quit after 300 frames\n");
        }
    }
    
    printf("DEBUG: About to create DemoManager\n");
    try {
        printf("DEBUG: Creating DemoManager instance\n");
        DemoManager demo_manager(1280, 800, debug_mode);
        printf("DEBUG: DemoManager created, about to run\n");
        demo_manager.run();
        printf("DEBUG: DemoManager run completed\n");
    } catch (const std::exception& e) {
        printf("EXCEPTION: %s\n", e.what());
        return 1;
    } catch (...) {
        printf("UNKNOWN EXCEPTION caught\n");
        return 1;
    }
    
    printf("Demo system shutting down cleanly\n");
    return 0;
} 