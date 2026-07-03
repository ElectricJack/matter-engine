#include "part_store.h"

#include "part_asset_v2.h"     // load_v2, cache_path_resolved, ChildInstance, LodLevels
#include "lod_bake.h"          // lod_bake::bake_lods, BakeTargets
#include "tlas_manager.hpp"    // TLASManager (load_v2 signature needs one)

#include <cassert>
#include <cmath>
#include <cstdio>
#include <sys/stat.h>

namespace viewer {

PartStore::PartStore(std::string cache_root) : cache_root_(std::move(cache_root)) {}

std::string PartStore::disk_path(uint64_t part_hash) const {
    // cache_path_resolved returns the RELATIVE "parts/<hash>.part"; prefix cache_root_.
    return cache_root_ + "/" + part_asset::cache_path_resolved(part_hash);
}

bool PartStore::has(uint64_t part_hash) const {
    if (loaded_.count(part_hash)) return true;
    struct stat st;
    return ::stat(disk_path(part_hash).c_str(), &st) == 0;
}

// Flat-preferred load: a bake-time flattened artifact (<hash>.flat.part) already
// carries the whole merged subtree plus per-cluster error-bounded LOD ladders.
// Tries v3 first (Task 11 format: clustered flat); falls back to legacy v2 flat
// if v3 is unavailable. Returns false (fall back to the compositional .part) when
// the file is absent or fails to load in either format.
bool PartStore::load_flat(uint64_t part_hash, LoadedPart& lp) {
    const std::string path = cache_root_ + "/" + part_asset::cache_path_flat(part_hash);

    // Sniff version first; fall back to compositional path when absent.
    uint32_t ver = part_asset::peek_format_version(path);
    if (ver == 0) return false;   // absent or unreadable

    if (ver == 3) {
        // --- v3 clustered flat ---
        BLASManager scratch;
        TLASManager scratch_tlas(65536);
        std::vector<part_asset::FlatCluster> clusters_in;
        if (!part_asset::load_flat_v3(path, part_hash, scratch, scratch_tlas, clusters_in) ||
            clusters_in.empty()) {
            printf("PartStore: v3 flat artifact unusable for %016llx (%s), falling back\n",
                   (unsigned long long)part_hash, path.c_str());
            return false;
        }
        // For the flat-preferred path, collapse all clusters' level-0 geometry
        // into one combined LOD ladder by flattening cluster[i].lods[j] across
        // clusters: register ALL cluster level-0 triangles together as lod_blas[0],
        // then all cluster level-1 triangles as lod_blas[1], etc. This gives the
        // PartStore consumer (WorldComposer/RasterComposer) a simple per-part LOD
        // array identical to the v2 flat path. Task 12 will consume clusters
        // individually per-cluster for GPU-side cluster culling.
        const auto& entries = scratch.get_entries();
        // Determine max LOD count across clusters.
        size_t max_lods = 0;
        for (const auto& cl : clusters_in) max_lods = std::max(max_lods, cl.lods.size());
        if (max_lods == 0) return false;

        bool radius_set = false;
        for (size_t li = 0; li < max_lods; ++li) {
            std::vector<Tri> tris;
            std::vector<TriEx> triex;
            float thr = 0.0f;
            for (const auto& cl : clusters_in) {
                if (li >= cl.lods.size()) continue;
                thr = cl.lods[li].screen_size_threshold; // same across clusters
                for (uint32_t bi : cl.lods[li].blas_indices) {
                    if (bi >= entries.size()) continue;
                    tris.insert(tris.end(), entries[bi]->triangles.begin(), entries[bi]->triangles.end());
                    triex.insert(triex.end(), entries[bi]->tri_extra.begin(), entries[bi]->tri_extra.end());
                }
            }
            if (tris.empty()) continue;
            const TriEx* ex = (triex.size() == tris.size()) ? triex.data() : nullptr;
            BLASHandle h = blas_.register_triangles(tris.data(), (int)tris.size(), ex);
            lp.thresholds.push_back(thr);
            lp.lod_blas.push_back(h);

            if (const auto* e = blas_.get_entry(lp.lod_blas.back())) {
                const TriEx* mesh_ex = (e->tri_extra.size() == e->triangles.size() && !e->tri_extra.empty())
                                          ? e->tri_extra.data() : nullptr;
                lp.lod_mesh_data.push_back(
                    build_raster_mesh_data(e->triangles.data(), mesh_ex, (int)e->triangles.size()));
            } else {
                lp.lod_mesh_data.push_back({});
            }

            if (!radius_set && !tris.empty()) {
                float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
                auto acc = [&](const float3& v){
                    mn[0]=std::fmin(mn[0],v.x); mx[0]=std::fmax(mx[0],v.x);
                    mn[1]=std::fmin(mn[1],v.y); mx[1]=std::fmax(mx[1],v.y);
                    mn[2]=std::fmin(mn[2],v.z); mx[2]=std::fmax(mx[2],v.z);
                };
                for (const auto& t : tris) { acc(t.vertex0); acc(t.vertex1); acc(t.vertex2); }
                float dx=mx[0]-mn[0], dy=mx[1]-mn[1], dz=mx[2]-mn[2];
                lp.bound_radius = 0.5f * std::sqrt(dx*dx+dy*dy+dz*dz);
                radius_set = true;
            }
        }
        if (lp.lod_blas.empty()) return false;
        printf("PartStore: loaded v3 FLAT part %016llx (%zu LOD levels, %zu clusters)\n",
               (unsigned long long)part_hash, lp.lod_blas.size(), clusters_in.size());
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
            lp.thresholds.push_back(lods_in[li].screen_size_threshold);
            lp.lod_blas.push_back(h);

            if (const auto* e = blas_.get_entry(lp.lod_blas.back())) {
                const TriEx* mesh_ex = (e->tri_extra.size() == e->triangles.size() && !e->tri_extra.empty())
                                          ? e->tri_extra.data() : nullptr;
                lp.lod_mesh_data.push_back(
                    build_raster_mesh_data(e->triangles.data(), mesh_ex, (int)e->triangles.size()));
            } else {
                lp.lod_mesh_data.push_back({});
            }

            if (li == 0) {
                float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
                auto acc = [&](const float3& v){
                    mn[0]=std::fmin(mn[0],v.x); mx[0]=std::fmax(mx[0],v.x);
                    mn[1]=std::fmin(mn[1],v.y); mx[1]=std::fmax(mx[1],v.y);
                    mn[2]=std::fmin(mn[2],v.z); mx[2]=std::fmax(mx[2],v.z);
                };
                for (const auto& t : tris) { acc(t.vertex0); acc(t.vertex1); acc(t.vertex2); }
                float dx=mx[0]-mn[0], dy=mx[1]-mn[1], dz=mx[2]-mn[2];
                lp.bound_radius = 0.5f * std::sqrt(dx*dx+dy*dy+dz*dz);
            }
        }
        if (lp.lod_blas.empty()) return false;
        printf("PartStore: loaded v2 FLAT part %016llx (%zu LOD levels)\n",
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
    if (load_failed_.count(part_hash)) return nullptr;

    {
        LoadedPart flat;
        if (load_flat(part_hash, flat)) {
            auto ins = loaded_.emplace(part_hash, std::move(flat));
            return &ins.first->second;
        }
    }

    const std::string path = disk_path(part_hash);

    // load_v2 registers the full-resolution geometry into a SCRATCH BLASManager;
    // we then re-bake LODs into the shared store BLASManager.
    BLASManager scratch;
    // Sized to match the part bake's group cap: a detailed trunk bakes to >256
    // mesh groups. The scratch TLAS is unused for geometry (we re-bake LODs from
    // the BLAS triangles below), but an undersized cap spams capacity warnings.
    TLASManager scratch_tlas(65536);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods_in;   // .part stores LOD0 only (empty levels)
    if (!part_asset::load_v2(path, part_hash, scratch, scratch_tlas, children, lods_in)) {
        printf("PartStore: load_v2 failed for %016llx (%s)\n",
               (unsigned long long)part_hash, path.c_str());
        load_failed_.insert(part_hash);
        return nullptr;
    }

    // Gather full-res triangles (and their parallel per-triangle TriEx, which
    // carries the baked materialId/tint/normals) for lod_bake. Without the TriEx
    // the re-baked LOD geometry has no per-triangle material, so every triangle
    // falls back to the instance material in the shader and the whole world renders
    // as one color. e->triangles and e->tri_extra are parallel in registration order.
    std::vector<Tri> tris;
    std::vector<TriEx> triex;
    for (const auto& e : scratch.get_entries()) {
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

    // Re-bake LODs into the SHARED store BLASManager. lod_bake stores the
    // ABSOLUTE entries_ index (== get_entries().size() before registration),
    // so use blas_indices[0] directly as the index — do NOT add 'before'.
    LoadedPart lp;
    lp.bound_radius = radius;
    lp.children = std::move(children);   // keep the baked child-instance table for the WorldComposer
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

        if (const auto* e = blas_.get_entry(lp.lod_blas.back())) {
            const TriEx* mesh_ex = (e->tri_extra.size() == e->triangles.size() && !e->tri_extra.empty())
                                      ? e->tri_extra.data() : nullptr;
            lp.lod_mesh_data.push_back(
                build_raster_mesh_data(e->triangles.data(), mesh_ex, (int)e->triangles.size()));
        } else {
            lp.lod_mesh_data.push_back({});
        }
    }
    if (lp.lod_blas.empty()) {
        // No geometry (empty part) -> log; lookups will see an empty LOD list.
        printf("PartStore: part %016llx produced no LOD geometry\n",
               (unsigned long long)part_hash);
    }

    auto ins = loaded_.emplace(part_hash, std::move(lp));
    return &ins.first->second;
}

lod_select::PartLodTable PartStore::part_lod_table() const {
    lod_select::PartLodTable table;
    for (const auto& kv : loaded_)
        table[kv.first] = lod_select::PartLod{ kv.second.bound_radius, kv.second.thresholds };
    return table;
}

} // namespace viewer
