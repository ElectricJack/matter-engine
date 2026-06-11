#pragma once

extern "C" {
    #include "raylib.h"
}

#include "precomp.h"
#include "bvh.h"

#include "profiler.hpp"
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstdint>

// BVH types are now in global namespace
using uint = unsigned int;
// float3 types come from precomp.h (global namespace)
// using ::float3;  // Already available globally
// using ::make_float3;  // Already available globally
// using ::normalize;  // Already available globally
// using ::cross;  // Already available globally

// Legacy Triangle struct for backward compatibility (where needed for different layouts)
struct LegacyTriangle {
    float3 v0, v1, v2;
    float3 centroid;
    float3 normal;
    int material_id;
};

// Legacy BVHNode struct for backward compatibility (where needed for different layouts)
struct LegacyBVHNode {
    float3 aabbMin;
    uint32_t leftFirst;
    float3 aabbMax;
    uint32_t triCount;
};

// Type aliases for backward compatibility (using different names to avoid conflicts)
using Triangle = LegacyTriangle;

using BLASHandle = uint32_t;
constexpr BLASHandle INVALID_BLAS_HANDLE = 0;

struct BLASOffsets {
    int triangle_offset;
    int node_offset;
};

class BLASManager {
public:
    // Forward declaration for visibility
    struct BLASEntry {
        BLASHandle handle;
        std::unique_ptr<BvhMesh> mesh;
        std::unique_ptr<BVH> bvh;
        std::vector<Tri> triangles;
        uint32_t hash;
        
        BLASEntry(BLASHandle h, std::unique_ptr<BvhMesh> m, std::unique_ptr<BVH> b, std::vector<Tri>&& tris, uint32_t hash_val) 
            : handle(h), mesh(std::move(m)), bvh(std::move(b)), triangles(std::move(tris)), hash(hash_val) {}
    };

    BLASManager();
    ~BLASManager();
    
    // Non-copyable but movable
    BLASManager(const BLASManager&) = delete;
    BLASManager& operator=(const BLASManager&) = delete;
    BLASManager(BLASManager&&) = default;
    BLASManager& operator=(BLASManager&&) = default;
    
    // Register mesh data and get BLAS handle
    BLASHandle register_triangles(const std::vector<Tri>& triangles);
    
    BLASHandle register_triangles(Tri* triangles, int triangle_count);
    
    // Legacy interface for old Triangle format
    //BLASHandle register_triangles_legacy(const std::vector<LegacyTriangle>& triangles);
    //BLASHandle register_triangles_legacy(LegacyTriangle* triangles, int triangle_count);
    
    // Check if a BLAS exists
    bool has_blas(BLASHandle handle) const;
    
    // Get BVH from handle
    BVH* get_bvh(BLASHandle handle) const;
    
    // Get Mesh from handle 
    BvhMesh* get_mesh(BLASHandle handle) const;
    
    // Get entry from handle (for visualization)
    const BLASEntry* get_entry(BLASHandle handle) const;
    
    // Get total counts for GPU texture generation
    int get_total_triangle_count() const;
    int get_total_node_count() const;
    int get_unique_blas_count() const { return static_cast<int>(entries_.size()); }
    
    // Access to internal entries for visualization
    const std::vector<std::unique_ptr<BLASEntry>>& get_entries() const { return entries_; }
    
    // Get offsets for a specific BLAS in the combined arrays
    BLASOffsets get_offsets(BLASHandle handle) const;
    
    // Generate combined data for GPU upload
    void generate_triangle_data(std::vector<Tri>& output_triangles) const;
    void generate_node_data(std::vector<LegacyBVHNode>& output_nodes) const;
    
    // Legacy interface for old Triangle format
    //void generate_triangle_data_legacy(std::vector<LegacyTriangle>& output_triangles) const;
    
    // GPU texture management (fully encapsulated)
    void ensure_gpu_textures_ready(); // Creates/updates textures if needed
    void bind_to_shader(Shader shader) const; // Manager owns textures completely
    
    // Legacy C-style interface for compatibility
    void generate_triangle_texture_data(Tri* output_triangles) const;
    void generate_node_texture_data(LegacyBVHNode* output_nodes) const;
    
    // Legacy interface for old Triangle format
    //void generate_triangle_texture_data_legacy(LegacyTriangle* output_triangles) const;
    
    // Statistics and debugging
    void print_stats() const;
    void reset_stats();
    
    // Clear all BLAS entries (use with caution - invalidates all existing handles)
    void clear();

private:
    // Mark data as dirty when BLAS changes
    void mark_dirty() const {
        totals_dirty_ = true;
        textures_dirty_ = true;
        shader_values_dirty_ = true;
    }
    
    // Conversion utilities
    static Tri convert_triangle(const LegacyTriangle& old_tri);
    static LegacyTriangle convert_triangle_back(const Tri& new_tri);
    
    // Hash calculation
    uint32_t calculate_hash(const Tri* triangles, int count) const;
    bool triangles_equal(const std::vector<Tri>& a, const Tri* b, int count) const;
    
    // Find existing BLAS by hash and triangle data
    BLASHandle find_existing_blas(const Tri* triangles, int count, uint32_t hash) const;
    
    // Legacy hash calculation for old Triangle format
    //uint32_t calculate_hash_legacy(const LegacyTriangle* triangles, int count) const;
    //BLASHandle find_existing_blas_legacy(const LegacyTriangle* triangles, int count, uint32_t hash) const;
    
    // Update cached totals
    void update_totals() const;
    
    std::vector<std::unique_ptr<BLASEntry>> entries_;
    std::unordered_multimap<uint32_t, size_t> hash_to_entry_; // hash -> entry index
    BLASHandle next_handle_;
    
    // Cached totals (mutable for lazy evaluation)
    mutable int cached_total_triangles_;
    mutable int cached_total_nodes_;
    mutable bool totals_dirty_;
    
    // GPU texture management
    mutable Texture2D triangles_texture_{};
    mutable Texture2D nodes_texture_{};
    mutable bool textures_dirty_ = true;
    
    // Shader binding optimization
    mutable uint32_t cached_shader_id_ = 0;
    mutable int triangle_count_loc_ = -1;
    mutable int blas_node_count_loc_ = -1;
    mutable int triangles_texture_loc_ = -1;
    mutable int blas_nodes_texture_loc_ = -1;
    mutable int intersection_mode_loc_ = -1;
    mutable bool shader_values_dirty_ = true;
    
};

// Factory functions for common geometry types
namespace BLASFactory {
    std::vector<Tri> create_cube_triangles(float size = 1.0f);
    std::vector<Tri> create_sphere_triangles(float radius, int segments, int rings);
    std::vector<Tri> create_plane_triangles(float width, float height);
    
    // Legacy functions for backward compatibility
    // std::vector<LegacyTriangle> create_cube_triangles_legacy(float size = 1.0f);
    // std::vector<LegacyTriangle> create_sphere_triangles_legacy(float radius, int segments, int rings);
    // std::vector<LegacyTriangle> create_plane_triangles_legacy(float width, float height);
    
    BLASHandle register_cube(BLASManager& manager, float size = 1.0f);
    BLASHandle register_sphere(BLASManager& manager, float radius, int segments = 32, int rings = 16);
    BLASHandle register_plane(BLASManager& manager, float width = 10.0f, float height = 10.0f);
}