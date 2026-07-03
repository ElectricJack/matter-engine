#include "../include/part_flatten.h"

#include "../include/part_asset_v2.h"   // load_v2/save_flat_v3, cache_path_*
#include "../include/lod_bake.h"        // decimate_to_error, reproject_triex
#include "../include/part_cluster.h"    // split_clusters
#include "tlas_manager.hpp"             // MSL TLASManager (load_v2 signature)

#include <cmath>
#include <cstring>
#include <map>
#include <memory>

namespace part_flatten {

namespace {

// Row-major 4x4 multiply; same convention as ChildInstance transforms and the
// viewer WorldComposer (translation lives in m[3], m[7], m[11]).
void mul16(const float* a, const float* b, float* out) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[i*4+k] * b[k*4+j];
            out[i*4+j] = s;
        }
}

float3 xform_point(const float* m, const float3& p) {
    return make_float3(m[0]*p.x + m[1]*p.y + m[2]*p.z  + m[3],
                       m[4]*p.x + m[5]*p.y + m[6]*p.z  + m[7],
                       m[8]*p.x + m[9]*p.y + m[10]*p.z + m[11]);
}

// Inverse-transpose of the upper 3x3, for shading normals under non-uniform
// scale. Falls back to the raw 3x3 when the matrix is (near-)singular.
struct NormalMat {
    float n[9];
    explicit NormalMat(const float* m) {
        const float a=m[0], b=m[1], c=m[2],
                    d=m[4], e=m[5], f=m[6],
                    g=m[8], h=m[9], i=m[10];
        const float A =  (e*i - f*h), B = -(d*i - f*g), C =  (d*h - e*g);
        const float det = a*A + b*B + c*C;
        if (std::fabs(det) < 1e-12f) {
            n[0]=a; n[1]=b; n[2]=c; n[3]=d; n[4]=e; n[5]=f; n[6]=g; n[7]=h; n[8]=i;
            return;
        }
        // inverse (cofactor/det), then transpose -> store transposed inverse.
        const float id = 1.0f / det;
        n[0] = A*id;             n[3] = -(b*i - c*h)*id;  n[6] =  (b*f - c*e)*id;
        n[1] = B*id;             n[4] =  (a*i - c*g)*id;  n[7] = -(a*f - c*d)*id;
        n[2] = C*id;             n[5] = -(a*h - b*g)*id;  n[8] =  (a*e - b*d)*id;
        // n currently holds inverse in column-major-of-row-major sense; we want
        // (M^-1)^T applied as row-major. Transpose in place.
        std::swap(n[1], n[3]); std::swap(n[2], n[6]); std::swap(n[5], n[7]);
    }
    float3 apply(const float3& v) const {
        float3 r = make_float3(n[0]*v.x + n[1]*v.y + n[2]*v.z,
                               n[3]*v.x + n[4]*v.y + n[5]*v.z,
                               n[6]*v.x + n[7]*v.y + n[8]*v.z);
        float len = std::sqrt(r.x*r.x + r.y*r.y + r.z*r.z);
        if (len > 1e-12f) { r.x/=len; r.y/=len; r.z/=len; }
        return r;
    }
};

// One part's geometry in its LOCAL frame, loaded once and re-instanced per
// placement (a tree places the same Leaf hash hundreds of times).
struct PartGeo {
    bool ok = false;
    std::vector<Tri>   tris;
    std::vector<TriEx> triex;                                // parallel to tris
    std::vector<part_asset::ChildInstance> children;
};

class Gatherer {
public:
    Gatherer(const std::string& cache_root, const FlattenTargets& t)
        : cache_root_(cache_root), targets_(t) {}

    bool gather(uint64_t hash, const float* world, int depth, std::string& err) {
        if (depth > targets_.max_depth) return true;   // silently cap, like the composer
        const PartGeo* geo = load(hash, err);
        if (!geo) return false;

        NormalMat nm(world);
        for (size_t i = 0; i < geo->tris.size(); ++i) {
            const Tri& s = geo->tris[i];
            Tri t;
            t.vertex0 = xform_point(world, s.vertex0);
            t.vertex1 = xform_point(world, s.vertex1);
            t.vertex2 = xform_point(world, s.vertex2);
            t.centroid = make_float3((t.vertex0.x+t.vertex1.x+t.vertex2.x)/3,
                                     (t.vertex0.y+t.vertex1.y+t.vertex2.y)/3,
                                     (t.vertex0.z+t.vertex1.z+t.vertex2.z)/3);
            TriEx ex = geo->triex[i];
            ex.N0 = nm.apply(ex.N0);
            ex.N1 = nm.apply(ex.N1);
            ex.N2 = nm.apply(ex.N2);
            tris_.push_back(t);
            triex_.push_back(ex);
        }
        for (const auto& c : geo->children) {
            float child_world[16];
            mul16(world, c.transform, child_world);
            if (!gather(c.child_resolved_hash, child_world, depth + 1, err))
                return false;
        }
        return true;
    }

    std::vector<Tri>&   tris()  { return tris_; }
    std::vector<TriEx>& triex() { return triex_; }

private:
    const PartGeo* load(uint64_t hash, std::string& err) {
        auto it = cache_.find(hash);
        if (it != cache_.end()) return it->second->ok ? it->second.get() : nullptr;

        auto geo = std::make_unique<PartGeo>();
        BLASManager scratch;
        TLASManager scratch_tlas(65536);   // unused for geometry; sized to keep quiet
        part_asset::LodLevels lods_in;
        const std::string path = cache_root_ + "/" + part_asset::cache_path_resolved(hash);
        if (!part_asset::load_v2(path, hash, scratch, scratch_tlas,
                                 geo->children, lods_in)) {
            err = "flatten: load_v2 failed for " + path;
            cache_.emplace(hash, std::move(geo));
            return nullptr;
        }
        // A baked .part registers one BLAS entry PER LOD level; merging all
        // entries would stack the full mesh with its own decimations. Take only
        // the finest level's entries (level 0); fall back to everything when the
        // file carries no LOD table.
        std::vector<const BLASManager::BLASEntry*> sources;
        if (!lods_in.empty() && !lods_in[0].blas_indices.empty()) {
            const auto& entries = scratch.get_entries();
            for (uint32_t bi : lods_in[0].blas_indices)
                if (bi < entries.size()) sources.push_back(entries[bi].get());
        } else {
            for (const auto& e : scratch.get_entries()) sources.push_back(e.get());
        }
        for (const BLASManager::BLASEntry* e : sources) {
            const size_t base = geo->tris.size();
            geo->tris.insert(geo->tris.end(), e->triangles.begin(), e->triangles.end());
            if (e->tri_extra.size() == e->triangles.size()) {
                geo->triex.insert(geo->triex.end(), e->tri_extra.begin(), e->tri_extra.end());
            } else {
                // No per-triangle table for this entry: synthesize neutral TriEx
                // (instance-material fallback, geometric normal) so the merged
                // arrays stay parallel.
                for (size_t i = base; i < geo->tris.size(); ++i) {
                    const Tri& t = geo->tris[i];
                    TriEx ex;
                    std::memset(&ex, 0, sizeof(TriEx));
                    ex.tint = make_float4(1, 1, 1, 0);
                    ex.ao0 = ex.ao1 = ex.ao2 = 1.0f;
                    float3 e1 = make_float3(t.vertex1.x-t.vertex0.x, t.vertex1.y-t.vertex0.y, t.vertex1.z-t.vertex0.z);
                    float3 e2 = make_float3(t.vertex2.x-t.vertex0.x, t.vertex2.y-t.vertex0.y, t.vertex2.z-t.vertex0.z);
                    float3 n = make_float3(e1.y*e2.z - e1.z*e2.y,
                                           e1.z*e2.x - e1.x*e2.z,
                                           e1.x*e2.y - e1.y*e2.x);
                    float len = std::sqrt(n.x*n.x + n.y*n.y + n.z*n.z);
                    if (len > 1e-12f) { n.x/=len; n.y/=len; n.z/=len; } else n = make_float3(0,1,0);
                    ex.N0 = ex.N1 = ex.N2 = n;
                    geo->triex.push_back(ex);
                }
            }
        }
        geo->ok = true;
        auto ins = cache_.emplace(hash, std::move(geo));
        return ins.first->second.get();
    }

    std::string cache_root_;
    FlattenTargets targets_;
    std::map<uint64_t, std::unique_ptr<PartGeo>> cache_;
    std::vector<Tri>   tris_;
    std::vector<TriEx> triex_;
};

const float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

} // namespace

FlattenResult flatten_part(const std::string& cache_root, uint64_t root_hash,
                           const FlattenTargets& targets) {
    FlattenResult res;

    Gatherer g(cache_root, targets);
    if (!g.gather(root_hash, kIdentity, 0, res.error)) return res;

    std::vector<Tri>&   full   = g.tris();
    std::vector<TriEx>& fullex = g.triex();
    if (full.empty()) { res.error = "flatten: merged mesh is empty"; return res; }
    res.full_tris = full.size();

    // Whole-mesh AABB and bound radius (half diagonal). Used to compute
    // consistent epsilon across all clusters: using the global radius means
    // a cluster's LOD ladder matches what an equivalent whole-mesh bake would
    // produce at the same view distance.
    float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
    auto acc = [&](const float3& v){
        mn[0]=std::fmin(mn[0],v.x); mx[0]=std::fmax(mx[0],v.x);
        mn[1]=std::fmin(mn[1],v.y); mx[1]=std::fmax(mx[1],v.y);
        mn[2]=std::fmin(mn[2],v.z); mx[2]=std::fmax(mx[2],v.z);
    };
    for (const auto& t : full) { acc(t.vertex0); acc(t.vertex1); acc(t.vertex2); }
    float dx=mx[0]-mn[0], dy=mx[1]-mn[1], dz=mx[2]-mn[2];
    const float radius = 0.5f * std::sqrt(dx*dx+dy*dy+dz*dz);

    // Split the merged mesh into spatial clusters.
    auto clusters = part_cluster::split_clusters(full, fullex, targets.cluster_target_tris);
    const size_t n_clusters = clusters.size();

    // Shared BLAS/TLAS for the flat artifact (all clusters share one BLAS table).
    BLASManager blas;
    TLASManager tlas(4);   // no internal instances in a flat artifact

    std::vector<part_asset::FlatCluster> flat_clusters;
    flat_clusters.reserve(n_clusters);

    size_t max_levels = 0;
    size_t coarsest_tris = 0;

    for (const auto& cl : clusters) {
        // Slice the cluster's triangles from the reordered arrays.
        std::vector<Tri> ctris(full.begin() + cl.first_tri,
                               full.begin() + cl.first_tri + cl.tri_count);
        std::vector<TriEx> ctriex;
        if (fullex.size() == full.size()) {
            ctriex.assign(fullex.begin() + cl.first_tri,
                          fullex.begin() + cl.first_tri + cl.tri_count);
        }

        // Build per-cluster LOD ladder.
        // Level 0: full cluster (no decimation).
        struct Level { std::vector<Tri> tris; std::vector<TriEx> triex; float eps; };
        std::vector<Level> levels;
        {
            Level L0;
            L0.tris  = ctris;
            L0.triex = ctriex;
            L0.eps   = 0.0f;
            levels.push_back(std::move(L0));
        }
        size_t prev_count = ctris.size();
        for (float div : targets.radius_divisor) {
            // Use global radius so epsilon is consistent across clusters.
            const float eps = radius / div;
            // use_aabb_bounds=false: only topological boundary lock (open edges =
            // cluster-cut seam edges) freezes cut vertices; face-plane locking
            // would over-freeze cluster interiors that touch the cluster AABB.
            std::vector<Tri> geo = lod_bake::decimate_to_error(ctris, eps, /*use_aabb_bounds=*/false);
            if (geo.empty() || geo.size() >= prev_count) continue;  // no progress
            std::vector<TriEx> ex = lod_bake::reproject_triex(geo, ctris, ctriex);
            levels.push_back({std::move(geo), std::move(ex), eps});
            prev_count = levels.back().tris.size();
            if (prev_count <= (size_t)targets.min_level_tris) break;
        }

        // Register each level into the shared BLAS table and build LodLevels.
        part_asset::LodLevels lods;
        for (size_t i = 0; i < levels.size(); ++i) {
            Level& L = levels[i];
            const TriEx* ex_ptr = L.triex.empty() ? nullptr : L.triex.data();
            BLASHandle h = blas.register_triangles(L.tris.data(), (int)L.tris.size(), ex_ptr);
            uint32_t idx = UINT32_MAX;
            const auto& entries = blas.get_entries();
            for (size_t k = 0; k < entries.size(); ++k)
                if (entries[k]->handle == h) { idx = (uint32_t)k; break; }
            if (idx == UINT32_MAX) { res.error = "flatten: BLAS registration failed"; return res; }

            // Threshold: the global radius gives consistent lod_select behaviour.
            float thr = 0.0f;
            if (i + 1 < levels.size()) {
                const float next_eps = levels[i + 1].eps;
                thr = radius * targets.pixel_budget * targets.pixel_angle / next_eps;
            }
            part_asset::LodLevel lvl;
            lvl.screen_size_threshold = thr;
            lvl.blas_indices.push_back(idx);
            lods.push_back(std::move(lvl));
        }

        if (levels.size() > max_levels) max_levels = levels.size();
        coarsest_tris = levels.back().tris.size();

        part_asset::FlatCluster fc;
        fc.aabb_min[0] = cl.aabb_min[0]; fc.aabb_min[1] = cl.aabb_min[1]; fc.aabb_min[2] = cl.aabb_min[2];
        fc.aabb_max[0] = cl.aabb_max[0]; fc.aabb_max[1] = cl.aabb_max[1]; fc.aabb_max[2] = cl.aabb_max[2];
        fc.lods = std::move(lods);
        flat_clusters.push_back(std::move(fc));
    }

    res.levels        = max_levels;
    res.clusters      = n_clusters;
    res.coarsest_tris = coarsest_tris;

    const std::string out_path = cache_root + "/" + part_asset::cache_path_flat(root_hash);
    if (!part_asset::save_flat_v3(out_path, blas, tlas, flat_clusters, root_hash)) {
        res.error = "flatten: save_flat_v3 failed for " + out_path;
        return res;
    }
    res.ok = true;
    return res;
}

} // namespace part_flatten
