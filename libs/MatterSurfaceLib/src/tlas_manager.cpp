#include "../include/tlas_manager.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>

TLASManager::TLASManager(int max_instances)
    : tlas_(nullptr), next_instance_id_(1), max_instances_(max_instances) {

    // Initialize matrix stack with identity
    matrix_stack_.push(mm::identity());
    
    // Reserve space for draw records
    draw_records_.reserve(max_instances);
}

TLASManager::~TLASManager() {
    // Clean up instance array
    if (instance_array_) {
        // Call destructors for existing instances
        for (size_t i = 0; i < instance_array_size_; i++) {
            instance_array_[i].~BVHInstance();
        }
        free(instance_array_);
    }
}

const mm::Mat4& TLASManager::get_current_matrix() const {
    return matrix_stack_.top();
}

mm::Mat4& TLASManager::get_current_matrix() {
    return const_cast<mm::Mat4&>(matrix_stack_.top());
}

void TLASManager::push_matrix() {
    if (matrix_stack_.size() >= 32) { // Reasonable limit
        printf("Warning: Matrix stack overflow in TLAS manager\n");
        return;
    }
    
    matrix_stack_.push(matrix_stack_.top());
}

void TLASManager::pop_matrix() {
    if (matrix_stack_.size() <= 1) {
        printf("Warning: Matrix stack underflow in TLAS manager\n");
        return;
    }
    
    matrix_stack_.pop();
}

void TLASManager::load_identity() {
    get_current_matrix() = mm::identity();
}

void TLASManager::load_matrix(const mm::Mat4& matrix) {
    get_current_matrix() = matrix;
}

void TLASManager::multiply_matrix(const mm::Mat4& matrix) {
    mm::Mat4& current = get_current_matrix();
    current = mm::multiply(current, matrix);
}

void TLASManager::translate(float x, float y, float z) {
    mm::Mat4 trans = mm::translation(mm::Vec3{x, y, z});
    multiply_matrix(trans);
}

void TLASManager::translate(const float3& translation) {
    translate(translation.x, translation.y, translation.z);
}

void TLASManager::scale(float sx, float sy, float sz) {
    mm::Mat4 scale_matrix = mm::scale(mm::Vec3{sx, sy, sz});
    multiply_matrix(scale_matrix);
}

void TLASManager::scale(float uniform_scale) {
    scale(uniform_scale, uniform_scale, uniform_scale);
}

void TLASManager::rotate_x(float angle_radians) {
    mm::Mat4 rot = mm::rotation_x(angle_radians);
    multiply_matrix(rot);
}

void TLASManager::rotate_y(float angle_radians) {
    mm::Mat4 rot = mm::rotation_y(angle_radians);
    multiply_matrix(rot);
}

void TLASManager::rotate_z(float angle_radians) {
    mm::Mat4 rot = mm::rotation_z(angle_radians);
    multiply_matrix(rot);
}

void TLASManager::rotate_axis(const float3& axis, float angle_radians) {
    // mm::rotation_axis requires a pre-normalized axis (returns identity()
    // otherwise); the deleted matrix_rotation_axis() normalized internally,
    // so normalize explicitly here to preserve that lenient behaviour for
    // any (currently nonexistent, but public-API) caller passing a
    // non-unit axis.
    mm::Vec3 axis_mm{axis.x, axis.y, axis.z};
    mm::Mat4 rot = mm::rotation_axis(mm::normalize(axis_mm), angle_radians);
    multiply_matrix(rot);
}

uint32_t TLASManager::draw(BLASHandle blas_handle, uint32_t material_id) {
    PROFILE_SECTION("TLAS Draw Call");
    
    if (blas_handle == INVALID_BLAS_HANDLE) return 0;
    
    if (draw_records_.size() >= static_cast<size_t>(max_instances_)) {
        printf("Warning: TLAS manager draw capacity exceeded (%d)\n", max_instances_);
        return 0;
    }
    
    uint32_t instance_id = next_instance_id_++;
    draw_records_.emplace_back(blas_handle, get_current_matrix(), material_id, instance_id);
    
    mark_dirty();
    return instance_id;
}

void TLASManager::draw_batch(const std::vector<DrawInstance>& instances) {
    PROFILE_SECTION("TLAS Batch Draw");

    for (const auto& instance : instances) {
        push_matrix();
        load_matrix(instance.transform);
        if (draw(instance.blas_handle, instance.material_id) != 0)
            draw_records_.back().is_imposter = instance.is_imposter; // draw appended a record
        pop_matrix();
    }
}

void TLASManager::clear() {
    draw_records_.clear();
    next_instance_id_ = 1;
    
    // Reset matrix stack to just identity
    while (matrix_stack_.size() > 1) {
        matrix_stack_.pop();
    }
    load_identity();
    
    // Clean up existing TLAS and instances
    tlas_.reset(nullptr);
    instances_.clear();
    
    // Clean up instance array
    if (instance_array_) {
        // Call destructors for existing instances
        for (size_t i = 0; i < instance_array_size_; i++) {
            instance_array_[i].~BVHInstance();
        }
        free(instance_array_);
        instance_array_ = nullptr;
        instance_array_size_ = 0;
    }
    
    mark_dirty();
}

void TLASManager::build(const BLASManager& blas_manager) {
    PROFILE_SECTION("TLAS Build");
    
    if (draw_records_.empty()) {
        printf("Warning: No draw records to build TLAS from\n");
        return;
    }
    
    // Clean up existing TLAS and instances
    tlas_.reset(nullptr);
    instances_.clear();
    
    // Clean up previous instance array
    if (instance_array_) {
        // Call destructors for existing instances
        for (size_t i = 0; i < instance_array_size_; i++) {
            instance_array_[i].~BVHInstance();
        }
        free(instance_array_);
        instance_array_ = nullptr;
        instance_array_size_ = 0;
    }
    
    // Create BVH instances from draw records (using unique_ptr approach for now)
    instances_.reserve(draw_records_.size());
    std::vector<BVHInstance*> instance_ptrs;
    instance_ptrs.reserve(draw_records_.size());

    for (const auto& record : draw_records_) {
        // Get BVH from manager
        BVH* bvh = blas_manager.get_bvh(record.blas_handle);
        if (!bvh) {
            printf("Warning: BLAS handle %u not found in BLAS manager\n", record.blas_handle);
            continue; // skip: no instance/BVH for this record
        }

        // Create BVH instance
        auto instance = std::make_unique<BVHInstance>(bvh, record.instance_id);

        // Convert and set transform - this will also calculate world bounds.
        // mm::Mat4 and mat4 (SpatialQueryLib/tri.h) are both row-major
        // float[16] with translation at [3],[7],[11] -- same layout, plain
        // element copy (the convert_matrix() shim this replaced was doing
        // exactly this loop).
        mat4 new_transform;
        for (int i = 0; i < 16; i++) {
            new_transform.cell[i] = record.transform.m[i];
        }
        instance->SetTransform(new_transform);

        // Add to our vectors — record and instance are added in lock-step
        instance_ptrs.push_back(instance.get());
        instances_.push_back(std::move(instance));
    }
    
    // Create and build TLAS
    if (!instance_ptrs.empty()) {
        tlas_.reset(); // old TLAS points into instance_storage_; drop it before mutating
        instance_storage_.clear();
        instance_storage_.reserve(instance_ptrs.size());
        for (BVHInstance* p : instance_ptrs) {
            instance_storage_.push_back(*p);
        }
        tlas_ = std::make_unique<TLAS>(instance_storage_.data(), static_cast<int>(instance_storage_.size()));
        tlas_->Build();
    }

    mark_dirty();
}

int TLASManager::get_instance_count() const {
    return tlas_ ? tlas_->blasCount : 0;
}

int TLASManager::get_node_count() const {
    return tlas_ ? tlas_->nodesUsed : 0;
}


void TLASManager::print_stats() const {
    // printf("=== TLAS Manager Statistics ===\n");
    // printf("Draw records: %zu/%d\n", draw_records_.size(), max_instances_);
    // printf("Matrix stack depth: %zu\n", matrix_stack_.size());
    // printf("Next instance ID: %u\n", next_instance_id_);
    
    // if (tlas_) {
    //     printf("Built TLAS: %d instances, %d nodes\n", 
    //            tlas_->blasCount, tlas_->nodesUsed);
    // } else {
    //     printf("TLAS: Not built\n");
    // }
}

// Scene building utilities implementation
namespace SceneBuilder {

void create_grid(TLASManager& manager, BLASHandle blas_handle, 
                int rows, int cols, float spacing, uint32_t material_id) {
    PROFILE_SECTION("Create Grid Scene");
    
    float start_x = -(cols - 1) * spacing * 0.5f;
    float start_z = -(rows - 1) * spacing * 0.5f;
    
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            TLAS_PUSH_MATRIX(manager);
            
            float x = start_x + col * spacing;
            float z = start_z + row * spacing;
            
            manager.translate(x, 0.0f, z);
            manager.draw(blas_handle, material_id);
        }
    }
}

void create_circle(TLASManager& manager, BLASHandle blas_handle,
                  int count, float radius, uint32_t material_id) {
    PROFILE_SECTION("Create Circle Scene");
    
    for (int i = 0; i < count; i++) {
        TLAS_PUSH_MATRIX(manager);
        
        float angle = static_cast<float>(i) / static_cast<float>(count) * 2.0f * static_cast<float>(M_PI);
        float x = radius * std::cos(angle);
        float z = radius * std::sin(angle);
        
        manager.translate(x, 0.0f, z);
        manager.rotate_y(angle); // Face inward
        manager.draw(blas_handle, material_id);
    }
}

void create_scatter(TLASManager& manager, BLASHandle blas_handle,
                   int count, float range, uint32_t material_id) {
    PROFILE_SECTION("Create Scatter Scene");
    
    for (int i = 0; i < count; i++) {
        TLAS_PUSH_MATRIX(manager);
        
        // Random position
        float x = (static_cast<float>(std::rand()) / RAND_MAX - 0.5f) * range * 2.0f;
        float y = (static_cast<float>(std::rand()) / RAND_MAX - 0.5f) * range * 0.5f;
        float z = (static_cast<float>(std::rand()) / RAND_MAX - 0.5f) * range * 2.0f;
        
        // Random rotation
        float rot_y = static_cast<float>(std::rand()) / RAND_MAX * 2.0f * static_cast<float>(M_PI);
        
        // Random scale
        float scale_factor = 0.5f + (static_cast<float>(std::rand()) / RAND_MAX) * 1.0f;
        
        manager.translate(x, y, z);
        manager.rotate_y(rot_y);
        manager.scale(scale_factor);
        manager.draw(blas_handle, material_id);
    }
}

} // namespace SceneBuilder