#pragma once

extern "C" {
    #include "bvh.h"
    #include "raylib.h"
}

#include "blas_manager.hpp"
#include "profiler.hpp"
#include <vector>
#include <stack>
#include <memory>
#include <cstdint>

class TLASManager {
public:
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
    void translate(const Vec3& translation);
    void scale(float sx, float sy, float sz);
    void scale(float uniform_scale);
    void rotate_x(float angle_radians);
    void rotate_y(float angle_radians);
    void rotate_z(float angle_radians);
    void rotate_axis(const Vec3& axis, float angle_radians);
    
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
    TLAS* get_tlas() const { return tlas_.get(); }
    
    // Statistics and debugging
    void print_stats() const;
    int get_draw_record_count() const { return static_cast<int>(draw_records_.size()); }
    int get_matrix_stack_depth() const { return static_cast<int>(matrix_stack_.size()); }

private:
    struct DrawRecord {
        BLASHandle blas_handle;
        Matrix4x4 transform;
        Matrix4x4 inv_transform;
        uint32_t material_id;
        uint32_t instance_id;
        
        DrawRecord(BLASHandle handle, const Matrix4x4& trans, uint32_t mat_id, uint32_t inst_id)
            : blas_handle(handle), transform(trans), material_id(mat_id), instance_id(inst_id) {
            inv_transform = matrix_inverse(&trans);
        }
    };
    
    // Get current matrix from top of stack
    const Matrix4x4& get_current_matrix() const;
    Matrix4x4& get_current_matrix();
    
    std::stack<Matrix4x4> matrix_stack_;
    std::vector<DrawRecord> draw_records_;
    std::unique_ptr<TLAS, void(*)(TLAS*)> tlas_;
    uint32_t next_instance_id_;
    int max_instances_;
    
    // GPU texture management
    mutable Texture2D nodes_texture_{};
    mutable Texture2D instances_texture_{};
    mutable bool textures_dirty_ = true;
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