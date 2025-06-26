#pragma once

extern "C" {
    #include "bvh.h"
    #include "raylib.h"
}

#include "profiler.hpp"
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstdint>

using BLASHandle = uint32_t;
constexpr BLASHandle INVALID_BLAS_HANDLE = 0;

struct BLASOffsets {
    int triangle_offset;
    int node_offset;
};

class BLASManager {
public:
    BLASManager();
    ~BLASManager();
    
    // Non-copyable but movable
    BLASManager(const BLASManager&) = delete;
    BLASManager& operator=(const BLASManager&) = delete;
    BLASManager(BLASManager&&) = default;
    BLASManager& operator=(BLASManager&&) = default;
    
    // Register mesh data and get BLAS handle
    BLASHandle register_triangles(const std::vector<Triangle>& triangles, 
                                 int max_triangles_per_leaf = 4);
    
    BLASHandle register_triangles(Triangle* triangles, 
                                 int triangle_count,
                                 int max_triangles_per_leaf = 4);
    
    // Check if a BLAS exists
    bool has_blas(BLASHandle handle) const;
    
    // Get BLAS from handle
    BLAS* get_blas(BLASHandle handle) const;
    
    // Get total counts for GPU texture generation
    int get_total_triangle_count() const;
    int get_total_node_count() const;
    int get_unique_blas_count() const { return static_cast<int>(entries_.size()); }
    
    // Get offsets for a specific BLAS in the combined arrays
    BLASOffsets get_offsets(BLASHandle handle) const;
    
    // Generate combined data for GPU upload
    void generate_triangle_data(std::vector<Triangle>& output_triangles) const;
    void generate_node_data(std::vector<BVHNode>& output_nodes) const;
    
    // GPU texture management (fully encapsulated)
    void ensure_gpu_textures_ready(); // Creates/updates textures if needed
    void bind_to_shader(Shader shader) const; // Manager owns textures completely
    
    // Legacy C-style interface for compatibility
    void generate_triangle_texture_data(Triangle* output_triangles) const;
    void generate_node_texture_data(BVHNode* output_nodes) const;
    
    // Statistics and debugging
    void print_stats() const;
    void reset_stats();

private:
    struct BLASEntry {
        BLASHandle handle;
        std::unique_ptr<BLAS, void(*)(BLAS*)> blas;
        std::vector<Triangle> triangles;
        uint32_t hash;
        
        BLASEntry(BLASHandle h, BLAS* b, std::vector<Triangle>&& tris, uint32_t hash_val) 
            : handle(h), blas(b, blas_destroy), triangles(std::move(tris)), hash(hash_val) {}
    };
    
    // Hash calculation
    uint32_t calculate_hash(const Triangle* triangles, int count) const;
    bool triangles_equal(const std::vector<Triangle>& a, const Triangle* b, int count) const;
    
    // Find existing BLAS by hash and triangle data
    BLASHandle find_existing_blas(const Triangle* triangles, int count, uint32_t hash) const;
    
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
    
};

// Factory functions for common geometry types
namespace BLASFactory {
    std::vector<Triangle> create_cube_triangles(float size = 1.0f);
    std::vector<Triangle> create_sphere_triangles(float radius, int segments, int rings);
    std::vector<Triangle> create_plane_triangles(float width, float height);
    
    BLASHandle register_cube(BLASManager& manager, float size = 1.0f);
    BLASHandle register_sphere(BLASManager& manager, float radius, int segments = 32, int rings = 16);
    BLASHandle register_plane(BLASManager& manager, float width = 10.0f, float height = 10.0f);
}