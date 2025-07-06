extern "C" {
    #include "raylib.h"
    #include "rlgl.h"
}

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

extern "C" {
    GLFWwindow* glfwGetCurrentContext();
}

#include "include/blas_manager.hpp"
#include "include/tlas_manager.hpp"
#include "include/bvh_visualizer.hpp"
#include "include/bvh_analyzer.h"
#include "include/cluster.h"
#include "include/cell.h"
#include "include/cell_debug_renderer.h"
#include "include/profiler.hpp"

class MatterSurfaceLibDemo {
public:
    MatterSurfaceLibDemo(int width, int height) 
        : screen_width_(width), screen_height_(height),
          blas_manager_(std::make_unique<BLASManager>()),
          tlas_manager_(std::make_unique<TLASManager>(1000)),
          bvh_visualizer_(std::make_unique<BVHVisualizer>()),
          test_cluster_(std::make_unique<Cluster>(0, *blas_manager_, *tlas_manager_, 5.0f)),
          cell_debug_renderer_(std::make_unique<CellDebugRenderer>()) {
        
        InitWindow(screen_width_, screen_height_, "MatterSurfaceLib - Cluster and Cell System");
        SetTargetFPS(60);
        
        // Start in UI interaction mode (cursor enabled) for immediate ImGui access
        cursor_disabled_ = false;
        EnableCursor();
        
        // Setup Dear ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        
        // Setup Dear ImGui style
        ImGui::StyleColorsDark();
        
        // Setup Platform/Renderer backends
        GLFWwindow* window = glfwGetCurrentContext();
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330");
        
        setup_rendering();
        register_scene_geometry();
        setup_matter_system();
        
        // Initialize BVH analysis system
        setup_bvh_analysis();
    }
    
    ~MatterSurfaceLibDemo() {
        cleanup();
        
        // ImGui Cleanup
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        
        EnableCursor();
        CloseWindow();
    }
    
    void run() {
        // Print initial BLAS/TLAS statistics
        print_rendering_stats();
        
        while (!WindowShouldClose()) {
            PROFILE_FRAME_BEGIN();
                update();
                render();
            PROFILE_FRAME_END();
            
            // Print performance stats every 60 frames (roughly once per second at 60 FPS)
            // static int frame_counter = 0;
            // frame_counter++;
            // if (frame_counter >= 60) {
            //     printf("\n=== FRAME %d PERFORMANCE REPORT ===\n", frame_counter);
            //     PROFILE_PRINT();
            //     print_rendering_stats();
                
            //     // Reset profiler stats to show per-period performance instead of cumulative
            //     PROFILE_RESET();
            //     frame_counter = 0;
            // }
        }
    }

private:
    void register_scene_geometry() {
        printf("=== Registering Scene Geometry ===\n");
        
        // Register sphere BLAS for reflective sphere
        sphere_blas_ = BLASFactory::register_sphere(*blas_manager_, 1.0f, 32, 16);
        printf("  Sphere BLAS registered: handle=%u\n", sphere_blas_);
        
        // Register plane BLAS for ground
        ground_blas_ = BLASFactory::register_plane(*blas_manager_, 50.0f, 50.0f);
        printf("  Ground plane BLAS registered: handle=%u\n", ground_blas_);
        
        printf("=== Scene Geometry Registration Complete ===\n");
    }


    void setup_matter_system() {
        // Create a cluster of particles to demonstrate the system
        printf("Setting up matter system with cluster and cells...\n");
        
        // Add particles in a roughly spherical distribution - First cluster
        for (int i = 0; i < 150; ++i) {
            float angle1 = (float)i * 0.05f;
            float angle2 = (float)i * 0.025f;
            
            Vector3 position = {
                cosf(angle1) * sinf(angle2) * 10.0f,
                sinf(angle1) * sinf(angle2) * 10.0f,
                cosf(angle2) * 10.0f
            };
            
            // Cycle through first 4 materials (0-3): Red metallic, Blue diffuse, Green ground, Gold metallic
            uint32_t material = (i / 20) % 4;
            test_cluster_->add_particle(position, 1.0f, material);
        }

        // Add particles in a roughly spherical distribution - Second cluster
        for (int i = 0; i < 150; ++i) {
            float angle1 = (float)i * 0.15f;
            float angle2 = (float)i * 0.035f;
            
            Vector3 position = {
                5 + cosf(angle1) * sinf(angle2) * 10.0f,
                -sinf(angle1) * sinf(angle2) * 10.0f,
                cosf(angle2) * 10.0f
            };
             
            // Cycle through materials 4-7: Glass, Emissive light, Green glass, Water
            uint32_t material = 4 + ((i / 20) % 4);
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
        test_cluster_->rebuild_dirty_cells();
        
        printf("Cluster has %u cells, %u dirty\n", 
               test_cluster_->get_cell_count(), test_cluster_->get_dirty_cell_count());
    }
    
    void setup_bvh_analysis() {
        // Register TLAS for analysis
        BVHReportManager::RegisterTLAS("Main TLAS", tlas_manager_->get_tlas());
        
        // Initial analysis update
        BVHReportManager::UpdateAllAnalyses();
        last_bvh_analysis_update_ = GetTime();
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
            
            // Tab key toggles cursor mode (primary toggle for UI interaction)
            if (IsKeyPressed(KEY_TAB)) {
                cursor_disabled_ = !cursor_disabled_;
                if (cursor_disabled_) {
                    DisableCursor();
                    printf("Camera control mode: Mouse locked for camera movement\n");
                } else {
                    EnableCursor();
                    printf("UI interaction mode: Mouse unlocked for ImGui\n");
                }
            }
            
            // ESC key also toggles cursor (backup/legacy control)
            if (IsKeyPressed(KEY_ESCAPE)) {
                cursor_disabled_ = !cursor_disabled_;
                if (cursor_disabled_) {
                    DisableCursor();
                    printf("Camera control mode: Mouse locked for camera movement\n");
                } else {
                    EnableCursor();
                    printf("UI interaction mode: Mouse unlocked for ImGui\n");
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
            
            // Manual BLAS manager clear (for debugging)
            if (IsKeyPressed(KEY_C)) {
                printf("Manual BLAS manager clear requested\n");
                blas_manager_->clear();
                
                // Rebuild everything
                test_cluster_->rebuild_dirty_cells();
                printf("BLAS manager cleared and scene rebuilt\n");
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
                    // Use all 8 material types (0-7) for dynamic particles
                    uint32_t material = GetRandomValue(0, 7);
                    test_cluster_->add_particle(new_pos, 0.5f, material);
                }
                
                {
                    PROFILE_SECTION("Rebuild Dirty Cells");
                    test_cluster_->rebuild_dirty_cells();
                }
                printf("Added 10 random particles. Cluster now has %u cells\n", 
                       test_cluster_->get_cell_count());
            }
        }
        
        {
            PROFILE_SECTION("LOD Controls");
            // LOD level controls
            if (IsKeyPressed(KEY_ONE)) {
                printf("\n=== LOD CHANGE TO 0 ===\n");
                // printf("BEFORE: ");
                // blas_manager_->print_stats();
                
                test_cluster_->set_lod_level(0, true);  // clear_blas = true
                test_cluster_->rebuild_dirty_cells();
                
                // printf("AFTER REBUILD: ");
                // blas_manager_->print_stats();
                
                // printf("AFTER TLAS REBUILD: ");
                // blas_manager_->print_stats();
                // printf("========================\n");
            }
            if (IsKeyPressed(KEY_TWO)) {
                printf("\n=== LOD CHANGE TO 1 ===\n");
                // printf("BEFORE: ");
                // blas_manager_->print_stats();
                
                test_cluster_->set_lod_level(1, true);  // clear_blas = true
                test_cluster_->rebuild_dirty_cells();
                
                // printf("AFTER REBUILD: ");
                // blas_manager_->print_stats();
                
                // printf("AFTER TLAS REBUILD: ");
                // blas_manager_->print_stats();
                // printf("========================\n");
            }
            if (IsKeyPressed(KEY_THREE)) {
                printf("\n=== LOD CHANGE TO 2 ===\n");
                // printf("BEFORE: ");
                // blas_manager_->print_stats();
                
                test_cluster_->set_lod_level(2, true);  // clear_blas = true
                test_cluster_->rebuild_dirty_cells();
                
                // printf("AFTER REBUILD: ");
                // blas_manager_->print_stats();
                
                // printf("AFTER TLAS REBUILD: ");
                // blas_manager_->print_stats();
                // printf("========================\n");
            }
            if (IsKeyPressed(KEY_FOUR)) {
                printf("\n=== LOD CHANGE TO 3 ===\n");
                // printf("BEFORE: ");
                // blas_manager_->print_stats();
                
                test_cluster_->set_lod_level(3, true);  // clear_blas = true
                test_cluster_->rebuild_dirty_cells();
                
                // printf("AFTER REBUILD: ");
                // blas_manager_->print_stats();
                
                // printf("AFTER TLAS REBUILD: ");
                // blas_manager_->print_stats();
                // printf("========================\n");
            }
            if (IsKeyPressed(KEY_FIVE)) {
                printf("\n=== LOD CHANGE TO 4 ===\n");
                // printf("BEFORE: ");
                // blas_manager_->print_stats();
                
                test_cluster_->set_lod_level(4, true);  // clear_blas = true
                test_cluster_->rebuild_dirty_cells();
                
                // printf("AFTER REBUILD: ");
                // blas_manager_->print_stats();
                
                // printf("AFTER TLAS REBUILD: ");
                // blas_manager_->print_stats();
                // printf("========================\n");
            }
        }
        
        {
            PROFILE_SECTION("Camera Update");
            // Only update camera when cursor is disabled (camera control mode)
            if (cursor_disabled_) {
                UpdateCamera(&camera_, CAMERA_FREE);
            }
        }
    }
    
    
    void render() {
        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        
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
                    // Also render cell meshes in solid mode
                    cell_debug_renderer_->set_wireframe_mode(false);
                    cell_debug_renderer_->set_show_meshes(true);
                    cell_debug_renderer_->set_show_bounds(false);
                    test_cluster_->accept(*cell_debug_renderer_);
                }
                cell_debug_renderer_->render_cluster_debug_bounds(*test_cluster_);
            } else if (render_mode_ == 2) {
                PROFILE_SECTION("Wireframe Meshes");
                if (show_meshes_) {
                    cell_debug_renderer_->set_wireframe_mode(true);
                    cell_debug_renderer_->set_show_meshes(true);
                    cell_debug_renderer_->set_show_bounds(false);
                    test_cluster_->accept(*cell_debug_renderer_);
                }
                cell_debug_renderer_->render_cluster_debug_bounds(*test_cluster_);
            } else if (render_mode_ == 3) {
                PROFILE_SECTION("Debug BVH Mode");
                if (show_meshes_) {
                    render_scene_meshes();
                }
                cell_debug_renderer_->render_cluster_debug_bounds(*test_cluster_);
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
        
        // Render ImGui
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        
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
        // Main control panel - positioned on the left
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(350, 500), ImGuiCond_FirstUseEver);
        ImGui::Begin("MatterSurfaceLib Controls");
        
        // Performance info
        double fps = 1000.0 / Performance::Profiler::instance().get_frame_time_ms();
        ImGui::Text("FPS: %.1f (%.2f ms)", fps, Performance::Profiler::instance().get_frame_time_ms());
        
        // Cursor mode indicator
        if (cursor_disabled_) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Mode: Camera Control (TAB to unlock)");
        } else {
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "Mode: UI Interaction (TAB to lock)");
        }
        
        ImGui::Separator();
        
        // Render mode selection
        const char* render_modes[] = {"Ray Tracing", "Surface Meshes", "Wireframe Meshes", "Debug BVH"};
        if (ImGui::Combo("Render Mode", &render_mode_, render_modes, 4)) {
            printf("Render mode changed to: %s\n", render_modes[render_mode_]);
        }
        
        ImGui::Separator();
        
        // Particle system controls
        ImGui::Text("Particle System");
        if (ImGui::Button("Add Random Particles")) {
            // Add 10 random particles (same as SPACE key)
            for (int i = 0; i < 10; ++i) {
                float x = (GetRandomValue(-50, 50) / 10.0f);
                float y = (GetRandomValue(-50, 50) / 10.0f);
                float z = (GetRandomValue(-50, 50) / 10.0f);
                
                Vector3 new_pos = {x, y, z};
                uint32_t material = GetRandomValue(0, 7);
                test_cluster_->add_particle(new_pos, 0.5f, material);
            }
            test_cluster_->rebuild_dirty_cells();
        }
        
        ImGui::Text("Particles: %u, Cells: %u", 
                   test_cluster_->get_particle_count(),
                   test_cluster_->get_cell_count());
        
        // LOD controls
        int current_lod = test_cluster_->get_lod_level();
        if (ImGui::SliderInt("LOD Level", &current_lod, 0, 4)) {
            test_cluster_->set_lod_level(current_lod, true);
            test_cluster_->rebuild_dirty_cells();
        }
        ImGui::Text("Cell Size: %.2f units", test_cluster_->get_current_cell_size());
        
        ImGui::Separator();
        
        // Visualization controls
        ImGui::Text("Visualization");
        ImGui::Checkbox("Show Meshes", &show_meshes_);
        ImGui::Checkbox("BVH Visualization", &show_bvh_visualization_);
        ImGui::Checkbox("Debug Triangle Tests", &debug_triangle_tests_);
        
        // BVH settings (only when visualization is enabled)
        if (show_bvh_visualization_ || render_mode_ == 3) {
            auto& settings = bvh_visualizer_->get_settings();
            ImGui::Text("BVH Settings:");
            ImGui::Checkbox("Show BLAS BVH", &settings.show_blas_bvh);
            ImGui::Checkbox("Show TLAS BVH", &settings.show_tlas_bvh);
            ImGui::Checkbox("Show Leaf Nodes", &settings.show_leaf_nodes);
            ImGui::Checkbox("Show Interior Nodes", &settings.show_interior_nodes);
            ImGui::Checkbox("Use Depth Colors", &settings.use_depth_colors);
            ImGui::Checkbox("Show Triangles", &settings.show_triangles);
            ImGui::SliderInt("Max Depth", &settings.max_depth_to_show, 1, 15);
        }
        
        ImGui::Separator();
        
        // System statistics
        ImGui::Text("System Statistics");
        ImGui::Text("BLAS Entries: %d", blas_manager_->get_unique_blas_count());
        ImGui::Text("Total Triangles: %d", blas_manager_->get_total_triangle_count());
        
        if (render_mode_ != 0) {
            ImGui::Text("Rendered Meshes: %d", last_meshes_rendered_);
            ImGui::Text("Rendered Triangles: %d", last_triangles_rendered_);
        }
        
        if (ImGui::Button("Clear BLAS Manager")) {
            blas_manager_->clear();
            test_cluster_->rebuild_dirty_cells();
            printf("BLAS manager cleared and scene rebuilt\n");
        }
        
        ImGui::End();
        
        // Material reference window - positioned bottom left
        ImGui::SetNextWindowPos(ImVec2(20, 540), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(350, 180), ImGuiCond_FirstUseEver);
        ImGui::Begin("Material Reference");
        ImGui::Text("Material Types:");
        ImGui::BulletText("0: Red metallic");
        ImGui::BulletText("1: Blue diffuse");
        ImGui::BulletText("2: Green ground");
        ImGui::BulletText("3: Gold metallic");
        ImGui::BulletText("4: Clear glass");
        ImGui::BulletText("5: Emissive light");
        ImGui::BulletText("6: Green glass");
        ImGui::BulletText("7: Water");
        ImGui::End();
        
        // Keyboard shortcuts help - positioned center bottom
        ImGui::SetNextWindowPos(ImVec2(390, 540), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(400, 180), ImGuiCond_FirstUseEver);
        ImGui::Begin("Keyboard Shortcuts");
        ImGui::Text("Controls:");
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "TAB: Toggle cursor mode (UI/Camera)");
        ImGui::BulletText("ESC: Toggle cursor (backup)");
        ImGui::BulletText("SPACE: Add random particles");
        ImGui::BulletText("R: Cycle render modes");
        ImGui::BulletText("B: Toggle BVH visualization");
        ImGui::BulletText("G: Toggle triangle test debug");
        ImGui::BulletText("M: Toggle mesh visibility");
        ImGui::BulletText("C: Clear BLAS manager");
        ImGui::BulletText("1-5: Change LOD level");
        if (show_bvh_visualization_) {
            ImGui::Text("BVH Controls:");
            ImGui::BulletText("Q: Toggle BLAS BVH");
            ImGui::BulletText("I: Toggle TLAS BVH");
            ImGui::BulletText("V: Toggle leaf nodes");
            ImGui::BulletText("T: Toggle interior nodes");
            ImGui::BulletText("Y: Toggle depth colors");
            ImGui::BulletText("U: Toggle triangles");
            ImGui::BulletText("UP/DOWN: Adjust max depth");
        }
        ImGui::End();
        
        // BVH Analysis Window - positioned on the right side
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 400, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(380, 600), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("BVH Analysis")) {
            ImGui::Checkbox("Show BVH Analysis", &show_bvh_analysis_window_);
            ImGui::Checkbox("Auto Update", &auto_update_bvh_analysis_);
            
            if (ImGui::Button("Manual Update All")) {
                BVHReportManager::UpdateAllAnalyses();
                last_bvh_analysis_update_ = GetTime();
            }
            
            ImGui::SameLine();
            if (ImGui::Button("Clear All")) {
                BVHReportManager::Clear();
            }
            
            ImGui::Text("Last Update: %.2f seconds ago", GetTime() - last_bvh_analysis_update_);
            
            // Auto-update if enabled and enough time has passed
            if (auto_update_bvh_analysis_ && (GetTime() - last_bvh_analysis_update_) > 2.0f) {
                BVHReportManager::UpdateAllAnalyses();
                last_bvh_analysis_update_ = GetTime();
            }
            
            ImGui::Separator();
            
            // List of registered BVH structures
            auto registered_names = BVHReportManager::GetRegisteredNames();
            if (!registered_names.empty()) {
                ImGui::Text("Registered BVH Structures:");
                for (const auto& name : registered_names) {
                    if (ImGui::Selectable(name.c_str(), selected_bvh_for_analysis_ == name)) {
                        selected_bvh_for_analysis_ = name;
                    }
                }
                
                ImGui::Separator();
                
                // Show quick stats for TLAS
                const TLASAnalysis* tlas_analysis = BVHReportManager::GetTLASAnalysis("Main TLAS");
                if (tlas_analysis) {
                    ImGui::Text("TLAS Quick Stats:");
                    ImGui::Text("Quality Score: %.1f/100", tlas_analysis->tlas_quality_score);
                    ImGui::Text("Instances: %u", tlas_analysis->total_instances);
                    ImGui::Text("TLAS Nodes: %u", tlas_analysis->tlas_nodes);
                    ImGui::Text("Max Depth: %u", tlas_analysis->max_tlas_depth);
                    ImGui::Text("Balance Factor: %.3f", tlas_analysis->tlas_balance_factor);
                    
                    // Color-code quality
                    if (tlas_analysis->tlas_quality_score >= 80) {
                        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Status: EXCELLENT");
                    } else if (tlas_analysis->tlas_quality_score >= 60) {
                        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Status: GOOD");
                    } else if (tlas_analysis->tlas_quality_score >= 40) {
                        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Status: FAIR");
                    } else {
                        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Status: POOR");
                    }
                }
                
                ImGui::Separator();
                
                // BLAS statistics summary
                ImGui::Text("BLAS Manager Stats:");
                ImGui::Text("Active BLAS: %d", blas_manager_->get_unique_blas_count());
                ImGui::Text("Total Triangles: %d", blas_manager_->get_total_triangle_count());
                
                // Register any new BLAS for analysis
                if (ImGui::Button("Register Current BLAS")) {
                    // This would need access to individual BLAS structures
                    // For now, we'll focus on TLAS analysis
                    ImGui::Text("(BLAS registration needs individual mesh access)");
                }
                
            } else {
                ImGui::Text("No BVH structures registered.");
                ImGui::Text("Analysis will begin after scene setup.");
            }
        }
        ImGui::End();
        
        // Detailed BVH Analysis Window (popup) - positioned center-right
        if (show_bvh_analysis_window_) {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 650, 100), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(620, 700), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Detailed BVH Analysis", &show_bvh_analysis_window_)) {
                
                if (!selected_bvh_for_analysis_.empty()) {
                    ImGui::Text("Analysis for: %s", selected_bvh_for_analysis_.c_str());
                    ImGui::Separator();
                    
                    // Show detailed TLAS analysis
                    const TLASAnalysis* tlas_analysis = BVHReportManager::GetTLASAnalysis("Main TLAS");
                    if (tlas_analysis && selected_bvh_for_analysis_.find("TLAS") != std::string::npos) {
                        
                        ImGui::Text("=== TLAS DETAILED ANALYSIS ===");
                        
                        // Quality metrics
                        ImGui::Text("Overall Quality Score: %.2f/100", tlas_analysis->tlas_quality_score);
                        ImGui::ProgressBar(tlas_analysis->tlas_quality_score / 100.0f);
                        
                        // Structure metrics
                        ImGui::Text("Structure Metrics:");
                        ImGui::Indent();
                        ImGui::Text("Total Instances: %u", tlas_analysis->total_instances);
                        ImGui::Text("TLAS Nodes: %u", tlas_analysis->tlas_nodes);
                        ImGui::Text("Max TLAS Depth: %u", tlas_analysis->max_tlas_depth);
                        ImGui::Text("Balance Factor: %.3f", tlas_analysis->tlas_balance_factor);
                        ImGui::Text("Surface Area: %.2f", tlas_analysis->tlas_surface_area);
                        ImGui::Unindent();
                        
                        // Performance metrics
                        ImGui::Text("Performance Metrics:");
                        ImGui::Indent();
                        ImGui::Text("Avg Instance Triangles: %.1f", tlas_analysis->avg_instance_triangles);
                        ImGui::Text("Instance Distribution Variance: %.2f", tlas_analysis->instance_distribution_variance);
                        ImGui::Text("Analysis Time: %.3f ms", tlas_analysis->total_analysis_time_ms);
                        ImGui::Unindent();
                        
                        // Issues and recommendations
                        if (!tlas_analysis->tlas_issues.empty()) {
                            ImGui::Text("Issues:");
                            ImGui::Indent();
                            for (const auto& issue : tlas_analysis->tlas_issues) {
                                ImGui::BulletText("%s", issue.c_str());
                            }
                            ImGui::Unindent();
                        }
                        
                        if (!tlas_analysis->tlas_recommendations.empty()) {
                            ImGui::Text("Recommendations:");
                            ImGui::Indent();
                            for (const auto& rec : tlas_analysis->tlas_recommendations) {
                                ImGui::BulletText("%s", rec.c_str());
                            }
                            ImGui::Unindent();
                        }
                        
                        // Generate text report button
                        if (ImGui::Button("Generate Full Text Report")) {
                            std::string report = BVHReportManager::GenerateFullReport();
                            printf("%s", report.c_str());
                        }
                    }
                    
                } else {
                    ImGui::Text("Select a BVH structure from the main analysis window.");
                }
            }
            ImGui::End();
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
    std::unique_ptr<CellDebugRenderer> cell_debug_renderer_;
    
    // Scene geometry BLAS handles
    BLASHandle sphere_blas_;
    BLASHandle ground_blas_;
    
    
    Camera camera_;
    Shader raytracing_shader_{};
    bool cursor_disabled_ = false;
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
    
    // BVH Analysis
    bool show_bvh_analysis_window_ = false;
    bool auto_update_bvh_analysis_ = true;
    std::string selected_bvh_for_analysis_;
    float last_bvh_analysis_update_ = 0.0f;

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