extern "C" {
    #include "raylib.h"
    #include "rlgl.h"
}

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "include/blas_manager.hpp"
#include "include/tlas_manager.hpp"
#include "include/profiler.hpp"
#include "include/bvh_visualizer.hpp"

class RayTracingDemo {
public:
    RayTracingDemo(int width, int height, bool debug_mode = false) 
        : screen_width_(width), screen_height_(height),
          debug_mode_(debug_mode), debug_frame_count_(0),
          animation_time_(0.0f), animate_scenes_(true),
          blas_manager_(std::make_unique<BLASManager>()),
          tlas_manager_(std::make_unique<TLASManager>(50)),
          bvh_visualizer_(std::make_unique<BVHVisualizer>()) {
        
        PROFILE_SECTION("Demo Initialization");
        
        InitWindow(screen_width_, screen_height_, "C++ Modular BLAS/TLAS with Performance Profiling");
        SetTargetFPS(120);
        
        // Disable cursor for first person camera control (hides cursor and captures mouse)
        DisableCursor();
        
        setup_scene();
        setup_rendering();
        
        printf("=== C++ Modular BLAS/TLAS System Initialized ===\n");
        printf("Animation enabled - TLAS will rebuild every frame\n");
    }
    
    ~RayTracingDemo() {
        cleanup();
        EnableCursor(); // Restore cursor functionality before closing
        CloseWindow();
    }
    
    void run() {
        int frame_count = 0;
        
        if (debug_mode_) {
            printf("=== DEBUG MODE: Will auto-quit after 60 frames ===\n");
            printf("DEBUG MODE: Setting up unit test scene\n");
            setup_unit_test_scene();
        }
        
        while (!WindowShouldClose()) {
            PROFILE_FRAME_BEGIN();
            
            frame_count++;
            debug_frame_count_ = frame_count;
            
            // Debug mode auto-quit
            if (debug_mode_ && frame_count >= 60) {
                printf("DEBUG MODE: Reached 60 frames, auto-quitting...\n");
                break;
            }
            
            // // Print simple frame count to check if we're making progress
            // if (frame_count % 30 == 0 || debug_mode_) {
            //     printf("Frame %d...\n", frame_count);
            // }
            
            // // Print performance stats every 60 frames (roughly every second at 60 FPS)
            // if (frame_count % 60 == 0) {
            //     PROFILE_PRINT();
            // }
            
            // // Reset stats every 5 seconds to show current performance
            // if (frame_count % 300 == 0) {
            //     printf("\n--- Performance Reset ---\n");
            //     PROFILE_RESET();
            // }
            
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
        
        printf("=== BLAS Registration Phase ===\n");
        
        // Register different geometry types using factory functions
        cube_blas_ = BLASFactory::register_cube(*blas_manager_, 1.0f);
        printf("  Cube BLAS registered: handle=%u\n", cube_blas_);
        
        sphere_blas_ = BLASFactory::register_sphere(*blas_manager_, 0.5f, 32, 16);
        printf("  Sphere BLAS registered: handle=%u\n", sphere_blas_);
        
        ground_blas_ = BLASFactory::register_plane(*blas_manager_, 200.0f, 200.0f);
        printf("  Ground BLAS registered: handle=%u\n", ground_blas_);
        
        printf("=== BLAS Statistics After Registration ===\n");
        blas_manager_->print_stats();
        
        printf("=== Setting Up Initial Test Scene ===\n");
        setup_test_scene(current_test_scene_);
        
        printf("=== Final Manager Statistics ===\n");
        blas_manager_->print_stats();
        tlas_manager_->print_stats();
    }
    
    void setup_test_scene(int test_number, float time = 0.0f) {
        PROFILE_SECTION("Test Scene Setup");
        
        // Clear previous scene
        tlas_manager_->clear();
        int smooth_normals_offset = 1000000;
        
        switch (test_number) {
            case 1: {
                // Ground plane
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -2.0f, 0.0f);
                tlas_manager_->scale(200.0f,0.1f,200.0f);
                tlas_manager_->draw(cube_blas_, 2);

                // Animated cube - rotating and bouncing
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, std::sin(time * 2.0f) * 0.5f, 0.0f);
                tlas_manager_->rotate_y(time);
                tlas_manager_->draw(cube_blas_, 0); // Red cube
                break;
            }
            
            case 2: {
                // Ground plane
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -2.0f, 0.0f);
                tlas_manager_->scale(200.0f,0.1f,200.0f);
                tlas_manager_->draw(cube_blas_, 2);

                // Animated sphere - figure-8 motion
                float x = std::sin(time) * 2.0f;
                float z = std::sin(time * 2.0f) * 1.0f;
                tlas_manager_->load_identity();
                tlas_manager_->translate(x, 0.0f, z);
                tlas_manager_->draw(sphere_blas_, smooth_normals_offset + 1); // Blue sphere
                break;
            }
            
            case 3: {
                // Ground plane positioned well below objects
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -2.0f, 0.0f);
                tlas_manager_->scale(200.0f,0.1f,200.0f);
                tlas_manager_->draw(cube_blas_, 2);

                // Orbiting cubes around center
                float orbit_radius = 2.0f;
                for (int i = 0; i < 2; i++) {
                    float angle = time + i * M_PI;
                    float x = std::cos(angle) * orbit_radius;
                    float z = std::sin(angle) * orbit_radius;
                    
                    tlas_manager_->load_identity();
                    tlas_manager_->translate(x, 1.0f, z);
                    tlas_manager_->rotate_y(time * 2.0f);
                    tlas_manager_->draw(cube_blas_, i); // Red and blue cubes
                }
                
                // Central sphere scaling up and down
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, 2.0f, 0.0f);
                float scale_factor = 1.0f + std::sin(time * 3.0f) * 0.5f;
                tlas_manager_->scale(scale_factor);
                tlas_manager_->draw(sphere_blas_, smooth_normals_offset + 3); // Gold sphere
                break;
            }
            
            case 4: {
                // Ground plane
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -2.0f, 0.0f);
                tlas_manager_->scale(200.0f,0.1f,200.0f);
                tlas_manager_->draw(cube_blas_, 2);
                
                // Four cubes in a square formation - rotating around center
                float positions[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f}};
                for (int i = 0; i < 4; i++) {
                    // Rotate the entire formation
                    float angle = time * 0.5f;
                    float x = positions[i][0] * std::cos(angle) - positions[i][1] * std::sin(angle);
                    float z = positions[i][0] * std::sin(angle) + positions[i][1] * std::cos(angle);
                    
                    tlas_manager_->load_identity();
                    tlas_manager_->translate(x, std::sin(time + i) * 0.3f, z);
                    tlas_manager_->rotate_y(time * 2.0f + i * M_PI / 2.0f);
                    tlas_manager_->draw(cube_blas_, i % 5);
                }
                break;
            }
            
            case 5: {
                // Ground plane
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -2.0f, 0.0f);
                tlas_manager_->scale(200.0f,0.1f,200.0f);
                tlas_manager_->draw(cube_blas_, 2);
                
                // Four cubes with wave motion
                float positions[4][2] = {{-1.5f, -1.5f}, {1.5f, -1.5f}, {-1.5f, 1.5f}, {1.5f, 1.5f}};
                for (int i = 0; i < 4; i++) {
                    float wave_offset = i * M_PI / 2.0f;
                    float y = std::sin(time * 2.0f + wave_offset) * 1.0f;
                    
                    tlas_manager_->load_identity();
                    tlas_manager_->translate(positions[i][0], y, positions[i][1]);
                    tlas_manager_->draw(cube_blas_, i);
                }
                
                // Central sphere with complex motion
                float sphere_x = std::cos(time * 0.7f) * 0.5f;
                float sphere_y = 3.0f + std::sin(time * 1.5f) * 1.0f;
                float sphere_z = std::sin(time * 0.7f) * 0.5f;
                float sphere_scale = 2.0f + std::sin(time * 2.0f) * 0.5f;
                
                tlas_manager_->load_identity();
                tlas_manager_->translate(sphere_x, sphere_y, sphere_z);
                tlas_manager_->scale(sphere_scale);
                tlas_manager_->draw(sphere_blas_, smooth_normals_offset + 4);
                break;
            }
            
            case 6: {
                // Ground plane
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -2.0f, 0.0f);
                tlas_manager_->scale(200.0f,0.1f,200.0f);
                tlas_manager_->draw(cube_blas_, 2);
                
                // Central sphere pulsing
                float scale = 1.0f + std::sin(time * 4.0f) * 0.3f;
                tlas_manager_->load_identity();
                tlas_manager_->scale(scale);
                tlas_manager_->draw(sphere_blas_, smooth_normals_offset + 4);
                
                // Animated circle of cubes - changing radius and rotation
                float base_radius = 2.5f + std::sin(time) * 0.5f;
                int cube_count = 6;
                for (int i = 0; i < cube_count; i++) {
                    float angle = (time * 0.8f) + (i * 2.0f * M_PI / cube_count);
                    float x = std::cos(angle) * base_radius;
                    float z = std::sin(angle) * base_radius;
                    float y = std::sin(time * 2.0f + i * 0.5f) * 0.5f;
                    
                    tlas_manager_->load_identity();
                    tlas_manager_->translate(x, y, z);
                    tlas_manager_->rotate_y(time * 3.0f + i);
                    tlas_manager_->draw(cube_blas_, i % 5);
                }
                break;
            }
            
            case 7: {
                // Ground plane
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -2.0f, 0.0f);
                tlas_manager_->scale(200.0f,0.1f,200.0f);
                tlas_manager_->draw(cube_blas_, 2);
                
                // 3x3 grid with wave motion
                for (int x = -1; x <= 1; x++) {
                    for (int z = -1; z <= 1; z++) {
                        float wave_x = std::sin(time + x * 0.5f) * 0.3f;
                        float wave_z = std::cos(time + z * 0.5f) * 0.3f;
                        float wave_y = std::sin(time * 2.0f + x * 0.8f + z * 0.8f) * 0.8f;
                        
                        tlas_manager_->load_identity();
                        tlas_manager_->translate(x * 2.0f + wave_x, wave_y, z * 2.0f + wave_z);
                        
                        if ((x + z) % 2 == 0) {
                            tlas_manager_->rotate_y(time + x + z);
                            tlas_manager_->draw(cube_blas_, (x + 1) + (z + 1) * 3);
                        } else {
                            float scale = 1.0f + std::sin(time * 3.0f + x + z) * 0.2f;
                            tlas_manager_->scale(scale);
                            tlas_manager_->draw(sphere_blas_, smooth_normals_offset + (x + 1) + (z + 1) * 3);
                        }
                    }
                }
                break;
            }
            
            case 8: {
                // Ground plane
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -2.0f, 0.0f);
                tlas_manager_->scale(200.0f,0.1f,200.0f);
                tlas_manager_->draw(cube_blas_, 2);
                
                // Ground level objects - rotating ring
                int ground_count = 8;
                float ground_radius = 3.0f;
                for (int i = 0; i < ground_count; i++) {
                    float angle = (time * 0.6f) + (i * 2.0f * M_PI / ground_count);
                    float x = std::cos(angle) * ground_radius;
                    float z = std::sin(angle) * ground_radius;
                    
                    tlas_manager_->load_identity();
                    tlas_manager_->translate(x, 0.0f, z);
                    tlas_manager_->rotate_y(time * 2.0f + i);
                    tlas_manager_->draw(cube_blas_, i % 5);
                }
                
                // Mid level spheres - counter-rotating
                for (int i = 0; i < 4; i++) {
                    float angle = -time * 0.8f + i * M_PI / 2.0f;
                    float x = std::cos(angle) * 1.5f;
                    float z = std::sin(angle) * 1.5f;
                    float y = 2.0f + std::sin(time * 2.0f + i) * 0.3f;
                    
                    tlas_manager_->load_identity();
                    tlas_manager_->translate(x, y, z);
                    tlas_manager_->draw(sphere_blas_, smooth_normals_offset + i + 1);
                }
                
                // Top level central cube - complex motion
                float top_y = 4.0f + std::sin(time * 1.2f) * 0.5f;
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, top_y, 0.0f);
                tlas_manager_->rotate_y(time * 1.5f);
                tlas_manager_->rotate_x(time * 0.7f);
                tlas_manager_->draw(cube_blas_, 4);
                break;
            }
            
            case 9: {
                // Ground plane
                tlas_manager_->load_identity();
                tlas_manager_->translate(0.0f, -2.0f, 0.0f);
                tlas_manager_->scale(200.0f,0.1f,200.0f);
                tlas_manager_->draw(cube_blas_, 2);
                
                // Central cluster with orbital motion
                float central_orbit = std::cos(time * 0.3f) * 0.5f;
                tlas_manager_->load_identity();
                tlas_manager_->translate(central_orbit, 0.0f, 0.0f);
                tlas_manager_->rotate_y(time);
                tlas_manager_->draw(cube_blas_, 0);
                
                tlas_manager_->load_identity();
                tlas_manager_->translate(-central_orbit, 2.0f + std::sin(time * 2.0f) * 0.3f, 0.0f);
                tlas_manager_->draw(sphere_blas_, smooth_normals_offset + 1);
                
                // First circle - rotating and expanding/contracting
                tlas_manager_->push_matrix();
                tlas_manager_->translate(0.0f, 0.0f, 0.0f);
                float circle1_radius = 2.0f + std::sin(time * 0.8f) * 0.5f;
                int circle1_count = 8;
                for (int i = 0; i < circle1_count; i++) {
                    float angle = (time * 0.7f) + (i * 2.0f * M_PI / circle1_count);
                    float x = std::cos(angle) * circle1_radius;
                    float z = std::sin(angle) * circle1_radius;
                    
                    tlas_manager_->push_matrix();
                    tlas_manager_->translate(x, 0.0f, z);
                    tlas_manager_->rotate_y(time * 2.0f + i);
                    tlas_manager_->draw(cube_blas_, i % 5);
                    tlas_manager_->pop_matrix();
                }
                tlas_manager_->pop_matrix();
                
                // Second circle - counter-rotating spheres
                tlas_manager_->push_matrix();
                tlas_manager_->translate(0.0f, 1.5f, 0.0f);
                float circle2_radius = 3.5f + std::cos(time * 1.2f) * 0.8f;
                int circle2_count = 6;
                for (int i = 0; i < circle2_count; i++) {
                    float angle = (-time * 1.1f) + (i * 2.0f * M_PI / circle2_count);
                    float x = std::cos(angle) * circle2_radius;
                    float z = std::sin(angle) * circle2_radius;
                    float scale = 1.0f + std::sin(time * 3.0f + i) * 0.3f;
                    
                    tlas_manager_->push_matrix();
                    tlas_manager_->translate(x, 0.0f, z);
                    tlas_manager_->scale(scale);
                    tlas_manager_->draw(sphere_blas_, smooth_normals_offset + i + 2);
                    tlas_manager_->pop_matrix();
                }
                tlas_manager_->pop_matrix();
                
                // Grid of floating objects with wave motion
                tlas_manager_->push_matrix();
                tlas_manager_->translate(-8.0f, 1.0f, -8.0f);
                for (int gx = 0; gx < 4; gx++) {
                    for (int gz = 0; gz < 4; gz++) {
                        float wave_y = std::sin(time * 1.5f + gx * 0.5f + gz * 0.5f) * 0.8f;
                        tlas_manager_->push_matrix();
                        tlas_manager_->translate(gx * 2.0f, wave_y, gz * 2.0f);
                        tlas_manager_->rotate_y(time + gx + gz);
                        tlas_manager_->draw(cube_blas_, (gx + gz) % 5);
                        tlas_manager_->pop_matrix();
                    }
                }
                tlas_manager_->pop_matrix();
                
                // Scattered orbiting spheres
                for (int i = 0; i < 6; i++) {
                    float orbit_angle = time * 0.4f + i * M_PI / 3.0f;
                    float orbit_radius = 6.0f + std::sin(time * 0.6f + i) * 1.0f;
                    float x = std::cos(orbit_angle) * orbit_radius;
                    float z = std::sin(orbit_angle) * orbit_radius;
                    float y = 3.0f + i * 0.5f + std::sin(time * 2.0f + i * 0.7f) * 1.0f;
                    float scale = 0.5f + i * 0.1f + std::cos(time * 3.0f + i) * 0.2f;
                    
                    tlas_manager_->load_identity();
                    tlas_manager_->translate(x, y, z);
                    tlas_manager_->scale(scale);
                    tlas_manager_->draw(sphere_blas_, smooth_normals_offset + i % 5);
                }
                break;
            }
            
            default:
                // Fallback to test 1
                setup_test_scene(1, time);
                return;
        }
        
        // Build TLAS from recorded draw calls
        printf("=== Building TLAS for scene %d ===\n", test_number);
        printf("  Draw records before build: %d\n", tlas_manager_->get_draw_record_count());
        
        tlas_manager_->build(*blas_manager_);
        
        printf("  TLAS built successfully!\n");
        printf("  Final counts: %d nodes, %d instances\n", 
               tlas_manager_->get_node_count(), tlas_manager_->get_instance_count());
        
        printf("Test scene %d setup complete!\n", test_number);
    }
    
    void setup_unit_test_scene() {
        printf("=== Setting Up Unit Test Scene ===\n");
        
        // Clear any existing scene
        tlas_manager_->clear();
        
        // Create a simple test geometry with just 3 triangles, well separated
        std::vector<Tri> test_triangles;
        
        // Triangle 1: At origin, facing up (Z+)
        Tri tri1;
        tri1.vertex0 = make_float3(-0.5f, 0.0f, 0.0f);
        tri1.vertex1 = make_float3( 0.5f, 0.0f, 0.0f);
        tri1.vertex2 = make_float3( 0.0f, 0.0f, 1.0f);
        tri1.centroid = make_float3(0.0f, 0.0f, 0.33f);
        test_triangles.push_back(tri1);
        
        // Triangle 2: To the right, at X=5
        Tri tri2;
        tri2.vertex0 = make_float3(4.5f, 0.0f, 0.0f);
        tri2.vertex1 = make_float3(5.5f, 0.0f, 0.0f);
        tri2.vertex2 = make_float3(5.0f, 0.0f, 1.0f);
        tri2.centroid = make_float3(5.0f, 0.0f, 0.33f);
        test_triangles.push_back(tri2);
        
        // Triangle 3: Above, at Y=5
        Tri tri3;
        tri3.vertex0 = make_float3(-0.5f, 5.0f, 0.0f);
        tri3.vertex1 = make_float3( 0.5f, 5.0f, 0.0f);
        tri3.vertex2 = make_float3( 0.0f, 5.0f, 1.0f);
        tri3.centroid = make_float3(0.0f, 5.0f, 0.33f);
        test_triangles.push_back(tri3);
        
        printf("Created 3 test triangles:\n");
        printf("  Triangle 1: Center at (0, 0, 0.33)\n");
        printf("  Triangle 2: Center at (5, 0, 0.33)\n");
        printf("  Triangle 3: Center at (0, 5, 0.33)\n");
        
        // Register the triangles and get the resulting BVH to analyze
        BLASHandle test_blas = blas_manager_->register_triangles(test_triangles);
        printf("Test BLAS registered: handle=%u\n", test_blas);
        
        // Get the BVH and analyze the subdivision
        BVH* test_bvh = blas_manager_->get_bvh(test_blas);
        if (test_bvh) {
            printf("Test BVH built with subdivToOnePrim=%s\n", test_bvh->subdivToOnePrim ? "true" : "false");
            
            printf("=== BVH Structure Analysis ===\n");
            printf("BVH nodes used: %u\n", test_bvh->nodesUsed);
            
            for (uint32_t i = 0; i < test_bvh->nodesUsed; i++) {
                const auto& node = test_bvh->bvhNode[i];
                printf("Node %u: %s\n", i, node.isLeaf() ? "LEAF" : "INTERIOR");
                printf("  AABB: (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f)\n",
                       node.aabbMin.x, node.aabbMin.y, node.aabbMin.z,
                       node.aabbMax.x, node.aabbMax.y, node.aabbMax.z);
                
                if (node.isLeaf()) {
                    printf("  Triangle count: %u, First triangle: %u\n", 
                           node.triCount, node.leftFirst);
                    // Print which triangles are in this leaf
                    for (uint32_t t = 0; t < node.triCount; t++) {
                        uint32_t tri_idx = test_bvh->triIdx[node.leftFirst + t];
                        printf("    Triangle %u: index %u\n", t, tri_idx);
                    }
                } else {
                    printf("  Left child: %u, Right child: %u\n", 
                           node.leftFirst, node.leftFirst + 1);
                }
            }
        }
        
        // Create a TLAS unit test with multiple well-separated instances
        printf("=== Creating TLAS Unit Test ===\n");
        
        // Add the same triangle BLAS at 3 different positions to test TLAS subdivision
        // Instance 1: At origin
        tlas_manager_->load_identity();
        tlas_manager_->draw(test_blas, 0);
        
        // Instance 2: Far to the right at X=10
        tlas_manager_->load_identity();
        tlas_manager_->translate(10.0f, 0.0f, 0.0f);
        tlas_manager_->draw(test_blas, 1);
        
        // Instance 3: Far up at Y=10  
        tlas_manager_->load_identity();
        tlas_manager_->translate(0.0f, 10.0f, 0.0f);
        tlas_manager_->draw(test_blas, 2);
        
        printf("Created 3 TLAS instances at well-separated positions:\n");
        printf("  Instance 1: Transform at (0, 0, 0)\n");
        printf("  Instance 2: Transform at (10, 0, 0)\n");
        printf("  Instance 3: Transform at (0, 10, 0)\n");
        
        // Build TLAS
        printf("=== Building TLAS ===\n");
        tlas_manager_->build(*blas_manager_);
        printf("TLAS built with %d instances, %d nodes\n", 
               tlas_manager_->get_instance_count(), tlas_manager_->get_node_count());
        
        // Analyze TLAS structure  
        printf("=== TLAS Structure Analysis ===\n");
        const TLAS* tlas = tlas_manager_->get_tlas();
        if (tlas) {
            printf("TLAS nodes used: %u\n", tlas->nodesUsed);
            printf("TLAS BLAS count: %u\n", tlas->blasCount);
            
            for (uint32_t i = 0; i < tlas->nodesUsed; i++) {
                const auto& node = tlas->tlasNode[i];
                printf("TLAS Node %u: %s\n", i, node.isLeaf() ? "LEAF" : "INTERIOR");
                printf("  AABB: (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f)\n",
                       node.aabbMin.x, node.aabbMin.y, node.aabbMin.z,
                       node.aabbMax.x, node.aabbMax.y, node.aabbMax.z);
                
                if (node.isLeaf()) {
                    printf("  BLAS index: %u\n", node.BLAS);
                    // Print transform information for this instance
                    if (node.BLAS < tlas->blasCount) {
                        const auto& instance = tlas->blas[node.BLAS];
                        printf("  Instance bounds: (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f)\n",
                               instance.bounds.bmin.x, instance.bounds.bmin.y, instance.bounds.bmin.z,
                               instance.bounds.bmax.x, instance.bounds.bmax.y, instance.bounds.bmax.z);
                    }
                } else {
                    printf("  Left child: %u, Right child: %u\n", 
                           node.leftRight & 0xFFFF, (node.leftRight >> 16) & 0xFFFF);
                }
            }
        }
        
        current_test_scene_ = 0; // Mark as unit test scene
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
        debug_mode_loc_    = GetShaderLocation(raytracing_shader_, "debugMode");
        
        // BLAS/TLAS uniforms are now handled by their respective managers
    }
    
    void update() {
        // Handle input
        handle_input();
        
        // Update animation time
        animation_time_ += GetFrameTime();
        
        // Rebuild scene with animation if enabled and not in unit test mode
        if (animate_scenes_ && current_test_scene_ > 0) {
            PROFILE_SECTION("Animated Scene Update");
            setup_test_scene(current_test_scene_, animation_time_);
        }
        
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
        
        // Toggle rendering mode
        if (IsKeyPressed(KEY_LEFT_SHIFT)) {
            use_raytracing_ = !use_raytracing_ && (raytracing_shader_.id != 0);
            printf("Switched to %s mode\n", use_raytracing_ ? "raytracing" : "rasterization");
        }
        
        // Test scene selection (1-9)
        for (int i = 1; i <= 9; i++) {
            if (IsKeyPressed(KEY_ONE + i - 1)) {
                if (current_test_scene_ != i) {
                    current_test_scene_ = i;
                    printf("Switching to test scene %d...\n", i);
                    if (!animate_scenes_) {
                        setup_test_scene(current_test_scene_);
                        blas_manager_->print_stats();
                        tlas_manager_->print_stats();
                    }
                }
                break;
            }
        }
        
        // Unit test scene
        if (IsKeyPressed(KEY_ZERO)) {
            printf("Loading unit test scene...\n");
            animate_scenes_ = false; // Disable animation for unit test
            setup_unit_test_scene();
        }
        
        // Animation toggle
        if (IsKeyPressed(KEY_N)) {
            animate_scenes_ = !animate_scenes_;
            printf("Animation %s\n", animate_scenes_ ? "enabled" : "disabled");
            if (!animate_scenes_) {
                // Rebuild current scene without animation
                setup_test_scene(current_test_scene_);
            }
        }
        
        // Performance controls
        if (IsKeyPressed(KEY_P)) {
            PROFILE_PRINT();
        }
        if (IsKeyPressed(KEY_X)) {
            printf("Resetting performance statistics...\n");
            PROFILE_RESET();
        }
        
        // BVH visualization toggle
        if (IsKeyPressed(KEY_B)) {
            show_bvh_visualization_ = !show_bvh_visualization_;
            printf("BVH visualization %s\n", show_bvh_visualization_ ? "enabled" : "disabled");
        }
        
        // BVH visualization settings (only work when visualization is enabled)
        if (show_bvh_visualization_) {
            auto& settings = bvh_visualizer_->get_settings();
            
            // Remapped keys to avoid conflict with WASD movement
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
        
        // Debug rendering mode toggle
        if (IsKeyPressed(KEY_M)) {
            render_debug_mode_ = (render_debug_mode_ + 1) % 3;
            const char* mode_names[] = {"Normal rendering", "Show interpolated normals", "Show face normals"};
            printf("Debug mode: %s\n", mode_names[render_debug_mode_]);
        }
    }
    
    void render() {
        PROFILE_SECTION("Frame Render");
        
        BeginDrawing();
        ClearBackground(BLACK);
        
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
        
        // Debug logging every 30 frames or always in debug mode
        bool should_log = (debug_frame_count_ % 30 == 1) || debug_mode_;
        
        if (should_log) {
            printf("=== Raytraced Render Frame %d ===\n", debug_frame_count_);
            printf("  Camera pos: (%.2f, %.2f, %.2f)\n", camera_.position.x, camera_.position.y, camera_.position.z);
            printf("  BLAS triangles: %d\n", blas_manager_->get_total_triangle_count());
            printf("  TLAS instances: %d, nodes: %d\n", 
                   tlas_manager_->get_instance_count(), tlas_manager_->get_node_count());
        }
        
        BeginShaderMode(raytracing_shader_);
        
        // Set shader uniforms
        {
            PROFILE_SECTION("Shader Uniforms");
            
            Vector2 screen_size = {static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())};
            
            if (should_log) {
                printf("  Setting camera uniforms...\n");
            }
            
            // Set camera uniforms
            SetShaderValue(raytracing_shader_, camera_pos_loc_, &camera_.position, SHADER_UNIFORM_VEC3);
            SetShaderValue(raytracing_shader_, camera_target_loc_, &camera_.target, SHADER_UNIFORM_VEC3);
            SetShaderValue(raytracing_shader_, camera_up_loc_, &camera_.up, SHADER_UNIFORM_VEC3);
            SetShaderValue(raytracing_shader_, camera_fovy_loc_, &camera_.fovy, SHADER_UNIFORM_FLOAT);
            SetShaderValue(raytracing_shader_, screen_size_loc_, &screen_size, SHADER_UNIFORM_VEC2);
            SetShaderValue(raytracing_shader_, debug_mode_loc_, &render_debug_mode_, SHADER_UNIFORM_INT);
            
            if (should_log) {
                printf("  Binding BLAS to shader...\n");
            }
            
            // Let managers handle their own shader binding and texture management
            blas_manager_->bind_to_shader(raytracing_shader_);
            
            if (should_log) {
                printf("  Binding TLAS to shader...\n");
            }
            
            tlas_manager_->bind_to_shader(raytracing_shader_, *blas_manager_);
            
            if (should_log) {
                printf("  All uniforms and textures bound.\n");
            }
        }
        
        // Draw fullscreen rectangle
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), WHITE);
        
        EndShaderMode();
    }
    
    void render_rasterized() {
        PROFILE_SECTION("Rasterized Rendering");
        
        BeginMode3D(camera_);
        
        // Render actual scene meshes from TLAS
        render_scene_meshes();
        
        // Draw reference grid
        DrawGrid(20, 1.0f);
        
        // Render BVH visualization if enabled
        if (show_bvh_visualization_) {
            bvh_visualizer_->render(*blas_manager_, *tlas_manager_);
        }
        
        EndMode3D();
    }
    
    void render_scene_meshes() {
        // Render meshes using draw records from TLAS manager to get proper transforms
        const auto& draw_records = tlas_manager_->get_draw_records();
        
        
        for (size_t i = 0; i < draw_records.size(); i++) {
            const auto& record = draw_records[i];
            
            // Get the mesh for this BLAS handle
            auto* mesh = blas_manager_->get_mesh(record.blas_handle);
            if (!mesh || !mesh->tri || mesh->triCount == 0) {
                continue;
            }
            
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
            
            // Render mesh as filled triangles with transparency
            rlBegin(RL_TRIANGLES);
            rlColor4ub(mesh_color.r, mesh_color.g, mesh_color.b, 120); // Semi-transparent
            
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
        if (current_test_scene_ == 0) {
            DrawText("Unit Test Scene (Press 0 for unit test, 1-9 for scenes)", 10, 90, 16, YELLOW);
        } else {
            DrawText(TextFormat("Test Scene %d (Press 0 for unit test, 1-9 to change)", current_test_scene_), 10, 90, 16, LIGHTGRAY);
        }
        
        // Animation status
        if (animate_scenes_ && current_test_scene_ > 0) {
            DrawText("ANIMATION ENABLED - TLAS rebuilds every frame", 10, 110, 16, GREEN);
            DrawText("Press N to toggle animation", 10, 130, 14, LIGHTGRAY);
        } else if (current_test_scene_ > 0) {
            DrawText("Animation disabled - Static scene", 10, 110, 16, YELLOW);
            DrawText("Press N to enable animation", 10, 130, 14, LIGHTGRAY);
        } else {
            DrawText("Unit Test Mode - Animation disabled", 10, 110, 16, LIGHTGRAY);
            DrawText("Press N to toggle animation", 10, 130, 14, LIGHTGRAY);
        }
        
        // Performance info
        double frame_time = Performance::Profiler::instance().get_frame_time_ms();
        DrawText(TextFormat("Frame: %.2f ms (%.1f FPS)", frame_time, 1000.0 / frame_time), 10, 150, 16, LIME);
        
        // Scene stats
        int total_instances_ = tlas_manager_->get_instance_count();
        DrawText(TextFormat("Scene: %d instances, %d triangles", 
                 total_instances_, total_triangles_), 10, 170, 14, LIGHTGRAY);
        
        // BVH visualization info
        if (show_bvh_visualization_) {
            const auto& settings = bvh_visualizer_->get_settings();
            DrawText("BVH VISUALIZATION MODE", 10, 190, 16, YELLOW);
            DrawText("Q:BLAS I:TLAS V:Leaf T:Interior Y:Colors U:Triangles", 10, 210, 12, LIGHTGRAY);
            DrawText("UP/DOWN: Depth | B: Toggle visualization", 10, 225, 12, LIGHTGRAY);
            DrawText(TextFormat("BLAS:%s TLAS:%s Leaf:%s Interior:%s Depth:%d", 
                     settings.show_blas_bvh ? "ON" : "OFF",
                     settings.show_tlas_bvh ? "ON" : "OFF",
                     settings.show_leaf_nodes ? "ON" : "OFF",
                     settings.show_interior_nodes ? "ON" : "OFF", 
                     settings.max_depth_to_show), 10, 240, 12, LIGHTGRAY);
        } else {
            DrawText("Press B to toggle BVH visualization", 10, 190, 14, LIGHTGRAY);
        }
        
        // Animation and performance controls
        DrawText("Controls: N=Animation, P=Performance stats, X=Reset", 10, screen_height_ - 110, 14, LIGHTGRAY);
        DrawText("BVH: B=Toggle visualization", 10, screen_height_ - 90, 14, LIGHTGRAY);
        
        // Debug rendering mode info
        const char* mode_names[] = {"Normal", "Interpolated Normals", "Face Normals"};
        DrawText(TextFormat("Debug: M=Cycle modes (%s)", mode_names[render_debug_mode_]), 10, screen_height_ - 70, 14, LIGHTGRAY);
        
        // Mouse control info
        if (cursor_disabled_) {
            DrawText("Mouse captured for first person camera control (ESC to release)", 10, screen_height_ - 50, 14, LIGHTGRAY);
        } else {
            DrawText("Mouse cursor free (ESC to capture for camera control)", 10, screen_height_ - 50, 14, YELLOW);
        }
        
        // System info
        DrawText("C++ Modular BLAS/TLAS System with Dynamic Animation", 10, screen_height_ - 30, 16, LIGHTGRAY);
        
        DrawFPS(10, 10);
    }
    
    void cleanup() {
        if (raytracing_shader_.id != 0) UnloadShader(raytracing_shader_);
        // Managers clean up their own textures in destructors
    }
    
private:
    // Window settings
    int screen_width_;
    int screen_height_;
    
    // Debug mode
    bool debug_mode_;
    int debug_frame_count_;
    
    // Animation
    float animation_time_;
    bool animate_scenes_;
    
    // Managers
    std::unique_ptr<BLASManager> blas_manager_;
    std::unique_ptr<TLASManager> tlas_manager_;
    std::unique_ptr<BVHVisualizer> bvh_visualizer_;
    
    // BLAS handles
    BLASHandle cube_blas_;
    BLASHandle sphere_blas_;
    BLASHandle ground_blas_;
    
    // Rendering
    Camera camera_;
    Shader raytracing_shader_{};
    bool use_raytracing_ = false;
    int current_test_scene_ = 1;
    bool show_bvh_visualization_ = false;
    bool cursor_disabled_ = true;
    int render_debug_mode_ = 0;  // 0=normal, 1=show normals, 2=show face normals
    
    // GPU textures are now managed by the managers themselves
    
    // Shader uniform locations (camera and scene-level only)
    int camera_pos_loc_;
    int camera_target_loc_;
    int camera_up_loc_;
    int camera_fovy_loc_;
    int screen_size_loc_;
    int debug_mode_loc_;
    

};

int main(int argc, char* argv[]) {
    printf("=== C++ Modular BLAS/TLAS System with Performance Profiling ===\n");
    
    bool debug_mode = false;
    
    // Check for debug flag
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0) {
            debug_mode = true;
            printf("DEBUG MODE ENABLED: Will auto-quit after 60 frames\n");
        }
    }
    
    try {
        RayTracingDemo demo(1280, 800, debug_mode);
        //RayTracingDemo demo(800, 600, debug_mode);
        demo.run();
    } catch (const std::exception& e) {
        printf("Error: %s\n", e.what());
        return 1;
    }
    
    return 0;
}