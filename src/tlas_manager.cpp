#include "../include/tlas_manager.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>

TLASManager::TLASManager(int max_instances) 
    : tlas_(nullptr, tlas_destroy), next_instance_id_(1), max_instances_(max_instances) {
    
    // Initialize matrix stack with identity
    matrix_stack_.push(matrix_identity());
    
    // Reserve space for draw records
    draw_records_.reserve(max_instances);
}

TLASManager::~TLASManager() {
    // Clean up GPU textures
    if (nodes_texture_.id != 0) UnloadTexture(nodes_texture_);
    if (instances_texture_.id != 0) UnloadTexture(instances_texture_);
}

const Matrix4x4& TLASManager::get_current_matrix() const {
    return matrix_stack_.top();
}

Matrix4x4& TLASManager::get_current_matrix() {
    return const_cast<Matrix4x4&>(matrix_stack_.top());
}

void TLASManager::push_matrix() {
    PROFILE_SECTION("Matrix Push");
    
    if (matrix_stack_.size() >= 32) { // Reasonable limit
        printf("Warning: Matrix stack overflow in TLAS manager\n");
        return;
    }
    
    matrix_stack_.push(matrix_stack_.top());
}

void TLASManager::pop_matrix() {
    PROFILE_SECTION("Matrix Pop");
    
    if (matrix_stack_.size() <= 1) {
        printf("Warning: Matrix stack underflow in TLAS manager\n");
        return;
    }
    
    matrix_stack_.pop();
}

void TLASManager::load_identity() {
    get_current_matrix() = matrix_identity();
}

void TLASManager::load_matrix(const Matrix4x4& matrix) {
    get_current_matrix() = matrix;
}

void TLASManager::multiply_matrix(const Matrix4x4& matrix) {
    Matrix4x4& current = get_current_matrix();
    current = matrix_multiply(&current, &matrix);
}

void TLASManager::translate(float x, float y, float z) {
    Matrix4x4 trans = matrix_translation(x, y, z);
    multiply_matrix(trans);
}

void TLASManager::translate(const Vec3& translation) {
    translate(translation.x, translation.y, translation.z);
}

void TLASManager::scale(float sx, float sy, float sz) {
    Matrix4x4 scale_matrix = matrix_scale(sx, sy, sz);
    multiply_matrix(scale_matrix);
}

void TLASManager::scale(float uniform_scale) {
    scale(uniform_scale, uniform_scale, uniform_scale);
}

void TLASManager::rotate_x(float angle_radians) {
    Matrix4x4 rot = matrix_rotation_x(angle_radians);
    multiply_matrix(rot);
}

void TLASManager::rotate_y(float angle_radians) {
    Matrix4x4 rot = matrix_rotation_y(angle_radians);
    multiply_matrix(rot);
}

void TLASManager::rotate_z(float angle_radians) {
    Matrix4x4 rot = matrix_rotation_z(angle_radians);
    multiply_matrix(rot);
}

void TLASManager::rotate_axis(const Vec3& axis, float angle_radians) {
    Matrix4x4 rot = matrix_rotation_axis(axis, angle_radians);
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
    
    textures_dirty_ = true; // Mark textures for regeneration
    return instance_id;
}

void TLASManager::draw_batch(const std::vector<DrawInstance>& instances) {
    PROFILE_SECTION("TLAS Batch Draw");
    
    for (const auto& instance : instances) {
        push_matrix();
        load_matrix(instance.transform);
        draw(instance.blas_handle, instance.material_id);
        pop_matrix();
    }
}

void TLASManager::clear() {
    PROFILE_SECTION("TLAS Clear");
    
    draw_records_.clear();
    next_instance_id_ = 1;
    
    // Reset matrix stack to just identity
    while (matrix_stack_.size() > 1) {
        matrix_stack_.pop();
    }
    load_identity();
    
    // Clean up existing TLAS
    tlas_.reset(nullptr);
    
    textures_dirty_ = true; // Mark textures for regeneration
}

void TLASManager::build(const BLASManager& blas_manager) {
    PROFILE_SECTION("TLAS Build");
    
    if (draw_records_.empty()) {
        printf("Warning: No draw records to build TLAS from\n");
        return;
    }
    
    // Clean up existing TLAS
    tlas_.reset(nullptr);
    
    // Create new TLAS
    TLAS* new_tlas = tlas_create(static_cast<int>(draw_records_.size()));
    if (!new_tlas) {
        printf("Failed to create TLAS for %zu instances\n", draw_records_.size());
        return;
    }
    tlas_.reset(new_tlas);
    
    // Convert draw records to BVH instances
    std::vector<std::unique_ptr<BVHInstance, void(*)(BVHInstance*)>> instances;
    instances.reserve(draw_records_.size());
    
    for (const auto& record : draw_records_) {
        // Get BLAS from manager
        BLAS* blas = blas_manager.get_blas(record.blas_handle);
        if (!blas) {
            printf("Warning: BLAS handle %u not found in BLAS manager\n", record.blas_handle);
            continue;
        }
        
        // Create BVH instance
        BVHInstance* instance = bvh_instance_create(blas, record.instance_id);
        if (!instance) {
            printf("Failed to create BVH instance for draw record\n");
            continue;
        }
        
        // Set transform
        bvh_instance_set_transform(instance, &record.transform);
        
        // Update BLAS start index from manager
        BLASOffsets offsets = blas_manager.get_offsets(record.blas_handle);
        instance->blas_start_index = offsets.node_offset;
        
        // Add to TLAS
        tlas_add_instance(tlas_.get(), instance);
        
        // Store in our vector for cleanup
        instances.emplace_back(instance, bvh_instance_destroy);
    }
    
    // Build TLAS
    if (tlas_->instance_count > 0) {
        tlas_build(tlas_.get());
    }
    
    textures_dirty_ = true; // Mark textures for regeneration
}

int TLASManager::get_instance_count() const {
    return tlas_ ? tlas_->instance_count : 0;
}

int TLASManager::get_node_count() const {
    return tlas_ ? tlas_->node_count : 0;
}

void TLASManager::generate_instance_texture_data(const BLASManager& /* blas_manager */,
                                                std::vector<float>& output_data, 
                                                int texture_width,
                                                int texture_height) const {
    PROFILE_SECTION("TLAS Instance Texture Generation");
    
    if (!tlas_) return;
    
    output_data.clear();
    output_data.resize(texture_width * texture_height * 4, 0.0f);
    
    for (int i = 0; i < tlas_->instance_count; i++) {
        const BVHInstance& inst = tlas_->instances[i];
        int baseIdx = i * 4;
        
        // Rows 0-3: transform matrix (4x4)
        for (int row = 0; row < 4; row++) {
            int rowIdx = texture_width * (row * 4) + baseIdx;
            if (rowIdx + 3 < static_cast<int>(output_data.size())) {
                output_data[rowIdx + 0] = inst.transform.m[row * 4 + 0];
                output_data[rowIdx + 1] = inst.transform.m[row * 4 + 1];
                output_data[rowIdx + 2] = inst.transform.m[row * 4 + 2];
                output_data[rowIdx + 3] = inst.transform.m[row * 4 + 3];
            }
        }
        
        // Rows 4-7: inverse transform matrix (4x4)
        for (int row = 0; row < 4; row++) {
            int rowIdx = texture_width * ((row + 4) * 4) + baseIdx;
            if (rowIdx + 3 < static_cast<int>(output_data.size())) {
                output_data[rowIdx + 0] = inst.inv_transform.m[row * 4 + 0];
                output_data[rowIdx + 1] = inst.inv_transform.m[row * 4 + 1];
                output_data[rowIdx + 2] = inst.inv_transform.m[row * 4 + 2];
                output_data[rowIdx + 3] = inst.inv_transform.m[row * 4 + 3];
            }
        }
        
        // Row 8: metadata (blasStartIndex + instanceId + padding)
        int metadataIdx = texture_width * (8 * 4) + baseIdx;
        if (metadataIdx + 3 < static_cast<int>(output_data.size())) {
            output_data[metadataIdx + 0] = static_cast<float>(inst.blas_start_index);
            output_data[metadataIdx + 1] = static_cast<float>(inst.instance_id);
            output_data[metadataIdx + 2] = 0.0f; // padding
            output_data[metadataIdx + 3] = 0.0f; // padding
        }
    }
}

void TLASManager::generate_node_texture_data(std::vector<float>& output_data,
                                            int texture_width, 
                                            int texture_height) const {
    PROFILE_SECTION("TLAS Node Texture Generation");
    
    if (!tlas_) return;
    
    output_data.clear();
    output_data.resize(texture_width * texture_height * 4, 0.0f);
    
    for (int i = 0; i < tlas_->node_count; i++) {
        const TLASNode& node = tlas_->nodes[i];
        int baseIdx = i * 4;
        
        // Row 0: aabbMin + leftRight
        if (baseIdx + 3 < static_cast<int>(output_data.size())) {
            output_data[baseIdx + 0] = node.aabb_min.x;
            output_data[baseIdx + 1] = node.aabb_min.y;
            output_data[baseIdx + 2] = node.aabb_min.z;
            output_data[baseIdx + 3] = static_cast<float>(node.left_right);
        }
        
        // Row 1: aabbMax + blasIndex
        int row1Idx = texture_width * 4 + baseIdx;
        if (row1Idx + 3 < static_cast<int>(output_data.size())) {
            output_data[row1Idx + 0] = node.aabb_max.x;
            output_data[row1Idx + 1] = node.aabb_max.y;
            output_data[row1Idx + 2] = node.aabb_max.z;
            output_data[row1Idx + 3] = static_cast<float>(node.blas_index);
        }
        
        // Row 2: padding
        int row2Idx = texture_width * 8 + baseIdx;
        if (row2Idx + 3 < static_cast<int>(output_data.size())) {
            output_data[row2Idx + 0] = 0.0f;
            output_data[row2Idx + 1] = 0.0f;
            output_data[row2Idx + 2] = 0.0f;
            output_data[row2Idx + 3] = 0.0f;
        }
    }
}

void TLASManager::generate_instance_texture_data(const BLASManager& blas_manager,
                                                float* output_data, 
                                                int texture_width,
                                                int texture_height) const {
    if (!output_data) return;
    
    std::vector<float> temp;
    generate_instance_texture_data(blas_manager, temp, texture_width, texture_height);
    std::copy(temp.begin(), temp.end(), output_data);
}

void TLASManager::generate_node_texture_data(float* output_data,
                                            int texture_width, 
                                            int texture_height) const {
    if (!output_data) return;
    
    std::vector<float> temp;
    generate_node_texture_data(temp, texture_width, texture_height);
    std::copy(temp.begin(), temp.end(), output_data);
}

void TLASManager::ensure_gpu_textures_ready(const BLASManager& blas_manager) {
    if (!textures_dirty_) return;
    
    PROFILE_SECTION("TLAS GPU Texture Update");
    
    // Clean up old textures
    if (nodes_texture_.id != 0) UnloadTexture(nodes_texture_);
    if (instances_texture_.id != 0) UnloadTexture(instances_texture_);
    
    // Generate instances texture
    if (get_instance_count() > 0) {
        PROFILE_SECTION("TLAS Instance Texture Generation");
        
        int texture_width = get_instance_count();
        int texture_height = 9; // 9 rows per instance (4 transform + 4 inverse + 1 metadata)
        
        std::vector<float> texture_data(texture_width * texture_height * 4);
        
        // Use existing method to generate the data
        generate_instance_texture_data(blas_manager, texture_data, texture_width, texture_height);
        
        Image instances_image = {
            .data = texture_data.data(),
            .width = texture_width,
            .height = texture_height,
            .format = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32,
            .mipmaps = 1
        };
        
        instances_texture_ = LoadTextureFromImage(instances_image);
        SetTextureFilter(instances_texture_, TEXTURE_FILTER_POINT);
    }
    
    // Generate nodes texture
    if (get_node_count() > 0) {
        PROFILE_SECTION("TLAS Node Texture Generation");
        
        int texture_width = get_node_count();
        int texture_height = 3; // 3 rows per node
        
        std::vector<float> texture_data(texture_width * texture_height * 4);
        
        // Use existing method to generate the data
        generate_node_texture_data(texture_data, texture_width, texture_height);
        
        Image tlas_image = {
            .data = texture_data.data(),
            .width = texture_width,
            .height = texture_height,
            .format = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32,
            .mipmaps = 1
        };
        
        nodes_texture_ = LoadTextureFromImage(tlas_image);
        SetTextureFilter(nodes_texture_, TEXTURE_FILTER_POINT);
    }
    
    textures_dirty_ = false;
}

void TLASManager::bind_to_shader(Shader shader, const BLASManager& blas_manager) const {
    PROFILE_SECTION("TLAS Shader Binding");
    
    // Ensure textures are ready
    const_cast<TLASManager*>(this)->ensure_gpu_textures_ready(blas_manager);
    
    // Get uniform locations
    int tlas_node_count_loc = GetShaderLocation(shader, "tlasNodeCount");
    int instance_count_loc = GetShaderLocation(shader, "instanceCount");
    int tlas_nodes_texture_loc = GetShaderLocation(shader, "tlasNodesTexture");
    int instances_texture_loc = GetShaderLocation(shader, "instancesTexture");
    
    // Set counts
    int node_count = get_node_count();
    int inst_count = get_instance_count();
    SetShaderValue(shader, tlas_node_count_loc, &node_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, instance_count_loc, &inst_count, SHADER_UNIFORM_INT);
    
    // Bind textures
    if (nodes_texture_.id != 0 && tlas_nodes_texture_loc != -1) {
        SetShaderValueTexture(shader, tlas_nodes_texture_loc, nodes_texture_);
    }
    if (instances_texture_.id != 0 && instances_texture_loc != -1) {
        SetShaderValueTexture(shader, instances_texture_loc, instances_texture_);
    }
}

void TLASManager::print_stats() const {
    printf("=== TLAS Manager Statistics ===\n");
    printf("Draw records: %zu/%d\n", draw_records_.size(), max_instances_);
    printf("Matrix stack depth: %zu\n", matrix_stack_.size());
    printf("Next instance ID: %u\n", next_instance_id_);
    
    if (tlas_) {
        printf("Built TLAS: %d instances, %d nodes\n", 
               tlas_->instance_count, tlas_->node_count);
    } else {
        printf("TLAS: Not built\n");
    }
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