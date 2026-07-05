#include "../include/part_flatten.h"

#include "../include/part_asset_v2.h"   // load_v2/save_flat_v3, cache_path_*, load_lod_sidecar
#include "../include/lod_bake.h"        // decimate_to_error, reproject_triex
#include "../include/part_cluster.h"    // split_clusters
#include "tlas_manager.hpp"             // MSL TLASManager (load_v2 signature)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>   // std::getenv
#include <cstring>
#include <limits>    // numeric_limits (per-cluster AABB init, bake-hardening #3)
#include <map>
#include <memory>
#include <new>       // std::bad_alloc
#include <unordered_map>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#  include <sys/resource.h>  // getrusage (peak-RSS measurement, bake-hardening #3)
#endif

namespace part_flatten {

namespace {

// Bake-hardening #3: peak-RSS measurement helper. Returns process peak resident
// set size in bytes (0 if unsupported). On Linux ru_maxrss is in KB; on macOS
// it's in bytes. Windows path (mingw) has no getrusage; returns 0. Opt-in via
// MATTER_FLATTEN_PEAK=1 so normal test output stays clean.
static size_t peak_rss_bytes() {
#if defined(__linux__)
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    return static_cast<size_t>(ru.ru_maxrss) * 1024ull;
#elif defined(__APPLE__)
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    return static_cast<size_t>(ru.ru_maxrss);
#else
    return 0;
#endif
}
static bool peak_logging_enabled() {
    const char* v = std::getenv("MATTER_FLATTEN_PEAK");
    return v && v[0] && v[0] != '0';
}

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

// Bake-hardening #3: skeleton-gather ticket. Each ticket points at a single
// source triangle (geo_idx into geos_ + local_tri_idx) instantiated under one
// world context (ctx_idx into contexts_). 16 bytes/ticket + 12 bytes/centroid
// = 28 bytes/tri kept alive between Pass 1 and Pass 2, versus ~160 bytes/tri
// (64 Tri + 96 TriEx) if we materialized the whole merged mesh up front.
struct Ticket {
    uint32_t geo_idx;         // index into Gatherer::geos_
    uint32_t ctx_idx;         // index into Gatherer::contexts_ (world+NormalMat)
    uint32_t local_tri_idx;   // index into geos_[geo_idx]->tris/triex
    uint32_t _pad;            // pad to 16 bytes so vector<Ticket> is trivially copyable
};

// One traversal node's world-space context, shared by every triangle the walk
// emits under it. NormalMat is the inverse-transpose of the world's upper 3x3
// — deriving it costs a few divs, so we cache it once per node instead of once
// per triangle (a scatter can have 50k+ tris under the same context).
struct Context {
    float world[16];
    NormalMat nm;
    explicit Context(const float* w) : nm(w) {
        std::memcpy(world, w, 16 * sizeof(float));
    }
};

class Gatherer {
public:
    Gatherer(const std::string& cache_root, const FlattenTargets& t)
        : cache_root_(cache_root), targets_(t) {}

    // Bake-hardening #2: register the per-hash flatten decision map before
    // gather(). Any child seen during the walk that maps to BOUNDARY is not
    // expanded into tris/triex; instead its (child_hash, world-transform) is
    // recorded into instance_refs_ for the caller to serialize.
    void set_decisions(const std::unordered_map<uint64_t, FlattenDecision>* d,
                       uint64_t self_hash) {
        decisions_ = d;
        self_hash_ = self_hash;
    }

    bool gather(uint64_t hash, const float* world, int depth, std::string& err) {
        if (depth > targets_.max_depth) return true;   // silently cap, like the composer
        // Bake-hardening #2: a child marked BOUNDARY stays out of the merge —
        // its (hash, world) pair is recorded so the writer emits a
        // FlatInstanceRef instead of expanding the mesh. The root itself is
        // never treated as a boundary of itself.
        if (decisions_ && hash != self_hash_) {
            auto it = decisions_->find(hash);
            if (it != decisions_->end() && it->second == FlattenDecision::BOUNDARY) {
                part_asset::FlatInstanceRef ref{};
                ref.child_resolved_hash = hash;
                std::memcpy(ref.transform, world, 16 * sizeof(float));
                instance_refs_.push_back(ref);
                return true;
            }
        }
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
        // Bake-hardening #2: if THIS part (the root of the flatten job) is
        // itself marked BOUNDARY, every direct child of ROOT is emitted as an
        // instance_ref regardless of the child's own decision. That's the
        // "shallow flatten" case from the roadmap: P.flat.part contains only
        // P's own geometry + refs for all its direct children, so P's own
        // artifact is bounded even when the full inline expansion would
        // exceed budget. Deeper descendants (grandchildren) never get an
        // opportunity to expand from here because they'd only be reached
        // through a child we just short-circuited.
        bool self_is_boundary = false;
        if (decisions_ && hash == self_hash_) {
            auto it = decisions_->find(hash);
            self_is_boundary = (it != decisions_->end() &&
                                it->second == FlattenDecision::BOUNDARY);
        }
        for (const auto& c : geo->children) {
            float child_world[16];
            mul16(world, c.transform, child_world);
            if (self_is_boundary) {
                part_asset::FlatInstanceRef ref{};
                ref.child_resolved_hash = c.child_resolved_hash;
                std::memcpy(ref.transform, child_world, 16 * sizeof(float));
                instance_refs_.push_back(ref);
                continue;
            }
            if (!gather(c.child_resolved_hash, child_world, depth + 1, err))
                return false;
        }
        return true;
    }

    // Bake-hardening #3: streaming variant of gather(). Walks the same subtree
    // in the same DFS order — so tickets_ ends up in identical order to what
    // gather() would put in tris_/triex_ — but only records per-triangle
    // centroids + tickets (28 bytes/tri) instead of materializing full Tri +
    // TriEx (~160 bytes/tri). The caller then feeds centroids_ into
    // part_cluster::split_centroids to get a permutation, and materializes each
    // cluster in turn from the tickets in Pass 2.
    //
    // Side-effect: also accumulates the merged vertex AABB (needed for the
    // global radius/epsilon used across all clusters). This is the ONE place
    // we still touch every source vertex; per triangle that's ~48 bytes read,
    // 12 bytes/side compare + no store — cheap.
    bool skeleton_gather(uint64_t hash, const float* world, int depth,
                         std::string& err,
                         float aabb_min[3], float aabb_max[3]) {
        if (depth > targets_.max_depth) return true;
        if (decisions_ && hash != self_hash_) {
            auto it = decisions_->find(hash);
            if (it != decisions_->end() && it->second == FlattenDecision::BOUNDARY) {
                part_asset::FlatInstanceRef ref{};
                ref.child_resolved_hash = hash;
                std::memcpy(ref.transform, world, 16 * sizeof(float));
                instance_refs_.push_back(ref);
                return true;
            }
        }
        const PartGeo* geo = load(hash, err);
        if (!geo) return false;

        // Register geo pointer + context once for the whole batch of triangles
        // this traversal node emits. The geo table is dedup'd — one entry per
        // unique hash, shared across all placements. The context table is NOT
        // dedup'd (each traversal node has its own world matrix).
        const uint32_t geo_idx = intern_geo(hash, geo);
        const uint32_t ctx_idx = intern_context(world);

        for (size_t i = 0; i < geo->tris.size(); ++i) {
            const Tri& s = geo->tris[i];
            const float3 v0 = xform_point(world, s.vertex0);
            const float3 v1 = xform_point(world, s.vertex1);
            const float3 v2 = xform_point(world, s.vertex2);
            // Accumulate vertex AABB inline — same three vertices the old code
            // would have written into the full mesh, but we skip the store.
            for (const float3* v : {&v0, &v1, &v2}) {
                if (v->x < aabb_min[0]) aabb_min[0] = v->x;
                if (v->y < aabb_min[1]) aabb_min[1] = v->y;
                if (v->z < aabb_min[2]) aabb_min[2] = v->z;
                if (v->x > aabb_max[0]) aabb_max[0] = v->x;
                if (v->y > aabb_max[1]) aabb_max[1] = v->y;
                if (v->z > aabb_max[2]) aabb_max[2] = v->z;
            }
            centroids_.push_back(make_float3((v0.x+v1.x+v2.x)/3,
                                             (v0.y+v1.y+v2.y)/3,
                                             (v0.z+v1.z+v2.z)/3));
            tickets_.push_back({geo_idx, ctx_idx, static_cast<uint32_t>(i), 0u});
        }

        bool self_is_boundary = false;
        if (decisions_ && hash == self_hash_) {
            auto it = decisions_->find(hash);
            self_is_boundary = (it != decisions_->end() &&
                                it->second == FlattenDecision::BOUNDARY);
        }
        for (const auto& c : geo->children) {
            float child_world[16];
            mul16(world, c.transform, child_world);
            if (self_is_boundary) {
                part_asset::FlatInstanceRef ref{};
                ref.child_resolved_hash = c.child_resolved_hash;
                std::memcpy(ref.transform, child_world, 16 * sizeof(float));
                instance_refs_.push_back(ref);
                continue;
            }
            if (!skeleton_gather(c.child_resolved_hash, child_world, depth + 1,
                                 err, aabb_min, aabb_max))
                return false;
        }
        return true;
    }

    // Bake-hardening #3: materialize a single cluster's worth of triangles from
    // the skeleton tables. `order[cluster_lo..cluster_hi)` are permuted ticket
    // indices from split_centroids. Writes the world-space Tri + TriEx into
    // `ctris` / `ctriex` in permuted order, and accumulates the cluster's
    // vertex AABB alongside.
    void materialize_range(const std::vector<uint32_t>& order,
                           uint32_t lo, uint32_t hi,
                           std::vector<Tri>&   ctris,
                           std::vector<TriEx>& ctriex,
                           float cl_min[3], float cl_max[3]) const {
        const size_t n = hi - lo;
        ctris.resize(n);
        ctriex.resize(n);
        for (int k = 0; k < 3; ++k) {
            cl_min[k] =  std::numeric_limits<float>::max();
            cl_max[k] = -std::numeric_limits<float>::max();
        }
        for (uint32_t j = lo; j < hi; ++j) {
            const uint32_t src_idx = order[j];
            const Ticket& tk = tickets_[src_idx];
            const PartGeo* g = geos_[tk.geo_idx];
            const Context& c = contexts_[tk.ctx_idx];
            const Tri& s = g->tris[tk.local_tri_idx];
            Tri t;
            t.vertex0 = xform_point(c.world, s.vertex0);
            t.vertex1 = xform_point(c.world, s.vertex1);
            t.vertex2 = xform_point(c.world, s.vertex2);
            t.centroid = make_float3((t.vertex0.x+t.vertex1.x+t.vertex2.x)/3,
                                     (t.vertex0.y+t.vertex1.y+t.vertex2.y)/3,
                                     (t.vertex0.z+t.vertex1.z+t.vertex2.z)/3);
            ctris[j - lo] = t;
            TriEx ex = g->triex[tk.local_tri_idx];
            ex.N0 = c.nm.apply(ex.N0);
            ex.N1 = c.nm.apply(ex.N1);
            ex.N2 = c.nm.apply(ex.N2);
            ctriex[j - lo] = ex;
            for (const float3* v : {&t.vertex0, &t.vertex1, &t.vertex2}) {
                if (v->x < cl_min[0]) cl_min[0] = v->x;
                if (v->y < cl_min[1]) cl_min[1] = v->y;
                if (v->z < cl_min[2]) cl_min[2] = v->z;
                if (v->x > cl_max[0]) cl_max[0] = v->x;
                if (v->y > cl_max[1]) cl_max[1] = v->y;
                if (v->z > cl_max[2]) cl_max[2] = v->z;
            }
        }
    }

    std::vector<Tri>&   tris()  { return tris_; }
    std::vector<TriEx>& triex() { return triex_; }
    std::vector<part_asset::FlatInstanceRef>& instance_refs() { return instance_refs_; }

    // Bake-hardening #3: skeleton-gather outputs.
    std::vector<float3>&  centroids() { return centroids_; }
    std::vector<Ticket>&  tickets()   { return tickets_; }
    size_t total_bytes_skeleton() const {
        return centroids_.capacity() * sizeof(float3)
             + tickets_.capacity()   * sizeof(Ticket)
             + contexts_.capacity()  * sizeof(Context)
             + geos_.capacity()      * sizeof(PartGeo*);
    }

    // Access the loaded PartGeo cache so callers can walk children/tri counts
    // for the estimation pass without a second round-trip to disk.
    const PartGeo* peek(uint64_t hash) const {
        auto it = cache_.find(hash);
        return (it == cache_.end()) ? nullptr : it->second.get();
    }
    // Load-and-return; exposes the lazy loader for the estimation pass.
    const PartGeo* load_public(uint64_t hash, std::string& err) {
        return load(hash, err);
    }

private:
    // Skeleton-gather interning: return an index into geos_ for `hash`,
    // creating an entry the first time we see the hash. Two different scatter
    // placements of the same part share the same geo_idx.
    uint32_t intern_geo(uint64_t hash, const PartGeo* geo) {
        auto it = geo_index_.find(hash);
        if (it != geo_index_.end()) return it->second;
        const uint32_t idx = static_cast<uint32_t>(geos_.size());
        geos_.push_back(geo);
        geo_index_[hash] = idx;
        return idx;
    }
    // Skeleton-gather interning: register a new context per traversal node.
    // Contexts are NOT dedup'd — each recursive gather() call gets its own
    // world matrix. Sharing would require comparing floats bitwise; the cost
    // of Context construction is small (a NormalMat cofactor + inversion).
    uint32_t intern_context(const float* world) {
        const uint32_t idx = static_cast<uint32_t>(contexts_.size());
        contexts_.emplace_back(world);
        return idx;
    }

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
    // Bake-hardening #2: children whose per-part decision is BOUNDARY are
    // recorded here (world-space transform + child hash) instead of being
    // expanded into tris/triex. The writer emits them as FlatInstanceRefs.
    std::vector<part_asset::FlatInstanceRef> instance_refs_;
    // Optional per-part decision map, keyed by resolved hash. self_hash_ is
    // the root of the flatten job — the root's own decision is not applied
    // to itself (a BOUNDARY root still writes its own .flat.part).
    const std::unordered_map<uint64_t, FlattenDecision>* decisions_ = nullptr;
    uint64_t self_hash_ = 0;

    // Bake-hardening #3: streaming-gather state. Populated by skeleton_gather;
    // consumed by materialize_range one cluster at a time so the merged mesh
    // never has to live in memory. `geos_` points into `cache_` (which owns
    // the PartGeo objects) — do not free cache entries while materializing.
    std::vector<const PartGeo*>          geos_;         // dedup'd; ptrs into cache_
    std::unordered_map<uint64_t, uint32_t> geo_index_;
    std::vector<Context>                 contexts_;     // one per traversal node
    std::vector<Ticket>                  tickets_;      // one per emitted triangle
    std::vector<float3>                  centroids_;    // parallel to tickets_
};

const float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

} // namespace

// Stage 3: assemble a flat artifact from budget-variant bakes (sidecar) instead
// of QEM. Single cluster; level i = variant i's full mesh with native TriEx.
static FlattenResult flatten_budget_ladder(const std::string& cache_root,
                                           uint64_t root_hash,
                                           const FlattenTargets& targets,
                                           Gatherer& g0, float radius,
                                           const part_asset::LodVariants& v) {
    FlattenResult res;
    res.full_tris = g0.tris().size();

    BLASManager blas;
    TLASManager tlas(4);
    part_asset::LodLevels lods;

    // Full res holds until an anchor-sized feature (a grass blade) is ~2 px;
    // each coarser level halves the switch size (same ratio-2 spirit as the
    // mesh ladder). Selection metric is the PART's projected size
    // (radius / dist), so convert: at blade==2px, dist = anchor/(2*pixel_angle)
    // => part psize = 2 * radius * pixel_angle / anchor.
    const float anchor = (v.anchor_size > 0.0) ? (float)v.anchor_size : radius;
    const float thr0 = 2.0f * radius * targets.pixel_angle * targets.pixel_budget / anchor;

    // Union AABB over ALL variants (widened blades overhang the level-0 box).
    float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
    auto acc = [&](const std::vector<Tri>& ts) {
        for (const auto& t : ts) {
            const float3* vs[3] = { &t.vertex0, &t.vertex1, &t.vertex2 };
            for (int k = 0; k < 3; ++k) {
                mn[0]=std::fmin(mn[0],vs[k]->x); mx[0]=std::fmax(mx[0],vs[k]->x);
                mn[1]=std::fmin(mn[1],vs[k]->y); mx[1]=std::fmax(mx[1],vs[k]->y);
                mn[2]=std::fmin(mn[2],vs[k]->z); mx[2]=std::fmax(mx[2],vs[k]->z);
            }
        }
    };

    // Cap levels: `1u << i` is UB for i >= 32; schemas have no upper bound on
    // lodBudgets.size(), so enforce a safe maximum here.
    const size_t n = std::min(v.hashes.size(), (size_t)16);
    for (size_t i = 0; i < n; ++i) {
        std::vector<Tri> tris; std::vector<TriEx> ex;
        if (v.hashes[i] == root_hash) {
            tris = g0.tris(); ex = g0.triex();
        } else {
            Gatherer gi(cache_root, targets);
            if (!gi.gather(v.hashes[i], kIdentity, 0, res.error)) return res;
            tris = gi.tris(); ex = gi.triex();
        }
        if (tris.empty()) { res.error = "flatten: empty budget variant"; return res; }
        acc(tris);

        const TriEx* exp = ex.empty() ? nullptr : ex.data();
        BLASHandle h = blas.register_triangles(tris.data(), (int)tris.size(), exp);
        uint32_t idx = UINT32_MAX;
        const auto& entries = blas.get_entries();
        for (size_t k = 0; k < entries.size(); ++k)
            if (entries[k]->handle == h) { idx = (uint32_t)k; break; }
        if (idx == UINT32_MAX) { res.error = "flatten: BLAS registration failed"; return res; }

        part_asset::LodLevel lvl;
        lvl.screen_size_threshold =
            (i + 1 < n) ? thr0 / (float)(1u << i) : 0.0f;
        lvl.blas_indices.push_back(idx);
        lods.push_back(std::move(lvl));
        res.coarsest_tris = tris.size();
    }

    part_asset::FlatCluster fc;
    for (int a = 0; a < 3; ++a) { fc.aabb_min[a] = mn[a]; fc.aabb_max[a] = mx[a]; }
    fc.lods = std::move(lods);
    std::vector<part_asset::FlatCluster> clusters;
    clusters.push_back(std::move(fc));

    res.levels   = n;
    res.clusters = 1;

    const std::string out_path = cache_root + "/" + part_asset::cache_path_flat(root_hash);
    // Budget-variant ladder has no instance boundaries: each variant is a
    // complete standalone mesh at its own tri budget.
    if (!part_asset::save_flat_v3(out_path, blas, tlas, clusters, root_hash)) {
        res.error = "flatten: save_flat_v3 failed for " + out_path;
        return res;
    }
    res.ok = true;
    return res;
}

// Bake-hardening #2: bottom-up traversal that decides FlattenDecision for
// every part reachable from `root_hash`. Cheap: it only calls part_asset::load_v2
// once per unique hash (via `Gatherer::load_public`, whose PartGeo cache
// is shared with the subsequent gather pass). Fills `decisions` and returns
// each part's estimated triangle count under the current decision map so the
// caller can budget-check the ROOT itself. Returns 0 on load failure and sets
// `err`.
//
// est_tri(P) = own_tris(P) + Σ over children c: (decision(c) == BOUNDARY ? 0 : est_tri(c))
//
// A part is marked BOUNDARY iff est_tri(P) * sizeof(TriEx) > budget_tri_bytes.
// Leaves are always INLINE (est_tri = own_tris; if own_tris alone busts the
// budget the leaf still can't be split further — it just stays INLINE and
// the merge tries; that case is the classic hand-authored composite).
static size_t decide_bottomup(uint64_t hash, int depth,
                              Gatherer& scratch,
                              const FlattenTargets& targets,
                              std::unordered_map<uint64_t, FlattenDecision>& decisions,
                              std::unordered_map<uint64_t, size_t>& est_tris,
                              std::string& err) {
    if (depth > targets.max_depth) return 0;  // silently cap, like the gather walk
    auto it_est = est_tris.find(hash);
    if (it_est != est_tris.end()) return it_est->second;

    const PartGeo* geo = scratch.load_public(hash, err);
    if (!geo) {
        // A missing/unreadable part means we can't reason about its size; the
        // outer gather will surface the same error. Cache 0 so we don't retry.
        est_tris[hash] = 0;
        return 0;
    }

    size_t est = geo->tris.size();
    for (const auto& c : geo->children) {
        size_t child_est =
            decide_bottomup(c.child_resolved_hash, depth + 1, scratch, targets,
                            decisions, est_tris, err);
        auto it_dec = decisions.find(c.child_resolved_hash);
        FlattenDecision cd =
            (it_dec == decisions.end()) ? FlattenDecision::INLINE : it_dec->second;
        // Only INLINE children contribute their expanded size to the parent's
        // estimate — BOUNDARY children are replaced by a single FlatInstanceRef.
        if (cd == FlattenDecision::INLINE) est += child_est;
    }

    // Decision: does inlining this whole subtree fit in the budget?
    // Leaves (empty children) always end up INLINE regardless of size — a
    // single leaf's own_tris blowing the budget is a hand-authored problem
    // task #3 (streaming flatten) addresses, not this task.
    FlattenDecision d = FlattenDecision::INLINE;
    if (!geo->children.empty() &&
        est * sizeof(TriEx) > targets.budget_tri_bytes) {
        d = FlattenDecision::BOUNDARY;
    }
    decisions[hash] = d;
    est_tris[hash] = est;
    return est;
}

static FlattenResult flatten_part_impl(const std::string& cache_root,
                                       uint64_t root_hash,
                                       const FlattenTargets& targets) {
    FlattenResult res;

    // Bake-hardening #3: peak-RSS instrumentation. Reports the delta between
    // baseline (entry to flatten_part_impl) and peak (just before save) so the
    // streaming refactor can be compared before/after. Opt-in via
    // MATTER_FLATTEN_PEAK=1 to keep normal test output quiet.
    const bool log_peak = peak_logging_enabled();
    const size_t rss_entry = log_peak ? peak_rss_bytes() : 0;

    Gatherer g(cache_root, targets);

    // Bake-hardening #2: bottom-up decision pass. Estimates each part's
    // post-inline triangle count and picks INLINE vs BOUNDARY per part. Runs
    // through the same Gatherer instance so the loaded PartGeo cache is
    // reused for the subsequent gather pass (no double disk I/O).
    std::unordered_map<uint64_t, FlattenDecision> decisions;
    std::unordered_map<uint64_t, size_t> est_tris;
    (void)decide_bottomup(root_hash, 0, g, targets, decisions, est_tris,
                          res.error);
    if (!res.error.empty()) {
        // decide_bottomup already logged the offending part; surface the same
        // error the old top-down gather would have surfaced.
        return res;
    }
    g.set_decisions(&decisions, root_hash);

    // Bake-hardening #3: the procedural budget-ladder path (Grass variants and
    // friends) needs the full materialized root mesh to feed
    // flatten_budget_ladder(), so detect the sidecar BEFORE picking a gather
    // strategy. When it exists we fall back to the classic full-gather path;
    // this is only used for LEAF parts with a variant sidecar, so their peak
    // is already bounded by the leaf's own tri count (small).
    const std::string sidecar_path =
        cache_root + "/" + part_asset::cache_path_lods(root_hash);
    part_asset::LodVariants variants;
    const bool has_variants = part_asset::load_lod_sidecar(sidecar_path, variants);

    if (has_variants) {
        if (!g.gather(root_hash, kIdentity, 0, res.error)) return res;
        std::vector<Tri>&   full_full   = g.tris();
        std::vector<TriEx>& full_fullex = g.triex();
        std::vector<part_asset::FlatInstanceRef>& refs_full = g.instance_refs();
        if (full_full.empty() && refs_full.empty()) {
            res.error = "flatten: merged mesh is empty"; return res;
        }
        res.full_tris = full_full.size();
        // Whole-mesh AABB & radius (same as before).
        float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
        for (const auto& t : full_full) {
            const float3* vs[3] = {&t.vertex0, &t.vertex1, &t.vertex2};
            for (int k = 0; k < 3; ++k) {
                mn[0]=std::fmin(mn[0],vs[k]->x); mx[0]=std::fmax(mx[0],vs[k]->x);
                mn[1]=std::fmin(mn[1],vs[k]->y); mx[1]=std::fmax(mx[1],vs[k]->y);
                mn[2]=std::fmin(mn[2],vs[k]->z); mx[2]=std::fmax(mx[2],vs[k]->z);
            }
        }
        const float dx = full_full.empty() ? 0.0f : mx[0]-mn[0];
        const float dy = full_full.empty() ? 0.0f : mx[1]-mn[1];
        const float dz = full_full.empty() ? 0.0f : mx[2]-mn[2];
        const float radius_full = 0.5f * std::sqrt(dx*dx+dy*dy+dz*dz);
        return flatten_budget_ladder(cache_root, root_hash, targets, g,
                                     radius_full, variants);
    }

    // Streaming path: Pass 1 gathers per-triangle centroids + tickets (~28
    // bytes/tri) instead of the whole merged mesh (~160 bytes/tri), computes
    // the merged vertex AABB inline, and then materializes triangles one
    // cluster at a time in Pass 2 (see the cluster loop below). The full
    // Tri+TriEx buffer never lives in memory at once — only the current
    // cluster's slice plus the growing BLAS.
    float world_aabb_min[3] = { std::numeric_limits<float>::max(),
                                std::numeric_limits<float>::max(),
                                std::numeric_limits<float>::max() };
    float world_aabb_max[3] = { -std::numeric_limits<float>::max(),
                                -std::numeric_limits<float>::max(),
                                -std::numeric_limits<float>::max() };
    if (!g.skeleton_gather(root_hash, kIdentity, 0, res.error,
                           world_aabb_min, world_aabb_max))
        return res;

    std::vector<part_asset::FlatInstanceRef>& refs = g.instance_refs();
    std::vector<float3>& centroids = g.centroids();
    std::vector<Ticket>& tickets   = g.tickets();
    if (centroids.empty() && refs.empty()) {
        res.error = "flatten: merged mesh is empty"; return res;
    }
    res.full_tris = centroids.size();

    // Global radius drives per-level epsilon; using the whole-mesh diagonal
    // keeps the ladder consistent across clusters (a small cluster's local
    // radius would give a very different ladder spacing than a large one).
    const float dx = centroids.empty() ? 0.0f : world_aabb_max[0] - world_aabb_min[0];
    const float dy = centroids.empty() ? 0.0f : world_aabb_max[1] - world_aabb_min[1];
    const float dz = centroids.empty() ? 0.0f : world_aabb_max[2] - world_aabb_min[2];
    const float radius = 0.5f * std::sqrt(dx*dx+dy*dy+dz*dz);

    // Split into clusters via the centroid-only variant — deterministic and
    // byte-identical to feeding a materialized Tri array into
    // split_clusters(), because both consume the same centroids in the same
    // order with the same tie-break rule.
    std::vector<uint32_t> order;
    auto clusters = part_cluster::split_centroids(centroids, order,
                                                  targets.cluster_target_tris);
    const size_t n_clusters = clusters.size();

    // Centroids are no longer needed once the permutation is fixed. Drop them
    // to shrink Pass-2 peak by ~12 bytes/tri. `tickets` and the interned
    // `contexts_`/`geos_` must stay alive because materialize_range() reads
    // them per triangle.
    std::vector<float3>().swap(centroids);

    // Shared BLAS/TLAS for the flat artifact (all clusters share one BLAS table).
    BLASManager blas;
    TLASManager tlas(4);   // no internal instances in a flat artifact

    std::vector<part_asset::FlatCluster> flat_clusters;
    flat_clusters.reserve(n_clusters);

    size_t max_levels = 0;
    size_t coarsest_tris = 0;

    // Helper: find the BLAS entry index for a freshly-registered handle. Used
    // per LOD level to fill each cluster's LodLevel.blas_indices.
    auto find_blas_idx = [&](BLASHandle h) -> uint32_t {
        const auto& entries = blas.get_entries();
        for (size_t k = 0; k < entries.size(); ++k)
            if (entries[k]->handle == h) return static_cast<uint32_t>(k);
        return UINT32_MAX;
    };

    // Bake-hardening #3: streaming per-cluster ladder build. The previous
    // implementation kept `levels[i].tris/triex` in a std::vector until the whole
    // cluster ladder was finished, so per-cluster peak included the LOD0 buffer
    // AS WELL AS a duplicate `ctris`/`ctriex` copy. Now we register each level's
    // triangles into BLAS as soon as it's decimated and drop the local buffer
    // immediately after (BLASManager already makes its own copy). Level 0 is
    // registered directly out of `ctris` — no extra copy. We only remember each
    // level's eps + blas index so per-level screen thresholds can be filled once
    // the next level's eps is known. Byte-identical output vs. the old path:
    // same BLAS registration order, same thresholds, same cluster order.
    struct LevelMeta { float eps; uint32_t blas_idx; };

    // Register a decimated level's triangles (or the raw cluster for level 0).
    // Returns false and sets res.error on BLAS registration failure.
    auto register_level = [&](const std::vector<Tri>& tris,
                              const std::vector<TriEx>& triex,
                              float eps,
                              std::vector<LevelMeta>& metas) -> bool {
        const TriEx* ex_ptr = triex.empty() ? nullptr : triex.data();
        BLASHandle h = blas.register_triangles(const_cast<Tri*>(tris.data()),
                                               static_cast<int>(tris.size()),
                                               ex_ptr);
        uint32_t idx = find_blas_idx(h);
        if (idx == UINT32_MAX) { res.error = "flatten: BLAS registration failed"; return false; }
        metas.push_back({eps, idx});
        return true;
    };

    for (const auto& cl : clusters) {
        // Bake-hardening #3: materialize this cluster's triangles from the
        // skeleton (tickets + centroids permutation). Fills ctris/ctriex in
        // permuted order and returns the per-cluster vertex AABB alongside —
        // saves a second pass over the vertices that the classic path did in
        // split_clusters(). Byte-identical to feeding a materialized Tri array
        // into split_clusters(): the DFS ordering of skeleton_gather() plus
        // the deterministic tie-break in split_centroids() produce the same
        // (child_hash, local_tri_idx, transform) sequence per cluster.
        std::vector<Tri>   ctris;
        std::vector<TriEx> ctriex;
        float cl_min[3], cl_max[3];
        g.materialize_range(order, cl.first_tri, cl.first_tri + cl.tri_count,
                            ctris, ctriex, cl_min, cl_max);

        // Build per-cluster LOD ladder. Level 0 (the raw cluster) is registered
        // FIRST — same order the old implementation used, which is what makes
        // the final .flat.part byte-identical across the refactor.
        std::vector<LevelMeta> level_metas;
        if (!register_level(ctris, ctriex, /*eps=*/0.0f, level_metas)) return res;

        size_t prev_count = ctris.size();
        size_t last_kept_count = ctris.size();
        for (float div : targets.radius_divisor) {
            // Use global radius so epsilon is consistent across clusters.
            const float eps = radius / div;
            // use_aabb_bounds=false: only topological boundary lock (open edges =
            // cluster-cut seam edges) freezes cut vertices; face-plane locking
            // would over-freeze cluster interiors that touch the cluster AABB.
            std::vector<Tri> geo = lod_bake::decimate_to_error(ctris, eps, /*use_aabb_bounds=*/false);
            if (geo.empty() || geo.size() >= prev_count) continue;  // no progress
            std::vector<TriEx> ex = lod_bake::reproject_triex(geo, ctris, ctriex);
            if (!register_level(geo, ex, eps, level_metas)) return res;
            prev_count = geo.size();
            last_kept_count = geo.size();
            // geo, ex go out of scope at the bottom of the loop body — freed
            // immediately after BLASManager makes its internal copy. This is
            // the streaming win: only one decimated level buffer is alive at a
            // time instead of the whole ladder in `levels`.
            if (prev_count <= (size_t)targets.min_level_tris) break;
        }

        // ctris/ctriex are no longer needed for this cluster; drop them before
        // moving to the next cluster's slice so their storage returns to the
        // allocator (empty std::vector destructor releases the buffer).
        std::vector<Tri>().swap(ctris);
        std::vector<TriEx>().swap(ctriex);

        // Fill in per-level screen_size_thresholds from the recorded metas.
        // Preserves the original semantics: level i's threshold is based on
        // level (i+1)'s eps; the coarsest level's threshold is 0.
        part_asset::LodLevels lods;
        lods.reserve(level_metas.size());
        for (size_t i = 0; i < level_metas.size(); ++i) {
            float thr = 0.0f;
            if (i + 1 < level_metas.size()) {
                const float next_eps = level_metas[i + 1].eps;
                thr = radius * targets.pixel_budget * targets.pixel_angle / next_eps;
            }
            part_asset::LodLevel lvl;
            lvl.screen_size_threshold = thr;
            lvl.blas_indices.push_back(level_metas[i].blas_idx);
            lods.push_back(std::move(lvl));
        }

        if (level_metas.size() > max_levels) max_levels = level_metas.size();
        coarsest_tris = last_kept_count;

        part_asset::FlatCluster fc;
        // Use the cluster-vertex AABB we computed while materializing; this
        // matches what split_clusters() would have written after its
        // permutation-then-recompute pass.
        fc.aabb_min[0] = cl_min[0]; fc.aabb_min[1] = cl_min[1]; fc.aabb_min[2] = cl_min[2];
        fc.aabb_max[0] = cl_max[0]; fc.aabb_max[1] = cl_max[1]; fc.aabb_max[2] = cl_max[2];
        fc.lods = std::move(lods);
        flat_clusters.push_back(std::move(fc));
    }

    res.levels        = max_levels;
    res.clusters      = n_clusters;
    res.coarsest_tris = coarsest_tris;
    res.instance_refs = refs.size();

    // Bake-hardening #3: release the skeleton state BEFORE serializing.
    // Tickets/order/geos/contexts are done once every cluster is materialized;
    // keeping them alive through save_flat_v3 needlessly widens the peak.
    // (The classic path used to hold `full`/`fullex` here — the streaming
    // path never had those alive in the first place, but the ticket buffer
    // and the permutation still occupy ~20 bytes/tri.)
    std::vector<Ticket>().swap(tickets);
    std::vector<uint32_t>().swap(order);

    const size_t rss_peak = log_peak ? peak_rss_bytes() : 0;

    const std::string out_path = cache_root + "/" + part_asset::cache_path_flat(root_hash);
    if (!part_asset::save_flat_v3(out_path, blas, tlas, flat_clusters, refs,
                                  root_hash)) {
        res.error = "flatten: save_flat_v3 failed for " + out_path;
        return res;
    }

    if (log_peak) {
        // Deltas in MB — big enough to be meaningful, small enough to skim.
        const double mb = 1.0 / (1024.0 * 1024.0);
        const size_t rss_exit = peak_rss_bytes();
        std::fprintf(stderr,
            "[flatten peak] root=%016llx tris=%zu clusters=%zu refs=%zu "
            "rss_entry=%.1fMB rss_pre_save=%.1fMB rss_exit=%.1fMB "
            "delta=%.1fMB\n",
            (unsigned long long)root_hash, res.full_tris, n_clusters, refs.size(),
            (double)rss_entry * mb, (double)rss_peak * mb, (double)rss_exit * mb,
            (double)(rss_exit - rss_entry) * mb);
    }

    res.ok = true;
    return res;
}

// Public entry point: outer boundary that converts a std::bad_alloc thrown by
// the merge/decimate/cluster pipeline (which routinely inflates scatter-style
// content 100-1000x during Gatherer::gather; StressForest*.js can peak at
// tens of GB) into a structured error string instead of aborting the viewer.
// The catch handler intentionally makes NO new heap allocations beyond a
// small stack buffer + snprintf into res.error's SSO / already-reserved
// storage; std::string assignment of a short literal may still allocate, but
// this is a best-effort catch (a fully-exhausted allocator can still fail
// during the message construction).
FlattenResult flatten_part(const std::string& cache_root, uint64_t root_hash,
                           const FlattenTargets& targets) {
    try {
        return flatten_part_impl(cache_root, root_hash, targets);
    } catch (const std::bad_alloc& e) {
        FlattenResult res;
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "OOM in part_flatten (root=%016llx, phase=flatten_part): %s",
                      (unsigned long long)root_hash, e.what());
        res.error = buf;
        res.ok = false;
        return res;
    }
}

} // namespace part_flatten
