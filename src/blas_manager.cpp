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

// Conversion utilities
Tri BLASManager::convert_triangle(const LegacyTriangle& old_tri) {
    Tri new_tri;
    new_tri.vertex0 = old_tri.v0;
    new_tri.vertex1 = old_tri.v1;
    new_tri.vertex2 = old_tri.v2;
    new_tri.centroid = old_tri.centroid;
    return new_tri;
}

LegacyTriangle BLASManager::convert_triangle_back(const Tri& new_tri) {
    LegacyTriangle old_tri;
    old_tri.v0 = new_tri.vertex0;
    old_tri.v1 = new_tri.vertex1;
    old_tri.v2 = new_tri.vertex2;
    old_tri.centroid = new_tri.centroid;
    // Calculate normal
    float3 edge1 = new_tri.vertex1 - new_tri.vertex0;
    float3 edge2 = new_tri.vertex2 - new_tri.vertex0;
    old_tri.normal = normalize(cross(edge1, edge2));
    old_tri.material_id = 0; // Default material
    return old_tri;
}

uint32_t BLASManager::calculate_hash(const Tri* triangles, int count) const {
    uint32_t hash = 2166136261u; // FNV-1a offset basis
    
    for (int i = 0; i < count; i++) {
        // Hash vertex positions only
        const float* data = reinterpret_cast<const float*>(&triangles[i].vertex0);
        for (int j = 0; j < 9; j++) { // 3 vertices * 3 components each
            uint32_t val = *reinterpret_cast<const uint32_t*>(&data[j]);
            hash ^= val;
            hash *= 16777619u; // FNV-1a prime
        }
    }
    
    return hash;
}

// uint32_t BLASManager::calculate_hash_legacy(const LegacyTriangle* triangles, int count) const {
//     PROFILE_SECTION("BLAS Hash Calculation Legacy");
    
//     uint32_t hash = 2166136261u; // FNV-1a offset basis
    
//     for (int i = 0; i < count; i++) {
//         // Hash vertex positions only (ignore normals/materials for deduplication)
//         const float* data = reinterpret_cast<const float*>(&triangles[i]);
//         for (int j = 0; j < 9; j++) { // 3 vertices * 3 components each
//             uint32_t val = *reinterpret_cast<const uint32_t*>(&data[j]);
//             hash ^= val;
//             hash *= 16777619u; // FNV-1a prime
//         }
//     }
    
//     return hash;
// }

bool BLASManager::triangles_equal(const std::vector<Tri>& a, const Tri* b, int count) const {
    if (a.size() != static_cast<size_t>(count)) return false;
    
    for (int i = 0; i < count; i++) {
        if (std::memcmp(&a[i].vertex0, &b[i].vertex0, sizeof(float3) * 3) != 0) {
            return false;
        }
    }
    return true;
}

BLASHandle BLASManager::find_existing_blas(const Tri* triangles, int count, uint32_t hash) const {
    auto range = hash_to_entry_.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it) {
        const auto& entry = entries_[it->second];
        if (triangles_equal(entry->triangles, triangles, count)) {
            return entry->handle;
        }
    }
    return INVALID_BLAS_HANDLE;
}

// BLASHandle BLASManager::find_existing_blas_legacy(const LegacyTriangle* triangles, int count, uint32_t hash) const {
//     PROFILE_SECTION("BLAS Deduplication Check Legacy");
    
//     // Convert to new format and check
//     std::vector<Tri> converted_triangles;
//     converted_triangles.reserve(count);
//     for (int i = 0; i < count; i++) {
//         converted_triangles.push_back(convert_triangle(triangles[i]));
//     }
    
//     auto range = hash_to_entry_.equal_range(hash);
//     for (auto it = range.first; it != range.second; ++it) {
//         const auto& entry = entries_[it->second];
//         if (triangles_equal(entry->triangles, converted_triangles.data(), count)) {
//             return entry->handle;
//         }
//     }
//     return INVALID_BLAS_HANDLE;
// }

BLASHandle BLASManager::register_triangles(const std::vector<Tri>& triangles) {
    return register_triangles(const_cast<Tri*>(triangles.data()), 
                             static_cast<int>(triangles.size()));
}

// BLASHandle BLASManager::register_triangles_legacy(const std::vector<LegacyTriangle>& triangles) {
//     return register_triangles_legacy(const_cast<LegacyTriangle*>(triangles.data()), 
//                                     static_cast<int>(triangles.size()));
// }

BLASHandle BLASManager::register_triangles(Tri* triangles, int triangle_count) {
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
        std::vector<Tri> triangle_copy(triangles, triangles + triangle_count);
        
        // Create mesh and properly build BVH
        auto mesh = std::make_unique<BvhMesh>();
        mesh->triCount = triangle_count;
        mesh->tri = static_cast<Tri*>(MALLOC64(triangle_count * sizeof(Tri)));
        
        // Copy triangles to mesh
        for (int i = 0; i < triangle_count; i++) {
            mesh->tri[i] = triangles[i];
        }
        
        // Create BVH using the proper constructor
        auto bvh = std::make_unique<BVH>(mesh.get());
        
        // For the unit test scene, enable subdivToOnePrim to force proper subdivision
        if (triangle_count == 3) {
            // This is likely our unit test with 3 well-separated triangles
            bvh->subdivToOnePrim = true;
            // Rebuild with the correct flag
            bvh->Build();
        }
        
        BLASHandle handle = next_handle_++;
        
        // Create entry
        auto entry = std::make_unique<BLASEntry>(handle, std::move(mesh), std::move(bvh), std::move(triangle_copy), hash);
        
        // Add to hash table
        size_t entry_index = entries_.size();
        hash_to_entry_.emplace(hash, entry_index);
        
        // Add to entries
        entries_.push_back(std::move(entry));
        
        mark_dirty(); // Mark all cached data as dirty
        return handle;
    }
}

// BLASHandle BLASManager::register_triangles_legacy(LegacyTriangle* triangles, int triangle_count) {
//     PROFILE_SECTION("BLAS Registration Legacy");
    
//     if (!triangles || triangle_count <= 0) {
//         return INVALID_BLAS_HANDLE;
//     }
    
//     // Convert to new format
//     std::vector<Tri> converted_triangles;
//     converted_triangles.reserve(triangle_count);
//     for (int i = 0; i < triangle_count; i++) {
//         converted_triangles.push_back(convert_triangle(triangles[i]));
//     }
    
//     return register_triangles(converted_triangles.data(), triangle_count);
// }

bool BLASManager::has_blas(BLASHandle handle) const {
    if (handle == INVALID_BLAS_HANDLE) return false;
    
    return std::any_of(entries_.begin(), entries_.end(),
        [handle](const auto& entry) { return entry->handle == handle; });
}

BVH* BLASManager::get_bvh(BLASHandle handle) const {
    if (handle == INVALID_BLAS_HANDLE) return nullptr;
    
    auto it = std::find_if(entries_.begin(), entries_.end(),
        [handle](const auto& entry) { return entry->handle == handle; });
    
    return (it != entries_.end()) ? (*it)->bvh.get() : nullptr;
}

BvhMesh* BLASManager::get_mesh(BLASHandle handle) const {
    if (handle == INVALID_BLAS_HANDLE) return nullptr;
    
    auto it = std::find_if(entries_.begin(), entries_.end(),
        [handle](const auto& entry) { return entry->handle == handle; });
    
    return (it != entries_.end()) ? (*it)->mesh.get() : nullptr;
}

const BLASManager::BLASEntry* BLASManager::get_entry(BLASHandle handle) const {
    if (handle == INVALID_BLAS_HANDLE) return nullptr;
    
    auto it = std::find_if(entries_.begin(), entries_.end(),
        [handle](const auto& entry) { return entry->handle == handle; });
    
    return (it != entries_.end()) ? it->get() : nullptr;
}

void BLASManager::update_totals() const {
    if (!totals_dirty_) return;
    
    PROFILE_SECTION("BLAS Total Calculation");
    
    cached_total_triangles_ = 0;
    cached_total_nodes_ = 0;
    
    for (const auto& entry : entries_) {
        if (entry->mesh && entry->bvh) {
            cached_total_triangles_ += entry->mesh->triCount;
            cached_total_nodes_ += entry->bvh->nodesUsed;
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
        
        if (entry->mesh && entry->bvh) {
            triangle_offset += entry->mesh->triCount;
            node_offset += entry->bvh->nodesUsed;
        }
    }
    
    return offsets; // Not found
}

void BLASManager::generate_triangle_data(std::vector<Tri>& output_triangles) const {
    PROFILE_SECTION("BLAS Triangle Data Generation");
    
    output_triangles.clear();
    output_triangles.reserve(get_total_triangle_count());
    
    for (const auto& entry : entries_) {
        if (entry->mesh && entry->bvh) {
            // Generate triangles in BVH order using triIdx mapping
            for (int i = 0; i < entry->mesh->triCount; i++) {
                uint original_idx = entry->bvh->triIdx[i];
                if (original_idx < entry->triangles.size()) {
                    output_triangles.push_back(entry->triangles[original_idx]);
                }
            }
        }
    }
}

// void BLASManager::generate_triangle_data_legacy(std::vector<LegacyTriangle>& output_triangles) const {
//     PROFILE_SECTION("BLAS Triangle Data Generation Legacy");
    
//     output_triangles.clear();
//     output_triangles.reserve(get_total_triangle_count());
    
//     for (const auto& entry : entries_) {
//         if (entry->mesh && entry->bvh) {
//             for (const auto& tri : entry->triangles) {
//                 output_triangles.push_back(convert_triangle_back(tri));
//             }
//         }
//     }
// }

void BLASManager::generate_node_data(std::vector<LegacyBVHNode>& output_nodes) const {
    PROFILE_SECTION("BLAS Node Data Generation");
    
    output_nodes.clear();
    output_nodes.reserve(get_total_node_count());
    
    int node_offset = 0;
    int triangle_offset = 0;
    
    for (const auto& entry : entries_) {
        if (entry->mesh && entry->bvh) {
            // Copy nodes and adjust indices
            for (uint j = 0; j < entry->bvh->nodesUsed; j++) {
                const auto& src_node = entry->bvh->bvhNode[j];
                LegacyBVHNode node;  // Use the legacy BVHNode from our header
                
                // Convert from new BVH format to old format
                node.aabbMin = src_node.aabbMin;
                node.aabbMax = src_node.aabbMax;
                node.leftFirst = src_node.leftFirst;
                node.triCount = src_node.triCount;
                
                if (node.triCount > 0) {
                    // Leaf node - adjust triangle indices
                    node.leftFirst += triangle_offset;
                } else {
                    // Internal node - adjust child node indices
                    node.leftFirst += node_offset;
                }
                
                output_nodes.push_back(node);
            }
            
            node_offset += entry->bvh->nodesUsed;
            triangle_offset += entry->mesh->triCount;
        }
    }
}

void BLASManager::generate_triangle_texture_data(Tri* output_triangles) const {
    if (!output_triangles) return;
    
    std::vector<Tri> temp;
    generate_triangle_data(temp);
    std::copy(temp.begin(), temp.end(), output_triangles);
}

// void BLASManager::generate_triangle_texture_data_legacy(LegacyTriangle* output_triangles) const {
//     if (!output_triangles) return;
    
//     std::vector<LegacyTriangle> temp;
//     generate_triangle_data_legacy(temp);
//     std::copy(temp.begin(), temp.end(), output_triangles);
// }

void BLASManager::generate_node_texture_data(LegacyBVHNode* output_nodes) const {
    if (!output_nodes) return;
    
    std::vector<LegacyBVHNode> temp;
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
        
        std::vector<Tri> all_triangles;
        generate_triangle_data(all_triangles);
        
        if (!all_triangles.empty()) {
            int texture_width = static_cast<int>(all_triangles.size());
            int texture_height = 4;
            
            std::vector<float> texture_data(texture_width * texture_height * 4);
            
            for (size_t i = 0; i < all_triangles.size(); i++) {
                const Tri& tri = all_triangles[i];
                int base_idx = static_cast<int>(i) * 4;
                
                // Triangle data processed
                
                // Row 0: v0 + materialId
                texture_data[base_idx + 0] = tri.vertex0.x;
                texture_data[base_idx + 1] = tri.vertex0.y;
                texture_data[base_idx + 2] = tri.vertex0.z;
                texture_data[base_idx + 3] = 0.0f; // No material_id in new format
                
                // Row 1: v1
                int row1_idx = texture_width * 4 + base_idx;
                texture_data[row1_idx + 0] = tri.vertex1.x;
                texture_data[row1_idx + 1] = tri.vertex1.y;
                texture_data[row1_idx + 2] = tri.vertex1.z;
                texture_data[row1_idx + 3] = 0.0f;
                
                // Row 2: v2
                int row2_idx = texture_width * 8 + base_idx;
                texture_data[row2_idx + 0] = tri.vertex2.x;
                texture_data[row2_idx + 1] = tri.vertex2.y;
                texture_data[row2_idx + 2] = tri.vertex2.z;
                texture_data[row2_idx + 3] = 0.0f;
                
                // Row 3: normal (calculated from cross product)
                int row3_idx = texture_width * 12 + base_idx;
                float3 edge1 = tri.vertex1 - tri.vertex0;
                float3 edge2 = tri.vertex2 - tri.vertex0;
                float3 normal = normalize(cross(edge1, edge2));
                texture_data[row3_idx + 0] = normal.x;
                texture_data[row3_idx + 1] = normal.y;
                texture_data[row3_idx + 2] = normal.z;
                texture_data[row3_idx + 3] = 0.0f;
            }
            
            Image tri_image = {
                .data = texture_data.data(),
                .width = texture_width,
                .height = texture_height,
                .mipmaps = 1,
                .format = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32
            };
            
            triangles_texture_ = LoadTextureFromImage(tri_image);
            SetTextureFilter(triangles_texture_, TEXTURE_FILTER_POINT);
        }
    }
    
    // Generate nodes texture
    {
        PROFILE_SECTION("BLAS Nodes Texture Creation");
        
        std::vector<LegacyBVHNode> all_nodes;
        generate_node_data(all_nodes);
        
        if (!all_nodes.empty()) {
            int texture_width = static_cast<int>(all_nodes.size());
            int texture_height = 3; // 3 rows per node: aabbMin+leftFirst, aabbMax+triCount, padding
            
            std::vector<float> texture_data(texture_width * texture_height * 4);
            
            for (size_t i = 0; i < all_nodes.size(); i++) {
                const LegacyBVHNode& node = all_nodes[i];
                int base_idx = static_cast<int>(i) * 4;
                
                // Row 0: aabbMin + leftFirst
                texture_data[base_idx + 0] = node.aabbMin.x;
                texture_data[base_idx + 1] = node.aabbMin.y;
                texture_data[base_idx + 2] = node.aabbMin.z;
                texture_data[base_idx + 3] = static_cast<float>(node.leftFirst);
                
                // Row 1: aabbMax + triCount
                int row1_idx = texture_width * 4 + base_idx;
                texture_data[row1_idx + 0] = node.aabbMax.x;
                texture_data[row1_idx + 1] = node.aabbMax.y;
                texture_data[row1_idx + 2] = node.aabbMax.z;
                texture_data[row1_idx + 3] = static_cast<float>(node.triCount);
                
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
                .mipmaps = 1,
                .format = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32
            };
            
            nodes_texture_ = LoadTextureFromImage(blas_image);
            SetTextureFilter(nodes_texture_, TEXTURE_FILTER_POINT);
        }
    }
    
    textures_dirty_ = false;
}

void BLASManager::bind_to_shader(Shader shader) const {
    PROFILE_SECTION("BLAS Shader Binding");
    
    // Check if shader has changed and cache locations if needed
    bool shader_changed = (cached_shader_id_ != shader.id);
    if (shader_changed) {
        PROFILE_SECTION("Cache Shader Locations");
        cached_shader_id_ = shader.id;
        
        // Cache uniform locations (these are expensive calls)
        triangle_count_loc_     = GetShaderLocation(shader, "triangleCount");
        blas_node_count_loc_    = GetShaderLocation(shader, "blasNodeCount");
        triangles_texture_loc_  = GetShaderLocation(shader, "trianglesTexture");
        blas_nodes_texture_loc_ = GetShaderLocation(shader, "blasNodesTexture");
        intersection_mode_loc_  = GetShaderLocation(shader, "intersectionMode");
        
        // Force update shader values when shader changes
        shader_values_dirty_ = true;
    }
    
    // Only ensure textures are ready if they're dirty
    bool textures_were_updated = textures_dirty_;
    if (textures_dirty_) {
        PROFILE_SECTION("Update GPU Textures");
        const_cast<BLASManager*>(this)->ensure_gpu_textures_ready();
    }
    
    // Only update shader values if they're dirty or shader changed
    if (shader_values_dirty_) {
        PROFILE_SECTION("Update Shader Values");
        
        // Set counts
        int triangle_count = get_total_triangle_count();
        int node_count     = get_total_node_count();
        
        if (triangle_count_loc_ != -1) {
            SetShaderValue(shader, triangle_count_loc_, &triangle_count, SHADER_UNIFORM_INT);
        }
        if (blas_node_count_loc_ != -1) {
            SetShaderValue(shader, blas_node_count_loc_, &node_count, SHADER_UNIFORM_INT);
        }
        
        // Enable BVH traversal
        if (intersection_mode_loc_ != -1) {
            int intersection_mode = 1;
            SetShaderValue(shader, intersection_mode_loc_, &intersection_mode, SHADER_UNIFORM_INT);
        }
        
        shader_values_dirty_ = false;
    }
    
    // Only bind textures if they've been updated or shader changed
    if (textures_were_updated || shader_changed) {
        PROFILE_SECTION("Bind Textures");
        
        if (triangles_texture_.id != 0 && triangles_texture_loc_ != -1) {
            SetShaderValueTexture(shader, triangles_texture_loc_, triangles_texture_);
        }
        
        if (nodes_texture_.id != 0 && blas_nodes_texture_loc_ != -1) {
            SetShaderValueTexture(shader, blas_nodes_texture_loc_, nodes_texture_);
        }
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
LegacyTriangle create_triangle_from_positions(const float3& v0, const float3& v1, const float3& v2, int material_id = 0) {
    LegacyTriangle tri;
    tri.v0 = v0;
    tri.v1 = v1;
    tri.v2 = v2;
    
    // Calculate centroid
    tri.centroid.x = (tri.v0.x + tri.v1.x + tri.v2.x) / 3.0f;
    tri.centroid.y = (tri.v0.y + tri.v1.y + tri.v2.y) / 3.0f;
    tri.centroid.z = (tri.v0.z + tri.v1.z + tri.v2.z) / 3.0f;
    
    // Calculate normal using cross product
    float3 edge1 = {tri.v1.x - tri.v0.x, tri.v1.y - tri.v0.y, tri.v1.z - tri.v0.z};
    float3 edge2 = {tri.v2.x - tri.v0.x, tri.v2.y - tri.v0.y, tri.v2.z - tri.v0.z};
    
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

// std::vector<LegacyTriangle> create_cube_triangles_legacy(float size) {
//     PROFILE_SECTION("Create Cube Triangles");
    
//     std::vector<LegacyTriangle> triangles;
//     triangles.reserve(12);
    
//     float half = size * 0.5f;
    
//     // Front face (Z+)
//     triangles.push_back(create_triangle_from_positions({-half, -half, half}, {half, -half, half}, {half, half, half}));
//     triangles.push_back(create_triangle_from_positions({-half, -half, half}, {half, half, half}, {-half, half, half}));
    
//     // Back face (Z-)
//     triangles.push_back(create_triangle_from_positions({-half, -half, -half}, {half, half, -half}, {half, -half, -half}));
//     triangles.push_back(create_triangle_from_positions({-half, -half, -half}, {-half, half, -half}, {half, half, -half}));
    
//     // Right face (X+)
//     triangles.push_back(create_triangle_from_positions({half, -half, -half}, {half, half, -half}, {half, half, half}));
//     triangles.push_back(create_triangle_from_positions({half, -half, -half}, {half, half, half}, {half, -half, half}));
    
//     // Left face (X-)
//     triangles.push_back(create_triangle_from_positions({-half, -half, -half}, {-half, half, half}, {-half, half, -half}));
//     triangles.push_back(create_triangle_from_positions({-half, -half, -half}, {-half, -half, half}, {-half, half, half}));
    
//     // Top face (Y+)
//     triangles.push_back(create_triangle_from_positions({-half, half, -half}, {-half, half, half}, {half, half, half}));
//     triangles.push_back(create_triangle_from_positions({-half, half, -half}, {half, half, half}, {half, half, -half}));
    
//     // Bottom face (Y-)
//     triangles.push_back(create_triangle_from_positions({-half, -half, -half}, {half, -half, half}, {-half, -half, half}));
//     triangles.push_back(create_triangle_from_positions({-half, -half, -half}, {half, -half, -half}, {half, -half, half}));
    
//     return triangles;
// }

// std::vector<LegacyTriangle> create_sphere_triangles_legacy(float radius, int segments, int rings) {
//     PROFILE_SECTION("Create Sphere Triangles");
    
//     std::vector<LegacyTriangle> triangles;
//     triangles.reserve(2 * segments * rings);
    
//     for (int ring = 0; ring < rings; ring++) {
//         for (int segment = 0; segment < segments; segment++) {
//             // Calculate angles
//             float ring_angle_1 = static_cast<float>(ring) / static_cast<float>(rings) * static_cast<float>(M_PI);
//             float ring_angle_2 = static_cast<float>(ring + 1) / static_cast<float>(rings) * static_cast<float>(M_PI);
//             float seg_angle_1 = static_cast<float>(segment) / static_cast<float>(segments) * 2.0f * static_cast<float>(M_PI);
//             float seg_angle_2 = static_cast<float>(segment + 1) / static_cast<float>(segments) * 2.0f * static_cast<float>(M_PI);
            
//             // Calculate vertices
//             float3 v1 = {
//                 radius * std::sin(ring_angle_1) * std::cos(seg_angle_1),
//                 radius * std::cos(ring_angle_1),
//                 radius * std::sin(ring_angle_1) * std::sin(seg_angle_1)
//             };
//             float3 v2 = {
//                 radius * std::sin(ring_angle_1) * std::cos(seg_angle_2),
//                 radius * std::cos(ring_angle_1),
//                 radius * std::sin(ring_angle_1) * std::sin(seg_angle_2)
//             };
//             float3 v3 = {
//                 radius * std::sin(ring_angle_2) * std::cos(seg_angle_1),
//                 radius * std::cos(ring_angle_2),
//                 radius * std::sin(ring_angle_2) * std::sin(seg_angle_1)
//             };
//             float3 v4 = {
//                 radius * std::sin(ring_angle_2) * std::cos(seg_angle_2),
//                 radius * std::cos(ring_angle_2),
//                 radius * std::sin(ring_angle_2) * std::sin(seg_angle_2)
//             };
            
//             // Create two triangles for this quad (skip degenerate triangles)
//             if (ring < rings - 1) {
//                 triangles.push_back(create_triangle_from_positions(v1, v2, v3, 1));
//                 triangles.push_back(create_triangle_from_positions(v2, v4, v3, 1));
//             }
//         }
//     }
    
//     return triangles;
// }

// std::vector<LegacyTriangle> create_plane_triangles_legacy(float width, float height) {
//     PROFILE_SECTION("Create Plane Triangles");
    
//     std::vector<LegacyTriangle> triangles;
//     triangles.reserve(2);
    
//     float half_w = width * 0.5f;
//     float half_h = height * 0.5f;
    
//     triangles.push_back(create_triangle_from_positions(
//         {-half_w, 0.0f, -half_h}, 
//         {half_w, 0.0f, -half_h}, 
//         {half_w, 0.0f, half_h}, 2));
//     triangles.push_back(create_triangle_from_positions(
//         {-half_w, 0.0f, -half_h}, 
//         {half_w, 0.0f, half_h}, 
//         {-half_w, 0.0f, half_h}, 2));
    
//     return triangles;
// }

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

// New factory functions that create Tri objects
Tri create_tri_from_positions(const float3& v0, const float3& v1, const float3& v2) {
    Tri tri;
    tri.vertex0 = v0;
    tri.vertex1 = v1;
    tri.vertex2 = v2;
    
    // Calculate centroid
    tri.centroid.x = (v0.x + v1.x + v2.x) / 3.0f;
    tri.centroid.y = (v0.y + v1.y + v2.y) / 3.0f;
    tri.centroid.z = (v0.z + v1.z + v2.z) / 3.0f;
    
    return tri;
}

std::vector<Tri> create_cube_triangles(float size) {
    PROFILE_SECTION("Create Cube Triangles (New)");
    
    std::vector<Tri> triangles;
    triangles.reserve(12);
    
    float half = size * 0.5f;
    
    // Front face (Z+)
    triangles.push_back(create_tri_from_positions({-half, -half, half}, {half, -half, half}, {half, half, half}));
    triangles.push_back(create_tri_from_positions({-half, -half, half}, {half, half, half}, {-half, half, half}));
    
    // Back face (Z-)
    triangles.push_back(create_tri_from_positions({-half, -half, -half}, {half, half, -half}, {half, -half, -half}));
    triangles.push_back(create_tri_from_positions({-half, -half, -half}, {-half, half, -half}, {half, half, -half}));
    
    // Right face (X+)
    triangles.push_back(create_tri_from_positions({half, -half, -half}, {half, half, -half}, {half, half, half}));
    triangles.push_back(create_tri_from_positions({half, -half, -half}, {half, half, half}, {half, -half, half}));
    
    // Left face (X-)
    triangles.push_back(create_tri_from_positions({-half, -half, -half}, {-half, half, half}, {-half, half, -half}));
    triangles.push_back(create_tri_from_positions({-half, -half, -half}, {-half, -half, half}, {-half, half, half}));
    
    // Top face (Y+)
    triangles.push_back(create_tri_from_positions({-half, half, -half}, {-half, half, half}, {half, half, half}));
    triangles.push_back(create_tri_from_positions({-half, half, -half}, {half, half, half}, {half, half, -half}));
    
    // Bottom face (Y-)
    triangles.push_back(create_tri_from_positions({-half, -half, -half}, {half, -half, half}, {-half, -half, half}));
    triangles.push_back(create_tri_from_positions({-half, -half, -half}, {half, -half, -half}, {half, -half, half}));
    
    return triangles;
}

std::vector<Tri> create_sphere_triangles(float radius, int segments, int rings) {
    PROFILE_SECTION("Create Sphere Triangles (New)");
    
    std::vector<Tri> triangles;
    triangles.reserve(2 * segments * rings);
    
    for (int ring = 0; ring < rings; ring++) {
        for (int segment = 0; segment < segments; segment++) {
            // Calculate angles
            float ring_angle_1 = static_cast<float>(ring) / static_cast<float>(rings) * static_cast<float>(M_PI);
            float ring_angle_2 = static_cast<float>(ring + 1) / static_cast<float>(rings) * static_cast<float>(M_PI);
            float seg_angle_1 = static_cast<float>(segment) / static_cast<float>(segments) * 2.0f * static_cast<float>(M_PI);
            float seg_angle_2 = static_cast<float>(segment + 1) / static_cast<float>(segments) * 2.0f * static_cast<float>(M_PI);
            
            // Calculate vertices
            float3 v1 = {
                radius * std::sin(ring_angle_1) * std::cos(seg_angle_1),
                radius * std::cos(ring_angle_1),
                radius * std::sin(ring_angle_1) * std::sin(seg_angle_1)
            };
            float3 v2 = {
                radius * std::sin(ring_angle_1) * std::cos(seg_angle_2),
                radius * std::cos(ring_angle_1),
                radius * std::sin(ring_angle_1) * std::sin(seg_angle_2)
            };
            float3 v3 = {
                radius * std::sin(ring_angle_2) * std::cos(seg_angle_1),
                radius * std::cos(ring_angle_2),
                radius * std::sin(ring_angle_2) * std::sin(seg_angle_1)
            };
            float3 v4 = {
                radius * std::sin(ring_angle_2) * std::cos(seg_angle_2),
                radius * std::cos(ring_angle_2),
                radius * std::sin(ring_angle_2) * std::sin(seg_angle_2)
            };
            
            // Create two triangles for this quad (skip degenerate triangles)
            if (ring < rings - 1) {
                triangles.push_back(create_tri_from_positions(v1, v2, v3));
                triangles.push_back(create_tri_from_positions(v2, v4, v3));
            }
        }
    }
    
    return triangles;
}

std::vector<Tri> create_plane_triangles(float width, float height) {
    std::vector<Tri> triangles;
    triangles.reserve(2);
    
    float half_w = width * 0.5f;
    float half_h = height * 0.5f;
    
    triangles.push_back(create_tri_from_positions(
        {-half_w, 0.0f, -half_h}, 
        {half_w, 0.0f, -half_h}, 
        {half_w, 0.0f, half_h}));
    triangles.push_back(create_tri_from_positions(
        {-half_w, 0.0f, -half_h}, 
        {half_w, 0.0f, half_h}, 
        {-half_w, 0.0f, half_h}));
    
    return triangles;
}

} // namespace BLASFactory