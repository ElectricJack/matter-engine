#include "../include/blas_manager.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>

BLASManager::BLASManager()
    : next_handle_(1), cached_total_triangles_(0), cached_total_nodes_(0), totals_dirty_(true) {
}

BLASManager::~BLASManager() {
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

    // Fold one float's bit pattern. memcpy, NOT reinterpret_cast<uint32_t*>:
    // reading float storage through a uint32_t lvalue is a strict-aliasing
    // violation, and GCC -O2 exploited it — TBAA concluded the float stores
    // initializing the old `tnt` local were dead relative to the integer
    // loads, so the compiled hash folded 16 NEVER-WRITTEN stack bytes per
    // triangle and geometry identity changed with ambient stack garbage
    // (found by partstore_race_tests proof C, confirmed in the disassembly:
    // the tint loop read rsp..rsp+16 with no prior store).
    const auto fold = [&hash](float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        hash ^= bits;
        hash *= 16777619u; // FNV-1a prime
    };

    for (int i = 0; i < count; i++) {
        // Hash vertex positions only — member-wise. Tri's union{float3;__m128}
        // slots are 16-byte strided, so "9 consecutive floats from &vertex0"
        // (the previous code) hashed the two padding words at +12/+28 and
        // never saw vertex2.y/z (partstore_race_tests proofs A/B).
        const Tri& t = triangles[i];
        fold(t.vertex0.x); fold(t.vertex0.y); fold(t.vertex0.z);
        fold(t.vertex1.x); fold(t.vertex1.y); fold(t.vertex1.z);
        fold(t.vertex2.x); fold(t.vertex2.y); fold(t.vertex2.z);

        // Fold the per-triangle materialId into identity so meshes with identical
        // geometry but different materials hash apart. No triEx -> constant sentinel
        // (matches the -1 "no per-triangle material" convention), applied consistently
        // in triangles_equal so both sides agree on the null-material case.
        uint32_t mat = triex ? static_cast<uint32_t>(triex[i].materialId) : 0xFFFFFFFFu;
        hash ^= mat;
        hash *= 16777619u;

        // Fold the per-triangle tint into identity so geometry that differs only
        // by tint is not deduplicated. No triEx -> neutral (0,0,0,0).
        if (triex) {
            const float4& tnt = triex[i].tint;
            fold(tnt.x); fold(tnt.y); fold(tnt.z); fold(tnt.w);
        } else {
            for (int k = 0; k < 4; k++) fold(0.0f);
        }
    }

    return hash;
}

bool BLASManager::triangles_equal(const BLASEntry& entry, const Tri* b, int count, const TriEx* triex) const {
    const std::vector<Tri>& a = entry.triangles;
    if (a.size() != static_cast<size_t>(count)) return false;

    const TriEx* a_ex = entry.mesh ? entry.mesh->triEx : nullptr;
    for (int i = 0; i < count; i++) {
        // Member-wise: Tri's union slots are 16-byte strided, so a 36-byte
        // contiguous memcmp from &vertex0 (the previous code) compared the
        // padding words at +12/+28 and IGNORED vertex2.y/z — meshes differing
        // only there deduplicated onto one entry, silently adopting another
        // mesh's geometry and BVH (partstore_race_tests proof B).
        if (std::memcmp(&a[i].vertex0, &b[i].vertex0, sizeof(float3)) != 0 ||
            std::memcmp(&a[i].vertex1, &b[i].vertex1, sizeof(float3)) != 0 ||
            std::memcmp(&a[i].vertex2, &b[i].vertex2, sizeof(float3)) != 0) {
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


BLASHandle BLASManager::register_triangles(const std::vector<Tri>& triangles, bool force_subdiv_one_prim) {
    return register_triangles(const_cast<Tri*>(triangles.data()),
                             static_cast<int>(triangles.size()), nullptr, force_subdiv_one_prim);
}


BLASHandle BLASManager::register_triangles(const std::vector<Tri>& triangles, const std::vector<TriEx>& triex,
                                           bool force_subdiv_one_prim) {
    const TriEx* triex_ptr = (triex.size() == triangles.size() && !triex.empty()) ? triex.data() : nullptr;
    return register_triangles(const_cast<Tri*>(triangles.data()),
                              static_cast<int>(triangles.size()), triex_ptr, force_subdiv_one_prim);
}


BLASHandle BLASManager::register_triangles(Tri* triangles, int triangle_count, const TriEx* triex,
                                           bool force_subdiv_one_prim) {
    PROFILE_SECTION("BLAS Registration");
    
    if (!triangles || triangle_count <= 0) {
        return INVALID_BLAS_HANDLE;
    }
    
    // Calculate hash for deduplication (geometry + per-triangle material).
    uint32_t hash = calculate_hash(triangles, triangle_count, triex);

    // Check if BLAS already exists; share it and bump its reference count.
    BLASHandle existing = find_existing_blas(triangles, triangle_count, hash, triex);
    if (existing != INVALID_BLAS_HANDLE) {
        auto idx_it = handle_to_index_.find(existing);
        if (idx_it != handle_to_index_.end() && idx_it->second < entries_.size()) {
            entries_[idx_it->second]->ref_count++;
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
        // Round up to multiple of 64: sizeof(TriEx)==96 is not a multiple of 64, so
        // aligned_alloc (MALLOC64) would fail for odd triangle counts without this guard.
        if (triex) {
            size_t triex_bytes = ((static_cast<size_t>(triangle_count) * sizeof(TriEx) + 63) & ~size_t(63));
            mesh->triEx = static_cast<TriEx*>(MALLOC64(triex_bytes));
            for (int i = 0; i < triangle_count; i++) {
                mesh->triEx[i] = triex[i];
            }
        }

        // Create BVH using the proper constructor
        auto bvh = std::make_unique<BVH>(mesh.get());
        // force_subdiv_one_prim: explicit flag requested by the caller.
        // Previously this was triggered heuristically by triangle_count==3, which
        // changed production behaviour for real 3-tri meshes (code-review smell fix).
        if (force_subdiv_one_prim) {
            bvh->subdivToOnePrim = true;
            bvh->Build();
        }
        
        BLASHandle handle = next_handle_++;

        // Build tri_extra parallel array (empty when no triex provided).
        std::vector<TriEx> tri_extra_copy;
        if (triex) {
            tri_extra_copy.assign(triex, triex + triangle_count);
        }

        // Create entry
        auto entry = std::make_unique<BLASEntry>(handle, std::move(mesh), std::move(bvh),
                                                 std::move(triangle_copy), std::move(tri_extra_copy), hash);
        
        // Add to hash table and handle map
        size_t entry_index = entries_.size();
        hash_to_entry_.emplace(hash, entry_index);
        handle_to_index_.emplace(handle, entry_index);

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

    // Copy triangles via explicit memcpy: the source is a raw file buffer with
    // NO alignment guarantee, while Tri/TriEx are 16-byte-aligned SSE types.
    // std::vector range-construct/assign can compile to aligned vector loads
    // (movaps) for these element types, which GP-faults (SIGSEGV) when a
    // mid-buffer pointer's offset is not a multiple of 16. Only multi-BLAS
    // .part files hit this (entry N's payload offset depends on entry N-1's
    // tri_count); every single-BLAS part happens to land 16-aligned, which is
    // why this never fired before the Stage-4 stress fixture's Tree bake.
    std::vector<Tri> triangle_copy(static_cast<size_t>(tri_count));
    std::memcpy(triangle_copy.data(), tris, static_cast<size_t>(tri_count) * sizeof(Tri));

    auto mesh = std::make_unique<BvhMesh>();
    mesh->triCount = tri_count;
    mesh->tri = static_cast<Tri*>(MALLOC64(tri_count * sizeof(Tri)));
    std::memcpy(mesh->tri, tris, tri_count * sizeof(Tri));
    if (triex) {
        // Round up to multiple of 64: sizeof(TriEx)==96 is not a multiple of 64, so
        // aligned_alloc (MALLOC64) would fail for odd tri_counts without this guard.
        size_t triex_bytes = ((static_cast<size_t>(tri_count) * sizeof(TriEx) + 63) & ~size_t(63));
        mesh->triEx = static_cast<TriEx*>(MALLOC64(triex_bytes));
        std::memcpy(mesh->triEx, triex, tri_count * sizeof(TriEx));
    }

    auto bvh = std::make_unique<BVH>(mesh.get(), nodes, nodes_used, tri_idx);

    BLASHandle handle = next_handle_++;
    std::vector<TriEx> tri_extra_copy;
    if (triex) {
        // memcpy, NOT assign: see the alignment note above triangle_copy.
        tri_extra_copy.resize(static_cast<size_t>(tri_count));
        std::memcpy(tri_extra_copy.data(), triex,
                    static_cast<size_t>(tri_count) * sizeof(TriEx));
    }
    auto entry = std::make_unique<BLASEntry>(handle, std::move(mesh), std::move(bvh),
                                             std::move(triangle_copy), std::move(tri_extra_copy), hash);
    entry->ref_count = ref_count;

    size_t entry_index = entries_.size();
    hash_to_entry_.emplace(hash, entry_index);
    handle_to_index_.emplace(handle, entry_index);
    entries_.push_back(std::move(entry));

    mark_dirty();
    return handle;
}


void BLASManager::adopt_from(const BLASManager& staged,
                             std::unordered_map<BLASHandle, BLASHandle>& remap) {
    remap.clear();
    remap.reserve(staged.entries_.size());

    for (const auto& src : staged.entries_) {
        if (!src || src->triangles.empty()) continue;

        const TriEx* src_triex =
            (src->tri_extra.size() == src->triangles.size() && !src->tri_extra.empty())
                ? src->tri_extra.data() : nullptr;
        const int tri_count = static_cast<int>(src->triangles.size());

        // Same dedup register_triangles performs, minus the build: an identical
        // BLAS already resident (a rock shared with a neighbouring sector) just
        // gains a reference.
        //
        // Reference MULTIPLICITY must carry over, not collapse to one. When
        // the staged ladder deduplicated internally (a decimated rung bit-
        // identical to its neighbour — routine for small assets whose QEM run
        // is an identity), the staged entry's ref_count counts one ownership
        // PER RUNG, and the committed LoadedPart's owned_blas lists the handle
        // once per rung too — release() will decrement once per occurrence.
        // Adopting with a hardcoded 1 under-counts by (ref_count-1), so a
        // later release of the sector erased entries other RESIDENT parts
        // still referenced, leaving their lod_blas handles dangling (caught by
        // partstore_race_tests on the StreamMeadow cache: "null BLAS entry" on
        // resident grass parts).
        const BLASHandle existing =
            find_existing_blas(src->triangles.data(), tri_count, src->hash, src_triex);
        if (existing != INVALID_BLAS_HANDLE) {
            auto idx_it = handle_to_index_.find(existing);
            if (idx_it != handle_to_index_.end() && idx_it->second < entries_.size()) {
                entries_[idx_it->second]->ref_count += src->ref_count;
            }
            remap[src->handle] = existing;
            continue;
        }

        // Newcomer: install the BVH the worker already built, taking over the
        // staged entry's FULL reference count (see multiplicity note above);
        // the staged manager is discarded by the caller, not released entry by
        // entry, so there is no double count to reconcile.
        if (!src->bvh || !src->bvh->bvhNode || src->bvh->nodesUsed == 0 ||
            !src->bvh->triIdx) {
            continue;   // nothing usable to adopt; caller sees no remap entry
        }
        const BLASHandle adopted = register_prebuilt(
            src->triangles.data(), src_triex, tri_count,
            src->bvh->bvhNode, src->bvh->nodesUsed, src->bvh->triIdx,
            src->hash, /*ref_count=*/src->ref_count);
        if (adopted != INVALID_BLAS_HANDLE) remap[src->handle] = adopted;
    }
}

void BLASManager::release_blas(BLASHandle handle) {
    if (handle == INVALID_BLAS_HANDLE) return;

    // O(1) handle lookup via handle_to_index_ map.
    auto idx_it = handle_to_index_.find(handle);
    if (idx_it == handle_to_index_.end()) return;
    size_t idx = idx_it->second;
    if (idx >= entries_.size()) return;

    if (entries_[idx]->ref_count > 1) {
        entries_[idx]->ref_count--;
        return;
    }

    // Last owner: drop the entry and reclaim its place in the combined arrays.
    //
    // SWAP-AND-POP, not erase-from-middle-and-rebuild.
    //
    // This used to erase at `idx` (an O(N) vector shift) and then rebuild BOTH
    // lookup tables from scratch, every single call -- which made a function
    // whose own comment promises "O(1) handle lookup" cost O(N) to use. A part
    // owns MANY handles, so releasing one part was O(handles x entries), and a
    // streamed world evicting ~5 sectors a frame against thousands of live
    // entries turned that into 7 ms/frame of pure map rebuilding and bursts of
    // several hundred ms inside the blocking stream.apply_evictions job. It was
    // the largest single cause of the sub-second render hitches while flying
    // (docs/vt-mesh-entry-allocation-2026-08-09.md).
    //
    // Only the moved entry's indices change, so only its two map entries need
    // fixing. Order within entries_ is not meaningful -- it was already being
    // permuted by every erase above, and mark_dirty() rebuilds whatever the GPU
    // side derives from it.
    // hash_to_entry_ is a MULTIMAP -- several entries may share a hash -- so
    // both sides here have to match on the VALUE (the index), not just the key.
    // Order matters: remove the dead entry's records first, then move the
    // survivor, so a shared hash can never leave the wrong record standing.
    const size_t last = entries_.size() - 1;
    const auto dead_hash = entries_[idx]->hash;

    handle_to_index_.erase(idx_it);
    for (auto r = hash_to_entry_.equal_range(dead_hash);
         r.first != r.second; ++r.first) {
        if (r.first->second == idx) { hash_to_entry_.erase(r.first); break; }
    }

    if (idx != last) {
        std::swap(entries_[idx], entries_[last]);
        // The survivor moved from `last` to `idx`; repoint exactly its two
        // records and nothing else.
        handle_to_index_[entries_[idx]->handle] = idx;
        for (auto r = hash_to_entry_.equal_range(entries_[idx]->hash);
             r.first != r.second; ++r.first) {
            if (r.first->second == last) { r.first->second = idx; break; }
        }
    }
    entries_.pop_back();

    mark_dirty();
}

bool BLASManager::has_blas(BLASHandle handle) const {
    if (handle == INVALID_BLAS_HANDLE) return false;
    auto it = handle_to_index_.find(handle);
    return (it != handle_to_index_.end() && it->second < entries_.size());
}

BVH* BLASManager::get_bvh(BLASHandle handle) const {
    if (handle == INVALID_BLAS_HANDLE) return nullptr;
    auto it = handle_to_index_.find(handle);
    if (it == handle_to_index_.end() || it->second >= entries_.size()) return nullptr;
    return entries_[it->second]->bvh.get();
}

BvhMesh* BLASManager::get_mesh(BLASHandle handle) const {
    if (handle == INVALID_BLAS_HANDLE) return nullptr;
    auto it = handle_to_index_.find(handle);
    if (it == handle_to_index_.end() || it->second >= entries_.size()) return nullptr;
    return entries_[it->second]->mesh.get();
}

const BLASManager::BLASEntry* BLASManager::get_entry(BLASHandle handle) const {
    if (handle == INVALID_BLAS_HANDLE) return nullptr;
    auto it = handle_to_index_.find(handle);
    if (it == handle_to_index_.end() || it->second >= entries_.size()) return nullptr;
    return entries_[it->second].get();
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

    // O(1) existence check; walk only up to the target index.
    auto idx_it = handle_to_index_.find(handle);
    if (idx_it == handle_to_index_.end()) return offsets; // Not found

    int triangle_offset = 0;
    int node_offset = 0;
    size_t target = idx_it->second;

    for (size_t i = 0; i < target && i < entries_.size(); ++i) {
        const auto& entry = entries_[i];
        if (entry->mesh && entry->bvh) {
            triangle_offset += entry->mesh->triCount;
            node_offset += entry->bvh->nodesUsed;
        }
    }

    offsets.triangle_offset = triangle_offset;
    offsets.node_offset = node_offset;
    return offsets;
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
    handle_to_index_.clear();
    next_handle_ = 1;
    totals_dirty_ = true;
}

void BLASManager::clear() {
    printf("BLASManager: Clearing all BLAS entries (%zu entries)\n", entries_.size());

    // Clear all data structures
    entries_.clear();
    hash_to_entry_.clear();
    handle_to_index_.clear();
    next_handle_ = 1;

    // Mark everything as dirty to force regeneration
    totals_dirty_ = true;

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