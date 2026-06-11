#pragma once

extern "C" {
    #include "raylib.h"
}

#include "bvh.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include <vector>
#include <string>

// BVH Visualization system for debugging and understanding acceleration structures
class BVHVisualizer {
public:
    struct VisualizationSettings {
        bool show_blas_bvh = true;           // Show BLAS (bottom-level) BVH nodes
        bool show_tlas_bvh = true;           // Show TLAS (top-level) BVH nodes
        bool show_leaf_nodes = true;         // Show leaf nodes
        bool show_interior_nodes = true;    // Show interior nodes
        bool use_depth_colors = true;       // Color nodes by depth
        bool show_triangles = false;        // Show actual triangle wireframes
        int max_depth_to_show = 10;         // Maximum depth to visualize
        float wireframe_thickness = 1.0f;   // Line thickness
        Color blas_color = GREEN;            // Color for BLAS nodes
        Color tlas_color = BLUE;             // Color for TLAS nodes
        Color leaf_color = RED;              // Color for leaf nodes
        Color triangle_color = WHITE;        // Color for triangles
        std::string selected_bvh_filter = "";  // Filter to show only specific BVH (empty = show all)
    };

    BVHVisualizer();
    ~BVHVisualizer() = default;

    // Main rendering function
    void render(const BLASManager& blas_manager, 
                const TLASManager& tlas_manager,
                const VisualizationSettings& settings);
    
    // Convenience method with default settings
    void render(const BLASManager& blas_manager, 
                const TLASManager& tlas_manager);

    // Individual rendering functions
    void render_blas_bvh(const BLASManager& blas_manager, 
                        const VisualizationSettings& settings);
    void render_blas_bvh_transformed(const BLASManager& blas_manager,
                                   const TLASManager& tlas_manager,
                                   const VisualizationSettings& settings);
    void render_tlas_bvh(const TLASManager& tlas_manager, 
                        const VisualizationSettings& settings);

    // Settings management
    VisualizationSettings& get_settings() { return default_settings_; }
    const VisualizationSettings& get_settings() const { return default_settings_; }

private:
    // Helper functions for rendering BVH nodes
    void render_bvh_node_recursive(const BVHNode* nodes, 
                                  int node_index, 
                                  int depth,
                                  const VisualizationSettings& settings,
                                  Color base_color);
    void render_bvh_node_recursive_transformed(const BVHNode* nodes, 
                                             int node_index, 
                                             int depth,
                                             const VisualizationSettings& settings,
                                             Color base_color,
                                             const TLASManager::DrawRecord& transform);

    void render_tlas_node_recursive(const TLASNode* nodes,
                                   int node_index,
                                   int depth, 
                                   const VisualizationSettings& settings,
                                   Color base_color);

    // Utility functions
    void draw_aabb_wireframe(Vector3 min_pos, Vector3 max_pos, Color color, float thickness = 1.0f);
    void draw_aabb_wireframe_transformed(Vector3 min_pos, Vector3 max_pos, Color color, 
                                       const TLASManager::DrawRecord& transform, float thickness = 1.0f);
    void draw_triangle_wireframe(const Tri& triangle, Color color, float thickness = 1.0f);
    void draw_triangle_wireframe_transformed(const Tri& triangle, Color color, 
                                           const TLASManager::DrawRecord& transform, float thickness = 1.0f);
    Color get_depth_color(int depth, int max_depth);
    Color blend_colors(Color base, Color overlay, float alpha);

    // Convert between math types
    Vector3 float3_to_vector3(const float3& v);
    float3 vector3_to_float3(const Vector3& v);
    Vector3 transform_point(const Vector3& point, const TLASManager::DrawRecord& transform);

    VisualizationSettings default_settings_;
};

// Convenience functions for quick visualization
namespace BVHVisualization {
    // Simple function to visualize BVH with default settings
    void render_debug(const BLASManager& blas_manager, 
                     const TLASManager& tlas_manager);
    
    // Render just BLAS or TLAS
    void render_blas_only(const BLASManager& blas_manager);
    void render_tlas_only(const TLASManager& tlas_manager);
}