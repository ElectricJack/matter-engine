#include "../include/tlas_manager.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>

// Tiled-texture width cap: TLAS instance/node data wider than this wraps into
// extra vertical tile rows so the texture width never exceeds GL_MAX_TEXTURE_SIZE
// (mirrors BLASManager::TEXTURE_TILE_WIDTH). For counts <= this, tiles_y == 1 and
// the layout is byte-identical to the old single-row form, so small scenes (and
// shaders that have not adopted tiledTexel) are unaffected.
static constexpr int kTlasTileWidth = 8192;

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
    
    // Clean up GPU textures
    if (nodes_texture_.id != 0) UnloadTexture(nodes_texture_);
    if (instances_texture_.id != 0) UnloadTexture(instances_texture_);
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
    active_draw_records_.clear(); // B4 fix: reset compacted record list
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
    
    // B4 fix: build a compacted list of draw records that have a valid BLAS so that
    // active_draw_records_[i] corresponds exactly to tlas_->blas[i].
    active_draw_records_.clear();

    // Create BVH instances from draw records (using unique_ptr approach for now)
    instances_.reserve(draw_records_.size());
    std::vector<BVHInstance*> instance_ptrs;
    instance_ptrs.reserve(draw_records_.size());

    for (const auto& record : draw_records_) {
        // Get BVH from manager
        BVH* bvh = blas_manager.get_bvh(record.blas_handle);
        if (!bvh) {
            printf("Warning: BLAS handle %u not found in BLAS manager\n", record.blas_handle);
            continue; // skip: do NOT add to active_draw_records_
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
        active_draw_records_.push_back(record); // B4: keep parallel with blas[]
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

void TLASManager::generate_instance_texture_data(const BLASManager& blas_manager,
                                                std::vector<float>& output_data, 
                                                int texture_width,
                                                int texture_height) const {
    if (!tlas_) return;
    
    output_data.clear();
    output_data.resize(texture_width * texture_height * 4, 0.0f);

    // Tiled texel offset: 9 rows per instance, tiled into vertical rows of
    // `texture_width` instances each (see kTlasTileWidth). For instance counts
    // <= texture_width this collapses to the old single-row layout.
    auto texel_off = [tw = texture_width](int idx, int row) {
        int tx = idx % tw, ty = idx / tw;
        return ((ty * 9 + row) * tw + tx) * 4;
    };

    for (int i = 0; i < static_cast<int>(tlas_->blasCount); i++) {
        const BVHInstance& inst = tlas_->blas[i];

        // Get transform matrix once for this instance
        mat4& transform_matrix = const_cast<BVHInstance&>(inst).GetTransform();

        // Rows 0-3: transform matrix (4x4)
        for (int row = 0; row < 4; row++) {
            int rowIdx = texel_off(i, row);
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
            int rowIdx = texel_off(i, row + 4);
            if (rowIdx + 3 < static_cast<int>(output_data.size())) {
                output_data[rowIdx + 0] = inv_transform.cell[row * 4 + 0];
                output_data[rowIdx + 1] = inv_transform.cell[row * 4 + 1];
                output_data[rowIdx + 2] = inv_transform.cell[row * 4 + 2];
                output_data[rowIdx + 3] = inv_transform.cell[row * 4 + 3];
            }
        }

        // Row 8: metadata (blasIndex + materialId + padding)
        // B4 fix: use active_draw_records_ (compacted parallel to tlas_->blas[])
        // instead of draw_records_ (which may have gaps from skipped BLAS handles).
        int metadataIdx = texel_off(i, 8);
        if (metadataIdx + 3 < static_cast<int>(output_data.size())) {
            BLASHandle blas_handle = i < static_cast<int>(active_draw_records_.size()) ?
                                   active_draw_records_[i].blas_handle : INVALID_BLAS_HANDLE;
            BLASOffsets offsets = blas_manager.get_offsets(blas_handle);
            output_data[metadataIdx + 0] = static_cast<float>(offsets.node_offset);

            uint32_t materialId = 0;
            bool is_imposter = false;
            if (i < static_cast<int>(active_draw_records_.size())) {
                materialId   = active_draw_records_[i].material_id;
                is_imposter  = active_draw_records_[i].is_imposter;
            }
            output_data[metadataIdx + 1] = static_cast<float>(materialId);
            output_data[metadataIdx + 2] = is_imposter ? 1.0f : 0.0f;
            output_data[metadataIdx + 3] = 0.0f; // reserved
        }
    }
}

void TLASManager::generate_node_texture_data(std::vector<float>& output_data,
                                            int texture_width, 
                                            int texture_height) const {
    if (!tlas_) return;
    
    output_data.clear();
    output_data.resize(texture_width * texture_height * 4, 0.0f);

    // Tiled texel offset: 3 rows per node, tiled into vertical rows of
    // `texture_width` nodes each (see kTlasTileWidth). For node counts
    // <= texture_width this collapses to the old single-row layout.
    auto texel_off = [tw = texture_width](int idx, int row) {
        int tx = idx % tw, ty = idx / tw;
        return ((ty * 3 + row) * tw + tx) * 4;
    };

    for (int i = 0; i < static_cast<int>(tlas_->nodesUsed); i++) {
        const TLASNode& node = tlas_->tlasNode[i];

        // Child indices are stored as SEPARATE floats (row0.w = left, row2.w = right)
        // rather than bit-packed into one float: a packed left|(right<<16) value
        // exceeds 2^24 once a child index reaches 256, and float32 cannot represent
        // such integers exactly, which corrupted the low (left) bits during traversal.
        uint32_t leftChild  = node.leftRight & 0xFFFFu;
        uint32_t rightChild = (node.leftRight >> 16) & 0xFFFFu;
        // 16-bit packing overflow is guarded at pack time (TLAS::BuildRecursive
        // in bvh.cpp); values extracted here are masked and cannot exceed 0xFFFF.

        // Row 0: aabbMin + leftChild (0 for leaf nodes, which acts as the leaf sentinel)
        int row0Idx = texel_off(i, 0);
        if (row0Idx + 3 < static_cast<int>(output_data.size())) {
            output_data[row0Idx + 0] = node.aabbMin.x;
            output_data[row0Idx + 1] = node.aabbMin.y;
            output_data[row0Idx + 2] = node.aabbMin.z;
            output_data[row0Idx + 3] = static_cast<float>(leftChild);
        }

        // Row 1: aabbMax + blasIndex
        int row1Idx = texel_off(i, 1);
        if (row1Idx + 3 < static_cast<int>(output_data.size())) {
            output_data[row1Idx + 0] = node.aabbMax.x;
            output_data[row1Idx + 1] = node.aabbMax.y;
            output_data[row1Idx + 2] = node.aabbMax.z;
            output_data[row1Idx + 3] = static_cast<float>(node.BLAS);

            // TLAS node data set
        }

        // Row 2: rightChild in .w
        int row2Idx = texel_off(i, 2);
        if (row2Idx + 3 < static_cast<int>(output_data.size())) {
            output_data[row2Idx + 0] = 0.0f;
            output_data[row2Idx + 1] = 0.0f;
            output_data[row2Idx + 2] = 0.0f;
            output_data[row2Idx + 3] = static_cast<float>(rightChild);
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
        // Tile wide instance data into vertical tile-rows so the texture width never
        // exceeds GL_MAX_TEXTURE_SIZE. For counts <= kTlasTileWidth, tiles_y == 1 and
        // the layout is byte-identical to the old single-row form.
        const int total = get_instance_count();
        const int tile_w = std::min(total, kTlasTileWidth);
        const int tiles_y = (total + tile_w - 1) / tile_w;
        int texture_width = tile_w;
        int texture_height = tiles_y * 9; // 9 rows per instance (4 transform + 4 inverse + 1 metadata)

        std::vector<float> texture_data(texture_width * texture_height * 4);
        
        // Use existing method to generate the data
        generate_instance_texture_data(blas_manager, texture_data, texture_width, texture_height);
        
        Image instances_image = {
            .data = texture_data.data(),
            .width = texture_width,
            .height = texture_height,
            .mipmaps = 1,
            .format = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32
        };
        
        instances_texture_ = LoadTextureFromImage(instances_image);
        SetTextureFilter(instances_texture_, TEXTURE_FILTER_POINT);
    }

    // Generate nodes texture
    if (get_node_count() > 0) {
        // Tile wide node data into vertical tile-rows so the texture width never
        // exceeds GL_MAX_TEXTURE_SIZE. For counts <= kTlasTileWidth, tiles_y == 1 and
        // the layout is byte-identical to the old single-row form.
        const int total = get_node_count();
        const int tile_w = std::min(total, kTlasTileWidth);
        const int tiles_y = (total + tile_w - 1) / tile_w;
        int texture_width = tile_w;
        int texture_height = tiles_y * 3; // 3 rows per node

        std::vector<float> texture_data(texture_width * texture_height * 4);
        
        // Use existing method to generate the data
        generate_node_texture_data(texture_data, texture_width, texture_height);
        
        Image tlas_image = {
            .data = texture_data.data(),
            .width = texture_width,
            .height = texture_height,
            .mipmaps = 1,
            .format = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32
        };
        
        nodes_texture_ = LoadTextureFromImage(tlas_image);
        SetTextureFilter(nodes_texture_, TEXTURE_FILTER_POINT);
    }

    textures_dirty_ = false;
}

void TLASManager::bind_to_shader(Shader shader, const BLASManager& blas_manager) const {
    PROFILE_SECTION("TLAS Shader Binding");
    
    // Check if shader has changed and cache locations if needed
    bool shader_changed = (cached_shader_id_ != shader.id);
    if (shader_changed) {
        PROFILE_SECTION("Cache Shader Locations");
        cached_shader_id_ = shader.id;
        
        // Cache uniform locations (these are expensive calls)
        tlas_node_count_loc_   = GetShaderLocation(shader, "tlasNodeCount");
        instance_count_loc_    = GetShaderLocation(shader, "instanceCount");
        tlas_nodes_texture_loc_ = GetShaderLocation(shader, "tlasNodesTexture");
        instances_texture_loc_ = GetShaderLocation(shader, "instancesTexture");
        
        // Force update shader values when shader changes
        shader_values_dirty_ = true;
    }
    
    // Only ensure textures are ready if they're dirty
    bool textures_were_updated = textures_dirty_;
    if (textures_dirty_) {
        PROFILE_SECTION("Update GPU Textures");
        const_cast<TLASManager*>(this)->ensure_gpu_textures_ready(blas_manager);
    }
    
    // Only update shader values if they're dirty or shader changed
    if (shader_values_dirty_) {
        PROFILE_SECTION("Update Shader Values");

        // Set counts
        int node_count = get_node_count();
        int inst_count = get_instance_count();
        if (tlas_node_count_loc_ != -1) {
            SetShaderValue(shader, tlas_node_count_loc_, &node_count, SHADER_UNIFORM_INT);
        }
        if (instance_count_loc_ != -1) {
            SetShaderValue(shader, instance_count_loc_, &inst_count, SHADER_UNIFORM_INT);
        }
        
        shader_values_dirty_ = false;
    }
    
    // Stage textures every frame; see BLASManager::bind_to_shader for why the
    // "bind only when dirty" optimization is unsafe under raylib's per-draw
    // sampler-slot reset.
    (void)textures_were_updated;
    if (nodes_texture_.id != 0 && tlas_nodes_texture_loc_ != -1) {
        SetShaderValueTexture(shader, tlas_nodes_texture_loc_, nodes_texture_);
    }
    if (instances_texture_.id != 0 && instances_texture_loc_ != -1) {
        SetShaderValueTexture(shader, instances_texture_loc_, instances_texture_);
    }
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