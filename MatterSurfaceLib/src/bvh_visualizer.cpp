#include "../include/bvh_visualizer.hpp"
#include "../include/profiler.hpp"
#include <cmath>
#include <algorithm>
#include <string>
#include <cstdlib>

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
        render_blas_bvh_transformed(blas_manager, tlas_manager, settings);
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
        
        const BVH* bvh = entry->bvh.get();
        if (!bvh->bvhNode || bvh->nodesUsed == 0) continue;
        
        // Apply BVH name filtering if specified
        if (!settings.selected_bvh_filter.empty()) {
            // Check if this BLAS matches the selected filter by triangle count
            if (entry->mesh && entry->mesh->triCount > 0) {
                // Extract triangle count from filter name if it matches pattern
                std::string filter = settings.selected_bvh_filter;
                size_t tris_pos = filter.find("tris");
                if (tris_pos != std::string::npos) {
                    // Find the number before "tris"
                    size_t num_start = filter.rfind('_', tris_pos);
                    if (num_start != std::string::npos) {
                        std::string tri_count_str = filter.substr(num_start + 1, tris_pos - num_start - 1);
                        int expected_tri_count = std::atoi(tri_count_str.c_str());
                        
                        // Only render if triangle counts match
                        if (entry->mesh->triCount != expected_tri_count) {
                            continue;  // Skip this BLAS, it doesn't match
                        }
                    }
                }
            } else {
                continue;  // Skip if no mesh or no triangles
            }
        }
        
        // Render this BLAS BVH starting from root node (in local space)
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

void BVHVisualizer::render_blas_bvh_transformed(const BLASManager& blas_manager,
                                               const TLASManager& tlas_manager,
                                               const VisualizationSettings& settings) {
    // Get draw records to get transforms for each BLAS instance
    const auto& draw_records = tlas_manager.get_draw_records();
    
    // Debug: Print transform info
    static bool debug_printed = false;
    if (!debug_printed) {
        printf("DEBUG: render_blas_bvh_transformed called with %zu draw records\n", draw_records.size());
        if (!draw_records.empty()) {
            const auto& first_record = draw_records[0];
            const auto& m = first_record.transform.m;
            printf("DEBUG: First transform matrix:\n");
            printf("  [%.2f %.2f %.2f %.2f]\n", m[0], m[1], m[2], m[3]);
            printf("  [%.2f %.2f %.2f %.2f]\n", m[4], m[5], m[6], m[7]);
            printf("  [%.2f %.2f %.2f %.2f]\n", m[8], m[9], m[10], m[11]);
            printf("  [%.2f %.2f %.2f %.2f]\n", m[12], m[13], m[14], m[15]);
        }
        debug_printed = true;
    }
    
    for (const auto& record : draw_records) {
        // Get the BLAS entry for this handle
        auto* entry = blas_manager.get_entry(record.blas_handle);
        if (!entry || !entry->bvh) continue;
        
        const BVH* bvh = entry->bvh.get();
        if (!bvh->bvhNode || bvh->nodesUsed == 0) continue;
        
        // Apply BVH name filtering if specified
        if (!settings.selected_bvh_filter.empty()) {
            // Check if this BLAS matches the selected filter
            // The filter contains names like "Cell(1,-1,0)_Mat6_170tris"
            // We need to match this against BLAS handles somehow
            // For now, we'll check triangle count as a heuristic
            if (entry->mesh && entry->mesh->triCount > 0) {
                // Extract triangle count from filter name if it matches pattern
                std::string filter = settings.selected_bvh_filter;
                size_t tris_pos = filter.find("tris");
                if (tris_pos != std::string::npos) {
                    // Find the number before "tris"
                    size_t num_start = filter.rfind('_', tris_pos);
                    if (num_start != std::string::npos) {
                        std::string tri_count_str = filter.substr(num_start + 1, tris_pos - num_start - 1);
                        int expected_tri_count = std::atoi(tri_count_str.c_str());
                        
                        // Only render if triangle counts match
                        if (entry->mesh->triCount != expected_tri_count) {
                            continue;  // Skip this BLAS, it doesn't match
                        }
                    }
                }
            } else {
                continue;  // Skip if no mesh or no triangles
            }
        }
        
        // Render this BLAS BVH with the transform applied
        render_bvh_node_recursive_transformed(bvh->bvhNode, 0, 0, settings, settings.blas_color, record);
        
        // Optionally render triangles with transforms
        if (settings.show_triangles && entry->mesh) {
            for (int i = 0; i < entry->mesh->triCount; i++) {
                draw_triangle_wireframe_transformed(entry->mesh->tri[i], 
                                                  settings.triangle_color, 
                                                  record,
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

void BVHVisualizer::render_bvh_node_recursive(const BVHNode* nodes, 
                                             int node_index, 
                                             int depth,
                                             const VisualizationSettings& settings,
                                             Color base_color) {
    if (depth > settings.max_depth_to_show) return;
    
    const BVHNode& node = nodes[node_index];
    
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

void BVHVisualizer::render_bvh_node_recursive_transformed(const BVHNode* nodes, 
                                                         int node_index, 
                                                         int depth,
                                                         const VisualizationSettings& settings,
                                                         Color base_color,
                                                         const TLASManager::DrawRecord& transform) {
    if (depth > settings.max_depth_to_show) return;
    
    const BVHNode& node = nodes[node_index];
    
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
    
    // Draw the AABB wireframe with transform applied
    Vector3 min_pos = float3_to_vector3(node.aabbMin);
    Vector3 max_pos = float3_to_vector3(node.aabbMax);
    draw_aabb_wireframe_transformed(min_pos, max_pos, node_color, transform, settings.wireframe_thickness);
    
    // Recursively render children if this is an interior node
    if (!is_leaf) {
        int left_child = node.leftFirst;
        int right_child = node.leftFirst + 1;
        
        render_bvh_node_recursive_transformed(nodes, left_child, depth + 1, settings, base_color, transform);
        render_bvh_node_recursive_transformed(nodes, right_child, depth + 1, settings, base_color, transform);
    }
}

void BVHVisualizer::render_tlas_node_recursive(const TLASNode* nodes,
                                              int node_index,
                                              int depth, 
                                              const VisualizationSettings& settings,
                                              Color base_color) {
    if (depth > settings.max_depth_to_show) return;
    
    const TLASNode& node = nodes[node_index];
    
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

void BVHVisualizer::draw_triangle_wireframe(const Tri& triangle, Color color, float thickness) {
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

Vector3 BVHVisualizer::transform_point(const Vector3& point, const TLASManager::DrawRecord& transform) {
    const auto& m = transform.transform.m;
    
    // Matrix4x4 is stored in row-major format:
    // m[0]  m[1]  m[2]  m[3]     <- Row 0 (x-component)
    // m[4]  m[5]  m[6]  m[7]     <- Row 1 (y-component) 
    // m[8]  m[9]  m[10] m[11]    <- Row 2 (z-component)
    // m[12] m[13] m[14] m[15]    <- Row 3 (translation)
    
    // Apply 4x4 matrix transformation to Vector3 (treating as homogeneous coordinate with w=1)
    float x = m[0] * point.x + m[1] * point.y + m[2]  * point.z + m[3];
    float y = m[4] * point.x + m[5] * point.y + m[6]  * point.z + m[7];
    float z = m[8] * point.x + m[9] * point.y + m[10] * point.z + m[11];
    
    return Vector3{x, y, z};
}

void BVHVisualizer::draw_aabb_wireframe_transformed(Vector3 min_pos, Vector3 max_pos, Color color, 
                                                   const TLASManager::DrawRecord& transform, float thickness) {
    (void)thickness; // Suppress unused parameter warning
    
    // Transform all 8 corners of the AABB
    Vector3 corners[8] = {
        {min_pos.x, min_pos.y, min_pos.z}, // 0: min corner
        {max_pos.x, min_pos.y, min_pos.z}, // 1
        {max_pos.x, max_pos.y, min_pos.z}, // 2
        {min_pos.x, max_pos.y, min_pos.z}, // 3
        {min_pos.x, min_pos.y, max_pos.z}, // 4
        {max_pos.x, min_pos.y, max_pos.z}, // 5
        {max_pos.x, max_pos.y, max_pos.z}, // 6: max corner
        {min_pos.x, max_pos.y, max_pos.z}  // 7
    };
    
    // Transform all corners
    for (int i = 0; i < 8; i++) {
        corners[i] = transform_point(corners[i], transform);
    }
    
    // Draw 12 edges of the AABB
    
    // Bottom face (4 edges)
    DrawLine3D(corners[0], corners[1], color);
    DrawLine3D(corners[1], corners[5], color);
    DrawLine3D(corners[5], corners[4], color);
    DrawLine3D(corners[4], corners[0], color);
    
    // Top face (4 edges)
    DrawLine3D(corners[3], corners[2], color);
    DrawLine3D(corners[2], corners[6], color);
    DrawLine3D(corners[6], corners[7], color);
    DrawLine3D(corners[7], corners[3], color);
    
    // Vertical edges (4 edges)
    DrawLine3D(corners[0], corners[3], color);
    DrawLine3D(corners[1], corners[2], color);
    DrawLine3D(corners[5], corners[6], color);
    DrawLine3D(corners[4], corners[7], color);
}

void BVHVisualizer::draw_triangle_wireframe_transformed(const Tri& triangle, Color color, 
                                                       const TLASManager::DrawRecord& transform, float thickness) {
    (void)thickness; // Suppress unused parameter warning
    
    Vector3 v0 = float3_to_vector3(triangle.vertex0);
    Vector3 v1 = float3_to_vector3(triangle.vertex1);
    Vector3 v2 = float3_to_vector3(triangle.vertex2);
    
    // Transform all vertices
    v0 = transform_point(v0, transform);
    v1 = transform_point(v1, transform);
    v2 = transform_point(v2, transform);
    
    DrawLine3D(v0, v1, color);
    DrawLine3D(v1, v2, color);
    DrawLine3D(v2, v0, color);
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