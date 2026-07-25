#include "../include/tlas_manager.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>

// Conversion utilities
mat4 TLASManager::convert_matrix(const Matrix4x4& legacy_matrix) {
    mat4 new_matrix;
    for (int i = 0; i < 16; i++) {
        new_matrix.cell[i] = legacy_matrix.m[i];
    }
    return new_matrix;
}

Matrix4x4 TLASManager::convert_matrix_back(const mat4& new_matrix) {
    Matrix4x4 legacy_matrix;
    for (int i = 0; i < 16; i++) {
        legacy_matrix.m[i] = new_matrix.cell[i];
    }
    return legacy_matrix;
}

// Helper functions for matrix operations
Matrix4x4 matrix_identity() {
    return Matrix4x4(); // Default constructor creates identity
}

Matrix4x4 matrix_multiply(const Matrix4x4* a, const Matrix4x4* b) {
    Matrix4x4 result;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            result.m[i * 4 + j] = 0.0f;
            for (int k = 0; k < 4; k++) {
                result.m[i * 4 + j] += a->m[i * 4 + k] * b->m[k * 4 + j];
            }
        }
    }
    return result;
}

Matrix4x4 matrix_translation(float x, float y, float z) {
    Matrix4x4 m;
    // Use column-major layout: translation goes in positions [3], [7], [11]
    // Matrix layout: [0  4  8  12]
    //                [1  5  9  13] 
    //                [2  6  10 14]
    //                [3  7  11 15]
    // Translation should be in the last column: [3], [7], [11]
    m.m[3] = x; m.m[7] = y; m.m[11] = z;
    return m;
}

Matrix4x4 matrix_scale(float x, float y, float z) {
    Matrix4x4 m;
    m.m[0] = x; m.m[5] = y; m.m[10] = z;
    return m;
}

Matrix4x4 matrix_rotation_x(float angle) {
    Matrix4x4 m;
    float c = std::cos(angle), s = std::sin(angle);
    m.m[5] = c; m.m[6] = -s;
    m.m[9] = s; m.m[10] = c;
    return m;
}

Matrix4x4 matrix_rotation_y(float angle) {
    Matrix4x4 m;
    float c = std::cos(angle), s = std::sin(angle);
    m.m[0] = c; m.m[2] = s;
    m.m[8] = -s; m.m[10] = c;
    return m;
}

Matrix4x4 matrix_rotation_z(float angle) {
    Matrix4x4 m;
    float c = std::cos(angle), s = std::sin(angle);
    m.m[0] = c; m.m[1] = -s;
    m.m[4] = s; m.m[5] = c;
    return m;
}

Matrix4x4 matrix_rotation_axis(const float3& axis, float angle) {
    // Rodrigues' rotation formula implementation
    Matrix4x4 m;
    float c = std::cos(angle), s = std::sin(angle);
    float3 n = normalize(axis);
    
    m.m[0] = c + n.x * n.x * (1 - c);
    m.m[1] = n.x * n.y * (1 - c) - n.z * s;
    m.m[2] = n.x * n.z * (1 - c) + n.y * s;
    
    m.m[4] = n.y * n.x * (1 - c) + n.z * s;
    m.m[5] = c + n.y * n.y * (1 - c);
    m.m[6] = n.y * n.z * (1 - c) - n.x * s;
    
    m.m[8] = n.z * n.x * (1 - c) - n.y * s;
    m.m[9] = n.z * n.y * (1 - c) + n.x * s;
    m.m[10] = c + n.z * n.z * (1 - c);
    
    return m;
}

TLASManager::TLASManager(int max_instances) 
    : tlas_(nullptr), next_instance_id_(1), max_instances_(max_instances) {
    
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

void TLASManager::translate(const float3& translation) {
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

void TLASManager::rotate_axis(const float3& axis, float angle_radians) {
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
    draw_records_.clear();
    active_records_.clear();
    next_instance_id_ = 1;

    // Reset matrix stack to just identity
    while (matrix_stack_.size() > 1) {
        matrix_stack_.pop();
    }
    load_identity();

    // Clean up existing TLAS and instances
    tlas_.reset(nullptr);
    instances_.clear();
    instance_storage_.clear();

    textures_dirty_ = true; // Mark textures for regeneration
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

    // Create BVH instances from draw records, compacting out any with missing BLAS.
    // active_records_ is kept in sync with instance_storage_/tlas_->blas so that
    // generate_instance_texture_data can index them in parallel.
    instances_.reserve(draw_records_.size());
    active_records_.clear();
    active_records_.reserve(draw_records_.size());
    std::vector<BVHInstance*> instance_ptrs;
    instance_ptrs.reserve(draw_records_.size());

    for (const auto& record : draw_records_) {
        // Get BVH from manager
        BVH* bvh = blas_manager.get_bvh(record.blas_handle);
        if (!bvh) {
            printf("Warning: BLAS handle %u not found in BLAS manager\n", record.blas_handle);
            continue;
        }

        // Create BVH instance
        auto instance = std::make_unique<BVHInstance>(bvh, record.instance_id);

        // Convert and set transform - this will also calculate world bounds
        mat4 new_transform = convert_matrix(record.transform);
        instance->SetTransform(new_transform);

        // Add to our vectors (keep active_records_ in sync with instance list)
        active_records_.push_back(record);
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

    textures_dirty_ = true; // Mark textures for regeneration
}

int TLASManager::get_instance_count() const {
    return tlas_ ? tlas_->blasCount : 0;
}

int TLASManager::get_node_count() const {
    return tlas_ ? tlas_->nodesUsed : 0;
}

void TLASManager::generate_instance_texture_data(const BLASManager& blas_manager,
                                                std::vector<float>& output_data, 
                                                int texture_width,
                                                int texture_height) const {
    if (!tlas_) return;
    
    output_data.clear();
    output_data.resize(texture_width * texture_height * 4, 0.0f);
    
    for (int i = 0; i < static_cast<int>(tlas_->blasCount); i++) {
        const BVHInstance& inst = tlas_->blas[i];
        int baseIdx = i * 4;
        
        // Get transform matrix once for this instance
        mat4& transform_matrix = const_cast<BVHInstance&>(inst).GetTransform();
        
        // Rows 0-3: transform matrix (4x4)
        for (int row = 0; row < 4; row++) {
            int rowIdx = texture_width * (row * 4) + baseIdx;
            if (rowIdx + 3 < static_cast<int>(output_data.size())) {
                output_data[rowIdx + 0] = transform_matrix.cell[row * 4 + 0];
                output_data[rowIdx + 1] = transform_matrix.cell[row * 4 + 1];
                output_data[rowIdx + 2] = transform_matrix.cell[row * 4 + 2];
                output_data[rowIdx + 3] = transform_matrix.cell[row * 4 + 3];
            }
        }
        
        // Rows 4-7: inverse transform matrix (4x4)
        // Get the actual inverse transform from the BVHInstance
        mat4 inv_transform = const_cast<BVHInstance&>(inst).GetInvTransform();
        for (int row = 0; row < 4; row++) {
            int rowIdx = texture_width * ((row + 4) * 4) + baseIdx;
            if (rowIdx + 3 < static_cast<int>(output_data.size())) {
                output_data[rowIdx + 0] = inv_transform.cell[row * 4 + 0];
                output_data[rowIdx + 1] = inv_transform.cell[row * 4 + 1];
                output_data[rowIdx + 2] = inv_transform.cell[row * 4 + 2];
                output_data[rowIdx + 3] = inv_transform.cell[row * 4 + 3];
            }
        }
        
        // Row 8: metadata (blasIndex + materialId + padding)
        int metadataIdx = texture_width * (8 * 4) + baseIdx;
        if (metadataIdx + 3 < static_cast<int>(output_data.size())) {
            // Get the actual BLAS node start offset for this instance.
            // Use active_records_ which is compacted to match tlas_->blas[i] exactly.
            BLASHandle blas_handle = i < static_cast<int>(active_records_.size()) ?
                                   active_records_[i].blas_handle : INVALID_BLAS_HANDLE;
            BLASOffsets offsets = blas_manager.get_offsets(blas_handle);
            output_data[metadataIdx + 0] = static_cast<float>(offsets.node_offset);

            // Get material ID from the compacted active record (not raw draw_records_[i])
            uint32_t materialId = 0;
            if (i < static_cast<int>(active_records_.size())) {
                materialId = active_records_[i].material_id;
            }
            output_data[metadataIdx + 1] = static_cast<float>(materialId);
            output_data[metadataIdx + 2] = 0.0f; // padding
            output_data[metadataIdx + 3] = 0.0f; // padding
        }
    }
}

void TLASManager::generate_node_texture_data(std::vector<float>& output_data,
                                            int texture_width, 
                                            int texture_height) const {
    if (!tlas_) return;
    
    output_data.clear();
    output_data.resize(texture_width * texture_height * 4, 0.0f);
    
    for (int i = 0; i < static_cast<int>(tlas_->nodesUsed); i++) {
        const TLASNode& node = tlas_->tlasNode[i];
        int baseIdx = i * 4;
        
        // Row 0: aabbMin + leftRight
        if (baseIdx + 3 < static_cast<int>(output_data.size())) {
            output_data[baseIdx + 0] = node.aabbMin.x;
            output_data[baseIdx + 1] = node.aabbMin.y;
            output_data[baseIdx + 2] = node.aabbMin.z;
            output_data[baseIdx + 3] = static_cast<float>(node.leftRight);
        }
        
        // Row 1: aabbMax + blasIndex
        int row1Idx = texture_width * 4 + baseIdx;
        if (row1Idx + 3 < static_cast<int>(output_data.size())) {
            output_data[row1Idx + 0] = node.aabbMax.x;
            output_data[row1Idx + 1] = node.aabbMax.y;
            output_data[row1Idx + 2] = node.aabbMax.z;
            output_data[row1Idx + 3] = static_cast<float>(node.BLAS);
            
            // TLAS node data set
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

    // --- instances texture ---
    {
        int inst_count = get_instance_count();
        const int INST_HEIGHT = 9; // 9 rows per instance (4 transform + 4 inverse + 1 metadata)

        if (inst_count > 0) {
            std::vector<float> texture_data(inst_count * INST_HEIGHT * 4);
            generate_instance_texture_data(blas_manager, texture_data, inst_count, INST_HEIGHT);

            if (instances_texture_.id != 0 && instances_texture_cap_ == inst_count) {
                // Same dimensions as last frame: reuse GPU texture, just push new data.
                // UpdateTexture requires data to be exactly width*height*format_bytes.
                UpdateTexture(instances_texture_, texture_data.data());
            } else {
                // Count changed or first allocation: (re)create the texture.
                if (instances_texture_.id != 0) UnloadTexture(instances_texture_);
                Image img = {
                    .data = texture_data.data(),
                    .width = inst_count,
                    .height = INST_HEIGHT,
                    .mipmaps = 1,
                    .format = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32
                };
                instances_texture_ = LoadTextureFromImage(img);
                SetTextureFilter(instances_texture_, TEXTURE_FILTER_POINT);
                instances_texture_cap_ = inst_count;
            }
        }
    }

    // --- nodes texture ---
    {
        int node_count = get_node_count();
        const int NODE_HEIGHT = 3; // 3 rows per node

        if (node_count > 0) {
            std::vector<float> texture_data(node_count * NODE_HEIGHT * 4);
            generate_node_texture_data(texture_data, node_count, NODE_HEIGHT);

            if (nodes_texture_.id != 0 && nodes_texture_cap_ == node_count) {
                UpdateTexture(nodes_texture_, texture_data.data());
            } else {
                if (nodes_texture_.id != 0) UnloadTexture(nodes_texture_);
                Image img = {
                    .data = texture_data.data(),
                    .width = node_count,
                    .height = NODE_HEIGHT,
                    .mipmaps = 1,
                    .format = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32
                };
                nodes_texture_ = LoadTextureFromImage(img);
                SetTextureFilter(nodes_texture_, TEXTURE_FILTER_POINT);
                nodes_texture_cap_ = node_count;
            }
        }
    }

    textures_dirty_ = false;
}

void TLASManager::bind_to_shader(Shader shader, const BLASManager& blas_manager) const {
    PROFILE_SECTION("TLAS Shader Binding");

    // Ensure textures are ready
    const_cast<TLASManager*>(this)->ensure_gpu_textures_ready(blas_manager);

    // Cache uniform locations once per shader (identified by shader.id)
    if (cached_shader_id_ != static_cast<int>(shader.id)) {
        cached_shader_id_        = static_cast<int>(shader.id);
        loc_tlas_node_count_     = GetShaderLocation(shader, "tlasNodeCount");
        loc_instance_count_      = GetShaderLocation(shader, "instanceCount");
        loc_tlas_nodes_texture_  = GetShaderLocation(shader, "tlasNodesTexture");
        loc_instances_texture_   = GetShaderLocation(shader, "instancesTexture");
    }

    // Set counts
    int node_count = get_node_count();
    int inst_count = get_instance_count();

    SetShaderValue(shader, loc_tlas_node_count_, &node_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, loc_instance_count_,  &inst_count, SHADER_UNIFORM_INT);

    // Bind textures
    if (nodes_texture_.id != 0 && loc_tlas_nodes_texture_ != -1) {
        SetShaderValueTexture(shader, loc_tlas_nodes_texture_, nodes_texture_);
    }

    if (instances_texture_.id != 0 && loc_instances_texture_ != -1) {
        SetShaderValueTexture(shader, loc_instances_texture_, instances_texture_);
    }
}

void TLASManager::print_stats() const {
    printf("=== TLAS Manager Statistics ===\n");
    printf("Draw records: %zu/%d\n", draw_records_.size(), max_instances_);
    printf("Matrix stack depth: %zu\n", matrix_stack_.size());
    printf("Next instance ID: %u\n", next_instance_id_);
    
    if (tlas_) {
        printf("Built TLAS: %d instances, %d nodes\n", 
               tlas_->blasCount, tlas_->nodesUsed);
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