#pragma once

#include "precomp.h"
#include "bvh.h"
#include "matter_math.h"

#include "blas_manager.hpp"
#include "profiler.hpp"
#include <vector>
#include <stack>
#include <memory>
#include <cstdint>

// BVH types are now in global namespace
// float3 and normalize are from global namespace via precomp.h

// Matrix4x4 (row-major float[16], identity-default) has been collapsed onto
// mm::Mat4 (libs/MathLib/include/matter_math.h) -- same layout, same default,
// see docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md Phase 2.

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
        mm::Mat4 transform;
        mm::Mat4 inv_transform;
        uint32_t material_id;
        uint32_t instance_id;
        bool is_imposter = false;

        DrawRecord(BLASHandle handle, const mm::Mat4& trans, uint32_t mat_id, uint32_t inst_id)
            : blas_handle(handle), transform(trans), material_id(mat_id), instance_id(inst_id) {
            // Left as identity: inv_transform is never read. The GPU upload path
            // (tlas_manager.cpp) uses BVHInstance::GetInvTransform() instead. The
            // field is dead — see tech-debt.md §1.
            inv_transform = mm::Mat4();
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
    void load_matrix(const mm::Mat4& matrix);
    void multiply_matrix(const mm::Mat4& matrix);
    
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
        mm::Mat4 transform;
        uint32_t material_id;
        bool is_imposter = false;
    };
    void draw_batch(const std::vector<DrawInstance>& instances);
    
    // Clear all recorded instances (for new frame)
    void clear();
    
    // Build TLAS from recorded instances (call after all draw() calls)
    void build(const BLASManager& blas_manager);
    
    int get_instance_count() const;
    int get_node_count() const;

    // Monotonic counter, bumped whenever the flattened instance/node content
    // changes — draw() records an instance, clear() drops them, build() rebuilds
    // the node array. See BLASManager::content_revision for the rationale; a
    // consumer uploading this content should track BOTH revisions, since the
    // TLAS instance records reference BLAS offsets and go stale when either
    // side moves:
    //
    //     if (tlas.content_revision() == seen_tlas_ &&
    //         blas.content_revision() == seen_blas_) return;  // nothing to do
    uint64_t content_revision() const { return content_revision_; }


    // Access to internal TLAS for visualization
    const TLAS* get_tlas() const { return tlas_.get(); }
    
    // Access to draw records for rasterization
    const std::vector<DrawRecord>& get_draw_records() const { return draw_records_; }

    // Statistics and debugging
    void print_stats() const;
    int  get_draw_record_count() const { return static_cast<int>(draw_records_.size()); }
    int  get_matrix_stack_depth() const { return static_cast<int>(matrix_stack_.size()); }

private:
    // Mark data as dirty when TLAS changes
    // Invariant, as in BLASManager: every call site here (draw, clear, build)
    // changes the flattened instance/node content.
    void mark_dirty() const {
        ++content_revision_;
    }
    
    // Get current matrix from top of stack
    const mm::Mat4& get_current_matrix() const;
    mm::Mat4&       get_current_matrix();

    std::stack<mm::Mat4>    matrix_stack_;
    std::vector<DrawRecord>  draw_records_;
    // B4 fix: compacted list of draw records that made it into the TLAS (i.e., had
    // a valid BLAS). Index i here corresponds exactly to tlas_->blas[i].
    std::vector<DrawRecord>  active_draw_records_;
    std::unique_ptr<TLAS>    tlas_;
    std::vector<BVHInstance> instance_storage_; // backing array owned by the manager; TLAS holds a raw pointer into it
    std::vector<std::unique_ptr<BVHInstance>> instances_; // Deprecated - kept for compatibility
    BVHInstance*  instance_array_ = nullptr; // Contiguous array for TLAS
    size_t        instance_array_size_ = 0;
    uint32_t      next_instance_id_;
    int           max_instances_;

    // Starts at 1 so a consumer default-initialising its last-seen value to 0
    // always performs its first upload.
    mutable uint64_t content_revision_ = 1;
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