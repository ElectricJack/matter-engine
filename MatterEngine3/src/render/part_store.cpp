#include "part_store.h"
#include "animation/anim_bundle.h"
#include "animation/animation_binding_bake.h"
#include "matrix_math.h"

#include "part_asset_v2.h"     // load_v2, cache_path_resolved, ChildInstance, LodLevels
#include "lod_bake.h"          // lod_bake::bake_lods, BakeTargets
#include "tlas_manager.hpp"    // TLASManager (load_v2 signature needs one)
#include "part_flatten.h"      // part_flatten::transform_uniform_scale

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <limits>
#include <unordered_set>
#include <sys/stat.h>

namespace viewer {

// Release exactly the references this LoadedPart registered.  Legacy view
// arrays are deliberately not authoritative: v2 mirrors a registration into a
// synthetic cluster, while v3 may legitimately register the same deduplicated
// handle several times.  Old injected test fixtures predate owned_blas, so
// retain their view-array fallback for that narrow test-only path.
static void release_loaded_part_blas(BLASManager& blas, const LoadedPart& lp) {
    if (!lp.owned_blas.empty()) {
        for (BLASHandle h : lp.owned_blas) {
            if (h != INVALID_BLAS_HANDLE) blas.release_blas(h);
        }
        return;
    }
    for (BLASHandle h : lp.lod_blas) {
        if (h != INVALID_BLAS_HANDLE) blas.release_blas(h);
    }
    for (const auto& cluster : lp.clusters) {
        for (BLASHandle h : cluster.lod_blas) {
            if (h != INVALID_BLAS_HANDLE) blas.release_blas(h);
        }
    }
}

static bool valid_indexed_mesh(const RasterMeshData& mesh) {
    if (mesh.vertex_count <= 0 || mesh.indices.empty() || mesh.indices.size() % 3 != 0)
        return false;
    const size_t vertices = static_cast<size_t>(mesh.vertex_count);
    if (mesh.vertices.size() != vertices * 3 || mesh.normals.size() != vertices * 3 ||
        mesh.colors.size() != vertices * 4 || mesh.texcoords.size() != vertices * 2 ||
        mesh.surface_uvs.size() != vertices * 2 || mesh.material_ids.size() != vertices ||
        mesh.baked_ao.size() != vertices)
        return false;
    return std::all_of(mesh.indices.begin(), mesh.indices.end(),
                       [vertices](uint32_t index) { return index < vertices; });
}

static void append_indexed_vertex(const RasterMeshData& source, uint32_t old_index,
                                  RasterMeshData& out) {
    const size_t vertex = static_cast<size_t>(old_index);
    out.vertices.insert(out.vertices.end(), source.vertices.begin() + vertex * 3,
                        source.vertices.begin() + vertex * 3 + 3);
    out.normals.insert(out.normals.end(), source.normals.begin() + vertex * 3,
                       source.normals.begin() + vertex * 3 + 3);
    out.colors.insert(out.colors.end(), source.colors.begin() + vertex * 4,
                      source.colors.begin() + vertex * 4 + 4);
    out.texcoords.insert(out.texcoords.end(), source.texcoords.begin() + vertex * 2,
                         source.texcoords.begin() + vertex * 2 + 2);
    out.surface_uvs.insert(out.surface_uvs.end(), source.surface_uvs.begin() + vertex * 2,
                           source.surface_uvs.begin() + vertex * 2 + 2);
    out.material_ids.push_back(source.material_ids[vertex]);
    out.baked_ao.push_back(source.baked_ao[vertex]);
}

static bool slice_rigid_segment_mesh(
        const RasterMeshData& source,
        const std::vector<matter::animation::BindingGeometryRange>& ranges,
        RasterMeshData& out) {
    if (!valid_indexed_mesh(source) || ranges.empty()) return false;
    const uint32_t triangle_count = static_cast<uint32_t>(source.indices.size() / 3);
    std::vector<bool> claimed(triangle_count, false);
    std::vector<uint32_t> remap(static_cast<size_t>(source.vertex_count), UINT32_MAX);
    for (const auto& range : ranges) {
        // A loaded Part retains indexed triangles, not the authoring field-op
        // stream.  A range with no direct-triangle ownership therefore cannot
        // be converted faithfully and must fail closed.
        if (range.triangle_begin >= range.triangle_end || range.triangle_end > triangle_count)
            return false;
        for (uint32_t triangle = range.triangle_begin; triangle != range.triangle_end; ++triangle) {
            if (claimed[triangle]) return false;
            claimed[triangle] = true;
            for (uint32_t corner = 0; corner != 3; ++corner) {
                const uint32_t old_index = source.indices[static_cast<size_t>(triangle) * 3 + corner];
                uint32_t& new_index = remap[old_index];
                if (new_index == UINT32_MAX) {
                    new_index = static_cast<uint32_t>(out.material_ids.size());
                    append_indexed_vertex(source, old_index, out);
                }
                out.indices.push_back(new_index);
            }
        }
    }
    out.vertex_count = static_cast<int>(out.material_ids.size());
    return out.vertex_count > 0 && !out.indices.empty();
}

static uint64_t rigid_subpart_hash(uint64_t source_hash, uint32_t segment_ordinal,
                                   const RasterMeshData& mesh) {
    uint64_t hash = 1469598103934665603ull;
    const uint32_t tag = 0x52475331u; // RGS1
    const uint64_t mesh_hash = indexed_part_geometry_signature(mesh, segment_ordinal);
    const auto append = [&hash](const auto& value) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t index = 0; index != sizeof(value); ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
    };
    append(tag);
    append(source_hash);
    append(segment_ordinal);
    append(mesh_hash);
    return hash ? hash : 1ull;
}

static float subpart_bound_radius(const RasterMeshData& mesh) {
    float minimum[3] = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
    float maximum[3] = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
    for (int index = 0; index != mesh.vertex_count; ++index) {
        for (int axis = 0; axis != 3; ++axis) {
            const float value = mesh.vertices[static_cast<size_t>(index) * 3 + axis];
            minimum[axis] = std::fmin(minimum[axis], value);
            maximum[axis] = std::fmax(maximum[axis], value);
        }
    }
    const float dx = maximum[0] - minimum[0];
    const float dy = maximum[1] - minimum[1];
    const float dz = maximum[2] - minimum[2];
    return 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
}

static void mesh_to_triangles(const RasterMeshData& mesh, std::vector<Tri>& triangles,
                              std::vector<TriEx>& extras) {
    triangles.reserve(mesh.indices.size() / 3);
    extras.reserve(mesh.indices.size() / 3);
    const auto position = [&mesh](uint32_t index) {
        const size_t at = static_cast<size_t>(index) * 3;
        return make_float3(mesh.vertices[at], mesh.vertices[at + 1], mesh.vertices[at + 2]);
    };
    const auto normal = [&mesh](uint32_t index) {
        const size_t at = static_cast<size_t>(index) * 3;
        return make_float3(mesh.normals[at], mesh.normals[at + 1], mesh.normals[at + 2]);
    };
    const auto uv = [&mesh](uint32_t index) {
        const size_t at = static_cast<size_t>(index) * 2;
        return make_float2(mesh.surface_uvs[at], mesh.surface_uvs[at + 1]);
    };
    for (size_t triangle = 0; triangle != mesh.indices.size() / 3; ++triangle) {
        const uint32_t a = mesh.indices[triangle * 3];
        const uint32_t b = mesh.indices[triangle * 3 + 1];
        const uint32_t c = mesh.indices[triangle * 3 + 2];
        Tri value{};
        value.vertex0 = position(a); value.vertex1 = position(b); value.vertex2 = position(c);
        value.centroid = make_float3((value.vertex0.x + value.vertex1.x + value.vertex2.x) / 3.0f,
                                     (value.vertex0.y + value.vertex1.y + value.vertex2.y) / 3.0f,
                                     (value.vertex0.z + value.vertex1.z + value.vertex2.z) / 3.0f);
        triangles.push_back(value);
        TriEx extra{};
        extra.N0 = normal(a); extra.N1 = normal(b); extra.N2 = normal(c);
        extra.uv0 = uv(a); extra.uv1 = uv(b); extra.uv2 = uv(c);
        extra.materialId = static_cast<int>(mesh.material_ids[a]);
        const size_t color = static_cast<size_t>(a) * 4;
        extra.tint = make_float4(mesh.colors[color] / 255.0f, mesh.colors[color + 1] / 255.0f,
                                 mesh.colors[color + 2] / 255.0f, mesh.colors[color + 3] / 255.0f);
        extra.ao0 = mesh.baked_ao[a]; extra.ao1 = mesh.baked_ao[b]; extra.ao2 = mesh.baked_ao[c];
        extras.push_back(extra);
    }
}

// ---------------------------------------------------------------------------
// walk_part_tree implementation — single recursive traversal shared by
// build_expansion, WorldComposer::compose, and the main.cpp TLAS-sizing walk.
// ---------------------------------------------------------------------------

static void walk_rec(uint64_t hash, const float parent_rel[16], int depth,
                     const std::function<const viewer::LoadedPart*(uint64_t)>& getter,
                     const std::function<void(const viewer::LoadedPart*, uint64_t,
                                              const float[16], int)>& visitor) {
    if (depth > 8) return;
    const viewer::LoadedPart* lp = getter(hash);
    if (!lp) return;
    visitor(lp, hash, parent_rel, depth);
    for (const auto& c : lp->children) {
        matter::Mat4f parent{};
        matter::Mat4f child{};
        std::memcpy(parent.m, parent_rel, sizeof parent.m);
        std::memcpy(child.m, c.transform, sizeof child.m);
        const matter::Mat4f rel = viewer::mat4_mul(parent, child);
        walk_rec(c.child_resolved_hash, rel.m, depth + 1, getter, visitor);
    }
}

void walk_part_tree(uint64_t root_hash,
        const std::function<const LoadedPart*(uint64_t)>& getter,
        const std::function<void(const LoadedPart*, uint64_t,
                                 const float[16], int)>& visitor) {
    static const float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    walk_rec(root_hash, kIdentity, 0, getter, visitor);
}

// ---------------------------------------------------------------------------
// build_expansion — thin wrapper over walk_part_tree
// ---------------------------------------------------------------------------

void build_expansion(uint64_t root_hash,
        const std::function<const LoadedPart*(uint64_t)>& getter,
        std::vector<ExpandedNode>& out) {
    walk_part_tree(root_hash, getter,
        [&](const LoadedPart* lp, uint64_t hash, const float rel[16], int /*depth*/) {
            if (lp->lod_mesh_data.empty()) return;
            ExpandedNode n;
            n.part_hash = hash;
            memcpy(n.rel_transform, rel, sizeof n.rel_transform);
            out.push_back(n);
        });
}

// ---------------------------------------------------------------------------

PartStore::PartStore(std::string cache_root) : cache_root_(std::move(cache_root)) {}

bool PartStore::build_rigid_segment_subparts(
        uint64_t source_part_hash, const matter::animation::BindingBake& binding,
        std::vector<uint64_t>& out_hashes) {
    out_hashes.clear();
    const auto source_it = loaded_.find(source_part_hash);
    if (source_part_hash == 0 || source_it == loaded_.end() ||
        binding.rigid_segments.empty())
        return false;
    const bool exact_partition =
        source_it->second.rigid_lod_mesh_data.size() == binding.rigid_segments.size() &&
        !source_it->second.rigid_lod_thresholds.empty();
    if (!exact_partition && source_it->second.lod_mesh_data.empty()) return false;

    // LOD0 is the complete indexed source stream.  The authored direct-triangle
    // ranges refer to that stream before the independent subpart LOD ladders are
    // rebuilt, so later decimation cannot change binding ownership.
    const RasterMeshData* source_mesh = exact_partition ? nullptr :
        &source_it->second.lod_mesh_data.front();
    std::vector<RasterMeshData> slices;
    slices.reserve(binding.rigid_segments.size());
    std::vector<uint64_t> hashes;
    hashes.reserve(binding.rigid_segments.size());
    for (size_t index = 0; index != binding.rigid_segments.size(); ++index) {
        RasterMeshData slice;
        if (exact_partition) {
            if (source_it->second.rigid_lod_mesh_data[index].empty() ||
                !valid_indexed_mesh(source_it->second.rigid_lod_mesh_data[index].front())) return false;
            slice = source_it->second.rigid_lod_mesh_data[index].front();
        } else if (!slice_rigid_segment_mesh(*source_mesh, binding.rigid_segments[index].geometry, slice)) {
            return false;
        }
        const uint64_t hash = rigid_subpart_hash(source_part_hash,
                                                  static_cast<uint32_t>(index), slice);
        if (hash == source_part_hash ||
            std::find(hashes.begin(), hashes.end(), hash) != hashes.end())
            return false;
        slices.push_back(std::move(slice));
        hashes.push_back(hash);
    }

    for (const RigidSubpartSet& cached : rigid_subparts_[source_part_hash]) {
        if (cached.hashes != hashes) continue;
        const bool complete = std::all_of(hashes.begin(), hashes.end(), [this](uint64_t hash) {
            return loaded_.find(hash) != loaded_.end() && rigid_subpart_owner_.find(hash) != rigid_subpart_owner_.end();
        });
        if (!complete) return false;
        out_hashes = hashes;
        return true;
    }
    for (uint64_t hash : hashes) {
        if (loaded_.find(hash) != loaded_.end() || rigid_subpart_owner_.find(hash) != rigid_subpart_owner_.end())
            return false;
    }

    std::vector<uint64_t> admitted;
    const auto rollback = [this, &admitted] {
        for (uint64_t hash : admitted) {
            const auto part = loaded_.find(hash);
            if (part != loaded_.end()) {
                release_loaded_part_blas(blas_, part->second);
                loaded_.erase(part);
            }
            rigid_subpart_owner_.erase(hash);
        }
    };
    for (size_t index = 0; index != slices.size(); ++index) {
        std::vector<Tri> triangles;
        std::vector<TriEx> extras;
        mesh_to_triangles(slices[index], triangles, extras);
        LoadedPart subpart;
        subpart.bound_radius = subpart_bound_radius(slices[index]);
        if (exact_partition) {
            const auto& meshes = source_it->second.rigid_lod_mesh_data[index];
            if (meshes.size() != source_it->second.rigid_lod_thresholds.size()) {
                rollback(); return false;
            }
            for (size_t level = 0; level != meshes.size(); ++level) {
                if (!valid_indexed_mesh(meshes[level])) { rollback(); return false; }
                std::vector<Tri> exact_triangles;
                std::vector<TriEx> exact_extras;
                mesh_to_triangles(meshes[level], exact_triangles, exact_extras);
                const BLASHandle handle = blas_.register_triangles(
                    exact_triangles.data(), static_cast<int>(exact_triangles.size()),
                    exact_extras.empty() ? nullptr : exact_extras.data());
                subpart.thresholds.push_back(source_it->second.rigid_lod_thresholds[level]);
                subpart.lod_blas.push_back(handle);
                subpart.owned_blas.push_back(handle);
                subpart.lod_mesh_data.push_back(meshes[level]);
            }
        } else {
            const lod_bake::LodLevels lods = lod_bake::bake_lods(
                triangles, lod_bake::BakeTargets{}, blas_, &extras);
            for (const auto& lod : lods) {
                if (lod.blas_indices.size() != 1 || lod.blas_indices[0] >= blas_.get_entries().size()) {
                    rollback(); return false;
                }
                const BLASHandle handle = blas_.get_entries()[lod.blas_indices[0]]->handle;
                const auto* entry = blas_.get_entry(handle);
                if (!entry) { rollback(); return false; }
                subpart.thresholds.push_back(lod.screen_size_threshold);
                subpart.lod_blas.push_back(handle);
                subpart.owned_blas.push_back(handle);
                if (subpart.lod_mesh_data.empty()) {
                    subpart.lod_mesh_data.push_back(slices[index]);
                } else {
                    const TriEx* extra = entry->tri_extra.size() == entry->triangles.size() &&
                                         !entry->tri_extra.empty() ? entry->tri_extra.data() : nullptr;
                    subpart.lod_mesh_data.push_back(build_raster_mesh_data(
                        entry->triangles.data(), extra, static_cast<int>(entry->triangles.size())));
                }
            }
        }
        if (subpart.lod_blas.empty()) {
            rollback();
            return false;
        }
        const uint64_t hash = hashes[index];
        loaded_.emplace(hash, std::move(subpart));
        rigid_subpart_owner_.emplace(hash, source_part_hash);
        admitted.push_back(hash);
    }
    rigid_subparts_[source_part_hash].push_back({hashes});
    out_hashes = std::move(hashes);
    return true;
}

std::string PartStore::disk_path(uint64_t part_hash) const {
    // cache_path_resolved returns the RELATIVE "parts/<hash>.part"; prefix cache_root_.
    return cache_root_ + "/" + part_asset::cache_path_resolved(part_hash);
}

// Task 2: resolve the actual disk path, checking scratch dir first, then cache.
static std::string resolve_artifact_path(uint64_t part_hash, const std::string& scratch_dir,
                                         const std::string& cache_root) {
    struct stat st;
    if (!scratch_dir.empty()) {
        std::string scratch_path = scratch_dir + "/" + part_asset::cache_path_resolved(part_hash);
        if (::stat(scratch_path.c_str(), &st) == 0) {
            return scratch_path;
        }
    }
    return cache_root + "/" + part_asset::cache_path_resolved(part_hash);
}

// A linked PART is a generation with MANM/MACM siblings, not three independent
// cache lookups.  Pick its artifact root once per attempt and use that root for
// every sibling.  In particular, a transient scratch PART must never be paired
// with a persistent-cache animation manifest from another build generation.
static std::string select_artifact_root(uint64_t part_hash, const std::string& scratch_dir,
                                        const std::string& cache_root) {
    struct stat st;
    if (!scratch_dir.empty()) {
        const std::string scratch_path = scratch_dir + "/" + part_asset::cache_path_resolved(part_hash);
        if (::stat(scratch_path.c_str(), &st) == 0) return scratch_dir;
    }
    return cache_root;
}

bool PartStore::has(uint64_t part_hash) const {
    if (loaded_.count(part_hash)) return true;
    struct stat st;
    return ::stat(resolve_artifact_path(part_hash, scratch_dir_, cache_root_).c_str(), &st) == 0;
}

// Flat-preferred load: a bake-time flattened artifact (<hash>.flat.part) already
// carries the whole merged subtree plus per-cluster error-bounded LOD ladders.
// Tries v3 first (Task 11 format: clustered flat); falls back to legacy v2 flat
// if v3 is unavailable. Returns false (fall back to the compositional .part) when
// the file is absent or fails to load in either format.
bool PartStore::load_flat(uint64_t part_hash, const std::string& artifact_root, LoadedPart& lp) {
    // The caller has already selected and validated the canonical `.part`
    // from this root as ANLK-free.  Do not independently probe scratch/cache:
    // a flat is only valid beside that exact canonical static Part.
    const std::string path = artifact_root + "/" + part_asset::cache_path_flat(part_hash);
    const auto rollback = [&] {
        release_loaded_part_blas(blas_, lp);
        lp = LoadedPart{};
        return false;
    };

    // Sniff version first; fall back to compositional path when absent.
    uint32_t ver = part_asset::peek_format_version(path);
    if (ver == 0) return false;   // absent or unreadable

    if (ver == part_asset::kFormatVersionFlat) {
        // --- v3 clustered flat ---
        BLASManager scratch;
        TLASManager scratch_tlas(65536);
        std::vector<part_asset::FlatCluster> clusters_in;
        std::vector<part_asset::FlatInstanceRef> refs_in;
        if (!part_asset::load_flat_v3(path, part_hash, scratch, scratch_tlas, clusters_in, refs_in) ||
            clusters_in.empty()) {
            printf("PartStore: v3 flat artifact unusable for %016llx (%s), falling back\n",
                   (unsigned long long)part_hash, path.c_str());
            return false;
        }

        // Determine if the flat is segmented (has any coarse-segment clusters).
        bool segmented = std::any_of(clusters_in.begin(), clusters_in.end(),
                                     [](const part_asset::FlatCluster& c){ return c.segment == 1; });

        // Partition fine clusters before coarse (stable: preserves within-segment order).
        // This must happen BEFORE the registration loops so lp.clusters[0..fine_count-1]
        // are contiguous fine-segment entries.
        std::stable_partition(clusters_in.begin(), clusters_in.end(),
                              [](const part_asset::FlatCluster& c){ return c.segment == 0; });

        const auto& entries = scratch.get_entries();

        // Determine max LOD count across clusters.
        // When segmented, use coarse clusters only for the legacy view — those are the
        // merged representation; fine clusters are trunk-only and should not inflate the
        // whole-part threshold.
        size_t max_lods = 0;
        for (const auto& cl : clusters_in) {
            if (segmented && cl.segment != 1) continue;
            max_lods = std::max(max_lods, cl.lods.size());
        }
        if (max_lods == 0) {
            // No coarse clusters (or empty): fall back to all clusters for max_lods.
            for (const auto& cl : clusters_in) max_lods = std::max(max_lods, cl.lods.size());
        }
        if (max_lods == 0) return rollback();

        // --- Step 1: Legacy whole-part view for the RT path (WorldComposer/TLAS). ---
        // IMPORTANT: lp.lod_mesh_data[0..max_lods-1] are the whole-part entries (parallel
        // to lp.lod_blas). Per-cluster mesh-data is appended AFTER these entries so that
        // the RasterComposer's `lp.lod_mesh_data[level]` access remains correct.
        //
        // Legacy level i = concatenation over clusters of level min(i, cluster.levels-1).
        //   When segmented: over COARSE clusters only (segment==1). That is the merged
        //   representation (children inlined); fine clusters are trunk-only stubs.
        // Legacy threshold i = max over those same clusters.
        // bound_radius = union of cluster AABBs from the stored FlatCluster AABBs
        //   over ALL clusters (both segments).
        float g_mn[3] = {1e30f,1e30f,1e30f}, g_mx[3] = {-1e30f,-1e30f,-1e30f};
        for (const auto& cl : clusters_in) {
            for (int k = 0; k < 3; ++k) {
                g_mn[k] = std::fmin(g_mn[k], cl.aabb_min[k]);
                g_mx[k] = std::fmax(g_mx[k], cl.aabb_max[k]);
            }
        }
        {
            float dx = g_mx[0]-g_mn[0], dy = g_mx[1]-g_mn[1], dz = g_mx[2]-g_mn[2];
            lp.bound_radius = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
        }

        for (size_t li = 0; li < max_lods; ++li) {
            std::vector<Tri> tris;
            std::vector<TriEx> triex;
            float thr = 0.0f;
            for (const auto& cl : clusters_in) {
                // When segmented, the legacy view uses only coarse clusters.
                if (segmented && cl.segment != 1) continue;
                // Use level min(li, cluster_levels-1) for clusters with fewer levels.
                size_t use_li = (li < cl.lods.size()) ? li : cl.lods.size() - 1;
                thr = std::fmax(thr, cl.lods[use_li].screen_size_threshold);
                for (uint32_t bi : cl.lods[use_li].blas_indices) {
                    if (bi >= entries.size()) continue;
                    tris.insert(tris.end(), entries[bi]->triangles.begin(), entries[bi]->triangles.end());
                    triex.insert(triex.end(), entries[bi]->tri_extra.begin(), entries[bi]->tri_extra.end());
                }
            }
            if (tris.empty()) continue;
            const TriEx* ex = (triex.size() == tris.size()) ? triex.data() : nullptr;
            BLASHandle h = blas_.register_triangles(tris.data(), (int)tris.size(), ex);
            lp.owned_blas.push_back(h);
            lp.thresholds.push_back(thr);
            lp.lod_blas.push_back(h);

            // Append legacy whole-part mesh-data at lod_mesh_data[li] (parallel to lod_blas).
            if (const auto* e = blas_.get_entry(h)) {
                const TriEx* mesh_ex = (e->tri_extra.size() == e->triangles.size() && !e->tri_extra.empty())
                                          ? e->tri_extra.data() : nullptr;
                lp.lod_mesh_data.push_back(
                    build_raster_mesh_data(e->triangles.data(), mesh_ex, (int)e->triangles.size()));
            } else {
                lp.lod_mesh_data.push_back({});
            }
        }
        if (lp.lod_blas.empty()) return rollback();

        // --- Step 2: Per-cluster data (for Task 13 per-cluster GPU culling). ---
        // Each cluster gets its own LoadedCluster with parallel thresholds / lod_blas /
        // lod_mesh arrays. Per-cluster BLAS entries are individually registered into the
        // shared blas_. Per-cluster mesh-data is APPENDED to lp.lod_mesh_data AFTER the
        // legacy whole-part entries (indices lp.lod_blas.size()..end). lod_mesh[i] is
        // an absolute index into lp.lod_mesh_data.
        //
        // Cluster order: fine (segment=0) clusters first (stable_partition above ensures
        // this). fine_pushed counts PUSHED (non-empty) fine clusters — the empty-cluster
        // skip means we can't simply count input clusters with segment==0.
        uint32_t fine_pushed = 0;
        lp.clusters.reserve(clusters_in.size());
        for (const auto& cl_in : clusters_in) {
            LoadedCluster cl_out;
            // AABB / radius from the FlatCluster's stored AABB.
            std::memcpy(cl_out.aabb_min, cl_in.aabb_min, sizeof cl_out.aabb_min);
            std::memcpy(cl_out.aabb_max, cl_in.aabb_max, sizeof cl_out.aabb_max);
            float dx = cl_in.aabb_max[0] - cl_in.aabb_min[0];
            float dy = cl_in.aabb_max[1] - cl_in.aabb_min[1];
            float dz = cl_in.aabb_max[2] - cl_in.aabb_min[2];
            cl_out.radius = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);

            for (size_t li = 0; li < cl_in.lods.size(); ++li) {
                const auto& lod_in = cl_in.lods[li];
                // Gather tris from this cluster's lod level.
                std::vector<Tri> ctris;
                std::vector<TriEx> ctriex;
                for (uint32_t bi : lod_in.blas_indices) {
                    if (bi >= entries.size()) continue;
                    ctris.insert(ctris.end(), entries[bi]->triangles.begin(), entries[bi]->triangles.end());
                    ctriex.insert(ctriex.end(), entries[bi]->tri_extra.begin(), entries[bi]->tri_extra.end());
                }
                if (ctris.empty()) continue;
                const TriEx* cex = (ctriex.size() == ctris.size()) ? ctriex.data() : nullptr;
                BLASHandle ch = blas_.register_triangles(ctris.data(), (int)ctris.size(), cex);
                lp.owned_blas.push_back(ch);

                // Append cluster-level mesh-data after the legacy whole-part entries.
                int mesh_idx = (int)lp.lod_mesh_data.size();
                if (const auto* ce = blas_.get_entry(ch)) {
                    const TriEx* mex = (ce->tri_extra.size() == ce->triangles.size() && !ce->tri_extra.empty())
                                           ? ce->tri_extra.data() : nullptr;
                    lp.lod_mesh_data.push_back(
                        build_raster_mesh_data(ce->triangles.data(), mex, (int)ce->triangles.size()));
                } else {
                    lp.lod_mesh_data.push_back({});
                }

                cl_out.thresholds.push_back(lod_in.screen_size_threshold);
                cl_out.lod_blas.push_back(ch);
                cl_out.lod_mesh.push_back(mesh_idx);
            }
            if (cl_out.lod_blas.empty()) {
                // Cluster yielded no geometry - skip rather than leaving an empty entry.
                // Do NOT increment fine_pushed here: we only count pushed (non-empty) clusters.
                continue;
            }
            if (segmented && cl_in.segment == 0) ++fine_pushed;
            lp.clusters.push_back(std::move(cl_out));
        }
        if (lp.clusters.empty()) return rollback();

        // Set fine_cluster_count: for segmented flats, count pushed fine clusters;
        // for unsegmented flats, all clusters are "fine" (fine_cluster_count == size).
        lp.fine_cluster_count = segmented ? fine_pushed : (uint32_t)lp.clusters.size();

        // Filter instance refs: only keep refs with inline_cutover > 0.
        // Budget-BOUNDARY refs have cutover == 0 (never inline) and are excluded.
        for (const auto& ref : refs_in) {
            if (ref.inline_cutover > 0.0f) {
                lp.flat_refs.push_back(ref);
                lp.inline_cutover = std::max(lp.inline_cutover, ref.inline_cutover);
            }
        }

        printf("PartStore: loaded v3 FLAT part %016llx (%zu LOD levels, %zu clusters, %zu refs)\n",
               (unsigned long long)part_hash, lp.lod_blas.size(), lp.clusters.size(),
               lp.flat_refs.size());
        return true;
    }

    // --- Legacy v2 flat (pre-Task-11) ---
    if (ver == 2) {
        BLASManager scratch;
        TLASManager scratch_tlas(65536);
        std::vector<part_asset::ChildInstance> children;
        part_asset::LodLevels lods_in;
        if (!part_asset::load_v2(path, part_hash, scratch, scratch_tlas, children, lods_in) ||
            lods_in.empty()) {
            printf("PartStore: flat artifact unusable for %016llx (%s), falling back\n",
                   (unsigned long long)part_hash, path.c_str());
            return false;
        }

        const auto& entries = scratch.get_entries();

        // Build the legacy whole-part LOD ladder AND accumulate a synthetic single
        // cluster from the loaded lods so the raster path is uniform.
        LoadedCluster syn_cl;
        float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
        bool aabb_set = false;

        for (size_t li = 0; li < lods_in.size(); ++li) {
            std::vector<Tri> tris;
            std::vector<TriEx> triex;
            for (uint32_t bi : lods_in[li].blas_indices) {
                if (bi >= entries.size()) continue;
                tris.insert(tris.end(), entries[bi]->triangles.begin(), entries[bi]->triangles.end());
                triex.insert(triex.end(), entries[bi]->tri_extra.begin(), entries[bi]->tri_extra.end());
            }
            if (tris.empty()) continue;
            const TriEx* ex = (triex.size() == tris.size()) ? triex.data() : nullptr;
            BLASHandle h = blas_.register_triangles(tris.data(), (int)tris.size(), ex);
            lp.owned_blas.push_back(h);
            lp.thresholds.push_back(lods_in[li].screen_size_threshold);
            lp.lod_blas.push_back(h);

            int mesh_idx = (int)lp.lod_mesh_data.size();
            if (const auto* e = blas_.get_entry(h)) {
                const TriEx* mesh_ex = (e->tri_extra.size() == e->triangles.size() && !e->tri_extra.empty())
                                          ? e->tri_extra.data() : nullptr;
                lp.lod_mesh_data.push_back(
                    build_raster_mesh_data(e->triangles.data(), mesh_ex, (int)e->triangles.size()));
            } else {
                lp.lod_mesh_data.push_back({});
            }

            // Accumulate synthetic cluster level (mirrors legacy view exactly).
            syn_cl.thresholds.push_back(lods_in[li].screen_size_threshold);
            syn_cl.lod_blas.push_back(h);
            syn_cl.lod_mesh.push_back(mesh_idx);

            if (!aabb_set) {
                auto acc = [&](const float3& v){
                    mn[0]=std::fmin(mn[0],v.x); mx[0]=std::fmax(mx[0],v.x);
                    mn[1]=std::fmin(mn[1],v.y); mx[1]=std::fmax(mx[1],v.y);
                    mn[2]=std::fmin(mn[2],v.z); mx[2]=std::fmax(mx[2],v.z);
                };
                for (const auto& t : tris) { acc(t.vertex0); acc(t.vertex1); acc(t.vertex2); }
                float dx=mx[0]-mn[0], dy=mx[1]-mn[1], dz=mx[2]-mn[2];
                lp.bound_radius = 0.5f * std::sqrt(dx*dx+dy*dy+dz*dz);
                aabb_set = true;
            }
        }
        if (lp.lod_blas.empty()) return rollback();

        // Finalise synthetic cluster AABB.
        std::memcpy(syn_cl.aabb_min, mn, sizeof mn);
        std::memcpy(syn_cl.aabb_max, mx, sizeof mx);
        {
            float dx = mx[0]-mn[0], dy = mx[1]-mn[1], dz = mx[2]-mn[2];
            syn_cl.radius = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
        }
        if (!syn_cl.lod_blas.empty()) lp.clusters.push_back(std::move(syn_cl));

        // v2 flat is never segmented: all clusters are fine.
        lp.fine_cluster_count = (uint32_t)lp.clusters.size();

        printf("PartStore: loaded v2 FLAT part %016llx (%zu LOD levels, 1 synthetic cluster)\n",
               (unsigned long long)part_hash, lp.lod_blas.size());
        return true;
    }

    // Unknown version.
    printf("PartStore: unrecognized flat artifact version %u for %016llx, falling back\n",
           ver, (unsigned long long)part_hash);
    return false;
}

const LoadedPart* PartStore::get_or_load(uint64_t part_hash) {
    auto cached = loaded_.find(part_hash);
    if (cached != loaded_.end()) return &cached->second;

    // A flat artifact is an acceleration of an ANLK-free canonical Part, not
    // an independently selectable cache entry.  Select the `.part` root
    // first, parse its exact suffix, and only then admit that root's flat
    // sibling.  A linked PART always takes the coherent PART/MANM/MACM path;
    // it must never silently downgrade to a static flat from either root.
    {
        const std::string selected_root = select_artifact_root(part_hash, scratch_dir_, cache_root_);
        const std::string canonical_part =
            selected_root + "/" + part_asset::cache_path_resolved(part_hash);
        uint64_t canonical_fingerprint = 0;
        LoadedPart flat;
        if (part_asset::load_static_part_snapshot(canonical_part, part_hash,
                                                  canonical_fingerprint) &&
            load_flat(part_hash, selected_root, flat)) {
#ifdef MATTER_TEST_CACHE_VALIDATION_HOOK
            if (flat_admission_hook_for_tests_) flat_admission_hook_for_tests_();
#endif
            // Both snapshots parse one exact canonical Part from the root we
            // selected before loading the flat.  A replacement (including a
            // newly linked generation) invalidates this static acceleration;
            // fall through and re-probe the normal coherent loader instead.
            uint64_t final_fingerprint = 0;
            if (part_asset::load_static_part_snapshot(canonical_part, part_hash,
                                                       final_fingerprint) &&
                final_fingerprint == canonical_fingerprint) {
                // Insert the parent FIRST (before any recursive child loads) to prevent
                // re-entrancy: if a child transitively references the same parent hash,
                // the early-out at the top of get_or_load will return the already-inserted
                // (partially constructed) entry rather than recursing infinitely.
                loaded_.emplace(part_hash, std::move(flat));

                // Recursively load each flat_ref child. The parent is already in loaded_
                // so circular references are safe.
                for (const auto& ref : loaded_[part_hash].flat_refs)
                    get_or_load(ref.child_resolved_hash);

                // Build expansion into a local vector first, then assign.
                std::vector<ExpandedNode> exp;
                build_expansion(part_hash, [this](uint64_t h){ return get_or_load(h); }, exp);
                loaded_[part_hash].expansion = std::move(exp);
                return &loaded_[part_hash];
            }
            // The decoded flat was never published. Undo every shared-BLAS
            // registration before retrying the coherent Part path below.
            release_loaded_part_blas(blas_, flat);
        }
    }

    // Read a linked artifact as a bounded coherent snapshot.  The manifest is
    // published last, so a reader can legitimately observe a new PART before
    // its matching MANM/MACM manifest.  Such a window is a cache miss, never a
    // static fallback; retrying a few fresh snapshots lets an in-flight atomic
    // publisher finish without ever accepting a mixed generation.
    std::unique_ptr<BLASManager> scratch;
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods_in;   // .part stores LOD0 only (empty levels)
    std::vector<part_asset::VolumeEmitter> emitters;
    std::optional<part_asset::PartAnimationLink> animation_link;
    matter::animation::AnimAsset loaded_animation;
    bool coherent = false;
    for (int attempt = 0; attempt != 3 && !coherent; ++attempt) {
        const std::string selected_root = select_artifact_root(part_hash, scratch_dir_, cache_root_);
        const std::string path = selected_root + "/" + part_asset::cache_path_resolved(part_hash);
        auto candidate_scratch = std::make_unique<BLASManager>();
        // Sized to match the part bake's group cap: a detailed trunk bakes to >256
        // mesh groups. The scratch TLAS is unused for geometry (we re-bake LODs from
        // the BLAS triangles below), but an undersized cap spams capacity warnings.
        TLASManager candidate_tlas(65536);
        std::vector<part_asset::ChildInstance> candidate_children;
        part_asset::LodLevels candidate_lods;
        std::vector<part_asset::VolumeEmitter> candidate_emitters;
        std::optional<part_asset::PartAnimationLink> candidate_link;
        if (!part_asset::load_v2(path, part_hash, *candidate_scratch, candidate_tlas,
                                 candidate_children, candidate_lods, candidate_emitters,
                                 candidate_link)) {
            continue;
        }

        matter::animation::AnimAsset candidate_animation;
        if (candidate_link) {
            // A linked Part is never a valid static fallback.  Validate the
            // whole committed sibling generation from this same selected root.
            BLASManager checked; matter::animation::Diagnostics diagnostics;
            if (!matter::animation::load_committed_animation_bundle(selected_root, part_hash,
                                                                      checked, candidate_animation,
                                                                      diagnostics)) {
                continue;
            }
            std::optional<part_asset::PartAnimationLink> final_link;
            if (!part_asset::load_animation_link(path, part_hash, final_link) || !final_link ||
                final_link->nonce_high != candidate_link->nonce_high ||
                final_link->nonce_low != candidate_link->nonce_low ||
                candidate_animation.nonce.high != candidate_link->nonce_high ||
                candidate_animation.nonce.low != candidate_link->nonce_low) {
                continue;
            }
        }

        scratch = std::move(candidate_scratch);
        children = std::move(candidate_children);
        lods_in = std::move(candidate_lods);
        emitters = std::move(candidate_emitters);
        animation_link = candidate_link;
        if (candidate_link) loaded_animation = std::move(candidate_animation);
        coherent = true;
    }
    if (!coherent) {
        printf("PartStore: coherent load failed for %016llx\n", (unsigned long long)part_hash);
        return nullptr;
    }
    // Failures remain uncommitted but are intentionally re-probed on the next
    // get_or_load.  HostBaker can publish a new coherent generation under the
    // same resolved hash after a transient corrupt/torn cache observation.
    const matter::animation::AnimAsset* animation_asset = animation_link
        ? animation_assets_.insert(std::move(loaded_animation)) : nullptr;

    // Animated artifacts carry finalized, owner-separated LOD streams.  Do
    // not concatenate and re-bake them here: that would reintroduce rigid
    // triangles into the skinned root and destroy the bake-time ownership
    // contract.  Instead materialize the skin stream as the root and retain
    // every rigid stream for `build_rigid_segment_subparts` below.
    matter::animation::BindingBake partition;
    const bool has_partition = animation_asset &&
        matter::animation::get_anim_binding_bake(*animation_asset, partition) &&
        (!partition.lods.empty() || !partition.rigid_segments.empty()) && !lods_in.empty();
    if (has_partition) {
        const size_t level_count = !partition.lods.empty()
            ? partition.lods.size()
            : partition.rigid_segments.front().lod_geometry.size();
        auto reject_partition = [&]() -> const LoadedPart* {
            std::printf("PartStore: invalid partitioned animation geometry for %016llx\n",
                        static_cast<unsigned long long>(part_hash));
            return nullptr;
        };
        if (level_count == 0 || lods_in.size() != level_count ||
            (!partition.lods.empty() && partition.lods.size() != level_count))
            return reject_partition();
        for (const auto& segment : partition.rigid_segments)
            if (segment.lod_geometry.size() != level_count) return reject_partition();
        const size_t owner_count = (partition.lods.empty() ? 0u : 1u) +
                                   partition.rigid_segments.size();
        const auto& source_entries = scratch->get_entries();
        for (const auto& level : lods_in) {
            if (level.blas_indices.size() != owner_count) return reject_partition();
            std::unordered_set<uint32_t> streams;
            for (uint32_t index : level.blas_indices)
                if (index >= source_entries.size() || !source_entries[index] ||
                    source_entries[index]->triangles.empty() ||
                    !streams.insert(index).second)
                    return reject_partition();
        }

        LoadedPart partitioned;
        partitioned.children = std::move(children);
        partitioned.animation_asset = animation_asset;
        partitioned.rigid_lod_mesh_data.resize(partition.rigid_segments.size());
        partitioned.rigid_lod_thresholds.reserve(level_count);
        float mn[3] = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
        float mx[3] = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
        const auto include_mesh = [&](const RasterMeshData& mesh) {
            for (int vertex = 0; vertex != mesh.vertex_count; ++vertex) {
                for (int axis = 0; axis != 3; ++axis) {
                    const float value = mesh.vertices[static_cast<size_t>(vertex) * 3 + axis];
                    mn[axis] = std::fmin(mn[axis], value); mx[axis] = std::fmax(mx[axis], value);
                }
            }
        };
        const auto source_mesh = [&](size_t level, uint32_t slot, RasterMeshData& out) {
            if (level >= lods_in.size() || slot >= lods_in[level].blas_indices.size()) return false;
            const uint32_t index = lods_in[level].blas_indices[slot];
            const auto& entries = scratch->get_entries();
            if (index >= entries.size() || !entries[index] || entries[index]->triangles.empty()) return false;
            const auto& entry = entries[index];
            const TriEx* extra = entry->tri_extra.size() == entry->triangles.size() && !entry->tri_extra.empty()
                ? entry->tri_extra.data() : nullptr;
            out = build_raster_mesh_data(entry->triangles.data(), extra,
                                         static_cast<int>(entry->triangles.size()));
            return valid_indexed_mesh(out);
        };
        for (size_t level = 0; level != level_count; ++level) {
            partitioned.rigid_lod_thresholds.push_back(lods_in[level].screen_size_threshold);
            if (!partition.lods.empty()) {
                const auto& skin = partition.lods[level];
                RasterMeshData mesh;
                if (!source_mesh(level, skin.blas_slot, mesh) ||
                    skin.vertex_count != static_cast<uint32_t>(mesh.vertex_count) ||
                    skin.indexed_vertex_signature != indexed_part_geometry_signature(mesh, static_cast<uint32_t>(level)))
                    return reject_partition();
                std::vector<Tri> triangles;
                std::vector<TriEx> extras;
                mesh_to_triangles(mesh, triangles, extras);
                const BLASHandle handle = blas_.register_triangles(
                    triangles.data(), static_cast<int>(triangles.size()),
                    extras.empty() ? nullptr : extras.data());
                partitioned.thresholds.push_back(lods_in[level].screen_size_threshold);
                partitioned.lod_blas.push_back(handle);
                partitioned.owned_blas.push_back(handle);
                partitioned.lod_mesh_data.push_back(std::move(mesh));
                include_mesh(partitioned.lod_mesh_data.back());
            }
            for (size_t segment = 0; segment != partition.rigid_segments.size(); ++segment) {
                const auto& ownership = partition.rigid_segments[segment].lod_geometry[level];
                RasterMeshData mesh;
                if (!source_mesh(level, ownership.blas_slot, mesh) ||
                    mesh.indices.size() / 3 != ownership.triangle_count)
                    return reject_partition();
                include_mesh(mesh);
                partitioned.rigid_lod_mesh_data[segment].push_back(std::move(mesh));
            }
        }
        if (std::isfinite(mn[0])) {
            const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
            partitioned.bound_radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        auto inserted = loaded_.emplace(part_hash, std::move(partitioned));
        std::vector<ExpandedNode> exp;
        build_expansion(part_hash, [this](uint64_t h){ return get_or_load(h); }, exp);
        inserted.first->second.expansion = std::move(exp);
        return &inserted.first->second;
    }

    // Gather full-res triangles (and their parallel per-triangle TriEx, which
    // carries the baked materialId/tint/normals) for lod_bake. Without the TriEx
    // the re-baked LOD geometry has no per-triangle material, so every triangle
    // falls back to the instance material in the shader and the whole world renders
    // as one color. e->triangles and e->tri_extra are parallel in registration order.
    std::vector<Tri> tris;
    std::vector<TriEx> triex;
    for (const auto& e : scratch->get_entries()) {
        tris.insert(tris.end(), e->triangles.begin(), e->triangles.end());
        triex.insert(triex.end(), e->tri_extra.begin(), e->tri_extra.end());
    }
    // Only pass TriEx through when it is fully parallel to the triangle list;
    // a partial/absent table would misalign materials.
    const std::vector<TriEx>* triex_ptr = (triex.size() == tris.size() && !triex.empty())
                                          ? &triex : nullptr;

    // Bound radius = half AABB diagonal (drives projected-size LOD math).
    float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
    auto acc = [&](const float3& v){
        mn[0]=std::fmin(mn[0],v.x); mx[0]=std::fmax(mx[0],v.x);
        mn[1]=std::fmin(mn[1],v.y); mx[1]=std::fmax(mx[1],v.y);
        mn[2]=std::fmin(mn[2],v.z); mx[2]=std::fmax(mx[2],v.z);
    };
    for (const auto& t : tris) { acc(t.vertex0); acc(t.vertex1); acc(t.vertex2); }
    float radius = 0.0f;
    if (!tris.empty()) {
        float dx=mx[0]-mn[0], dy=mx[1]-mn[1], dz=mx[2]-mn[2];
        radius = 0.5f * std::sqrt(dx*dx+dy*dy+dz*dz);
    }

    // Compute dominant material from full-res TriEx for LOD fallback.
    float dominant_mat = -1.0f;
    if (triex_ptr && !triex_ptr->empty()) {
        int counts[256] = {};
        for (const auto& t : *triex_ptr) {
            int m = t.materialId;
            if (m >= 0 && m < 256) counts[m]++;
        }
        int max_cnt = 0;
        for (int i = 0; i < 256; ++i)
            if (counts[i] > max_cnt) { max_cnt = counts[i]; dominant_mat = (float)i; }
    }

    // Re-bake LODs into the SHARED store BLASManager. lod_bake stores the
    // ABSOLUTE entries_ index (== get_entries().size() before registration),
    // so use blas_indices[0] directly as the index — do NOT add 'before'.
    LoadedPart lp;
    lp.bound_radius = radius;
    lp.children = std::move(children);   // keep the baked child-instance table for the WorldComposer
    lp.animation_asset = animation_asset;
    lod_bake::LodLevels lods = lod_bake::bake_lods(tris, lod_bake::BakeTargets{}, blas_, triex_ptr);
    for (const auto& L : lods) {
        // A geometry-less part (one that only places children) bakes to empty
        // triangles and yields LOD levels with no BLAS -> skip them, leaving
        // lod_blas empty so the part is treated as a pure assembler.
        if (L.blas_indices.empty()) continue;
        // bake_lods registers exactly one BLAS per non-empty level; guard the
        // assumption since the LodLevel type can carry multiple indices.
        assert(L.blas_indices.size() == 1);
        lp.thresholds.push_back(L.screen_size_threshold);
        size_t abs_idx = L.blas_indices[0];   // absolute index into blas_.get_entries()
        lp.lod_blas.push_back(blas_.get_entries()[abs_idx]->handle);
        lp.owned_blas.push_back(lp.lod_blas.back());

        if (const auto* e = blas_.get_entry(lp.lod_blas.back())) {
            const TriEx* mesh_ex = (e->tri_extra.size() == e->triangles.size() && !e->tri_extra.empty())
                                      ? e->tri_extra.data() : nullptr;
            lp.lod_mesh_data.push_back(
                build_raster_mesh_data(e->triangles.data(), mesh_ex, (int)e->triangles.size(), dominant_mat));
        } else {
            lp.lod_mesh_data.push_back({});
        }
    }
    if (lp.lod_blas.empty()) {
        // No geometry (empty part) -> log; lookups will see an empty LOD list.
        printf("PartStore: part %016llx produced no LOD geometry\n",
               (unsigned long long)part_hash);
    }

    // Compositional path: no flat artifact, so clusters is empty; treat all as fine.
    lp.fine_cluster_count = (uint32_t)lp.clusters.size();  // 0 for compositional parts

    auto ins = loaded_.emplace(part_hash, std::move(lp));
    // Build expansion into a local vector first (see flat path comment above).
    std::vector<ExpandedNode> exp;
    build_expansion(part_hash, [this](uint64_t h){ return get_or_load(h); }, exp);
    loaded_[part_hash].expansion = std::move(exp);
    return &ins.first->second;
}

lod_select::PartLodTable PartStore::part_lod_table() const {
    lod_select::PartLodTable table;
    for (const auto& kv : loaded_) {
        const LoadedPart& lp = kv.second;
        lod_select::PartLod pl;
        pl.bound_radius    = lp.bound_radius;
        pl.thresholds      = lp.thresholds;
        pl.inline_cutover  = lp.inline_cutover;
        for (const auto& ref : lp.flat_refs) {
            lod_select::PartLodRef r;
            r.child_hash = ref.child_resolved_hash;
            std::memcpy(r.rel_transform, ref.transform, sizeof r.rel_transform);
            r.child_scale = part_flatten::transform_uniform_scale(ref.transform);
            pl.refs.push_back(r);
        }
        table[kv.first] = std::move(pl);
    }
    return table;
}

// ---------------------------------------------------------------------------
// release — evict a loaded part from the CPU store.
//
// Erasing the map entry destroys the LoadedPart in-place, which releases
// lod_mesh_data vectors and runs the BLASHandle destructors.  BLASManager
// handles the reference-counted triangle buffers; the shared blas_ remains
// valid for other parts that share BLAS entries.
//
// After this call:
//   - loaded_.count(part_hash) == 0
//   - get_or_load(part_hash) will re-read from disk (or return nullptr if no
//     disk artifact exists).
// ---------------------------------------------------------------------------
void PartStore::release(uint64_t part_hash) {
    // Rigid segment parts are lifetime-owned by their animated root.  A direct
    // eviction would leave the immutable bridge asset with a valid hash that no
    // longer resolves, so only the root release tears down the whole set.
    if (rigid_subpart_owner_.find(part_hash) != rigid_subpart_owner_.end()) return;

    const auto subparts = rigid_subparts_.find(part_hash);
    if (subparts != rigid_subparts_.end()) {
        for (const RigidSubpartSet& set : subparts->second) {
            for (uint64_t subpart_hash : set.hashes) {
                const auto child = loaded_.find(subpart_hash);
                if (child != loaded_.end()) {
                    release_loaded_part_blas(blas_, child->second);
                    loaded_.erase(child);
                }
                rigid_subpart_owner_.erase(subpart_hash);
            }
        }
        rigid_subparts_.erase(subparts);
    }
    auto it = loaded_.find(part_hash);
    if (it == loaded_.end()) return;  // safe no-op for unknown hashes

    const LoadedPart& lp = it->second;

    release_loaded_part_blas(blas_, lp);

    // Now safe to erase the LoadedPart from memory.
    loaded_.erase(it);
}

} // namespace viewer
