// world_tracer.cpp — GL-free CPU tracer over the placed world (raycast query API).
// See world_tracer.h for public API.
#include "world_tracer.h"

#include "part_asset_v2.h"        // load_v2, cache_path_flat, cache_path_resolved, ChildInstance, LodLevels
#include "blas_manager.hpp"       // BLASManager, BLASEntry
#include "tlas_manager.hpp"       // TLASManager (required by load_v2 signature)
#include "bvh.h"                  // BVHRay, Intersection, Tri, TriEx
#include "material_registry.h"    // MaterialRegistryGet

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

// Shared row-major math helpers (mul16, NormalMat).
#include "mat_math.h"

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Full 4x4 row-major matrix inversion via the adjugate method.
// Returns false and writes identity if |det| < 1e-12.
static bool invert4x4(const float* m, float* out) {
    // Cofactor expansion — standard adjugate/det method.
    // We compute all 16 cofactors then divide by det.
    // Indexing: m[row*4 + col], same for out.
    float c[16];
    // Cofactor C_ij = (-1)^(i+j) * M_ij  (M_ij = 3x3 minor)
    // We label the original matrix rows r0..r3, cols c0..c3.
    // Precompute 2x2 sub-determinants of the bottom 3 rows to save work:
    const float
        a00=m[0],  a01=m[1],  a02=m[2],  a03=m[3],
        a10=m[4],  a11=m[5],  a12=m[6],  a13=m[7],
        a20=m[8],  a21=m[9],  a22=m[10], a23=m[11],
        a30=m[12], a31=m[13], a32=m[14], a33=m[15];

    // 3×3 minors of m[0..15] — we need them for each cofactor row.
    // Minor for (0,0): submatrix removing row0, col0
    auto minor3 = [&](int r0,int r1,int r2,int c0,int c1,int c2) -> float {
        const float* rows[4] = { m, m+4, m+8, m+12 };
        return rows[r0][c0]*(rows[r1][c1]*rows[r2][c2]-rows[r1][c2]*rows[r2][c1])
              -rows[r0][c1]*(rows[r1][c0]*rows[r2][c2]-rows[r1][c2]*rows[r2][c0])
              +rows[r0][c2]*(rows[r1][c0]*rows[r2][c1]-rows[r1][c1]*rows[r2][c0]);
    };
    (void)a00; (void)a01; (void)a02; (void)a03;
    (void)a10; (void)a11; (void)a12; (void)a13;
    (void)a20; (void)a21; (void)a22; (void)a23;
    (void)a30; (void)a31; (void)a32; (void)a33;

    // Cofactors (transposed adjugate). out[col*4 + row] = sign * minor.
    // We want inv = adjugate^T / det, where adjugate[i][j] = cofactor[j][i].
    // So out[i*4+j] = cofactor[j][i] / det  — i.e., transpose of cofactor matrix.
    c[0]  =  minor3(1,2,3, 1,2,3);
    c[1]  = -minor3(1,2,3, 0,2,3);
    c[2]  =  minor3(1,2,3, 0,1,3);
    c[3]  = -minor3(1,2,3, 0,1,2);

    c[4]  = -minor3(0,2,3, 1,2,3);
    c[5]  =  minor3(0,2,3, 0,2,3);
    c[6]  = -minor3(0,2,3, 0,1,3);
    c[7]  =  minor3(0,2,3, 0,1,2);

    c[8]  =  minor3(0,1,3, 1,2,3);
    c[9]  = -minor3(0,1,3, 0,2,3);
    c[10] =  minor3(0,1,3, 0,1,3);
    c[11] = -minor3(0,1,3, 0,1,2);

    c[12] = -minor3(0,1,2, 1,2,3);
    c[13] =  minor3(0,1,2, 0,2,3);
    c[14] = -minor3(0,1,2, 0,1,3);
    c[15] =  minor3(0,1,2, 0,1,2);

    // det = expansion along row 0
    float det = m[0]*c[0] + m[1]*c[1] + m[2]*c[2] + m[3]*c[3];
    if (std::fabs(det) < 1e-12f) {
        // Singular — write identity and warn
        std::memset(out, 0, 64);
        out[0] = out[5] = out[10] = out[15] = 1.f;
        return false;
    }
    float id = 1.f / det;
    // The adjugate is the TRANSPOSE of the cofactor matrix.
    // cofactor matrix c[i*4+j] → adjugate[j*4+i].
    // inv = adjugate / det.
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i*4+j] = c[j*4+i] * id;
    return true;
}

// NormalMat (inverse-transpose upper-3×3) is now in mat_math.h.
// NormalMat was a local copy; callers now use NormalMat directly.

// Transform a point by a row-major 4×4 (w=1, no divide).
static void xform_point(const float* m, const float p[3], float out[3]) {
    out[0] = m[0]*p[0] + m[1]*p[1] + m[2]*p[2]  + m[3];
    out[1] = m[4]*p[0] + m[5]*p[1] + m[6]*p[2]  + m[7];
    out[2] = m[8]*p[0] + m[9]*p[1] + m[10]*p[2] + m[11];
}

// Transform a vector (direction) by the upper 3×3 of a row-major 4×4.
static void xform_vec(const float* m, const float v[3], float out[3]) {
    out[0] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2];
    out[1] = m[4]*v[0] + m[5]*v[1] + m[6]*v[2];
    out[2] = m[8]*v[0] + m[9]*v[1] + m[10]*v[2];
}

// AABB slab test. Returns true if ray hits [bmin,bmax] with t_hit < best_t.
static bool aabb_hit(const float bmin[3], const float bmax[3],
                     const float O[3],    const float rD[3],
                     float best_t) {
    float tmin = 0.f, tmax = best_t;
    for (int i = 0; i < 3; ++i) {
        float t0 = (bmin[i] - O[i]) * rD[i];
        float t1 = (bmax[i] - O[i]) * rD[i];
        if (t0 > t1) { float tmp=t0; t0=t1; t1=tmp; }
        tmin = std::fmax(tmin, t0);
        tmax = std::fmin(tmax, t1);
        if (tmin > tmax) return false;
    }
    return tmin < best_t;
}

// ---------------------------------------------------------------------------
// Per-hash loaded geometry
// ---------------------------------------------------------------------------

struct BLASSlice {
    // Pointer into the owning BLASManager's entry list.
    const BLASManager::BLASEntry* entry = nullptr;
};

struct LoadedTracePart {
    // Owning managers — kept alive for the tracer's lifetime.
    std::unique_ptr<BLASManager> blas_mgr;
    std::unique_ptr<TLASManager> tlas_mgr;
    // Entries to trace (pointers into blas_mgr).
    std::vector<BLASSlice> slices;
    // Children from the compositional file (empty if loaded from flat artifact).
    std::vector<part_asset::ChildInstance> children;
    // True when loaded from flat artifact (children will be empty and not expanded).
    // False when loaded from compositional .part (children, if present, are expanded).
    bool loaded_flat = false;
    // Local AABB of the part (union of all traced entry triangles).
    float local_mn[3], local_mx[3];
    bool ok = false;
};

// ---------------------------------------------------------------------------
// Instance BVH node (int32 children, leaf when count > 0)
// ---------------------------------------------------------------------------

struct IBVHNode {
    float bmin[3], bmax[3];
    int left  = 0;   // index of left child  (internal only)
    int right = 0;   // index of right child (internal only)
    int first = 0;   // first instance index (leaf only)
    int count = 0;   // 0 = internal node, >0 = leaf
};

// ---------------------------------------------------------------------------
// Expanded instance record
// ---------------------------------------------------------------------------

struct ExpandedInst {
    const LoadedTracePart* part = nullptr;
    uint64_t part_hash = 0;          // hash of the part (for expanded_instance())
    float transform[16];   // row-major world placement
    float inv[16];         // inverse of transform
    NormalMat* nm = nullptr; // inverse-transpose 3×3 for normals (owned by pool)
    float world_mn[3], world_mx[3];  // world AABB
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// WorldTracer::Impl
// ---------------------------------------------------------------------------

namespace world_tracer {

struct WorldTracer::Impl {
    // Part cache (by resolved hash)
    std::unordered_map<uint64_t, std::unique_ptr<LoadedTracePart>> parts_;
    // NormalMat pool — deque so push_back never invalidates existing element pointers.
    std::deque<NormalMat> nm_pool_;
    // Expanded instances
    std::vector<ExpandedInst> expanded_;
    // Instance BVH nodes
    std::vector<IBVHNode> ibvh_;
    // World bounds (union of all instance world AABBs)
    float world_mn_[3] = { 1e30f,  1e30f,  1e30f};
    float world_mx_[3] = {-1e30f, -1e30f, -1e30f};
    bool built_ = false;
    std::string cache_root_;
    std::string scratch_dir_;

    // ---- Loading ----

    LoadedTracePart* load_part(const std::string& cache_root,
                               uint64_t hash, std::string& err) {
        auto it = parts_.find(hash);
        if (it != parts_.end()) return it->second->ok ? it->second.get() : nullptr;

        auto ltp = std::make_unique<LoadedTracePart>();
        ltp->blas_mgr = std::make_unique<BLASManager>();
        ltp->tlas_mgr = std::make_unique<TLASManager>(65536);

        std::vector<part_asset::ChildInstance> children;
        part_asset::LodLevels lods;

        // Try flat artifact first; fall back to compositional.
        bool loaded = false;
        auto file_exists = [](const std::string& p) {
            struct stat st; return ::stat(p.c_str(), &st) == 0;
        };
        std::string flat_path = cache_root + "/" + part_asset::cache_path_flat(hash);
        std::string comp_path = cache_root + "/" + part_asset::cache_path_resolved(hash);
        if (!scratch_dir_.empty()) {
            const std::string sf = scratch_dir_ + "/" + part_asset::cache_path_flat(hash);
            const std::string sc = scratch_dir_ + "/" + part_asset::cache_path_resolved(hash);
            if (file_exists(sf)) flat_path = sf;
            if (file_exists(sc)) comp_path = sc;
        }

        if (part_asset::load_v2(flat_path, hash,
                                *ltp->blas_mgr, *ltp->tlas_mgr,
                                children, lods)) {
            loaded = true;
            ltp->loaded_flat = true;
        } else if (part_asset::load_v2(comp_path, hash,
                                       *ltp->blas_mgr, *ltp->tlas_mgr,
                                       children, lods)) {
            loaded = true;
            ltp->loaded_flat = false;
        }
        if (!loaded) {
            err = "world_tracer: load_v2 failed for hash "
                  + std::to_string(hash) + " (tried flat + compositional)";
            ltp->ok = false;
            parts_.emplace(hash, std::move(ltp));
            return nullptr;
        }

        // Select entries to trace: coarsest LOD level's blas_indices (or all).
        const auto& entries = ltp->blas_mgr->get_entries();
        if (!lods.empty()) {
            const auto& coarsest = lods.back();
            for (uint32_t bi : coarsest.blas_indices) {
                if (bi < (uint32_t)entries.size()) {
                    BLASSlice s;
                    s.entry = entries[bi].get();
                    ltp->slices.push_back(s);
                }
            }
        }
        if (ltp->slices.empty()) {
            // Fallback: all entries
            for (const auto& e : entries) {
                BLASSlice s;
                s.entry = e.get();
                ltp->slices.push_back(s);
            }
        }

        // Store children for compositional expansion by expand_instance()
        ltp->children = std::move(children);

        // Compute local AABB
        ltp->local_mn[0] = ltp->local_mn[1] = ltp->local_mn[2] =  1e30f;
        ltp->local_mx[0] = ltp->local_mx[1] = ltp->local_mx[2] = -1e30f;
        for (const BLASSlice& s : ltp->slices) {
            for (const Tri& t : s.entry->triangles) {
                const float3* vs[3] = {&t.vertex0, &t.vertex1, &t.vertex2};
                for (const float3* v : vs) {
                    ltp->local_mn[0] = std::fmin(ltp->local_mn[0], v->x);
                    ltp->local_mn[1] = std::fmin(ltp->local_mn[1], v->y);
                    ltp->local_mn[2] = std::fmin(ltp->local_mn[2], v->z);
                    ltp->local_mx[0] = std::fmax(ltp->local_mx[0], v->x);
                    ltp->local_mx[1] = std::fmax(ltp->local_mx[1], v->y);
                    ltp->local_mx[2] = std::fmax(ltp->local_mx[2], v->z);
                }
            }
        }

        // If the part loaded but has no slices/triangles, make a valid degenerate:
        // keep local AABB as empty but mark ok so we don't re-attempt.
        ltp->ok = true;
        LoadedTracePart* raw = ltp.get();
        parts_.emplace(hash, std::move(ltp));
        return raw;
    }

    // ---- Instance expansion (recursive, depth-capped) ----

    void expand_instance(const std::string& cache_root,
                         uint64_t hash, const float* world_xf,
                         int depth, std::string& err) {
        if (depth > 8) return;

        LoadedTracePart* part = load_part(cache_root, hash, err);
        if (!part) return;

        // If the part was loaded from a compositional .part and has children,
        // expand each child as an additional pending instance with
        // transform = world_xf × child_transform (row-major multiply).
        // This is the fix for the gap: the flat artifact is preferred and
        // has merged geometry (no children), but a compositional .part only
        // has its own BLAS entries + a child table. We must recurse into the
        // children to trace the full subtree.
        if (!part->loaded_flat && !part->children.empty()) {
            // Expand each child recursively.
            for (const part_asset::ChildInstance& ci : part->children) {
                float combined[16];
                mul16(world_xf, ci.transform, combined);
                expand_instance(cache_root, ci.child_resolved_hash,
                                combined, depth + 1, err);
                if (!err.empty()) {
                    std::fprintf(stderr,
                        "world_tracer: warning expanding child 0x%llx of 0x%llx: %s\n",
                        (unsigned long long)ci.child_resolved_hash,
                        (unsigned long long)hash, err.c_str());
                    err.clear();
                }
            }
            // Also emit the parent's own geometry if it has any slices,
            // so that parts which have both their own BLAS entries AND children
            // (mixed compositional) are fully traced.
            if (part->slices.empty()) return;
            // Fall through to emit own geometry below.
        }

        // Emit this part's own geometry as a leaf ExpandedInst.
        if (part->slices.empty()) return; // nothing to trace

        nm_pool_.emplace_back(world_xf);
        ExpandedInst ei;
        ei.part = part;
        ei.part_hash = hash;
        std::memcpy(ei.transform, world_xf, 64);
        if (!invert4x4(world_xf, ei.inv)) {
            std::fprintf(stderr, "world_tracer: near-singular transform for hash %llu\n",
                         (unsigned long long)hash);
        }
        ei.nm = &nm_pool_.back();

        // World AABB: transform 8 corners of the local AABB
        const float* mn = part->local_mn;
        const float* mx = part->local_mx;
        ei.world_mn[0] = ei.world_mn[1] = ei.world_mn[2] =  1e30f;
        ei.world_mx[0] = ei.world_mx[1] = ei.world_mx[2] = -1e30f;
        for (int xi=0;xi<2;++xi) for (int yi=0;yi<2;++yi) for (int zi=0;zi<2;++zi) {
            float lp[3] = { xi?mx[0]:mn[0], yi?mx[1]:mn[1], zi?mx[2]:mn[2] };
            float wp[3];
            xform_point(world_xf, lp, wp);
            for (int k=0;k<3;++k) {
                ei.world_mn[k] = std::fmin(ei.world_mn[k], wp[k]);
                ei.world_mx[k] = std::fmax(ei.world_mx[k], wp[k]);
            }
        }

        // Update global bounds
        for (int k=0;k<3;++k) {
            world_mn_[k] = std::fmin(world_mn_[k], ei.world_mn[k]);
            world_mx_[k] = std::fmax(world_mx_[k], ei.world_mx[k]);
        }

        expanded_.push_back(ei);
    }

    // ---- Instance BVH construction (median-split, leaf ≤ 4) ----

    int build_ibvh(std::vector<int>& indices, int first, int count) {
        int node_idx = (int)ibvh_.size();
        ibvh_.push_back(IBVHNode{});
        IBVHNode& node = ibvh_[node_idx];

        // Compute union AABB
        node.bmin[0]=node.bmin[1]=node.bmin[2]= 1e30f;
        node.bmax[0]=node.bmax[1]=node.bmax[2]=-1e30f;
        for (int i=first;i<first+count;++i) {
            const ExpandedInst& ei = expanded_[indices[i]];
            for (int k=0;k<3;++k) {
                node.bmin[k] = std::fmin(node.bmin[k], ei.world_mn[k]);
                node.bmax[k] = std::fmax(node.bmax[k], ei.world_mx[k]);
            }
        }

        if (count <= 4) {
            node.first = first;
            node.count = count;
            return node_idx;
        }

        // Find longest axis of centroid range
        float cmn[3] = {1e30f,1e30f,1e30f}, cmx[3] = {-1e30f,-1e30f,-1e30f};
        for (int i=first;i<first+count;++i) {
            const ExpandedInst& ei = expanded_[indices[i]];
            for (int k=0;k<3;++k) {
                float c = (ei.world_mn[k]+ei.world_mx[k])*0.5f;
                cmn[k]=std::fmin(cmn[k],c); cmx[k]=std::fmax(cmx[k],c);
            }
        }
        int axis = 0;
        float best = cmx[0]-cmn[0];
        for (int k=1;k<3;++k) { float d=cmx[k]-cmn[k]; if(d>best){best=d;axis=k;} }
        float mid = (cmn[axis]+cmx[axis])*0.5f;

        // Partition around median
        int split = first;
        for (int i=first;i<first+count;++i) {
            const ExpandedInst& ei = expanded_[indices[i]];
            float c = (ei.world_mn[axis]+ei.world_mx[axis])*0.5f;
            if (c < mid) { std::swap(indices[i], indices[split]); ++split; }
        }
        if (split == first || split == first+count) split = first + count/2;

        int left_count  = split - first;
        int right_count = count - left_count;

        int left_child  = build_ibvh(indices, first, left_count);
        // After recursion, ibvh_ may have grown; re-fetch node ref below.
        int right_child = build_ibvh(indices, first + left_count, right_count);

        // Fix node (may have been invalidated by push_back):
        ibvh_[node_idx].left  = left_child;
        ibvh_[node_idx].right = right_child;
        ibvh_[node_idx].count = 0;
        ibvh_[node_idx].first = 0;

        return node_idx;
    }

    // ---- Ray vs one instance ----

    bool intersect_instance(const ExpandedInst& ei,
                            const float worldO[3], const float worldD[3],
                            float& best_t,
                            float best_normal[3], int& best_mat) const {
        // Transform to local space WITHOUT normalizing direction — preserves world t.
        float localO[3], localD[3];
        xform_point(ei.inv, worldO, localO);
        xform_vec(ei.inv, worldD, localD);

        float rD[3];
        rD[0] = (std::fabs(localD[0]) > 1e-30f) ? 1.f/localD[0] : 1e30f;
        rD[1] = (std::fabs(localD[1]) > 1e-30f) ? 1.f/localD[1] : 1e30f;
        rD[2] = (std::fabs(localD[2]) > 1e-30f) ? 1.f/localD[2] : 1e30f;

        bool improved = false;
        for (const BLASSlice& s : ei.part->slices) {
            if (!s.entry || !s.entry->bvh) continue;

            BVHRay ray;
            ray.O.x = localO[0]; ray.O.y = localO[1]; ray.O.z = localO[2];
            ray.D.x = localD[0]; ray.D.y = localD[1]; ray.D.z = localD[2];
            ray.rD.x = rD[0];   ray.rD.y = rD[1];   ray.rD.z = rD[2];
            ray.hit.t = best_t;
            ray.hit.u = ray.hit.v = 0.f;
            ray.hit.instPrim = 0xFFFFFFFFu;

            float t_before = ray.hit.t;
            s.entry->bvh->Intersect(ray, 0);

            if (ray.hit.t < t_before - 1e-7f) {
                // Determine tri index (low 20 bits of instPrim)
                uint32_t tri_idx = ray.hit.instPrim & 0xFFFFFu;

                // Only commit the hit (and update normal/material) when tri_idx
                // is valid. If it is out of range the BVH returned a bogus hit;
                // keep the previous best rather than advancing best_t with stale
                // normal/material (B7: stale-shading commit fix).
                if (tri_idx < (uint32_t)s.entry->triangles.size()) {
                    best_t = ray.hit.t;
                    improved = true;

                    const Tri& tri = s.entry->triangles[tri_idx];
                    // Local edge vectors
                    float e1[3] = { tri.vertex1.x-tri.vertex0.x,
                                    tri.vertex1.y-tri.vertex0.y,
                                    tri.vertex1.z-tri.vertex0.z };
                    float e2[3] = { tri.vertex2.x-tri.vertex0.x,
                                    tri.vertex2.y-tri.vertex0.y,
                                    tri.vertex2.z-tri.vertex0.z };
                    // Local geometric normal = cross(e1, e2)
                    float localN[3] = {
                        e1[1]*e2[2] - e1[2]*e2[1],
                        e1[2]*e2[0] - e1[0]*e2[2],
                        e1[0]*e2[1] - e1[1]*e2[0]
                    };
                    // Transform to world space using inverse-transpose
                    float worldN[3];
                    ei.nm->apply(localN, worldN);
                    // Flip to face ray origin (dot(n, worldD) < 0)
                    float dotn = worldN[0]*worldD[0]+worldN[1]*worldD[1]+worldN[2]*worldD[2];
                    if (dotn > 0.f) { worldN[0]=-worldN[0]; worldN[1]=-worldN[1]; worldN[2]=-worldN[2]; }
                    best_normal[0] = worldN[0];
                    best_normal[1] = worldN[1];
                    best_normal[2] = worldN[2];

                    // Material
                    if (!s.entry->tri_extra.empty() &&
                        tri_idx < (uint32_t)s.entry->tri_extra.size()) {
                        best_mat = s.entry->tri_extra[tri_idx].materialId % 1000000;
                    } else {
                        best_mat = -1;
                    }
                }
            }
        }
        return improved;
    }

    // ---- Recursive instance BVH traversal ----

    void traverse_ibvh(int node_idx,
                       const std::vector<int>& order,
                       const float worldO[3], const float worldD[3],
                       const float rD_world[3],
                       float& best_t,
                       float best_normal[3], int& best_mat,
                       int& best_inst) const {
        const IBVHNode& node = ibvh_[node_idx];
        if (!aabb_hit(node.bmin, node.bmax, worldO, rD_world, best_t)) return;

        if (node.count > 0) {
            // Leaf
            for (int i = node.first; i < node.first + node.count; ++i) {
                int inst_idx = order[i];
                const ExpandedInst& ei = expanded_[inst_idx];
                if (!aabb_hit(ei.world_mn, ei.world_mx, worldO, rD_world, best_t)) continue;
                if (intersect_instance(ei, worldO, worldD, best_t, best_normal, best_mat)) {
                    best_inst = inst_idx;
                }
            }
        } else {
            traverse_ibvh(node.left,  order, worldO, worldD, rD_world,
                          best_t, best_normal, best_mat, best_inst);
            traverse_ibvh(node.right, order, worldO, worldD, rD_world,
                          best_t, best_normal, best_mat, best_inst);
        }
    }

    std::vector<int> ibvh_order_; // instance index array used by the BVH builder
};

// ---------------------------------------------------------------------------
// WorldTracer public API
// ---------------------------------------------------------------------------

WorldTracer::WorldTracer() = default;
WorldTracer::~WorldTracer() = default;

void WorldTracer::set_scratch_dir(const std::string& dir) {
    scratch_dir_ = dir;
}

bool WorldTracer::build(const std::string& cache_root,
                        const std::vector<TraceInstance>& instances,
                        std::string& err) {
    impl_ = std::make_unique<Impl>();
    Impl& im = *impl_;
    im.cache_root_ = cache_root;
    im.scratch_dir_ = scratch_dir_;

    // nm_pool_ is a deque so pointers remain stable across push_back.
    im.expanded_.reserve(instances.size());

    for (const TraceInstance& ti : instances) {
        im.expand_instance(cache_root, ti.part_hash, ti.transform, 0, err);
        // Non-fatal: warn on missing parts but keep going.
        if (!err.empty()) {
            std::fprintf(stderr, "world_tracer: warning: %s\n", err.c_str());
            err.clear();
        }
    }

    // Handle empty case
    if (im.expanded_.empty()) {
        // world_bounds returns unit box at origin
        im.world_mn_[0] = im.world_mn_[1] = im.world_mn_[2] = -0.5f;
        im.world_mx_[0] = im.world_mx_[1] = im.world_mx_[2] =  0.5f;
        im.built_ = true;
        return true;
    }

    // Build instance BVH
    int N = (int)im.expanded_.size();
    im.ibvh_order_.resize(N);
    for (int i=0;i<N;++i) im.ibvh_order_[i] = i;
    im.ibvh_.reserve(N * 2 + 4);
    im.build_ibvh(im.ibvh_order_, 0, N);

    im.built_ = true;
    return true;
}

bool WorldTracer::trace(const float origin[3], const float dir[3],
                        float max_t, Hit& hit) const {
    if (!impl_ || !impl_->built_) return false;
    Impl& im = *impl_;

    if (im.expanded_.empty()) return false;

    float best_t = max_t;
    float best_normal[3] = {0,0,1};
    int   best_mat = -1;
    int   best_inst = -1;

    float rD_world[3];
    rD_world[0] = (std::fabs(dir[0]) > 1e-30f) ? 1.f/dir[0] : 1e30f;
    rD_world[1] = (std::fabs(dir[1]) > 1e-30f) ? 1.f/dir[1] : 1e30f;
    rD_world[2] = (std::fabs(dir[2]) > 1e-30f) ? 1.f/dir[2] : 1e30f;

    im.traverse_ibvh(0, im.ibvh_order_, origin, dir, rD_world,
                     best_t, best_normal, best_mat, best_inst);

    if (best_t >= max_t - 1e-7f) return false;

    hit.t = best_t;
    hit.normal[0] = best_normal[0];
    hit.normal[1] = best_normal[1];
    hit.normal[2] = best_normal[2];
    hit.material_id = best_mat;
    hit.instance = (best_inst >= 0) ? (uint32_t)best_inst : 0xffffffffu;

    if (best_mat >= 0) {
        const MaterialDef* mat = MaterialRegistryGet(best_mat);
        hit.emission = mat ? mat->emission : 0.f;
        if (mat) {
            hit.albedo[0] = mat->albedo[0];
            hit.albedo[1] = mat->albedo[1];
            hit.albedo[2] = mat->albedo[2];
        } else {
            hit.albedo[0] = hit.albedo[1] = hit.albedo[2] = 0.5f;
        }
    } else {
        hit.emission = 0.f;
        hit.albedo[0] = hit.albedo[1] = hit.albedo[2] = 0.5f;
    }
    return true;
}

bool WorldTracer::occluded(const float origin[3], const float dir[3],
                           float max_t) const {
    Hit hit;
    bool did_hit = trace(origin, dir, max_t, hit);
    // Self-hit guard for shadow rays originating on/near surfaces
    return did_hit && hit.t > 1e-4f && hit.t < max_t;
}

void WorldTracer::world_bounds(float mn[3], float mx[3]) const {
    if (!impl_ || !impl_->built_) {
        mn[0]=mn[1]=mn[2]=-0.5f; mx[0]=mx[1]=mx[2]=0.5f; return;
    }
    mn[0]=impl_->world_mn_[0]; mn[1]=impl_->world_mn_[1]; mn[2]=impl_->world_mn_[2];
    mx[0]=impl_->world_mx_[0]; mx[1]=impl_->world_mx_[1]; mx[2]=impl_->world_mx_[2];
}

size_t WorldTracer::instance_count() const {
    return impl_ ? impl_->expanded_.size() : 0;
}

size_t WorldTracer::expanded_instance_count() const {
    return impl_ ? impl_->expanded_.size() : 0;
}

bool WorldTracer::expanded_instance(size_t idx, uint64_t& part_hash,
                                    float transform[16]) const {
    if (!impl_ || idx >= impl_->expanded_.size()) return false;
    const ExpandedInst& ei = impl_->expanded_[idx];
    part_hash = ei.part_hash;
    std::memcpy(transform, ei.transform, 64);
    return true;
}

} // namespace world_tracer
