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
#include "include/bvh_visualizer.hpp"
#include "include/cluster.h"
#include "include/cell.h"
#include "include/profiler.hpp"

class MatterSurfaceLibDemo {
public:
    MatterSurfaceLibDemo(int width, int height) 
        : screen_width_(width), screen_height_(height),
          blas_manager_(std::make_unique<BLASManager>()),
          tlas_manager_(std::make_unique<TLASManager>(1000)),
          bvh_visualizer_(std::make_unique<BVHVisualizer>()),
          test_cluster_(std::make_unique<Cluster>(0, 3.0f)) {
        
        InitWindow(screen_width_, screen_height_, "MatterSurfaceLib - Cluster and Cell System");
        SetTargetFPS(60);
        
        DisableCursor();
        
        setup_rendering();
        setup_matter_system();
        setup_scene();
    }
    
    ~MatterSurfaceLibDemo() {
        cleanup();
        EnableCursor();
        CloseWindow();
    }
    
    void run() {
        // Print initial BLAS/TLAS statistics
        print_rendering_stats();
        
        while (!WindowShouldClose()) {
            PROFILE_FRAME_BEGIN();
            
            {
                PROFILE_SECTION("Update");
                update();
            }
            
            {
                PROFILE_SECTION("Render");
                render();
            }
            
            PROFILE_FRAME_END();
            
            // Print performance stats every 60 frames (roughly once per second at 60 FPS)
            static int frame_counter = 0;
            frame_counter++;
            if (frame_counter >= 60) {
                printf("\n=== FRAME %d PERFORMANCE REPORT ===\n", frame_counter);
                PROFILE_PRINT();
                print_rendering_stats();
                
                // Reset profiler stats to show per-period performance instead of cumulative
                PROFILE_RESET();
                frame_counter = 0;
            }
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
        for (int i = 0; i < 200; ++i) {
            float angle1 = (float)i * 0.05f;
            float angle2 = (float)i * 0.025f;
            
            Vector3 position = {
                cosf(angle1) * sinf(angle2) * 10.0f,
                sinf(angle1) * sinf(angle2) * 10.0f,
                cosf(angle2) * 10.0f
            };
            
            uint32_t material = 0;//i % 3; // Cycle through materials
            test_cluster_->add_particle(position, 1.0f, material);
        }
        
        // Add some additional particles in a line
        // for (int i = 0; i < 10; ++i) {
        //     Vector3 position = {(float)i - 10.0f, 0.0f, 0.0f};
        //     test_cluster_->add_particle(position, 0.8f, 1);
        // }
        
        printf("Added %u particles to cluster\n", test_cluster_->get_particle_count());
        
        // Position cluster in world space
        test_cluster_->set_position({0.0f, 2.0f, 0.0f});
        
        // Force initial mesh rebuild
        test_cluster_->set_lod_level(1);
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
        debug_triangle_tests_loc_ = GetShaderLocation(raytracing_shader_, "debugTriangleTests");
        
        // BLAS/TLAS uniforms are now handled by their respective managers
    }
    
    void update() {
        {
            PROFILE_SECTION("Input Handling");
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
                       render_mode_ == 2 ? "Wireframe Meshes" : "Debug BVH");
            }
            
            // BVH visualization toggle
            if (IsKeyPressed(KEY_B)) {
                show_bvh_visualization_ = !show_bvh_visualization_;
                printf("BVH visualization %s\n", show_bvh_visualization_ ? "enabled" : "disabled");
            }
            
            // Triangle test debug mode toggle
            if (IsKeyPressed(KEY_G)) {
                debug_triangle_tests_ = !debug_triangle_tests_;
                printf("Triangle test debug mode %s\n", debug_triangle_tests_ ? "enabled" : "disabled");
                printf("Green = few triangle tests, Yellow = moderate, Red = many tests per ray\n");
            }
            
            // Mesh visibility toggle
            if (IsKeyPressed(KEY_M)) {
                show_meshes_ = !show_meshes_;
                printf("Mesh visibility %s\n", show_meshes_ ? "enabled" : "disabled");
            }
        }
        
        {
            PROFILE_SECTION("BVH Settings");
            // BVH visualization settings (only work when visualization is enabled)
            if (show_bvh_visualization_) {
                auto& settings = bvh_visualizer_->get_settings();
                
                if (IsKeyPressed(KEY_Q)) {
                    settings.show_blas_bvh = !settings.show_blas_bvh;
                    printf("BLAS BVH visualization %s\n", settings.show_blas_bvh ? "enabled" : "disabled");
                }
                if (IsKeyPressed(KEY_I)) {
                    settings.show_tlas_bvh = !settings.show_tlas_bvh;
                    printf("TLAS BVH visualization %s\n", settings.show_tlas_bvh ? "enabled" : "disabled");
                }
                if (IsKeyPressed(KEY_V)) {
                    settings.show_leaf_nodes = !settings.show_leaf_nodes;
                    printf("Leaf nodes %s\n", settings.show_leaf_nodes ? "enabled" : "disabled");
                }
                if (IsKeyPressed(KEY_T)) {
                    settings.show_interior_nodes = !settings.show_interior_nodes;
                    printf("Interior nodes %s\n", settings.show_interior_nodes ? "enabled" : "disabled");
                }
                if (IsKeyPressed(KEY_Y)) {
                    settings.use_depth_colors = !settings.use_depth_colors;
                    printf("Depth colors %s\n", settings.use_depth_colors ? "enabled" : "disabled");
                }
                if (IsKeyPressed(KEY_U)) {
                    settings.show_triangles = !settings.show_triangles;
                    printf("Triangle wireframes %s\n", settings.show_triangles ? "enabled" : "disabled");
                }
                if (IsKeyPressed(KEY_UP)) {
                    settings.max_depth_to_show = std::min(15, settings.max_depth_to_show + 1);
                    printf("Max depth to show: %d\n", settings.max_depth_to_show);
                }
                if (IsKeyPressed(KEY_DOWN)) {
                    settings.max_depth_to_show = std::max(1, settings.max_depth_to_show - 1);
                    printf("Max depth to show: %d\n", settings.max_depth_to_show);
                }
            }
        }
        
        {
            PROFILE_SECTION("Particle System Updates");
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
                
                {
                    PROFILE_SECTION("Rebuild Dirty Cells");
                    test_cluster_->rebuild_dirty_cells(*blas_manager_);
                    rebuild_tlas_after_cell_changes();
                }
                printf("Added 10 random particles. Cluster now has %u cells\n", 
                       test_cluster_->get_cell_count());
            }
        }
        
        {
            PROFILE_SECTION("LOD Controls");
            // LOD level controls
            if (IsKeyPressed(KEY_ONE)) {
                test_cluster_->set_lod_level(0);
                test_cluster_->rebuild_dirty_cells(*blas_manager_);
                rebuild_tlas_after_cell_changes();
            }
            if (IsKeyPressed(KEY_TWO)) {
                test_cluster_->set_lod_level(1);
                test_cluster_->rebuild_dirty_cells(*blas_manager_);
                rebuild_tlas_after_cell_changes();
            }
            if (IsKeyPressed(KEY_THREE)) {
                test_cluster_->set_lod_level(2);
                test_cluster_->rebuild_dirty_cells(*blas_manager_);
                rebuild_tlas_after_cell_changes();
            }
            if (IsKeyPressed(KEY_FOUR)) {
                test_cluster_->set_lod_level(3);
                test_cluster_->rebuild_dirty_cells(*blas_manager_);
                rebuild_tlas_after_cell_changes();
            }
            if (IsKeyPressed(KEY_FIVE)) {
                test_cluster_->set_lod_level(4);
                test_cluster_->rebuild_dirty_cells(*blas_manager_);
                rebuild_tlas_after_cell_changes();
            }
        }
        
        {
            PROFILE_SECTION("Camera Update");
            UpdateCamera(&camera_, CAMERA_FREE);
        }
    }
    
    
    void render() {
        PROFILE_SECTION("BeginDrawing");
        BeginDrawing();
        ClearBackground(BLACK);
        
        if (render_mode_ == 0 && raytracing_shader_.id != 0) {
            PROFILE_SECTION("RayTracing Mode");
            render_raytraced();
        } else {
            PROFILE_SECTION("3D Rasterization Mode");
            
            {
                PROFILE_SECTION("BeginMode3D");
                BeginMode3D(camera_);
            }
            
            if (render_mode_ == 1) {
                PROFILE_SECTION("Solid Surface Meshes");
                if (show_meshes_) {
                    render_scene_meshes();
                }
                test_cluster_->render_debug_bounds();
            } else if (render_mode_ == 2) {
                PROFILE_SECTION("Wireframe Meshes");
                if (show_meshes_) {
                    test_cluster_->render_cells(true);
                }
                test_cluster_->render_debug_bounds();
            } else if (render_mode_ == 3) {
                PROFILE_SECTION("Debug BVH Mode");
                if (show_meshes_) {
                    render_scene_meshes();
                }
                test_cluster_->render_debug_bounds();
            }
            
            // Render BVH visualization if enabled or in debug mode
            if (show_bvh_visualization_ || render_mode_ == 3) {
                PROFILE_SECTION("BVH Visualization");
                bvh_visualizer_->render(*blas_manager_, *tlas_manager_);
            }
            
            {
                PROFILE_SECTION("Draw Grid");
                DrawGrid(20, 1.0f);
            }
            
            {
                PROFILE_SECTION("EndMode3D");
                EndMode3D();
            }
        }
        
        {
            PROFILE_SECTION("UI Rendering");
            render_ui();
        }
        
        {
            PROFILE_SECTION("EndDrawing");
            EndDrawing();
        }
    }
    
    void render_raytraced() {
        {
            PROFILE_SECTION("Shader Setup");
            BeginShaderMode(raytracing_shader_);
            
            Vector2 screen_size = {static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())};
            
            SetShaderValue(raytracing_shader_, camera_pos_loc_, &camera_.position, SHADER_UNIFORM_VEC3);
            SetShaderValue(raytracing_shader_, camera_target_loc_, &camera_.target, SHADER_UNIFORM_VEC3);
            SetShaderValue(raytracing_shader_, camera_up_loc_, &camera_.up, SHADER_UNIFORM_VEC3);
            SetShaderValue(raytracing_shader_, camera_fovy_loc_, &camera_.fovy, SHADER_UNIFORM_FLOAT);
            SetShaderValue(raytracing_shader_, screen_size_loc_, &screen_size, SHADER_UNIFORM_VEC2);
            
            int debug_mode = debug_triangle_tests_ ? 1 : 0;
            SetShaderValue(raytracing_shader_, debug_triangle_tests_loc_, &debug_mode, SHADER_UNIFORM_INT);
        }
        
        {
            PROFILE_SECTION("BLAS Binding");
            blas_manager_->bind_to_shader(raytracing_shader_);
        }
        
        {
            PROFILE_SECTION("TLAS Binding");
            tlas_manager_->bind_to_shader(raytracing_shader_, *blas_manager_);
        }
        
        {
            PROFILE_SECTION("Fullscreen Quad");
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), WHITE);
        }
        
        {
            PROFILE_SECTION("End Shader");
            EndShaderMode();
        }
    }
    
    void render_scene_meshes() {
        const auto& draw_records = [this]() {
            PROFILE_SECTION("Get Draw Records");
            return tlas_manager_->get_draw_records();
        }();
        
        PROFILE_SECTION("Mesh Rendering Loop");
        int triangles_rendered = 0;
        int meshes_rendered = 0;
        
        for (size_t i = 0; i < draw_records.size(); i++) {
            const auto& record = draw_records[i];
            
            // Get the mesh for this BLAS handle
            auto* mesh = blas_manager_->get_mesh(record.blas_handle);
            if (!mesh || !mesh->tri || mesh->triCount == 0) {
                continue;
            }
            
            meshes_rendered++;
            triangles_rendered += mesh->triCount;
            
            // Choose color based on material ID and instance
            Color mesh_colors[] = {GREEN, BLUE, RED, YELLOW, PURPLE, ORANGE};
            Color mesh_color = mesh_colors[(record.material_id + record.instance_id) % 6];
            
            // Apply transform matrix
            rlPushMatrix();
            
            // Convert Matrix4x4 to OpenGL matrix format (column-major)
            const auto& m = record.transform.m;
            float gl_matrix[16] = {
                m[0], m[4], m[8],  m[12],
                m[1], m[5], m[9],  m[13],
                m[2], m[6], m[10], m[14],
                m[3], m[7], m[11], m[15]
            };
            rlMultMatrixf(gl_matrix);
            
            // Determine transparency based on render mode
            unsigned char alpha = (render_mode_ == 3) ? 80 : 120; // More transparent in debug mode
            
            // Render mesh as filled triangles with transparency
            rlBegin(RL_TRIANGLES);
            rlColor4ub(mesh_color.r, mesh_color.g, mesh_color.b, alpha);
            
            for (int tri_idx = 0; tri_idx < mesh->triCount; tri_idx++) {
                const auto& tri = mesh->tri[tri_idx];
                
                // Vertex 0
                rlVertex3f(tri.vertex0.x, tri.vertex0.y, tri.vertex0.z);
                // Vertex 1  
                rlVertex3f(tri.vertex1.x, tri.vertex1.y, tri.vertex1.z);
                // Vertex 2
                rlVertex3f(tri.vertex2.x, tri.vertex2.y, tri.vertex2.z);
            }
            rlEnd();
            
            // Also draw wireframe edges for better definition
            rlBegin(RL_LINES);
            rlColor4ub(mesh_color.r, mesh_color.g, mesh_color.b, 255); // Full opacity for edges
            
            for (int tri_idx = 0; tri_idx < mesh->triCount; tri_idx++) {
                const auto& tri = mesh->tri[tri_idx];
                
                // Edge 0-1
                rlVertex3f(tri.vertex0.x, tri.vertex0.y, tri.vertex0.z);
                rlVertex3f(tri.vertex1.x, tri.vertex1.y, tri.vertex1.z);
                
                // Edge 1-2
                rlVertex3f(tri.vertex1.x, tri.vertex1.y, tri.vertex1.z);
                rlVertex3f(tri.vertex2.x, tri.vertex2.y, tri.vertex2.z);
                
                // Edge 2-0
                rlVertex3f(tri.vertex2.x, tri.vertex2.y, tri.vertex2.z);
                rlVertex3f(tri.vertex0.x, tri.vertex0.y, tri.vertex0.z);
            }
            rlEnd();
            
            rlPopMatrix();
        }
        
        // Store stats for reporting
        last_triangles_rendered_ = triangles_rendered;
        last_meshes_rendered_ = meshes_rendered;
    }
    
    
    void render_ui() {
        // Performance info
        double fps = 1000.0 / Performance::Profiler::instance().get_frame_time_ms();
        DrawText(TextFormat("FPS: %.1f (%.2f ms)", fps, Performance::Profiler::instance().get_frame_time_ms()), 10, 10, 16, YELLOW);
        
        // Render mode info
        DrawText(TextFormat("Render Mode: %s (Press R to cycle)", 
                           render_mode_ == 0 ? "Ray Tracing" : 
                           render_mode_ == 1 ? "Surface Meshes" : 
                           render_mode_ == 2 ? "Wireframe Meshes" : "Debug BVH"), 
                 10, 30, 20, WHITE);
        
        DrawText("Press SPACE to add random particles", 10, 60, 20, WHITE);
        DrawText("Press ESC to toggle cursor", 10, 90, 20, WHITE);
        
        DrawText(TextFormat("Particles: %u, Cells: %u, LOD: %d (%.1f unit cells)", 
                           test_cluster_->get_particle_count(),
                           test_cluster_->get_cell_count(),
                           test_cluster_->get_lod_level(),
                           test_cluster_->get_current_cell_size()), 
                 10, 120, 20, WHITE);
        
        DrawText("Press 1-5 to change LOD level", 10, 150, 16, LIGHTGRAY);
        
        // Rendering stats
        if (render_mode_ != 0) {
            DrawText(TextFormat("Meshes: %d, Triangles: %d", last_meshes_rendered_, last_triangles_rendered_), 
                     10, 180, 16, GREEN);
        }
        
        // BVH visualization info
        if (show_bvh_visualization_ || render_mode_ == 3) {
            const auto& settings = bvh_visualizer_->get_settings();
            DrawText("BVH VISUALIZATION MODE", 10, 210, 16, YELLOW);
            DrawText("Q:BLAS I:TLAS V:Leaf T:Interior Y:Colors U:Triangles", 10, 230, 12, LIGHTGRAY);
            DrawText("UP/DOWN: Depth | B: Toggle visualization", 10, 245, 12, LIGHTGRAY);
            DrawText(TextFormat("BLAS:%s TLAS:%s Leaf:%s Interior:%s Depth:%d", 
                     settings.show_blas_bvh ? "ON" : "OFF",
                     settings.show_tlas_bvh ? "ON" : "OFF",
                     settings.show_leaf_nodes ? "ON" : "OFF",
                     settings.show_interior_nodes ? "ON" : "OFF", 
                     settings.max_depth_to_show), 10, 260, 12, LIGHTGRAY);
        } else {
            DrawText("Press B to toggle BVH visualization", 10, 210, 14, LIGHTGRAY);
        }
        
        // Debug triangle test mode info
        if (debug_triangle_tests_) {
            DrawText("TRIANGLE TEST DEBUG MODE - Press G to toggle", 10, 290, 14, RED);
            DrawText("Green=few tests, Yellow=moderate, Red=many tests per ray", 10, 310, 12, LIGHTGRAY);
        } else {
            DrawText("Press G to toggle triangle test debug mode", 10, 290, 12, LIGHTGRAY);
        }
        
        // Mesh visibility info
        if (render_mode_ != 0) { // Not in raytracing mode
            DrawText(TextFormat("Mesh visibility: %s (Press M to toggle)", 
                               show_meshes_ ? "ON" : "OFF"), 
                     10, 330, 12, show_meshes_ ? GREEN : RED);
        }
    }
    
    void print_rendering_stats() {
        printf("\n=== RENDERING SYSTEM STATISTICS ===\n");
        
        // BLAS stats
        printf("BLAS Manager:\n");
        printf("  - Active BLAS count: %d\n", blas_manager_->get_unique_blas_count());
        
        size_t total_triangles = 0;
        size_t total_vertices = 0;
        int blas_count = blas_manager_->get_unique_blas_count();
        for (int i = 0; i < blas_count; i++) {
            auto* mesh = blas_manager_->get_mesh(i);
            if (mesh) {
                total_triangles += mesh->triCount;
                total_vertices += mesh->triCount * 3; // Approximate vertex count
            }
        }
        printf("  - Total triangles: %zu\n", total_triangles);
        printf("  - Total vertices: %zu\n", total_vertices);
        printf("  - Memory usage: ~%.2f MB\n", (total_triangles * sizeof(Tri) + total_vertices * sizeof(Vector3)) / (1024.0 * 1024.0));
        
        // TLAS stats
        printf("TLAS Manager:\n");
        const auto& draw_records = tlas_manager_->get_draw_records();
        printf("  - Draw records: %zu\n", draw_records.size());
        printf("  - Active instances: %zu\n", draw_records.size());
        
        // Cluster stats
        printf("Cluster System:\n");
        printf("  - Particles: %u\n", test_cluster_->get_particle_count());
        printf("  - Cells: %u\n", test_cluster_->get_cell_count());
        printf("  - Dirty cells: %u\n", test_cluster_->get_dirty_cell_count());
        printf("  - LOD level: %d (%.2f unit cells)\n", 
               test_cluster_->get_lod_level(), test_cluster_->get_current_cell_size());
        
        // Shader stats
        printf("Shader System:\n");
        printf("  - Ray tracing shader loaded: %s\n", raytracing_shader_.id != 0 ? "YES" : "NO");
        if (raytracing_shader_.id != 0) {
            printf("  - Shader ID: %u\n", raytracing_shader_.id);
        }
        
        printf("========================================\n");
    }
    
    void rebuild_tlas_after_cell_changes() {
        PROFILE_SECTION("TLAS Rebuild After Cell Changes");
        
        // Clear old TLAS data
        tlas_manager_->clear();
        
        // Re-add all cluster meshes to TLAS
        test_cluster_->add_to_tlas(*tlas_manager_);
        
        // Build the new TLAS structure
        tlas_manager_->build(*blas_manager_);
        
        printf("TLAS rebuilt: %d instances, %d nodes\n", 
               tlas_manager_->get_instance_count(), 
               tlas_manager_->get_node_count());
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
    std::unique_ptr<BVHVisualizer> bvh_visualizer_;
    std::unique_ptr<Cluster> test_cluster_;
    
    
    Camera camera_;
    Shader raytracing_shader_{};
    bool cursor_disabled_ = true;
    int render_mode_ = 0; // 0=raytracing, 1=solid_meshes, 2=wireframe_meshes, 3=debug_bvh
    bool show_bvh_visualization_ = false;
    bool show_meshes_ = true;
    
    int camera_pos_loc_;
    int camera_target_loc_;
    int camera_up_loc_;
    int camera_fovy_loc_;
    int screen_size_loc_;
    int debug_triangle_tests_loc_;
    
    // Debug modes
    bool debug_triangle_tests_ = false;
    
    // Performance tracking
    int last_triangles_rendered_ = 0;
    int last_meshes_rendered_ = 0;

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