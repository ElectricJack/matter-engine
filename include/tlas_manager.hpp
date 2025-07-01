#pragma once

extern "C" {
    #include "raylib.h"
}

#include "precomp.h"
#include "bvh.h"

#include "blas_manager.hpp"
#include "profiler.hpp"
#include <vector>
#include <stack>
#include <memory>
#include <cstdint>

// BVH types are now in global namespace
// float3 and normalize are from global namespace via precomp.h

// Legacy types for backward compatibility
struct Matrix4x4 {
    float m[16];
    Matrix4x4() {
        for (int i = 0; i < 16; i++) m[i] = 0.0f;
        m[0] = m[5] = m[10] = m[15] = 1.0f; // Identity
    }
};

// TLASNode is now available from bvh.h in global namespace

struct LegacyBVHInstance {
    mat4 transform;
    mat4 invTransform;
    uint32_t instanceId;
    BVH* bvh;
    aabb bounds;
};

class TLASManager {
public:
    // DrawRecord struct for accessing instance data
    struct DrawRecord {
        BLASHandle blas_handle;
        Matrix4x4 transform;
        Matrix4x4 inv_transform;
        uint32_t material_id;
        uint32_t instance_id;
        
        DrawRecord(BLASHandle handle, const Matrix4x4& trans, uint32_t mat_id, uint32_t inst_id)
            : blas_handle(handle), transform(trans), material_id(mat_id), instance_id(inst_id) {
            inv_transform = Matrix4x4(); // Will implement matrix_inverse later
        }
    };

    explicit TLASManager(int max_instances = 100);
    ~TLASManager();
    
    // Non-copyable but movable
    TLASManager(const TLASManager&) = delete;
    TLASManager& operator=(const TLASManager&) = delete;
    TLASManager(TLASManager&&) = default;
    TLASManager& operator=(TLASManager&&) = default;
    
    // Matrix stack operations - similar to OpenGL matrix stack
    void push_matrix();
    void pop_matrix();
    void load_identity();
    void load_matrix(const Matrix4x4& matrix);
    void multiply_matrix(const Matrix4x4& matrix);
    
    // Transformation convenience functions
    void translate(float x, float y, float z);
    void translate(const float3& translation);
    void scale(float sx, float sy, float sz);
    void scale(float uniform_scale);
    void rotate_x(float angle_radians);
    void rotate_y(float angle_radians);
    void rotate_z(float angle_radians);
    void rotate_axis(const float3& axis, float angle_radians);
    
    // Drawing operations - records instances with current transform
    uint32_t draw(BLASHandle blas_handle, uint32_t material_id = 0);
    
    // Batch drawing operations
    struct DrawInstance {
        BLASHandle blas_handle;
        Matrix4x4 transform;
        uint32_t material_id;
    };
    void draw_batch(const std::vector<DrawInstance>& instances);
    
    // Clear all recorded instances (for new frame)
    void clear();
    
    // Build TLAS from recorded instances (call after all draw() calls)
    void build(const BLASManager& blas_manager);
    
    // GPU texture generation  
    int get_instance_count() const;
    int get_node_count() const;
    
    // Access to internal TLAS for visualization
    const TLAS* get_tlas() const { return tlas_.get(); }
    
    // Access to draw records for rasterization
    const std::vector<DrawRecord>& get_draw_records() const { return draw_records_; }
    
    // GPU texture management (fully encapsulated)
    void ensure_gpu_textures_ready(const BLASManager& blas_manager); // Creates/updates textures if needed
    void bind_to_shader(Shader shader, const BLASManager& blas_manager) const; // Manager owns textures completely
    
    // Generate texture data for GPU upload
    void generate_instance_texture_data(const BLASManager& blas_manager,
                                       std::vector<float>& output_data, 
                                       int texture_width,
                                       int texture_height) const;
    
    void generate_node_texture_data(std::vector<float>& output_data,
                                   int texture_width, 
                                   int texture_height) const;
    
    // Legacy C-style interface for compatibility
    void generate_instance_texture_data(const BLASManager& blas_manager,
                                       float* output_data, 
                                       int texture_width,
                                       int texture_height) const;
    
    void generate_node_texture_data(float* output_data,
                                   int texture_width, 
                                   int texture_height) const;
    
    // Get underlying TLAS for compatibility with existing code
    
    // Statistics and debugging
    void print_stats() const;
    int  get_draw_record_count() const { return static_cast<int>(draw_records_.size()); }
    int  get_matrix_stack_depth() const { return static_cast<int>(matrix_stack_.size()); }

private:
    // Mark data as dirty when TLAS changes
    void mark_dirty() const {
        textures_dirty_ = true;
        shader_values_dirty_ = true;
    }
    
    // Conversion utilities
    static mat4      convert_matrix(const Matrix4x4& legacy_matrix);
    static Matrix4x4 convert_matrix_back(const mat4& new_matrix);
    
    // Get current matrix from top of stack
    const Matrix4x4& get_current_matrix() const;
    Matrix4x4&       get_current_matrix();
    
    std::stack<Matrix4x4>    matrix_stack_;
    std::vector<DrawRecord>  draw_records_;
    std::unique_ptr<TLAS>    tlas_;
    std::vector<std::unique_ptr<BVHInstance>> instances_; // Deprecated - kept for compatibility
    BVHInstance*  instance_array_ = nullptr; // Contiguous array for TLAS
    size_t        instance_array_size_ = 0;
    uint32_t      next_instance_id_;
    int           max_instances_;
    
    // GPU texture management
    mutable Texture2D nodes_texture_{};
    mutable Texture2D instances_texture_{};
    mutable bool textures_dirty_ = true;
    
    // Shader binding optimization
    mutable uint32_t cached_shader_id_ = 0;
    mutable int tlas_node_count_loc_ = -1;
    mutable int instance_count_loc_ = -1;
    mutable int tlas_nodes_texture_loc_ = -1;
    mutable int instances_texture_loc_ = -1;
    mutable bool shader_values_dirty_ = true;
};

// Utility class for automatic matrix push/pop using RAII
class ScopedMatrix {
public:
    explicit ScopedMatrix(TLASManager& manager) : manager_(manager) {
        manager_.push_matrix();
    }
    
    ~ScopedMatrix() {
        manager_.pop_matrix();
    }

private:
    TLASManager& manager_;
};

// Helper macros for convenient matrix scoping
#define TLAS_PUSH_MATRIX(manager) Performance::ScopedTimer _matrix_scope("Matrix Operations"); ScopedMatrix _matrix_guard(manager)

// Scene building utilities
namespace SceneBuilder {
    // Create a grid of instances
    void create_grid(TLASManager& manager, BLASHandle blas_handle, 
                    int rows, int cols, float spacing, uint32_t material_id = 0);
    
    // Create a circular arrangement of instances
    void create_circle(TLASManager& manager, BLASHandle blas_handle,
                      int count, float radius, uint32_t material_id = 0);
    
    // Create a random scatter of instances
    void create_scatter(TLASManager& manager, BLASHandle blas_handle,
                       int count, float range, uint32_t material_id = 0);
}