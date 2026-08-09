#include "part_store.h"
#include "profile.h"
#include "animation/anim_bundle.h"
#include "animation/animation_binding_bake.h"
#include "matrix_math.h"

#include "part_asset_v2.h"     // load_v2, cache_path_resolved, ChildInstance, LodLevels
#include "lod_bake.h"          // lod_bake::bake_lods, BakeTargets
#include "warp_field.h"        // VT Phase 2: warped ground coordinate solve
#include "tlas_manager.hpp"    // TLASManager (load_v2 signature needs one)
#include "part_flatten.h"      // part_flatten::transform_uniform_scale

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>   // offsetof (TriEx named-member span, see snapshot_from_baked)
#include <cstdio>
#include <cstdlib>
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
        [&](const LoadedPart* lp, uint64_t hash, const float rel[16], int depth) {
            if (lp->lod_mesh_data.empty()) return;
            ExpandedNode n;
            n.part_hash = hash;
            memcpy(n.rel_transform, rel, sizeof n.rel_transform);
            n.depth = depth;
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
            // Handles, not blas_indices: blas_ is the shared store manager and
            // release_blas() erases from entries_, shifting every absolute index
            // above the removed one. See lod_bake.h.
            std::vector<BLASHandle> lod_handles;
            const lod_bake::LodLevels lods = lod_bake::bake_lods(
                triangles, lod_bake::BakeTargets{}, blas_, &extras, nullptr,
                &lod_handles);
            if (lod_handles.size() != lods.size()) { rollback(); return false; }
            for (size_t li = 0; li < lods.size(); ++li) {
                const auto& lod = lods[li];
                const BLASHandle handle = lod_handles[li];
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
    // cache_path_resolved returns the RELATIVE "parts/<hash>.bundle"; prefix cache_root_.
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

        // ------------------------------------------------------------------
        // M2.5 — resolve the terminal impostor rung against its atlas sidecar.
        //
        // The ladder in the artifact already ends in a two-triangle billboard
        // for every cluster the bake found eligible. The PIXELS live beside it
        // in a `.fimp`. This block decides, per cluster, whether that rung can
        // actually draw: it recomputes the depicts-hash from the mesh rung the
        // billboard replaces and matches it against the sidecar's.
        //
        // EVERY REJECTION IS LOGGED ONCE, NAMING THE PART AND THE REASON. That
        // sentence is the whole point of this block. On the abandoned branch an
        // entire rendering tier was absent for a full generation of artifacts
        // and produced no diagnostic anywhere, because the equivalent code was
        // a bare `return` inside a `catch (...)`. A rung with no atlas behind
        // it is dropped here so the finest surviving mesh rung holds at any
        // distance -- degraded, never wrong, and never silent.
        //
        // `impostor_rung[i]` is the index into clusters_in[i].lods of the
        // billboard rung, or SIZE_MAX for "this cluster draws mesh all the way
        // down". Populated only for UNSEGMENTED flats: the segmented
        // (LOD-instanced-children) bake path does not emit impostors, so a
        // segmented artifact never carries one and the stable_partition below
        // can never disturb the cluster indices the sidecar names.
        std::vector<size_t> impostor_rung(clusters_in.size(), SIZE_MAX);
        std::vector<const impostor::ClusterImpostor*> impostor_data(
            clusters_in.size(), nullptr);
        impostor::PartImpostor loaded_impostors;
        if (!segmented) {
            const auto& probe_entries = scratch.get_entries();
            uint64_t depicts = impostor::depicts_hash_begin();
            bool any_rung = false;
            for (size_t ci = 0; ci < clusters_in.size(); ++ci) {
                const auto& lods = clusters_in[ci].lods;
                if (lods.size() < 2) continue;
                const auto& last = lods.back().blas_indices;
                if (last.size() != 1 || last[0] >= probe_entries.size()) continue;
                const BLASManager::BLASEntry* e = probe_entries[last[0]].get();
                if (!e || e->triangles.size() != 2 || e->tri_extra.size() != 2 ||
                    !(e->tri_extra[0].uv0.x >= impostor::kQuadMarker))
                    continue;
                // What the billboard DEPICTS is rep 0 -- the authored mesh --
                // not the rung it takes over from. This used to read
                // lods[size-2]; the bake changed to source rep 0 (see
                // part_flatten's note: a 16x16 cell resolves silhouette and
                // shading, and the coarsest rung has already discarded both),
                // and this recomputation MUST follow it. If the two disagree
                // the depicts-hash never matches and every atlas in the world
                // is rejected as stale -- which degrades silently to
                // mesh-only, exactly the failure that went unnoticed for a
                // generation of artifacts on the abandoned branch.
                const auto& src = lods[0].blas_indices;
                if (src.size() != 1 || src[0] >= probe_entries.size()) continue;
                impostor_rung[ci] = lods.size() - 1;
                // tri_extra alongside triangles, and from the SAME entry: the
                // hash now folds the referenced materials' registry albedos
                // (the tint layer is baked from them), so reader and writer
                // must present identical material sets. Feeding triangles
                // without their tri_extra would fold an empty material list
                // here and a populated one in part_flatten, rejecting every
                // atlas in the world as stale -- the silent mesh-only
                // degradation this block's comment above already warns about.
                impostor::depicts_hash_add_cluster(
                    depicts, static_cast<uint32_t>(ci),
                    probe_entries[src[0]]->triangles,
                    probe_entries[src[0]]->tri_extra);
                any_rung = true;
            }
            if (any_rung) {
                const std::string imp_path =
                    artifact_root + "/" + impostor::cache_path_impostor(part_hash);
                impostor::LoadFailure fail = impostor::LoadFailure::None;
                std::string reason;
                if (impostor::load(imp_path, part_hash,
                                   impostor::depicts_hash_finish(depicts),
                                   loaded_impostors, &fail, &reason)) {
                    for (const auto& c : loaded_impostors.clusters) {
                        if (c.cluster_index < impostor_data.size() &&
                            impostor_rung[c.cluster_index] != SIZE_MAX)
                            impostor_data[c.cluster_index] = &c;
                    }
                }
                // Anything the sidecar did not answer for -- a hard rejection,
                // or a cluster the file simply does not mention -- loses its
                // rung, and says so exactly once per part.
                size_t unbacked = 0;
                for (size_t ci = 0; ci < clusters_in.size(); ++ci)
                    if (impostor_rung[ci] != SIZE_MAX && !impostor_data[ci])
                        ++unbacked;
                if (unbacked > 0 && impostor_load_logged_.insert(part_hash).second) {
                    printf("PartStore: impostor atlas unusable for part %016llx "
                           "(%s): %s -- %zu impostor rung(s) dropped, coarsest "
                           "mesh rung holds\n",
                           (unsigned long long)part_hash,
                           impostor::cache_path_impostor(part_hash).c_str(),
                           fail == impostor::LoadFailure::None
                               ? "sidecar omits this cluster"
                               : (reason.empty()
                                      ? impostor::load_failure_text(fail)
                                      : reason.c_str()),
                           unbacked);
                    fflush(stdout);
                }
                // PRUNE, so nothing downstream has to know this happened: an
                // unbacked billboard rung leaves the ladder entirely and the
                // artifact reads exactly as a mesh-only ladder would.
                for (size_t ci = 0; ci < clusters_in.size(); ++ci) {
                    if (impostor_rung[ci] == SIZE_MAX || impostor_data[ci]) continue;
                    clusters_in[ci].lods.pop_back();
                    impostor_rung[ci] = SIZE_MAX;
                }
            }
        }

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

        // M6: this ladder's shared parameterisation, established by the first
        // rung that charts. The flat path is the one authored props take, so
        // leaving it per-rung would have left exactly the parts the impostor
        // and LOD work is about still churning their pages on every switch.
        const bool unify_charts = lod_bake::unify_parameterisation_enabled();
        chart_atlas::ChartAtlasRung chart_base;
        std::vector<Tri> chart_base_tris;

        for (size_t li = 0; li < max_lods; ++li) {
            std::vector<Tri> tris;
            std::vector<TriEx> triex;
            float thr = 0.0f;
            // M2.5: a billboard rung carries no surface to chart -- its texel
            // detail IS the atlas -- so charting it would build a VT page for
            // two triangles nothing samples.
            bool legacy_impostor = false;
            for (size_t ci = 0; ci < clusters_in.size(); ++ci) {
                const auto& cl = clusters_in[ci];
                // When segmented, the legacy view uses only coarse clusters.
                if (segmented && cl.segment != 1) continue;
                // Use level min(li, cluster_levels-1) for clusters with fewer levels.
                size_t use_li = (li < cl.lods.size()) ? li : cl.lods.size() - 1;
                if (use_li == impostor_rung[ci]) legacy_impostor = true;
                thr = std::fmax(thr, cl.lods[use_li].screen_size_threshold);
                for (uint32_t bi : cl.lods[use_li].blas_indices) {
                    if (bi >= entries.size()) continue;
                    tris.insert(tris.end(), entries[bi]->triangles.begin(), entries[bi]->triangles.end());
                    triex.insert(triex.end(), entries[bi]->tri_extra.begin(), entries[bi]->tri_extra.end());
                }
            }
            if (tris.empty()) continue;
            const TriEx* ex = (triex.size() == tris.size()) ? triex.data() : nullptr;
            // WP-A follow-up (flat coverage): chart this rung before
            // registration, same policy as stage_from_snapshot's prop path
            // (16 t/m, no per-rung halving — flats are scattered props;
            // terrain sectors never take the flat path). Fail-closed: a rung
            // whose chart build fails ships an empty table (charts = 0).
            std::vector<TriEx> charted;
            chart_atlas::ChartAtlasRung rung_table;
            bool charted_ok = false;
            if (ex && !legacy_impostor) {
                charted.assign(triex.begin(), triex.end());
                charted_ok = lod_bake::chart_rung_unified(
                    tris, charted, 16.0f, chart_atlas::kChartNormalConeDeg,
                    unify_charts, chart_base, chart_base_tris, rung_table);
                if (charted_ok) ex = charted.data();
            }
            BLASHandle h = blas_.register_triangles(tris.data(), (int)tris.size(), ex);
            lp.owned_blas.push_back(h);
            lp.thresholds.push_back(thr);
            lp.lod_blas.push_back(h);

            // Append legacy whole-part mesh-data at lod_mesh_data[li] (parallel to lod_blas).
            // Charted rungs build from the LOCAL charted TriEx rather than the
            // registered entry: register_triangles dedups on geometry+material
            // +tint (not UV), so the entry may carry another registrant's UVs
            // — the chart table must stay coherent with the vertex stream.
            if (charted_ok) {
                lp.lod_mesh_data.push_back(
                    build_raster_mesh_data(tris.data(), charted.data(), (int)tris.size()));
            } else if (const auto* e = blas_.get_entry(h)) {
                const TriEx* mesh_ex = (e->tri_extra.size() == e->triangles.size() && !e->tri_extra.empty())
                                          ? e->tri_extra.data() : nullptr;
                lp.lod_mesh_data.push_back(
                    build_raster_mesh_data(e->triangles.data(), mesh_ex, (int)e->triangles.size()));
            } else {
                lp.lod_mesh_data.push_back({});
            }
            lp.lod_charts.push_back(std::move(rung_table));   // parallel to lod_mesh_data
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
        for (size_t ci = 0; ci < clusters_in.size(); ++ci) {
            const auto& cl_in = clusters_in[ci];
            LoadedCluster cl_out;
            int impostor_mesh_index = -1;
            // AABB / radius from the FlatCluster's stored AABB.
            std::memcpy(cl_out.aabb_min, cl_in.aabb_min, sizeof cl_out.aabb_min);
            std::memcpy(cl_out.aabb_max, cl_in.aabb_max, sizeof cl_out.aabb_max);
            float dx = cl_in.aabb_max[0] - cl_in.aabb_min[0];
            float dy = cl_in.aabb_max[1] - cl_in.aabb_min[1];
            float dz = cl_in.aabb_max[2] - cl_in.aabb_min[2];
            cl_out.radius = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);

            // M6: PER-CLUSTER, deliberately. A cluster is its own piece of
            // surface with its own charts, so the shared parameterisation is
            // shared down a cluster's rungs and never across clusters —
            // hoisting these two lines out of the `ci` loop would hand one
            // cluster's chart planes to another cluster's geometry.
            chart_atlas::ChartAtlasRung cchart_base;
            std::vector<Tri> cchart_base_tris;

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
                const bool is_impostor = (li == impostor_rung[ci]);
                const TriEx* cex = (ctriex.size() == ctris.size()) ? ctriex.data() : nullptr;
                // WP-A follow-up: chart the cluster rung (same contract as the
                // legacy-view loop above). These are the meshes the raster
                // clusters actually draw (chart_rung = mesh index).
                std::vector<TriEx> ccharted;
                chart_atlas::ChartAtlasRung crung_table;
                bool ccharted_ok = false;
                if (cex && !is_impostor) {
                    ccharted.assign(ctriex.begin(), ctriex.end());
                    ccharted_ok = lod_bake::chart_rung_unified(
                        ctris, ccharted, 16.0f, chart_atlas::kChartNormalConeDeg,
                        unify_charts, cchart_base, cchart_base_tris, crung_table);
                    if (ccharted_ok) cex = ccharted.data();
                }
                BLASHandle ch = blas_.register_triangles(ctris.data(), (int)ctris.size(), cex);
                lp.owned_blas.push_back(ch);

                // Append cluster-level mesh-data after the legacy whole-part entries.
                int mesh_idx = (int)lp.lod_mesh_data.size();
                if (ccharted_ok) {
                    lp.lod_mesh_data.push_back(
                        build_raster_mesh_data(ctris.data(), ccharted.data(), (int)ctris.size()));
                } else if (const auto* ce = blas_.get_entry(ch)) {
                    const TriEx* mex = (ce->tri_extra.size() == ce->triangles.size() && !ce->tri_extra.empty())
                                           ? ce->tri_extra.data() : nullptr;
                    lp.lod_mesh_data.push_back(
                        build_raster_mesh_data(ce->triangles.data(), mex, (int)ce->triangles.size()));
                } else {
                    lp.lod_mesh_data.push_back({});
                }
                lp.lod_charts.push_back(std::move(crung_table));  // parallel to lod_mesh_data

                cl_out.thresholds.push_back(lod_in.screen_size_threshold);
                cl_out.lod_blas.push_back(ch);
                cl_out.lod_mesh.push_back(mesh_idx);
                if (is_impostor) impostor_mesh_index = mesh_idx;
            }
            if (cl_out.lod_blas.empty()) {
                // Cluster yielded no geometry - skip rather than leaving an empty entry.
                // Do NOT increment fine_pushed here: we only count pushed (non-empty) clusters.
                continue;
            }
            if (segmented && cl_in.segment == 0) ++fine_pushed;
            if (impostor_mesh_index >= 0 && impostor_data[ci]) {
                LoadedPart::ResidentImpostor res_imp;
                res_imp.cluster = static_cast<uint32_t>(lp.clusters.size());
                res_imp.ordinal = static_cast<uint32_t>(
                    impostor_data[ci] - loaded_impostors.clusters.data());
                res_imp.mesh_index = impostor_mesh_index;
                res_imp.data = *impostor_data[ci];
                lp.impostors.push_back(std::move(res_imp));
            }
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

        printf("PartStore: loaded v3 FLAT part %016llx (%zu LOD levels, %zu clusters, "
               "%zu refs, %zu impostors)\n",
               (unsigned long long)part_hash, lp.lod_blas.size(), lp.clusters.size(),
               lp.flat_refs.size(), lp.impostors.size());
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

        // M6: same rule, flat v2's ladder.
        const bool unify_charts_v2 = lod_bake::unify_parameterisation_enabled();
        chart_atlas::ChartAtlasRung chart_base_v2;
        std::vector<Tri> chart_base_v2_tris;

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
            // WP-A follow-up: chart the v2-flat rung (same contract as the v3
            // branch above).
            std::vector<TriEx> charted;
            chart_atlas::ChartAtlasRung rung_table;
            bool charted_ok = false;
            if (ex) {
                charted.assign(triex.begin(), triex.end());
                charted_ok = lod_bake::chart_rung_unified(
                    tris, charted, 16.0f, chart_atlas::kChartNormalConeDeg,
                    unify_charts_v2, chart_base_v2, chart_base_v2_tris,
                    rung_table);
                if (charted_ok) ex = charted.data();
            }
            BLASHandle h = blas_.register_triangles(tris.data(), (int)tris.size(), ex);
            lp.owned_blas.push_back(h);
            lp.thresholds.push_back(lods_in[li].screen_size_threshold);
            lp.lod_blas.push_back(h);

            int mesh_idx = (int)lp.lod_mesh_data.size();
            if (charted_ok) {
                lp.lod_mesh_data.push_back(
                    build_raster_mesh_data(tris.data(), charted.data(), (int)tris.size()));
            } else if (const auto* e = blas_.get_entry(h)) {
                const TriEx* mesh_ex = (e->tri_extra.size() == e->triangles.size() && !e->tri_extra.empty())
                                          ? e->tri_extra.data() : nullptr;
                lp.lod_mesh_data.push_back(
                    build_raster_mesh_data(e->triangles.data(), mesh_ex, (int)e->triangles.size()));
            } else {
                lp.lod_mesh_data.push_back({});
            }
            lp.lod_charts.push_back(std::move(rung_table));   // parallel to lod_mesh_data

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

bool PartStore::read_coherent_snapshot(uint64_t part_hash,
                                       CoherentSnapshot& out) const {
    // The manifest is published last, so a reader can legitimately observe a
    // new PART before its matching MANM/MACM manifest. Such a window is a cache
    // miss, never a static fallback; retrying a few fresh snapshots lets an
    // in-flight atomic publisher finish without ever accepting a mixed
    // generation.
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

        out.scratch = std::move(candidate_scratch);
        out.children = std::move(candidate_children);
        out.lods_in = std::move(candidate_lods);
        out.emitters = std::move(candidate_emitters);
        out.animation_link = candidate_link;
        if (candidate_link) out.loaded_animation = std::move(candidate_animation);
        coherent = true;
    }
    return coherent;
}

PartStore::StagedPart PartStore::stage_from_snapshot(
        uint64_t part_hash, CoherentSnapshot& snapshot,
        const matter::animation::AnimAsset* animation_asset,
        size_t first_rung, bool terrain_sector, const WarpAnchor& warp) {
    StagedPart staged;
    staged.part_hash = part_hash;
    staged.staging   = std::make_unique<BLASManager>();
    auto stage_mark = std::chrono::steady_clock::now();
    auto stage_split = [&stage_mark]() -> double {
        const auto now = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(now - stage_mark).count();
        stage_mark = now;
        return ms;
    };
    auto& scratch  = snapshot.scratch;
    auto& children = snapshot.children;
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

    staged.lp.bound_radius = radius;
    staged.lp.children = std::move(children);   // keep the baked child-instance table for the WorldComposer
    staged.lp.animation_asset = animation_asset;
    // Bake the ladder into a PRIVATE manager, then adopt it into the shared one
    // in a single bounded step.
    //
    // This is the seam for getting the load off the app/GL thread. Everything
    // above already works on a local `scratch`, so `staging` makes the whole
    // expensive stretch -- decimation, TriEx reprojection, BVH construction --
    // touch no shared state at all. What is left against blas_ is adopt_from:
    // O(entries) hash lookups plus an array copy, with no BVH rebuilt, because
    // a content match takes a reference and a newcomer installs the BVH the
    // bake already produced. Splitting this into stage_load()/commit_staged()
    // is then code motion rather than a redesign.
    //
    // Dedup still applies across the boundary: a part whose geometry is already
    // resident collapses onto that entry instead of duplicating it.
    //
    // Handles, not blas_indices -- see lod_bake.h. The staged handles are
    // meaningless in blas_, so lod_blas/owned_blas are patched through the
    // remap adopt_from reports.

    std::vector<BLASHandle> lod_handles;
    // Streamed terrain sectors get the error-bounded terrain ladder: the
    // generic 10%/1% ratio ladder shattered distant tiles into giant facets
    // framed by their frozen full-res rims (visible as seams on every distant
    // mountain), and kept every invisible border-skirt triangle at all rungs.
    //
    // Detection is now TOLD, not inferred. It used to key off the mesher's
    // exactly-vertical skirt curtains, which only terrain tiles carried; skirts
    // were removed on 2026-07-30 (see terrain_mesher.cpp), so a fresh sector
    // has no vertical-edge fringe and the old test would silently answer
    // "not terrain" and drop every streamed sector onto the ratio ladder --
    // reintroducing exactly the seam grid described above. The streaming
    // caller knows what it is staging and passes `terrain_sector`.
    //
    // The skirt heuristic is KEPT as a fallback, not out of caution but because
    // the disc cache is full of .part artifacts baked before this change that
    // really do still contain skirts. Those must keep taking the terrain ladder
    // AND keep getting their skirts stripped from the coarse rungs, until they
    // are re-baked. count_terrain_skirt_tris stays for the same reason: on a
    // post-change bake it returns 0 and an all-zero mask, which bake_terrain_lods
    // handles as "nothing to drop".
    std::vector<uint8_t> skirt_mask;
    const size_t skirt_count =
        lod_bake::count_terrain_skirt_tris(tris, &skirt_mask);
    const bool legacy_skirted_tile =
        skirt_count >= 8 && skirt_count * 2 <= tris.size();
    // The size guard is common to both: a terrain sector is large in mesh-local
    // space (>= half a sector), and it is the one condition worth keeping even
    // against an explicit caller assertion, since the ladder's error bound is
    // scaled by exactly this radius.
    const bool terrain_tile =
        radius >= 32.0f && (terrain_sector || legacy_skirted_tile);
    // WP-A (chart-space VT): every staged part gets per-rung chart tables and
    // chart UVs in its TriEx (flowing to the render vertex surface.xy through
    // build_raster_mesh_data below). Density policy: props 16 t/m at every
    // rung; terrain sectors 16 t/m at rung 0 halving per coarser rung. A rung
    // whose chart build fails ships an empty table (charts = 0, legacy path).
    lod_bake::ChartBakeOptions chart_opts;
    chart_opts.texels_per_meter = 16.0f;
    // Nested sector LOD: a level-L terrain tile is 2^L times wider than a
    // level-0 one for the SAME triangle count, so a fixed texels-per-metre
    // would ask for 2^L times the texels across -- a 2 km level-5 tile would
    // want ~32k texels wide, far past the atlas and the 2048-layer array cap.
    // Scaling the base density down by the tile's size ratio keeps
    // texels-per-TILE constant and texels-per-metre matched to the voxel,
    // which is the same ratio a level-0 sector has today. Density at a given
    // DISTANCE is therefore unchanged, because level replaces exactly the
    // per-rung halving it displaces. Inert at level 0 (ratio 1).
    if (terrain_tile && warp.valid && warp.sector_size > 0.0f &&
        warp.base_sector_size > 0.0f) {
        const float ratio = warp.base_sector_size / warp.sector_size;
        if (ratio > 0.0f && ratio < 1.0f) chart_opts.texels_per_meter *= ratio;
    }
    chart_opts.halve_per_rung = terrain_tile;
    // M6: one parameterisation per part. Note this SUPERSEDES halve_per_rung
    // when on — one table means one density, which is the point: a per-rung
    // density is a per-rung parameterisation. Terrain sectors are the case
    // that matters, since their horizon lives in these page texels.
    chart_opts.unify_parameterisation = lod_bake::unify_parameterisation_enabled();
    std::vector<chart_atlas::ChartAtlasRung> rung_charts;
    staged.prep_ms = stage_split();
    // first_rung: skip ladder rungs this part can never be DRAWN at. The
    // caller asserts it from the part's placement -- a streamed sector's
    // scatter tier is a distance proxy -- and only the terrain ladder honours
    // it, because only terrain sectors are placed at a known distance. Rungs
    // below it cost nothing: no decimation, no reprojection, no registration.
    // 0 (every non-streaming caller) is exactly today's behaviour.
    lod_bake::TerrainBakeTargets terrain_targets;
    terrain_targets.first_rung = first_rung;
    lod_bake::LodLevels lods = terrain_tile
        ? lod_bake::bake_terrain_lods(tris, skirt_mask, radius,
                                      terrain_targets,
                                      *staged.staging, triex_ptr, observer_,
                                      &lod_handles, &chart_opts, &rung_charts)
        : lod_bake::bake_lods(tris, lod_bake::BakeTargets{}, *staged.staging,
                              triex_ptr, observer_, &lod_handles,
                              &chart_opts, &rung_charts);
    staged.ladder_ms = stage_split();
    assert(lod_handles.size() == lods.size());
    for (size_t li = 0; li < lods.size() && li < lod_handles.size(); ++li) {
        const auto& L = lods[li];
        // A geometry-less part (one that only places children) bakes to empty
        // triangles and yields LOD levels with no BLAS -> skip them, leaving
        // lod_blas empty so the part is treated as a pure assembler.
        if (L.blas_indices.empty()) continue;
        staged.lp.thresholds.push_back(L.screen_size_threshold);
        staged.lp.lod_blas.push_back(lod_handles[li]);
        staged.lp.owned_blas.push_back(staged.lp.lod_blas.back());
        // Keep lod_charts parallel to lod_blas. register_triangles dedups on
        // geometry+material+tint (NOT uv), so if a coarser rung collapsed onto
        // an earlier rung's entry, the entry carries the EARLIER rung's UVs —
        // reuse that rung's chart table so table and UVs stay coherent.
        {
            chart_atlas::ChartAtlasRung table =
                li < rung_charts.size() ? std::move(rung_charts[li])
                                        : chart_atlas::ChartAtlasRung{};
            for (size_t prev = 0; prev + 1 < staged.lp.lod_blas.size(); ++prev) {
                if (staged.lp.lod_blas[prev] == lod_handles[li]) {
                    table = staged.lp.lod_charts[prev];
                    break;
                }
            }
            staged.lp.lod_charts.push_back(std::move(table));
        }

        // Raster mesh data is a copy, so build it from the staged entry before
        // adoption; it does not reference the manager afterwards.
        if (const auto* e = staged.staging->get_entry(staged.lp.lod_blas.back())) {
            const TriEx* mesh_ex = (e->tri_extra.size() == e->triangles.size() && !e->tri_extra.empty())
                                      ? e->tri_extra.data() : nullptr;
            staged.lp.lod_mesh_data.push_back(
                build_raster_mesh_data(e->triangles.data(), mesh_ex, (int)e->triangles.size(), dominant_mat));
        } else {
            staged.lp.lod_mesh_data.push_back({});
        }
    }
    if (!staged.lp.lod_mesh_data.empty()) {
        LoadedCluster cluster;
        float lod_min[3] = {
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()};
        float lod_max[3] = {
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max()};
        bool have_vertex = false;
        for (size_t lod = 0; lod < staged.lp.lod_mesh_data.size(); ++lod) {
            const RasterMeshData& mesh = staged.lp.lod_mesh_data[lod];
            for (int vertex = 0; vertex < mesh.vertex_count; ++vertex) {
                for (int axis = 0; axis < 3; ++axis) {
                    const float value =
                        mesh.vertices[static_cast<size_t>(vertex) * 3 + axis];
                    lod_min[axis] = std::min(lod_min[axis], value);
                    lod_max[axis] = std::max(lod_max[axis], value);
                }
                have_vertex = true;
            }
            cluster.thresholds.push_back(staged.lp.thresholds[lod]);
            cluster.lod_blas.push_back(staged.lp.lod_blas[lod]);
            cluster.lod_mesh.push_back(static_cast<int>(lod));
        }
        if (have_vertex) {
            std::memcpy(cluster.aabb_min, lod_min, sizeof lod_min);
            std::memcpy(cluster.aabb_max, lod_max, sizeof lod_max);
            const float dx = lod_max[0] - lod_min[0];
            const float dy = lod_max[1] - lod_min[1];
            const float dz = lod_max[2] - lod_min[2];
            cluster.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
            staged.lp.bound_radius = cluster.radius;
            staged.lp.clusters.push_back(std::move(cluster));
        }
    }
    // A staged part must be fully formed: commit_staged re-derives this from
    // clusters.size(), and leaving it 0 here made the staged flavor diverge
    // from the committed flavor once the synthetic cluster above existed
    // (partstore_race_tests' golden phase folds fine_cluster_count).
    staged.lp.fine_cluster_count = (uint32_t)staged.lp.clusters.size();
    staged.tail_ms = stage_split();

    // Warp field (VT Phase 2): solve the sector's warped ground coordinate on
    // the full-res surface (skirts excluded) and evaluate it at every rung
    // mesh's welded vertices — rung 0 hits the solve vertices bitwise, the
    // decimated rungs reproject via nearest solve triangle (warp_field.h).
    // Only terrain sectors with a known world anchor get a field; everything
    // else ships zeroed warp data and the shader's world-XZ fallback.
    if (terrain_tile && warp.valid && !tris.empty()) {
        warp_field::SolveOptions wopts;
        wopts.anchor_x = warp.x;
        wopts.anchor_z = warp.z;
        wopts.sector_size = warp.sector_size;
        warp_field::Field field;
        if (warp_field::solve(tris.data(), tris.size(),
                              skirt_mask.empty() ? nullptr : skirt_mask.data(),
                              wopts, field)) {
            const auto eval_t0 = std::chrono::steady_clock::now();
            for (auto& mesh : staged.lp.lod_mesh_data) {
                if (mesh.vertex_count <= 0 ||
                    mesh.normals.size() <
                        static_cast<size_t>(mesh.vertex_count) * 3)
                    continue;
                mesh.warp_uvs.assign(size_t(mesh.vertex_count) * 2, 0.0f);
                mesh.warp_frames.assign(size_t(mesh.vertex_count) * 2, 0u);
                warp_field::evaluate(field, mesh.vertices.data(),
                                     mesh.normals.data(),
                                     size_t(mesh.vertex_count),
                                     mesh.warp_uvs.data(),
                                     mesh.warp_frames.data());
            }
            warp_field::warp_census_add_evaluate_us(
                (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - eval_t0)
                    .count());
        }
    }
    staged.warp_ms = stage_split();
    staged.ok = true;
    return staged;
}

PartStore::StagedPart PartStore::stage_load(uint64_t part_hash,
                                            size_t first_rung,
                                            bool terrain_sector,
                                            const WarpAnchor& warp) {
    StagedPart staged;
    staged.part_hash = part_hash;
    CoherentSnapshot snapshot;
    const auto read_t0 = std::chrono::steady_clock::now();
    const bool read_ok = read_coherent_snapshot(part_hash, snapshot);
    const double read_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - read_t0).count();
    if (!read_ok) { staged.read_ms = read_ms; return staged; }
    // Animated parts take the partitioned path, which registers into the shared
    // BLAS manager and inserts into the shared animation asset store. Not
    // stageable; the caller loads them on the owning thread.
    if (snapshot.animation_link) { staged.read_ms = read_ms; return staged; }
    StagedPart out =
        stage_from_snapshot(part_hash, snapshot, nullptr, first_rung,
                            terrain_sector, warp);
    out.read_ms = read_ms;
    return out;
}

// ---------------------------------------------------------------------------
// stage_from_bake — the artifact round-trip elision.
//
// A streamed sector's bake writes a .part (12 ms) that stage_load then decodes
// straight back (10.9 ms) into geometry the bake still had in registers. The
// artifact still has to be written -- it is the content-addressed cache entry
// and the publication ledger's retained artifact -- but nothing has to read it
// back, so long as what we hand to stage_from_snapshot is EXACTLY what the
// decode would have produced.
//
// That equivalence is not assumed, it is reconstructed. save_v2's writer
// (append_common_body) applies exactly two transforms on the way out:
//
//   1. TriEx source selection: an entry serializes `tri_extra` when that array
//      is parallel to the triangles, else the mesh's own triEx array.
//   2. TriEx trailing alignment padding (the 4 bytes after ao2, since
//      sizeof(TriEx) == 96 but the named members end at 92) is zeroed through a
//      staging copy, so that allocator garbage there cannot make two otherwise
//      identical bakes byte-differ.
//
// Everything else is a verbatim byte copy: triangles, BVH nodes, triangle
// indices, the entry hash and ref_count, and the entry ORDER. The reader
// (parse_common_body + publish_common_body) replays those bytes through
// register_prebuilt -- which is exactly what this does, with the same two
// transforms applied. So the reconstructed manager is the manager load_v2 would
// have built, entry for entry and byte for byte, and stage_from_snapshot cannot
// tell the two apart.
// ---------------------------------------------------------------------------
bool PartStore::snapshot_from_baked(const script_host::BakedGeometry& baked,
                                    CoherentSnapshot& out) {
    if (!baked.blas) return false;
    // save_v2's staging copy hardcodes 92 named bytes. Pin that here too: if
    // TriEx ever grows a member, BOTH the writer's normalization and this
    // reconstruction are wrong, and a build break is the only honest outcome.
    constexpr size_t kTriExNamedBytes = offsetof(TriEx, ao2) + sizeof(float);
    static_assert(sizeof(TriEx) == 96 && kTriExNamedBytes == 92,
                  "TriEx layout changed; part_asset_v2.cpp's save-side padding "
                  "normalization (kTriExPad = 92) must change with it");

    auto scratch = std::make_unique<BLASManager>();
    std::vector<TriEx> normalized;
    for (const auto& e : baked.blas->get_entries()) {
        // Anything whose parallel arrays are not self-consistent is something
        // this reconstruction cannot vouch for. Bail rather than guess -- the
        // caller falls back to decoding the artifact, which is always correct.
        if (!e || !e->mesh || !e->bvh) return false;
        if (e->mesh->triCount <= 0) return false;
        const size_t tri_count = static_cast<size_t>(e->mesh->triCount);
        if (e->triangles.size() != tri_count) return false;
        if (e->bvh->nodesUsed == 0 || !e->bvh->bvhNode || !e->bvh->triIdx) return false;

        // (1) Exactly append_common_body's selection.
        const TriEx* triex_src = (!e->tri_extra.empty() && e->tri_extra.size() == tri_count)
                                     ? e->tri_extra.data() : e->mesh->triEx;
        if (triex_src) {
            // (2) Exactly append_common_body's padding normalization.
            normalized.assign(tri_count, TriEx{});
            for (size_t t = 0; t < tri_count; ++t) {
                std::memset(&normalized[t], 0, sizeof(TriEx));
                std::memcpy(&normalized[t], &triex_src[t], kTriExNamedBytes);
            }
        }
        // register_prebuilt, not register_triangles: no BVH is rebuilt and no
        // dedup runs, which is what the reader does. (Dedup already happened
        // when the bake registered these entries; re-running it here would
        // collapse entries the artifact keeps separate.)
        if (scratch->register_prebuilt(
                e->triangles.data(), triex_src ? normalized.data() : nullptr,
                static_cast<int>(tri_count), e->bvh->bvhNode, e->bvh->nodesUsed,
                e->bvh->triIdx, e->hash, e->ref_count) == INVALID_BLAS_HANDLE)
            return false;
    }

    out.scratch = std::move(scratch);
    out.children = baked.children;
    out.lods_in = baked.lods;
    out.emitters = baked.emitters;
    // No ANLK: BakedGeometry is retained only on the static save path, so the
    // artifact this stands in for carries no animation link either.
    out.animation_link.reset();
    out.loaded_animation = {};
    return true;
}

PartStore::StagedPart PartStore::stage_from_bake(
        uint64_t part_hash, const script_host::BakedGeometry& baked,
        size_t first_rung, bool terrain_sector, const WarpAnchor& warp) {
    StagedPart staged;
    staged.part_hash = part_hash;
    CoherentSnapshot snapshot;
    // Charged to read_ms, the slot the artifact decode used to occupy, so the
    // [stream.stage] telemetry keeps reading as "cost of getting the geometry
    // in front of the ladder" on both paths and the saving is legible there.
    const auto read_t0 = std::chrono::steady_clock::now();
    const bool built = snapshot_from_baked(baked, snapshot);
    const double read_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - read_t0).count();
    if (!built) { staged.read_ms = read_ms; return staged; }
    StagedPart out =
        stage_from_snapshot(part_hash, snapshot, nullptr, first_rung,
                            terrain_sector, warp);
    out.read_ms = read_ms;
    return out;
}

// ---------------------------------------------------------------------------
// staged_parts_equal — the proof, not a spot check.
// ---------------------------------------------------------------------------
namespace {

// Bitwise vector compare. operator== on a float vector makes NaN != NaN and
// -0.0f == +0.0f, neither of which is what "the two paths produced the same
// bytes" means.
template <class T>
bool bitwise_equal(const std::vector<T>& a, const std::vector<T>& b) {
    if (a.size() != b.size()) return false;
    return a.empty() ||
           std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0;
}

bool bitwise_equal(float a, float b) {
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

bool mesh_data_equal(const RasterMeshData& a, const RasterMeshData& b) {
    return a.vertex_count == b.vertex_count &&
           bitwise_equal(a.vertices, b.vertices) &&
           bitwise_equal(a.normals, b.normals) &&
           bitwise_equal(a.colors, b.colors) &&
           bitwise_equal(a.texcoords, b.texcoords) &&
           bitwise_equal(a.surface_uvs, b.surface_uvs) &&
           bitwise_equal(a.material_ids, b.material_ids) &&
           bitwise_equal(a.baked_ao, b.baked_ao) &&
           bitwise_equal(a.indices, b.indices) &&
           bitwise_equal(a.warp_uvs, b.warp_uvs) &&
           bitwise_equal(a.warp_frames, b.warp_frames);
}

bool chart_rung_equal(const chart_atlas::ChartAtlasRung& a,
                      const chart_atlas::ChartAtlasRung& b) {
    return a.atlas_w == b.atlas_w && a.atlas_h == b.atlas_h &&
           bitwise_equal(a.charts, b.charts) &&
           bitwise_equal(a.tri_order, b.tri_order);
}

// Geometry identity, member-wise — NOT a raw memcmp of the Tri/TriEx arrays.
//
// Both types carry bytes nothing ever reads: Tri unions each float3 with an
// __m128, leaving 4 unused bytes per vertex slot, and TriEx is 96 bytes with
// its named members ending at 92. The engine defines geometry identity
// member-wise everywhere for exactly this reason (BLASManager::triangles_equal
// and calculate_hash were both FIXED to be member-wise after a contiguous
// compare read padding and ignored vertex2.y/z — partstore_race_tests proofs
// A/B). The LOD ladder that produces these entries has no obligation to zero
// those bytes, so comparing them would report allocator residue as a geometry
// difference and make this gate cry wolf. Every byte the GPU, the BVH, the
// chart bake and the raster mesh actually consume IS compared here.
bool tri_streams_equal(const std::vector<Tri>& a, const std::vector<Tri>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::memcmp(&a[i].vertex0,  &b[i].vertex0,  sizeof(float3)) != 0 ||
            std::memcmp(&a[i].vertex1,  &b[i].vertex1,  sizeof(float3)) != 0 ||
            std::memcmp(&a[i].vertex2,  &b[i].vertex2,  sizeof(float3)) != 0 ||
            std::memcmp(&a[i].centroid, &b[i].centroid, sizeof(float3)) != 0)
            return false;
    }
    return true;
}

bool triex_streams_equal(const std::vector<TriEx>& a, const std::vector<TriEx>& b) {
    if (a.size() != b.size()) return false;
    constexpr size_t kNamed = offsetof(TriEx, ao2) + sizeof(float);  // 92 of 96
    for (size_t i = 0; i < a.size(); ++i)
        if (std::memcmp(&a[i], &b[i], kNamed) != 0) return false;
    return true;
}

bool cluster_equal(const LoadedCluster& a, const LoadedCluster& b) {
    return std::memcmp(a.aabb_min, b.aabb_min, sizeof a.aabb_min) == 0 &&
           std::memcmp(a.aabb_max, b.aabb_max, sizeof a.aabb_max) == 0 &&
           bitwise_equal(a.radius, b.radius) &&
           bitwise_equal(a.thresholds, b.thresholds) &&
           bitwise_equal(a.lod_blas, b.lod_blas) &&
           bitwise_equal(a.lod_mesh, b.lod_mesh);
}

} // namespace

bool staged_parts_equal(const PartStore::StagedPart& a,
                        const PartStore::StagedPart& b,
                        std::string* first_difference) {
    const auto differ = [first_difference](const char* what) {
        if (first_difference) *first_difference = what;
        return false;
    };
    if (a.ok != b.ok)               return differ("ok");
    if (!a.ok)                      return true;   // both unusable: nothing to compare
    if (a.part_hash != b.part_hash) return differ("part_hash");

    const LoadedPart& x = a.lp;
    const LoadedPart& y = b.lp;
    if (!bitwise_equal(x.bound_radius, y.bound_radius)) return differ("bound_radius");
    if (x.fine_cluster_count != y.fine_cluster_count)   return differ("fine_cluster_count");
    if (!bitwise_equal(x.inline_cutover, y.inline_cutover)) return differ("inline_cutover");
    if (!bitwise_equal(x.thresholds, y.thresholds))     return differ("thresholds");
    if (!bitwise_equal(x.lod_blas, y.lod_blas))         return differ("lod_blas");
    if (!bitwise_equal(x.owned_blas, y.owned_blas))     return differ("owned_blas");
    // ChildInstance is padding-free by static_assert, so a flat memcmp is exact.
    if (!bitwise_equal(x.children, y.children))         return differ("children");
    if (!bitwise_equal(x.flat_refs, y.flat_refs))       return differ("flat_refs");
    if (x.animation_asset != y.animation_asset)         return differ("animation_asset");

    if (x.lod_mesh_data.size() != y.lod_mesh_data.size()) return differ("lod_mesh_data.size");
    for (size_t i = 0; i < x.lod_mesh_data.size(); ++i)
        if (!mesh_data_equal(x.lod_mesh_data[i], y.lod_mesh_data[i]))
            return differ("lod_mesh_data");

    if (x.lod_charts.size() != y.lod_charts.size()) return differ("lod_charts.size");
    for (size_t i = 0; i < x.lod_charts.size(); ++i)
        if (!chart_rung_equal(x.lod_charts[i], y.lod_charts[i]))
            return differ("lod_charts");

    if (x.clusters.size() != y.clusters.size()) return differ("clusters.size");
    for (size_t i = 0; i < x.clusters.size(); ++i)
        if (!cluster_equal(x.clusters[i], y.clusters[i]))
            return differ("clusters");

    // The staged BLAS entries are the geometry itself; comparing only the
    // LoadedPart would miss a ladder that produced the same handles over
    // different triangles.
    if (!a.staging || !b.staging) return differ("staging");
    const auto& ea = a.staging->get_entries();
    const auto& eb = b.staging->get_entries();
    if (ea.size() != eb.size()) return differ("staging.entries.size");
    for (size_t i = 0; i < ea.size(); ++i) {
        if (!ea[i] || !eb[i])                          return differ("staging.entry");
        if (ea[i]->hash != eb[i]->hash)                return differ("staging.entry.hash");
        if (ea[i]->ref_count != eb[i]->ref_count)      return differ("staging.entry.ref_count");
        if (!tri_streams_equal(ea[i]->triangles, eb[i]->triangles))
            return differ("staging.entry.triangles");
        if (!triex_streams_equal(ea[i]->tri_extra, eb[i]->tri_extra))
            return differ("staging.entry.tri_extra");
    }
    return true;
}

const LoadedPart* PartStore::commit_staged(StagedPart staged) {
    if (!staged.ok || !staged.staging) return nullptr;
    const uint64_t part_hash = staged.part_hash;
    // A concurrent load may have published this hash while we staged. Keep the
    // resident copy and drop ours rather than double-insert.
    auto existing = loaded_.find(part_hash);
    if (existing != loaded_.end()) return &existing->second;
    {
        PROFILE_SCOPE("commit.adopt");
        std::unordered_map<BLASHandle, BLASHandle> remap;
        blas_.adopt_from(*staged.staging, remap);
        PROFILE_COUNT("adopt_entries", remap.size());
        auto patch = [&remap](std::vector<BLASHandle>& handles) {
            for (BLASHandle& h : handles) {
                auto it = remap.find(h);
                h = (it != remap.end()) ? it->second : INVALID_BLAS_HANDLE;
            }
        };
        patch(staged.lp.lod_blas);
        patch(staged.lp.owned_blas);
        for (LoadedCluster& cluster : staged.lp.clusters)
            patch(cluster.lod_blas);
    }
    if (staged.lp.lod_blas.empty()) {
        // No geometry (empty part) -> log; lookups will see an empty LOD list.
        printf("PartStore: part %016llx produced no LOD geometry\n",
               (unsigned long long)part_hash);
    }

    // Compositional geometry is represented by one synthetic cluster spanning
    // every generated LOD. Geometry-less assemblers keep zero clusters.
    staged.lp.fine_cluster_count = (uint32_t)staged.lp.clusters.size();

    auto ins = loaded_.emplace(part_hash, std::move(staged.lp));
    {
        PROFILE_SCOPE("commit.expansion");
        // Build expansion into a local vector first (see flat path comment).
        std::vector<ExpandedNode> exp;
        build_expansion(part_hash, [this](uint64_t h){ return get_or_load(h); },
                        exp);
        // Deep-vs-wide discriminator: expansion.nodes is the total instance
        // paths the walk visited (huge => uncollapsed subtree); expansion.direct
        // is the sector's own child count (small => the fan-out is below it).
        PROFILE_COUNT("expansion.nodes", exp.size());
        PROFILE_COUNT("expansion.direct", loaded_[part_hash].children.size());
        loaded_[part_hash].expansion = std::move(exp);
    }
    return &ins.first->second;
}

const LoadedPart* PartStore::get_or_load(uint64_t part_hash) {
    auto cached = loaded_.find(part_hash);
    if (cached != loaded_.end()) return &cached->second;
    // A cold decode. On the render thread (via build_expansion during a publish)
    // this is the hitch the install-time child pre-warm exists to eliminate; the
    // counter is the observable invariant (must read 0 per render frame during a
    // cold fill once pre-warm is in). During install/pre-warm it fires freely --
    // that's the point, moving the cost there.
    PROFILE_COUNT("expansion.coldload", 1);

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
        // MATTER_FLAT_GATE_LOG: which admission gate rejects a written flat.
        // Fable's hypothesis is that installed variants are LINKED, so gate #1
        // (load_static_part_snapshot) fails and the coherent path with its full
        // child subtree runs instead -- the uncollapsed walk.
        static const bool flat_gate_log =
            std::getenv("MATTER_FLAT_GATE_LOG") != nullptr;
        const bool snap_ok = part_asset::load_static_part_snapshot(
            canonical_part, part_hash, canonical_fingerprint);
        const bool flat_ok = snap_ok && load_flat(part_hash, selected_root, flat);
        if (flat_gate_log && !flat_ok)
            std::fprintf(stderr,
                         "[flatgate] %016llx REJECT snapshot=%d load_flat=%d\n",
                         (unsigned long long)part_hash, snap_ok ? 1 : 0,
                         snap_ok ? (flat_ok ? 1 : 0) : -1);
        // Same question as MATTER_FLAT_GATE_LOG, as a counter rather than a
        // console line -- a rejection rate is what matters here, not each
        // individual hash, and per-item stderr has measurably distorted this
        // engine's own profiling before.
        //
        // WHY THIS IS THE SUSPECT: a rejected flat falls through to the
        // coherent loader, which walks the part's FULL CHILD SUBTREE instead of
        // the collapsed flat. Same part, same sector -- more instances. A
        // 2026-08-08 capture had resident_sectors pinned at ~2105 and
        // cull.clusters at ~1105 while cull.instances climbed 81830 -> 105934
        // in 17 s, i.e. 39 -> 50 instances per sector with nothing new
        // arriving. An uncollapsed walk is the only mechanism found so far that
        // produces exactly that shape.
        //
        // Two separate call sites on purpose: PROFILE_COUNT caches the
        // interned counter id in a function-local static, so a ternary on the
        // NAME would register once with whichever branch ran first and bucket
        // every later count into it.
        if (flat_ok) {
            PROFILE_COUNT("partstore.flat_ok", 1);
        } else {
            PROFILE_COUNT("partstore.flat_reject", 1);
        }
        if (!snap_ok) PROFILE_COUNT("partstore.flat_no_snapshot", 1);
        if (flat_ok) {
#ifdef MATTER_TEST_CACHE_VALIDATION_HOOK
            if (flat_admission_hook_for_tests_) flat_admission_hook_for_tests_();
#endif
            // Both snapshots parse one exact canonical Part from the root we
            // selected before loading the flat.  A replacement (including a
            // newly linked generation) invalidates this static acceleration;
            // fall through and re-probe the normal coherent loader instead.
            uint64_t final_fingerprint = 0;
            const bool fingerprint_stable =
                part_asset::load_static_part_snapshot(canonical_part, part_hash,
                                                      final_fingerprint) &&
                final_fingerprint == canonical_fingerprint;
            // The SECOND way a flat is abandoned: it loaded fine, but the part
            // was replaced (a newly linked generation) between the two
            // snapshots, so this falls through to the coherent loader too.
            // Counted apart from flat_reject because the remedies differ --
            // a rejection is a gate problem, a re-link is a churn problem.
            if (!fingerprint_stable) PROFILE_COUNT("partstore.flat_relinked", 1);
            if (fingerprint_stable) {
                // Insert the parent FIRST (before any recursive child loads) to prevent
                // re-entrancy: if a child transitively references the same parent hash,
                // the early-out at the top of get_or_load will return the already-inserted
                // (partially constructed) entry rather than recursing infinitely.
                loaded_.emplace(part_hash, std::move(flat));

                // MATTER_PARTSTORE_PROFILE: split the flat path. This function
                // runs inside the stream.publish GpuJob on the app/GL thread,
                // where it measured 3-6 s per sector while the Vulkan
                // registration next to it took 0.4 ms -- so the whole streaming
                // stall lives in here and nothing said which part of it.
                const bool ps_prof =
                    std::getenv("MATTER_PARTSTORE_PROFILE") != nullptr;
                const auto ps_t0 = std::chrono::steady_clock::now();

                // Recursively load each flat_ref child. The parent is already in loaded_
                // so circular references are safe.
                for (const auto& ref : loaded_[part_hash].flat_refs)
                    get_or_load(ref.child_resolved_hash);

                const auto ps_t1 = std::chrono::steady_clock::now();

                // Build expansion into a local vector first, then assign.
                std::vector<ExpandedNode> exp;
                build_expansion(part_hash, [this](uint64_t h){ return get_or_load(h); }, exp);
                const auto ps_t2 = std::chrono::steady_clock::now();
                if (ps_prof) {
                    const auto ms = [](auto a, auto b) {
                        return std::chrono::duration<double, std::milli>(b - a).count();
                    };
                    std::fprintf(stderr,
                        "[partstore] %016llx refs=%zu children=%.1f "
                        "expansion=%.1f ms (nodes=%zu)\n",
                        (unsigned long long)part_hash,
                        loaded_[part_hash].flat_refs.size(),
                        ms(ps_t0, ps_t1), ms(ps_t1, ps_t2), exp.size());
                }
                loaded_[part_hash].expansion = std::move(exp);
                return &loaded_[part_hash];
            }
            // The decoded flat was never published. Undo every shared-BLAS
            // registration before retrying the coherent Part path below.
            release_loaded_part_blas(blas_, flat);
        }
    }

    // Read a linked artifact as a bounded coherent snapshot (see
    // read_coherent_snapshot). Bound to references so the rest of this function
    // reads exactly as it did when these were inline locals.
    CoherentSnapshot snapshot_;
    const bool coherent_ok = read_coherent_snapshot(part_hash, snapshot_);
    std::unique_ptr<BLASManager>&                 scratch         = snapshot_.scratch;
    std::vector<part_asset::ChildInstance>&       children        = snapshot_.children;
    part_asset::LodLevels&                        lods_in         = snapshot_.lods_in;
    std::vector<part_asset::VolumeEmitter>&       emitters        = snapshot_.emitters;
    std::optional<part_asset::PartAnimationLink>& animation_link  = snapshot_.animation_link;
    matter::animation::AnimAsset&                 loaded_animation = snapshot_.loaded_animation;
    if (!coherent_ok) {
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

    // Non-partitioned coherent part: stage it (touches no shared state) then
    // commit it (bounded). Split so a streaming worker can call stage_load().
    StagedPart staged = stage_from_snapshot(part_hash, snapshot_, animation_asset);
    if (!staged.ok) return nullptr;
    return commit_staged(std::move(staged));
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

    // Split because PartStore::release measured 84% of a sector eviction's
    // cost (9.6 of 11.4 ms), and the two halves want different fixes: BLAS
    // release touches the GPU, while erasing the LoadedPart is pure CPU
    // teardown of a sector's whole mesh (thousands of triangles across LOD
    // levels and clusters) and could be handed to a worker.
    {
        PROFILE_SCOPE("store.blas_release");
        release_loaded_part_blas(blas_, lp);
    }

    // Now safe to erase the LoadedPart from memory.
    {
        PROFILE_SCOPE("store.erase_part");
        loaded_.erase(it);
    }
}

} // namespace viewer
