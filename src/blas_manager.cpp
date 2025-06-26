#include "../include/blas_manager.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>

BLASManager::BLASManager() 
    : next_handle_(1), cached_total_triangles_(0), cached_total_nodes_(0), totals_dirty_(true) {
}

BLASManager::~BLASManager() {
    // Clean up GPU textures
    if (triangles_texture_.id != 0) UnloadTexture(triangles_texture_);
    if (nodes_texture_.id != 0) UnloadTexture(nodes_texture_);
}

uint32_t BLASManager::calculate_hash(const Triangle* triangles, int count) const {
    PROFILE_SECTION("BLAS Hash Calculation");
    
    uint32_t hash = 2166136261u; // FNV-1a offset basis
    
    for (int i = 0; i < count; i++) {
        // Hash vertex positions only (ignore normals/materials for deduplication)
        const float* data = reinterpret_cast<const float*>(&triangles[i]);
        for (int j = 0; j < 9; j++) { // 3 vertices * 3 components each
            uint32_t val = *reinterpret_cast<const uint32_t*>(&data[j]);
            hash ^= val;
            hash *= 16777619u; // FNV-1a prime
        }
    }
    
    return hash;
}

bool BLASManager::triangles_equal(const std::vector<Triangle>& a, const Triangle* b, int count) const {
    if (a.size() != static_cast<size_t>(count)) return false;
    
    for (int i = 0; i < count; i++) {
        if (std::memcmp(&a[i].v0, &b[i].v0, sizeof(Vec3) * 3) != 0) {
            return false;
        }
    }
    return true;
}

BLASHandle BLASManager::find_existing_blas(const Triangle* triangles, int count, uint32_t hash) const {
    PROFILE_SECTION("BLAS Deduplication Check");
    
    auto range = hash_to_entry_.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it) {
        const auto& entry = entries_[it->second];
        if (triangles_equal(entry->triangles, triangles, count)) {
            return entry->handle;
        }
    }
    return INVALID_BLAS_HANDLE;
}

BLASHandle BLASManager::register_triangles(const std::vector<Triangle>& triangles, 
                                          int max_triangles_per_leaf) {
    return register_triangles(const_cast<Triangle*>(triangles.data()), 
                             static_cast<int>(triangles.size()), max_triangles_per_leaf);
}

BLASHandle BLASManager::register_triangles(Triangle* triangles, 
                                          int triangle_count,
                                          int max_triangles_per_leaf) {
    PROFILE_SECTION("BLAS Registration");
    
    if (!triangles || triangle_count <= 0) {
        return INVALID_BLAS_HANDLE;
    }
    
    // Calculate hash for deduplication
    uint32_t hash = calculate_hash(triangles, triangle_count);
    
    // Check if BLAS already exists
    BLASHandle existing = find_existing_blas(triangles, triangle_count, hash);
    if (existing != INVALID_BLAS_HANDLE) {
        return existing;
    }
    
    // Create new BLAS
    {
        PROFILE_SECTION("BLAS Creation");
        
        // Copy triangle data
        std::vector<Triangle> triangle_copy(triangles, triangles + triangle_count);
        
        // Create BLAS
        BLAS* blas = blas_create(triangle_copy.data(), triangle_count, max_triangles_per_leaf);
        if (!blas) {
            return INVALID_BLAS_HANDLE;
        }
        
        blas_build(blas);
        
        BLASHandle handle = next_handle_++;
        
        // Create entry
        auto entry = std::make_unique<BLASEntry>(handle, blas, std::move(triangle_copy), hash);
        
        // Add to hash table
        size_t entry_index = entries_.size();
        hash_to_entry_.emplace(hash, entry_index);
        
        // Add to entries
        entries_.push_back(std::move(entry));
        
        totals_dirty_ = true;
        textures_dirty_ = true; // Mark textures for regeneration
        return handle;
    }
}

bool BLASManager::has_blas(BLASHandle handle) const {
    if (handle == INVALID_BLAS_HANDLE) return false;
    
    return std::any_of(entries_.begin(), entries_.end(),
        [handle](const auto& entry) { return entry->handle == handle; });
}

BLAS* BLASManager::get_blas(BLASHandle handle) const {
    if (handle == INVALID_BLAS_HANDLE) return nullptr;
    
    auto it = std::find_if(entries_.begin(), entries_.end(),
        [handle](const auto& entry) { return entry->handle == handle; });
    
    return (it != entries_.end()) ? (*it)->blas.get() : nullptr;
}

void BLASManager::update_totals() const {
    if (!totals_dirty_) return;
    
    PROFILE_SECTION("BLAS Total Calculation");
    
    cached_total_triangles_ = 0;
    cached_total_nodes_ = 0;
    
    for (const auto& entry : entries_) {
        if (entry->blas) {
            cached_total_triangles_ += entry->blas->triangle_count;
            cached_total_nodes_ += entry->blas->node_count;
        }
    }
    
    totals_dirty_ = false;
}

int BLASManager::get_total_triangle_count() const {
    update_totals();
    return cached_total_triangles_;
}

int BLASManager::get_total_node_count() const {
    update_totals();
    return cached_total_nodes_;
}

BLASOffsets BLASManager::get_offsets(BLASHandle handle) const {
    BLASOffsets offsets{0, 0};
    if (handle == INVALID_BLAS_HANDLE) return offsets;
    
    int triangle_offset = 0;
    int node_offset = 0;
    
    for (const auto& entry : entries_) {
        if (entry->handle == handle) {
            offsets.triangle_offset = triangle_offset;
            offsets.node_offset = node_offset;
            return offsets;
        }
        
        if (entry->blas) {
            triangle_offset += entry->blas->triangle_count;
            node_offset += entry->blas->node_count;
        }
    }
    
    return offsets; // Not found
}

void BLASManager::generate_triangle_data(std::vector<Triangle>& output_triangles) const {
    PROFILE_SECTION("BLAS Triangle Data Generation");
    
    output_triangles.clear();
    output_triangles.reserve(get_total_triangle_count());
    
    for (const auto& entry : entries_) {
        if (entry->blas) {
            output_triangles.insert(output_triangles.end(), 
                                   entry->triangles.begin(), 
                                   entry->triangles.end());
        }
    }
}

void BLASManager::generate_node_data(std::vector<BVHNode>& output_nodes) const {
    PROFILE_SECTION("BLAS Node Data Generation");
    
    output_nodes.clear();
    output_nodes.reserve(get_total_node_count());
    
    int node_offset = 0;
    int triangle_offset = 0;
    
    for (const auto& entry : entries_) {
        if (entry->blas) {
            // Copy nodes and adjust indices
            for (int j = 0; j < entry->blas->node_count; j++) {
                BVHNode node = entry->blas->nodes[j];
                
                if (node.tri_count > 0) {
                    // Leaf node - adjust triangle indices
                    node.left_first += triangle_offset;
                } else {
                    // Internal node - adjust child node indices
                    node.left_first += node_offset;
                }
                
                output_nodes.push_back(node);
            }
            
            node_offset += entry->blas->node_count;
            triangle_offset += entry->blas->triangle_count;
        }
    }
}

void BLASManager::generate_triangle_texture_data(Triangle* output_triangles) const {
    if (!output_triangles) return;
    
    std::vector<Triangle> temp;
    generate_triangle_data(temp);
    std::copy(temp.begin(), temp.end(), output_triangles);
}

void BLASManager::generate_node_texture_data(BVHNode* output_nodes) const {
    if (!output_nodes) return;
    
    std::vector<BVHNode> temp;
    generate_node_data(temp);
    std::copy(temp.begin(), temp.end(), output_nodes);
}

void BLASManager::ensure_gpu_textures_ready() {
    if (!textures_dirty_) return;
    
    PROFILE_SECTION("BLAS GPU Texture Update");
    
    // Clean up old textures
    if (triangles_texture_.id != 0) UnloadTexture(triangles_texture_);
    if (nodes_texture_.id != 0) UnloadTexture(nodes_texture_);
    
    // Generate triangle texture
    {
        PROFILE_SECTION("BLAS Triangle Texture Creation");
        
        std::vector<Triangle> all_triangles;
        generate_triangle_data(all_triangles);
        
        if (!all_triangles.empty()) {
            int texture_width = static_cast<int>(all_triangles.size());
            int texture_height = 4;
            
            std::vector<float> texture_data(texture_width * texture_height * 4);
            
            for (size_t i = 0; i < all_triangles.size(); i++) {
                const Triangle& tri = all_triangles[i];
                int base_idx = static_cast<int>(i) * 4;
                
                // Row 0: v0 + materialId
                texture_data[base_idx + 0] = tri.v0.x;
                texture_data[base_idx + 1] = tri.v0.y;
                texture_data[base_idx + 2] = tri.v0.z;
                texture_data[base_idx + 3] = static_cast<float>(tri.material_id);
                
                // Row 1: v1
                int row1_idx = texture_width * 4 + base_idx;
                texture_data[row1_idx + 0] = tri.v1.x;
                texture_data[row1_idx + 1] = tri.v1.y;
                texture_data[row1_idx + 2] = tri.v1.z;
                texture_data[row1_idx + 3] = 0.0f;
                
                // Row 2: v2
                int row2_idx = texture_width * 8 + base_idx;
                texture_data[row2_idx + 0] = tri.v2.x;
                texture_data[row2_idx + 1] = tri.v2.y;
                texture_data[row2_idx + 2] = tri.v2.z;
                texture_data[row2_idx + 3] = 0.0f;
                
                // Row 3: normal
                int row3_idx = texture_width * 12 + base_idx;
                texture_data[row3_idx + 0] = tri.normal.x;
                texture_data[row3_idx + 1] = tri.normal.y;
                texture_data[row3_idx + 2] = tri.normal.z;
                texture_data[row3_idx + 3] = 0.0f;
            }
            
            Image tri_image = {
                .data = texture_data.data(),
                .width = texture_width,
                .height = texture_height,
                .format = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32,
                .mipmaps = 1
            };
            
            triangles_texture_ = LoadTextureFromImage(tri_image);
            SetTextureFilter(triangles_texture_, TEXTURE_FILTER_POINT);
        }
    }
    
    // Generate nodes texture
    {
        PROFILE_SECTION("BLAS Nodes Texture Creation");
        
        std::vector<BVHNode> all_nodes;
        generate_node_data(all_nodes);
        
        if (!all_nodes.empty()) {
            int texture_width = static_cast<int>(all_nodes.size());
            int texture_height = 3; // 3 rows per node: aabbMin+leftFirst, aabbMax+triCount, padding
            
            std::vector<float> texture_data(texture_width * texture_height * 4);
            
            for (size_t i = 0; i < all_nodes.size(); i++) {
                const BVHNode& node = all_nodes[i];
                int base_idx = static_cast<int>(i) * 4;
                
                // Row 0: aabbMin + leftFirst
                texture_data[base_idx + 0] = node.aabb_min.x;
                texture_data[base_idx + 1] = node.aabb_min.y;
                texture_data[base_idx + 2] = node.aabb_min.z;
                texture_data[base_idx + 3] = static_cast<float>(node.left_first);
                
                // Row 1: aabbMax + triCount
                int row1_idx = texture_width * 4 + base_idx;
                texture_data[row1_idx + 0] = node.aabb_max.x;
                texture_data[row1_idx + 1] = node.aabb_max.y;
                texture_data[row1_idx + 2] = node.aabb_max.z;
                texture_data[row1_idx + 3] = static_cast<float>(node.tri_count);
                
                // Row 2: padding
                int row2_idx = texture_width * 8 + base_idx;
                texture_data[row2_idx + 0] = 0.0f;
                texture_data[row2_idx + 1] = 0.0f;
                texture_data[row2_idx + 2] = 0.0f;
                texture_data[row2_idx + 3] = 0.0f;
            }
            
            Image blas_image = {
                .data = texture_data.data(),
                .width = texture_width,
                .height = texture_height,
                .format = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32,
                .mipmaps = 1
            };
            
            nodes_texture_ = LoadTextureFromImage(blas_image);
            SetTextureFilter(nodes_texture_, TEXTURE_FILTER_POINT);
        }
    }
    
    textures_dirty_ = false;
}

void BLASManager::bind_to_shader(Shader shader) const {
    PROFILE_SECTION("BLAS Shader Binding");
    
    // Ensure textures are ready
    const_cast<BLASManager*>(this)->ensure_gpu_textures_ready();
    
    // Get uniform locations
    int triangle_count_loc     = GetShaderLocation(shader, "triangleCount");
    int blas_node_count_loc    = GetShaderLocation(shader, "blasNodeCount");
    int triangles_texture_loc  = GetShaderLocation(shader, "trianglesTexture");
    int blas_nodes_texture_loc = GetShaderLocation(shader, "blasNodesTexture");
    int intersection_mode_loc  = GetShaderLocation(shader, "intersectionMode");
    
    // Set counts
    int triangle_count = get_total_triangle_count();
    int node_count     = get_total_node_count();
    SetShaderValue(shader, triangle_count_loc,  &triangle_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, blas_node_count_loc, &node_count,     SHADER_UNIFORM_INT);
    
    // Enable BVH traversal
    int intersection_mode = 1;
    SetShaderValue(shader, intersection_mode_loc, &intersection_mode, SHADER_UNIFORM_INT);
    
    // Bind textures
    if (triangles_texture_.id != 0 && triangles_texture_loc != -1) {
        SetShaderValueTexture(shader, triangles_texture_loc, triangles_texture_);
    }
    if (nodes_texture_.id != 0 && blas_nodes_texture_loc != -1) {
        SetShaderValueTexture(shader, blas_nodes_texture_loc, nodes_texture_);
    }
}

void BLASManager::print_stats() const {
    update_totals();
    
    printf("=== BLAS Manager Statistics ===\n");
    printf("Unique BLAS count: %zu\n", entries_.size());
    printf("Total triangles: %d\n", cached_total_triangles_);
    printf("Total nodes: %d\n", cached_total_nodes_);
    printf("Next handle: %u\n", next_handle_);
    
    // Hash table statistics
    std::unordered_map<uint32_t, int> bucket_sizes;
    for (const auto& pair : hash_to_entry_) {
        bucket_sizes[pair.first]++;
    }
    
    int max_bucket_size = 0;
    for (const auto& pair : bucket_sizes) {
        max_bucket_size = std::max(max_bucket_size, pair.second);
    }
    
    printf("Hash buckets: %zu used, max chain length: %d\n", 
           bucket_sizes.size(), max_bucket_size);
}

void BLASManager::reset_stats() {
    // This would clear all data - be careful!
    entries_.clear();
    hash_to_entry_.clear();
    next_handle_ = 1;
    totals_dirty_ = true;
}

// Factory functions implementation
namespace BLASFactory {

// Helper function to create triangle from positions
Triangle create_triangle_from_positions(const Vec3& v0, const Vec3& v1, const Vec3& v2, int material_id = 0) {
    Triangle tri;
    tri.v0 = v0;
    tri.v1 = v1;
    tri.v2 = v2;
    
    // Calculate centroid
    tri.centroid.x = (tri.v0.x + tri.v1.x + tri.v2.x) / 3.0f;
    tri.centroid.y = (tri.v0.y + tri.v1.y + tri.v2.y) / 3.0f;
    tri.centroid.z = (tri.v0.z + tri.v1.z + tri.v2.z) / 3.0f;
    
    // Calculate normal using cross product
    Vec3 edge1 = {tri.v1.x - tri.v0.x, tri.v1.y - tri.v0.y, tri.v1.z - tri.v0.z};
    Vec3 edge2 = {tri.v2.x - tri.v0.x, tri.v2.y - tri.v0.y, tri.v2.z - tri.v0.z};
    
    tri.normal.x = edge1.y * edge2.z - edge1.z * edge2.y;
    tri.normal.y = edge1.z * edge2.x - edge1.x * edge2.z;
    tri.normal.z = edge1.x * edge2.y - edge1.y * edge2.x;
    
    // Normalize
    float len = std::sqrt(tri.normal.x * tri.normal.x + tri.normal.y * tri.normal.y + tri.normal.z * tri.normal.z);
    if (len > 0.0f) {
        tri.normal.x /= len;
        tri.normal.y /= len;
        tri.normal.z /= len;
    }
    
    tri.material_id = material_id;
    return tri;
}

std::vector<Triangle> create_cube_triangles(float size) {
    PROFILE_SECTION("Create Cube Triangles");
    
    std::vector<Triangle> triangles;
    triangles.reserve(12);
    
    float half = size * 0.5f;
    
    // Front face (Z+)
    triangles.push_back(create_triangle_from_positions({-half, -half, half}, {half, -half, half}, {half, half, half}));
    triangles.push_back(create_triangle_from_positions({-half, -half, half}, {half, half, half}, {-half, half, half}));
    
    // Back face (Z-)
    triangles.push_back(create_triangle_from_positions({-half, -half, -half}, {half, half, -half}, {half, -half, -half}));
    triangles.push_back(create_triangle_from_positions({-half, -half, -half}, {-half, half, -half}, {half, half, -half}));
    
    // Right face (X+)
    triangles.push_back(create_triangle_from_positions({half, -half, -half}, {half, half, -half}, {half, half, half}));
    triangles.push_back(create_triangle_from_positions({half, -half, -half}, {half, half, half}, {half, -half, half}));
    
    // Left face (X-)
    triangles.push_back(create_triangle_from_positions({-half, -half, -half}, {-half, half, half}, {-half, half, -half}));
    triangles.push_back(create_triangle_from_positions({-half, -half, -half}, {-half, -half, half}, {-half, half, half}));
    
    // Top face (Y+)
    triangles.push_back(create_triangle_from_positions({-half, half, -half}, {-half, half, half}, {half, half, half}));
    triangles.push_back(create_triangle_from_positions({-half, half, -half}, {half, half, half}, {half, half, -half}));
    
    // Bottom face (Y-)
    triangles.push_back(create_triangle_from_positions({-half, -half, -half}, {half, -half, half}, {-half, -half, half}));
    triangles.push_back(create_triangle_from_positions({-half, -half, -half}, {half, -half, -half}, {half, -half, half}));
    
    return triangles;
}

std::vector<Triangle> create_sphere_triangles(float radius, int segments, int rings) {
    PROFILE_SECTION("Create Sphere Triangles");
    
    std::vector<Triangle> triangles;
    triangles.reserve(2 * segments * rings);
    
    for (int ring = 0; ring < rings; ring++) {
        for (int segment = 0; segment < segments; segment++) {
            // Calculate angles
            float ring_angle_1 = static_cast<float>(ring) / static_cast<float>(rings) * static_cast<float>(M_PI);
            float ring_angle_2 = static_cast<float>(ring + 1) / static_cast<float>(rings) * static_cast<float>(M_PI);
            float seg_angle_1 = static_cast<float>(segment) / static_cast<float>(segments) * 2.0f * static_cast<float>(M_PI);
            float seg_angle_2 = static_cast<float>(segment + 1) / static_cast<float>(segments) * 2.0f * static_cast<float>(M_PI);
            
            // Calculate vertices
            Vec3 v1 = {
                radius * std::sin(ring_angle_1) * std::cos(seg_angle_1),
                radius * std::cos(ring_angle_1),
                radius * std::sin(ring_angle_1) * std::sin(seg_angle_1)
            };
            Vec3 v2 = {
                radius * std::sin(ring_angle_1) * std::cos(seg_angle_2),
                radius * std::cos(ring_angle_1),
                radius * std::sin(ring_angle_1) * std::sin(seg_angle_2)
            };
            Vec3 v3 = {
                radius * std::sin(ring_angle_2) * std::cos(seg_angle_1),
                radius * std::cos(ring_angle_2),
                radius * std::sin(ring_angle_2) * std::sin(seg_angle_1)
            };
            Vec3 v4 = {
                radius * std::sin(ring_angle_2) * std::cos(seg_angle_2),
                radius * std::cos(ring_angle_2),
                radius * std::sin(ring_angle_2) * std::sin(seg_angle_2)
            };
            
            // Create two triangles for this quad (skip degenerate triangles)
            if (ring < rings - 1) {
                triangles.push_back(create_triangle_from_positions(v1, v2, v3, 1));
                triangles.push_back(create_triangle_from_positions(v2, v4, v3, 1));
            }
        }
    }
    
    return triangles;
}

std::vector<Triangle> create_plane_triangles(float width, float height) {
    PROFILE_SECTION("Create Plane Triangles");
    
    std::vector<Triangle> triangles;
    triangles.reserve(2);
    
    float half_w = width * 0.5f;
    float half_h = height * 0.5f;
    
    triangles.push_back(create_triangle_from_positions(
        {-half_w, 0.0f, -half_h}, 
        {half_w, 0.0f, -half_h}, 
        {half_w, 0.0f, half_h}, 2));
    triangles.push_back(create_triangle_from_positions(
        {-half_w, 0.0f, -half_h}, 
        {half_w, 0.0f, half_h}, 
        {-half_w, 0.0f, half_h}, 2));
    
    return triangles;
}

BLASHandle register_cube(BLASManager& manager, float size) {
    auto triangles = create_cube_triangles(size);
    return manager.register_triangles(triangles);
}

BLASHandle register_sphere(BLASManager& manager, float radius, int segments, int rings) {
    auto triangles = create_sphere_triangles(radius, segments, rings);
    return manager.register_triangles(triangles);
}

BLASHandle register_plane(BLASManager& manager, float width, float height) {
    auto triangles = create_plane_triangles(width, height);
    return manager.register_triangles(triangles);
}

} // namespace BLASFactory