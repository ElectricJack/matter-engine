#include "../include/bvh_visualizer.hpp"
#include "../include/profiler.hpp"
#include <cmath>
#include <algorithm>

BVHVisualizer::BVHVisualizer() {
    // Initialize default settings
    default_settings_.show_blas_bvh = true;
    default_settings_.show_tlas_bvh = true;
    default_settings_.show_leaf_nodes = true;
    default_settings_.show_interior_nodes = true;
    default_settings_.use_depth_colors = true;
    default_settings_.show_triangles = false;
    default_settings_.max_depth_to_show = 8;
    default_settings_.wireframe_thickness = 1.0f;
    default_settings_.blas_color = GREEN;
    default_settings_.tlas_color = BLUE;
    default_settings_.leaf_color = RED;
    default_settings_.triangle_color = WHITE;
}

void BVHVisualizer::render(const BLASManager& blas_manager, 
                          const TLASManager& tlas_manager,
                          const VisualizationSettings& settings) {
    PROFILE_SECTION("BVH Visualization");
    
    if (settings.show_blas_bvh) {
        render_blas_bvh(blas_manager, settings);
    }
    
    if (settings.show_tlas_bvh) {
        render_tlas_bvh(tlas_manager, settings);
    }
}

void BVHVisualizer::render(const BLASManager& blas_manager, 
                          const TLASManager& tlas_manager) {
    render(blas_manager, tlas_manager, default_settings_);
}

void BVHVisualizer::render_blas_bvh(const BLASManager& blas_manager, 
                                   const VisualizationSettings& settings) {
    // Get all BLAS entries and render their BVH structures
    const auto& entries = blas_manager.get_entries();
    
    for (const auto& entry : entries) {
        if (!entry || !entry->bvh) continue;
        
        const Tmpl8::BVH* bvh = entry->bvh.get();
        if (!bvh->bvhNode || bvh->nodesUsed == 0) continue;
        
        // Render this BLAS BVH starting from root node
        render_bvh_node_recursive(bvh->bvhNode, 0, 0, settings, settings.blas_color);
        
        // Optionally render triangles
        if (settings.show_triangles && entry->mesh) {
            for (int i = 0; i < entry->mesh->triCount; i++) {
                draw_triangle_wireframe(entry->mesh->tri[i], 
                                      settings.triangle_color, 
                                      settings.wireframe_thickness);
            }
        }
    }
}

void BVHVisualizer::render_tlas_bvh(const TLASManager& tlas_manager, 
                                   const VisualizationSettings& settings) {
    // Get TLAS and render its structure
    const auto* tlas = tlas_manager.get_tlas();
    if (!tlas || !tlas->tlasNode || tlas->nodesUsed == 0) return;
    
    // Render TLAS BVH starting from root node
    render_tlas_node_recursive(tlas->tlasNode, 0, 0, settings, settings.tlas_color);
}

void BVHVisualizer::render_bvh_node_recursive(const Tmpl8::BVHNode* nodes, 
                                             int node_index, 
                                             int depth,
                                             const VisualizationSettings& settings,
                                             Color base_color) {
    if (depth > settings.max_depth_to_show) return;
    
    const Tmpl8::BVHNode& node = nodes[node_index];
    
    // Determine if we should show this node
    bool is_leaf = node.isLeaf();
    if ((is_leaf && !settings.show_leaf_nodes) || 
        (!is_leaf && !settings.show_interior_nodes)) {
        return;
    }
    
    // Choose color
    Color node_color = base_color;
    if (settings.use_depth_colors) {
        node_color = get_depth_color(depth, settings.max_depth_to_show);
    } else if (is_leaf) {
        node_color = settings.leaf_color;
    }
    
    // Make interior nodes more transparent
    if (!is_leaf) {
        node_color.a = static_cast<unsigned char>(node_color.a * 0.3f);
    }
    
    // Draw the AABB wireframe
    Vector3 min_pos = float3_to_vector3(node.aabbMin);
    Vector3 max_pos = float3_to_vector3(node.aabbMax);
    draw_aabb_wireframe(min_pos, max_pos, node_color, settings.wireframe_thickness);
    
    // Recursively render children if this is an interior node
    if (!is_leaf) {
        int left_child = node.leftFirst;
        int right_child = node.leftFirst + 1;
        
        render_bvh_node_recursive(nodes, left_child, depth + 1, settings, base_color);
        render_bvh_node_recursive(nodes, right_child, depth + 1, settings, base_color);
    }
}

void BVHVisualizer::render_tlas_node_recursive(const Tmpl8::TLASNode* nodes,
                                              int node_index,
                                              int depth, 
                                              const VisualizationSettings& settings,
                                              Color base_color) {
    if (depth > settings.max_depth_to_show) return;
    
    const Tmpl8::TLASNode& node = nodes[node_index];
    
    // Determine if we should show this node
    bool is_leaf = node.isLeaf();
    if ((is_leaf && !settings.show_leaf_nodes) || 
        (!is_leaf && !settings.show_interior_nodes)) {
        return;
    }
    
    // Choose color
    Color node_color = base_color;
    if (settings.use_depth_colors) {
        node_color = get_depth_color(depth, settings.max_depth_to_show);
    } else if (is_leaf) {
        node_color = settings.leaf_color;
    }
    
    // Make interior nodes more transparent
    if (!is_leaf) {
        node_color.a = static_cast<unsigned char>(node_color.a * 0.4f);
    }
    
    // Draw the AABB wireframe
    Vector3 min_pos = float3_to_vector3(node.aabbMin);
    Vector3 max_pos = float3_to_vector3(node.aabbMax);
    draw_aabb_wireframe(min_pos, max_pos, node_color, settings.wireframe_thickness + 1.0f);
    
    // Recursively render children if this is an interior node
    if (!is_leaf) {
        uint leftChild = node.leftRight & 0xFFFF;
        uint rightChild = (node.leftRight >> 16) & 0xFFFF;
        
        render_tlas_node_recursive(nodes, leftChild, depth + 1, settings, base_color);
        render_tlas_node_recursive(nodes, rightChild, depth + 1, settings, base_color);
    }
}

void BVHVisualizer::draw_aabb_wireframe(Vector3 min_pos, Vector3 max_pos, Color color, float thickness) {
    (void)thickness; // Suppress unused parameter warning
    
    // Draw 12 edges of the AABB as lines
    
    // Bottom face (4 edges)
    DrawLine3D({min_pos.x, min_pos.y, min_pos.z}, {max_pos.x, min_pos.y, min_pos.z}, color);
    DrawLine3D({max_pos.x, min_pos.y, min_pos.z}, {max_pos.x, min_pos.y, max_pos.z}, color);
    DrawLine3D({max_pos.x, min_pos.y, max_pos.z}, {min_pos.x, min_pos.y, max_pos.z}, color);
    DrawLine3D({min_pos.x, min_pos.y, max_pos.z}, {min_pos.x, min_pos.y, min_pos.z}, color);
    
    // Top face (4 edges)
    DrawLine3D({min_pos.x, max_pos.y, min_pos.z}, {max_pos.x, max_pos.y, min_pos.z}, color);
    DrawLine3D({max_pos.x, max_pos.y, min_pos.z}, {max_pos.x, max_pos.y, max_pos.z}, color);
    DrawLine3D({max_pos.x, max_pos.y, max_pos.z}, {min_pos.x, max_pos.y, max_pos.z}, color);
    DrawLine3D({min_pos.x, max_pos.y, max_pos.z}, {min_pos.x, max_pos.y, min_pos.z}, color);
    
    // Vertical edges (4 edges)
    DrawLine3D({min_pos.x, min_pos.y, min_pos.z}, {min_pos.x, max_pos.y, min_pos.z}, color);
    DrawLine3D({max_pos.x, min_pos.y, min_pos.z}, {max_pos.x, max_pos.y, min_pos.z}, color);
    DrawLine3D({max_pos.x, min_pos.y, max_pos.z}, {max_pos.x, max_pos.y, max_pos.z}, color);
    DrawLine3D({min_pos.x, min_pos.y, max_pos.z}, {min_pos.x, max_pos.y, max_pos.z}, color);
}

void BVHVisualizer::draw_triangle_wireframe(const Tmpl8::Tri& triangle, Color color, float thickness) {
    (void)thickness; // Suppress unused parameter warning
    
    Vector3 v0 = float3_to_vector3(triangle.vertex0);
    Vector3 v1 = float3_to_vector3(triangle.vertex1);
    Vector3 v2 = float3_to_vector3(triangle.vertex2);
    
    DrawLine3D(v0, v1, color);
    DrawLine3D(v1, v2, color);
    DrawLine3D(v2, v0, color);
}

Color BVHVisualizer::get_depth_color(int depth, int max_depth) {
    if (max_depth <= 0) return WHITE;
    
    float t = static_cast<float>(depth) / static_cast<float>(max_depth);
    t = (t < 0.0f) ? 0.0f : (t > 1.0f) ? 1.0f : t;
    
    // Create a rainbow effect from red (depth 0) to violet (max depth)
    float hue = t * 300.0f; // 0 to 300 degrees (red to violet)
    
    // Convert HSV to RGB (simplified)
    float c = 1.0f; // saturation = 1, value = 1
    float x = c * (1.0f - std::abs(fmod(hue / 60.0f, 2.0f) - 1.0f));
    float m = 0.0f;
    
    float r, g, b;
    if (hue < 60.0f) {
        r = c; g = x; b = 0;
    } else if (hue < 120.0f) {
        r = x; g = c; b = 0;
    } else if (hue < 180.0f) {
        r = 0; g = c; b = x;
    } else if (hue < 240.0f) {
        r = 0; g = x; b = c;
    } else if (hue < 300.0f) {
        r = x; g = 0; b = c;
    } else {
        r = c; g = 0; b = x;
    }
    
    return Color{
        static_cast<unsigned char>((r + m) * 255),
        static_cast<unsigned char>((g + m) * 255),
        static_cast<unsigned char>((b + m) * 255),
        255
    };
}

Color BVHVisualizer::blend_colors(Color base, Color overlay, float alpha) {
    alpha = (alpha < 0.0f) ? 0.0f : (alpha > 1.0f) ? 1.0f : alpha;
    
    return Color{
        static_cast<unsigned char>(base.r * (1.0f - alpha) + overlay.r * alpha),
        static_cast<unsigned char>(base.g * (1.0f - alpha) + overlay.g * alpha),
        static_cast<unsigned char>(base.b * (1.0f - alpha) + overlay.b * alpha),
        static_cast<unsigned char>(base.a * (1.0f - alpha) + overlay.a * alpha)
    };
}

Vector3 BVHVisualizer::float3_to_vector3(const float3& v) {
    return Vector3{v.x, v.y, v.z};
}

float3 BVHVisualizer::vector3_to_float3(const Vector3& v) {
    return make_float3(v.x, v.y, v.z);
}

// Convenience functions
namespace BVHVisualization {
    void render_debug(const BLASManager& blas_manager, 
                     const TLASManager& tlas_manager) {
        static BVHVisualizer visualizer;
        visualizer.render(blas_manager, tlas_manager);
    }
    
    void render_blas_only(const BLASManager& blas_manager) {
        static BVHVisualizer visualizer;
        auto settings = visualizer.get_settings();
        settings.show_tlas_bvh = false;
        settings.show_blas_bvh = true;
        
        static TLASManager dummy_tlas(1);
        visualizer.render(blas_manager, dummy_tlas, settings);
    }
    
    void render_tlas_only(const TLASManager& tlas_manager) {
        static BVHVisualizer visualizer;
        auto settings = visualizer.get_settings();
        settings.show_tlas_bvh = true;
        settings.show_blas_bvh = false;
        
        static BLASManager dummy_blas;
        visualizer.render(dummy_blas, tlas_manager, settings);
    }
}