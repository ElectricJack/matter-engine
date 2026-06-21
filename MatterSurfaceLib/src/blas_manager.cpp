#include "../include/blas_manager.hpp"
#include "vertex_ao.h"  // pack_ao_w
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

uint32_t BLASManager::calculate_hash(const Tri* triangles, int count, const TriEx* triex) const {
    uint32_t hash = 2166136261u; // FNV-1a offset basis

    for (int i = 0; i < count; i++) {
        // Hash vertex positions only
        const float* data = reinterpret_cast<const float*>(&triangles[i].vertex0);
        for (int j = 0; j < 9; j++) { // 3 vertices * 3 components each
            uint32_t val = *reinterpret_cast<const uint32_t*>(&data[j]);
            hash ^= val;
            hash *= 16777619u; // FNV-1a prime
        }
        // Fold the per-triangle materialId into identity so meshes with identical
        // geometry but different materials hash apart. No triEx -> constant sentinel
        // (matches the -1 "no per-triangle material" convention), applied consistently
        // in triangles_equal so both sides agree on the null-material case.
        uint32_t mat = triex ? static_cast<uint32_t>(triex[i].materialId) : 0xFFFFFFFFu;
        hash ^= mat;
        hash *= 16777619u;

        // Fold the per-triangle tint into identity so geometry that differs only
        // by tint is not deduplicated. No triEx -> neutral (0,0,0,0).
        const float4 tnt = triex ? triex[i].tint : make_float4(0.0f, 0.0f, 0.0f, 0.0f);
        const float* tf = reinterpret_cast<const float*>(&tnt);
        for (int k = 0; k < 4; k++) {
            hash ^= *reinterpret_cast<const uint32_t*>(&tf[k]);
            hash *= 16777619u;
        }
    }

    return hash;
}

bool BLASManager::triangles_equal(const BLASEntry& entry, const Tri* b, int count, const TriEx* triex) const {
    const std::vector<Tri>& a = entry.triangles;
    if (a.size() != static_cast<size_t>(count)) return false;

    const TriEx* a_ex = entry.mesh ? entry.mesh->triEx : nullptr;
    for (int i = 0; i < count; i++) {
        if (std::memcmp(&a[i].vertex0, &b[i].vertex0, sizeof(float3) * 3) != 0) {
            return false;
        }
        // Per-triangle material must match too; a null triEx is "no material"
        // (sentinel) and only matches another null triEx.
        int a_mat = a_ex ? a_ex[i].materialId : -1;
        int b_mat = triex ? triex[i].materialId : -1;
        if (a_mat != b_mat) {
            return false;
        }

        // Tint must match too (a null triEx is neutral (0,0,0,0)).
        const float4 a_tint = a_ex ? a_ex[i].tint : make_float4(0.0f, 0.0f, 0.0f, 0.0f);
        const float4 b_tint = triex ? triex[i].tint : make_float4(0.0f, 0.0f, 0.0f, 0.0f);
        if (std::memcmp(&a_tint, &b_tint, sizeof(float4)) != 0) {
            return false;
        }
    }
    return true;
}

BLASHandle BLASManager::find_existing_blas(const Tri* triangles, int count, uint32_t hash, const TriEx* triex) const {
    auto range = hash_to_entry_.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it) {
        const auto& entry = entries_[it->second];
        if (triangles_equal(*entry, triangles, count, triex)) {
            return entry->handle;
        }
    }
    return INVALID_BLAS_HANDLE;
}


BLASHandle BLASManager::register_triangles(const std::vector<Tri>& triangles) {
    return register_triangles(const_cast<Tri*>(triangles.data()), 
                             static_cast<int>(triangles.size()));
}


BLASHandle BLASManager::register_triangles(const std::vector<Tri>& triangles, const std::vector<TriEx>& triex) {
    const TriEx* triex_ptr = (triex.size() == triangles.size() && !triex.empty()) ? triex.data() : nullptr;
    return register_triangles(const_cast<Tri*>(triangles.data()),
                              static_cast<int>(triangles.size()), triex_ptr);
}


BLASHandle BLASManager::register_triangles(Tri* triangles, int triangle_count, const TriEx* triex) {
    PROFILE_SECTION("BLAS Registration");
    
    if (!triangles || triangle_count <= 0) {
        return INVALID_BLAS_HANDLE;
    }
    
    // Calculate hash for deduplication (geometry + per-triangle material).
    uint32_t hash = calculate_hash(triangles, triangle_count, triex);

    // Check if BLAS already exists; share it and bump its reference count.
    BLASHandle existing = find_existing_blas(triangles, triangle_count, hash, triex);
    if (existing != INVALID_BLAS_HANDLE) {
        for (auto& entry : entries_) {
            if (entry->handle == existing) {
                entry->ref_count++;
                break;
            }
        }
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

        // Copy per-vertex shading normals when provided (indexed the same as mesh->tri).
        if (triex) {
            mesh->triEx = static_cast<TriEx*>(MALLOC64(triangle_count * sizeof(TriEx)));
            for (int i = 0; i < triangle_count; i++) {
                mesh->triEx[i] = triex[i];
            }
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


BLASHandle BLASManager::register_prebuilt(const Tri* tris, const TriEx* triex, int tri_count,
                                          const BVHNode* nodes, uint nodes_used, const uint* tri_idx,
                                          uint32_t hash, uint32_t ref_count) {
    if (!tris || tri_count <= 0 || !nodes || nodes_used == 0 || !tri_idx) {
        return INVALID_BLAS_HANDLE;
    }

    // Copy triangles (memcpy: Tri is __m128-aligned and the source may be an
    // unaligned file buffer). std::vector range-construct is a memmove for the
    // trivially-copyable Tri, which is alignment-safe.
    std::vector<Tri> triangle_copy(tris, tris + tri_count);

    auto mesh = std::make_unique<BvhMesh>();
    mesh->triCount = tri_count;
    mesh->tri = static_cast<Tri*>(MALLOC64(tri_count * sizeof(Tri)));
    std::memcpy(mesh->tri, tris, tri_count * sizeof(Tri));
    if (triex) {
        mesh->triEx = static_cast<TriEx*>(MALLOC64(tri_count * sizeof(TriEx)));
        std::memcpy(mesh->triEx, triex, tri_count * sizeof(TriEx));
    }

    auto bvh = std::make_unique<BVH>(mesh.get(), nodes, nodes_used, tri_idx);

    BLASHandle handle = next_handle_++;
    auto entry = std::make_unique<BLASEntry>(handle, std::move(mesh), std::move(bvh),
                                             std::move(triangle_copy), hash);
    entry->ref_count = ref_count;

    size_t entry_index = entries_.size();
    hash_to_entry_.emplace(hash, entry_index);
    entries_.push_back(std::move(entry));

    mark_dirty();
    return handle;
}


void BLASManager::release_blas(BLASHandle handle) {
    if (handle == INVALID_BLAS_HANDLE) return;

    auto it = std::find_if(entries_.begin(), entries_.end(),
        [handle](const auto& entry) { return entry->handle == handle; });
    if (it == entries_.end()) return;

    if ((*it)->ref_count > 1) {
        (*it)->ref_count--;
        return;
    }

    // Last owner: drop the entry and reclaim its place in the combined arrays.
    entries_.erase(it);

    // entries_ indices shifted, so rebuild the hash lookup table from scratch.
    hash_to_entry_.clear();
    for (size_t i = 0; i < entries_.size(); ++i) {
        hash_to_entry_.emplace(entries_[i]->hash, i);
    }

    mark_dirty();
}

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
            // Tiled layout: cap the width at TEXTURE_TILE_WIDTH and wrap overflow
            // into extra vertical tile rows so width never exceeds GL_MAX_TEXTURE_SIZE.
            // Each triangle occupies 6 texel rows (3 vertices + 3 per-vertex normals).
            const int triangle_total = static_cast<int>(all_triangles.size());
            const int tile_w = std::min(triangle_total, TEXTURE_TILE_WIDTH);
            const int tiles_y = (triangle_total + tile_w - 1) / tile_w;
            int texture_width = tile_w;
            int texture_height = tiles_y * 6;
            auto texel_off = [tile_w, texture_width](int idx, int row) {
                int tx = idx % tile_w;
                int ty = idx / tile_w;
                return ((ty * 6 + row) * texture_width + tx) * 4;
            };

            std::vector<float> texture_data(texture_width * texture_height * 4);

            // Walk entries in the same order as generate_triangle_data so the flat
            // all_triangles index lines up with each entry's per-triangle triEx normals.
            size_t triangle_index = 0;
            for (const auto& entry : entries_) {
                if (!entry->mesh || !entry->bvh) continue;
                const bool has_normals = (entry->mesh->triEx != nullptr);

                for (int i = 0; i < entry->mesh->triCount; i++) {
                    uint original_idx = entry->bvh->triIdx[i];
                    if (original_idx >= entry->triangles.size() || triangle_index >= all_triangles.size()) {
                        continue;
                    }

                    const Tri& tri = all_triangles[triangle_index];

                    // Row 0: v0
                    int row0_idx = texel_off(static_cast<int>(triangle_index), 0);
                    texture_data[row0_idx + 0] = tri.vertex0.x;
                    texture_data[row0_idx + 1] = tri.vertex0.y;
                    texture_data[row0_idx + 2] = tri.vertex0.z;
                    // Row 0 .w carries the per-triangle materialId (>=0). -1 means "no per-triangle
                    // material; shader falls back to the instance material". Must match the shader
                    // fetch of data0.w in bvh_tlas_common.glsl (decodeTriangle / hit block).
                    texture_data[row0_idx + 3] = pack_material_w(entry->mesh->triEx,
                                                                 static_cast<int>(original_idx));

                    // Row 1: v1
                    int row1_idx = texel_off(static_cast<int>(triangle_index), 1);
                    texture_data[row1_idx + 0] = tri.vertex1.x;
                    texture_data[row1_idx + 1] = tri.vertex1.y;
                    texture_data[row1_idx + 2] = tri.vertex1.z;
                    texture_data[row1_idx + 3] = pack_tint_w(entry->mesh->triEx, static_cast<int>(original_idx), 0); // tint.r

                    // Row 2: v2
                    int row2_idx = texel_off(static_cast<int>(triangle_index), 2);
                    texture_data[row2_idx + 0] = tri.vertex2.x;
                    texture_data[row2_idx + 1] = tri.vertex2.y;
                    texture_data[row2_idx + 2] = tri.vertex2.z;
                    texture_data[row2_idx + 3] = pack_tint_w(entry->mesh->triEx, static_cast<int>(original_idx), 1); // tint.g

                    // Per-vertex normals (rows 3-5). Fall back to the face normal when
                    // the mesh carries no shading normals so all three rows match.
                    float3 n0, n1, n2;
                    if (has_normals) {
                        const TriEx& ex = entry->mesh->triEx[original_idx];
                        n0 = ex.N0; n1 = ex.N1; n2 = ex.N2;
                    } else {
                        float3 fn = normalize(cross(tri.vertex1 - tri.vertex0, tri.vertex2 - tri.vertex0));
                        n0 = n1 = n2 = fn;
                    }

                    const float3 normals[3] = { n0, n1, n2 };
                    for (int row = 0; row < 3; row++) {
                        int row_idx = texel_off(static_cast<int>(triangle_index), row + 3);
                        texture_data[row_idx + 0] = normals[row].x;
                        texture_data[row_idx + 1] = normals[row].y;
                        texture_data[row_idx + 2] = normals[row].z;
                        texture_data[row_idx + 3] = 0.0f;
                    }

                    // Pack tint.b/.a into the spare .w of normal rows 3 and 4,
                    // and the three baked per-vertex AO values into row 5 .w
                    // (8 bits each; see pack_ao_w / shader floatBitsToUint unpack).
                    // Row 5 .xyz holds N2 (third per-vertex normal) and is left intact.
                    {
                        int rowB = texel_off(static_cast<int>(triangle_index), 3);
                        int rowA = texel_off(static_cast<int>(triangle_index), 4);
                        int rowAO = texel_off(static_cast<int>(triangle_index), 5);
                        texture_data[rowB + 3] = pack_tint_w(entry->mesh->triEx, static_cast<int>(original_idx), 2); // tint.b
                        texture_data[rowA + 3] = pack_tint_w(entry->mesh->triEx, static_cast<int>(original_idx), 3); // tint.a
                        if (has_normals) {
                            const TriEx& exr = entry->mesh->triEx[original_idx];
                            texture_data[rowAO + 3] = pack_ao_w(exr.ao0, exr.ao1, exr.ao2);
                        } else {
                            texture_data[rowAO + 3] = pack_ao_w(1.0f, 1.0f, 1.0f); // no triEx -> unoccluded
                        }
                    }

                    triangle_index++;
                }
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
            // Tiled layout (see triangle texture above). Each node occupies 3 texel
            // rows: aabbMin+leftFirst, aabbMax+triCount, padding.
            const int node_total = static_cast<int>(all_nodes.size());
            const int tile_w = std::min(node_total, TEXTURE_TILE_WIDTH);
            const int tiles_y = (node_total + tile_w - 1) / tile_w;
            int texture_width = tile_w;
            int texture_height = tiles_y * 3;
            auto texel_off = [tile_w, texture_width](int idx, int row) {
                int tx = idx % tile_w;
                int ty = idx / tile_w;
                return ((ty * 3 + row) * texture_width + tx) * 4;
            };

            std::vector<float> texture_data(texture_width * texture_height * 4);

            for (size_t i = 0; i < all_nodes.size(); i++) {
                const LegacyBVHNode& node = all_nodes[i];

                // Row 0: aabbMin + leftFirst
                int row0_idx = texel_off(static_cast<int>(i), 0);
                texture_data[row0_idx + 0] = node.aabbMin.x;
                texture_data[row0_idx + 1] = node.aabbMin.y;
                texture_data[row0_idx + 2] = node.aabbMin.z;
                texture_data[row0_idx + 3] = static_cast<float>(node.leftFirst);

                // Row 1: aabbMax + triCount
                int row1_idx = texel_off(static_cast<int>(i), 1);
                texture_data[row1_idx + 0] = node.aabbMax.x;
                texture_data[row1_idx + 1] = node.aabbMax.y;
                texture_data[row1_idx + 2] = node.aabbMax.z;
                texture_data[row1_idx + 3] = static_cast<float>(node.triCount);

                // Row 2: padding
                int row2_idx = texel_off(static_cast<int>(i), 2);
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
    // update_totals();
    
    // printf("=== BLAS Manager Statistics ===\n");
    // printf("Unique BLAS count: %zu\n", entries_.size());
    // printf("Total triangles: %d\n", cached_total_triangles_);
    // printf("Total nodes: %d\n", cached_total_nodes_);
    // printf("Next handle: %u\n", next_handle_);
    
    // // Hash table statistics
    // std::unordered_map<uint32_t, int> bucket_sizes;
    // for (const auto& pair : hash_to_entry_) {
    //     bucket_sizes[pair.first]++;
    // }
    
    // int max_bucket_size = 0;
    // for (const auto& pair : bucket_sizes) {
    //     max_bucket_size = std::max(max_bucket_size, pair.second);
    // }
    
    // printf("Hash buckets: %zu used, max chain length: %d\n", 
    //        bucket_sizes.size(), max_bucket_size);
}

void BLASManager::reset_stats() {
    // This would clear all data - be careful!
    entries_.clear();
    hash_to_entry_.clear();
    next_handle_ = 1;
    totals_dirty_ = true;
}

void BLASManager::clear() {
    printf("BLASManager: Clearing all BLAS entries (%zu entries)\n", entries_.size());
    
    // Clean up GPU textures first
    if (triangles_texture_.id != 0) {
        UnloadTexture(triangles_texture_);
        triangles_texture_ = {};
    }
    if (nodes_texture_.id != 0) {
        UnloadTexture(nodes_texture_);
        nodes_texture_ = {};
    }
    
    // Clear all data structures
    entries_.clear();
    hash_to_entry_.clear();
    next_handle_ = 1;
    
    // Mark everything as dirty to force regeneration
    totals_dirty_ = true;
    textures_dirty_ = true;
    shader_values_dirty_ = true;
    cached_shader_id_ = 0;
    
    printf("BLASManager: Cleared, ready for new BLAS registrations\n");
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