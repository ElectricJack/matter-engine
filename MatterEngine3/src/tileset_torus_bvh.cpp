// tileset_torus_bvh.cpp — see header for interface.
//
// CPU-only: builds BLAS/TLAS data structures ready for GPU upload. No upload
// call is made here — the GL upload path (ensure_gpu_textures_ready/
// bind_to_shader) was deleted outright in Phase 5a (tech-debt.md §6). The
// real Vulkan uploader (render/tileset_bake_vk.cpp) walks
// BLASManager::get_entries()/TLASManager::get_draw_records() and rebuilds
// unconditionally on every bake -- it does NOT consult content_revision().
// content_revision() (see those headers' doc comments) is a sound,
// landed incremental-rebuild signal, it is simply not wired up to any
// consumer yet (tech-debt.md §6).

#include "tileset_torus_bvh.h"

#include "tileset_bake.h"    // SettledTorus, SettledInstance, BakeInputs
#include "tileset_spec.h"    // TileConfig, BaseField
#include "tileset_layout.h"  // kTorusN
#include "part_asset_v2.h"   // part_asset::load_v2, cache_path_resolved
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "bvh.h"             // Tri, TriEx, float3

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace tileset {

// -----------------------------------------------------------------------------
// Base heightfield → BLAS.
// The torus is kTorusN * cfg.size meters on a side; the base field is one tile
// sampled kSamplesPerTile^2 times, repeated toroidally across the 4x4 grid.
// We tessellate the full torus into (kTorusN*n) x (kTorusN*n) quads.
// -----------------------------------------------------------------------------
static bool build_base_blas(const SettledTorus& st, BLASManager& blas,
                             BLASHandle& out_handle, std::string& err)
{
    const int n = st.base.n;
    if (n <= 0 || (int)st.base.heights.size() < n * n || !st.base.set) {
        err = "assemble_torus_bvh: base field empty / not set";
        return false;
    }
    const int total_n = kTorusN * n;   // sample count across the whole torus
    const float cell  = st.base.cell;  // meters between samples
    if (cell <= 0.0f) { err = "assemble_torus_bvh: base cell <= 0"; return false; }

    // Sample the periodic base: global column i, global row k → height.
    auto sample_y = [&](int gi, int gk) -> float {
        int i = ((gi % n) + n) % n;   // handle negative gi from central-diff below
        int k = ((gk % n) + n) % n;
        return st.base.heights[(size_t)k * n + i];
    };

    // Central-difference vertex normal at grid corner (gi, gk).  For a
    // heightfield y = h(x, z) with grid spacing `cell`:
    //   dy/dx = (h(i+1,k) - h(i-1,k)) / (2*cell)
    //   dy/dz = (h(i,k+1) - h(i,k-1)) / (2*cell)
    //   N = normalize((-dy/dx, 1, -dy/dz))
    // Periodic wrap on the sample grid is toroidal.  Populating real per-vertex
    // normals lets the AO / lighting path smooth-shade the base without ever
    // hitting the `normalize(vec3(0))` NaN that the earlier flatShading-forced
    // workaround was papering over.
    auto vertex_normal = [&](int gi, int gk) -> float3 {
        const float inv_2c = 0.5f / cell;
        const float dydx = (sample_y(gi + 1, gk) - sample_y(gi - 1, gk)) * inv_2c;
        const float dydz = (sample_y(gi, gk + 1) - sample_y(gi, gk - 1)) * inv_2c;
        float3 n{ -dydx, 1.0f, -dydz };
        const float len = std::sqrt(n.x*n.x + n.y*n.y + n.z*n.z);
        // len >= 1.0 because the y component is 1; safe divide.
        return float3{ n.x / len, n.y / len, n.z / len };
    };

    // Tessellate total_n*total_n quads (toroidal wrap) → 2 * total_n^2 triangles.
    std::vector<Tri>   tris;
    std::vector<TriEx> triex;
    tris.reserve((size_t)total_n * total_n * 2);
    triex.reserve((size_t)total_n * total_n * 2);

    for (int k = 0; k < total_n; ++k) {
        int k1 = (k + 1) % total_n;
        for (int i = 0; i < total_n; ++i) {
            int i1 = (i + 1) % total_n;

            // World x,z positions — on wrap edges we extend one cell beyond so
            // there is no discontinuity in the grid (the UV wraps in sample_y).
            float x0 = (float)i  * cell;
            float z0 = (float)k  * cell;
            float x1 = (float)(i + 1) * cell;
            float z1 = (float)(k + 1) * cell;

            float y00 = sample_y(i,  k);
            float y10 = sample_y(i1, k);
            float y01 = sample_y(i,  k1);
            float y11 = sample_y(i1, k1);

            // Per-corner smooth normals (central difference, toroidal wrap).
            float3 n00 = vertex_normal(i,  k);
            float3 n10 = vertex_normal(i1, k);
            float3 n01 = vertex_normal(i,  k1);
            float3 n11 = vertex_normal(i1, k1);

            Tri a{}, b{};
            a.vertex0 = float3{ x0, y00, z0 };
            a.vertex1 = float3{ x1, y10, z0 };
            a.vertex2 = float3{ x1, y11, z1 };
            a.centroid = float3{
                (x0 + x1 + x1) * (1.0f/3.0f),
                (y00 + y10 + y11) * (1.0f/3.0f),
                (z0 + z0 + z1) * (1.0f/3.0f)
            };

            b.vertex0 = float3{ x0, y00, z0 };
            b.vertex1 = float3{ x1, y11, z1 };
            b.vertex2 = float3{ x0, y01, z1 };
            b.centroid = float3{
                (x0 + x1 + x0) * (1.0f/3.0f),
                (y00 + y11 + y01) * (1.0f/3.0f),
                (z0 + z1 + z1) * (1.0f/3.0f)
            };

            tris.push_back(a);
            tris.push_back(b);

            TriEx exa{};
            exa.materialId = (int)st.base.material;
            exa.tint = float4{ 1.0f, 1.0f, 1.0f, 0.0f };
            exa.ao0 = exa.ao1 = exa.ao2 = 1.0f;
            exa.N0 = n00; exa.N1 = n10; exa.N2 = n11;
            TriEx exb{};
            exb.materialId = (int)st.base.material;
            exb.tint = float4{ 1.0f, 1.0f, 1.0f, 0.0f };
            exb.ao0 = exb.ao1 = exb.ao2 = 1.0f;
            exb.N0 = n00; exb.N1 = n11; exb.N2 = n01;
            triex.push_back(exa);
            triex.push_back(exb);
        }
    }

    if (tris.empty()) {
        err = "assemble_torus_bvh: base tessellation produced zero triangles";
        return false;
    }

    out_handle = blas.register_triangles(tris, triex);
    if (out_handle == INVALID_BLAS_HANDLE) {
        err = "assemble_torus_bvh: register_triangles returned INVALID_BLAS_HANDLE for base";
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Quaternion (qx, qy, qz, qw) + translation + uniform scale → mm::Mat4.
// TLASManager uses ROW-major layout: m[0..3]=row0, m[4..7]=row1, m[8..11]=row2.
// mat4::TransformPoint reads translation from cell[3], cell[7], cell[11], and
// mm::translation() sets m[3], m[7], m[11] the same way.
// Layout: [R00*s  R01*s  R02*s  px]
//         [R10*s  R11*s  R12*s  py]
//         [R20*s  R21*s  R22*s  pz]
//         [0      0      0      1 ]
// where the rotation rows use the standard right-handed quaternion formula.
// -----------------------------------------------------------------------------
static mm::Mat4 mat4_from_pose_scale(const Pose& p, float s)
{
    float qx = p.qx, qy = p.qy, qz = p.qz, qw = p.qw;
    float xx = qx*qx, yy = qy*qy, zz = qz*qz;
    float xy = qx*qy, xz = qx*qz, yz = qy*qz;
    float wx = qw*qx, wy = qw*qy, wz = qw*qz;

    mm::Mat4 M;
    // Row 0: [R00  R01  R02  tx]
    M.m[0] = s * (1.0f - 2.0f*(yy + zz));
    M.m[1] = s * (2.0f*(xy - wz));
    M.m[2] = s * (2.0f*(xz + wy));
    M.m[3] = p.px;
    // Row 1: [R10  R11  R12  ty]
    M.m[4] = s * (2.0f*(xy + wz));
    M.m[5] = s * (1.0f - 2.0f*(xx + zz));
    M.m[6] = s * (2.0f*(yz - wx));
    M.m[7] = p.py;
    // Row 2: [R20  R21  R22  tz]
    M.m[8]  = s * (2.0f*(xz - wy));
    M.m[9]  = s * (2.0f*(yz + wx));
    M.m[10] = s * (1.0f - 2.0f*(xx + yy));
    M.m[11] = p.pz;
    // Row 3: [0  0  0  1]
    M.m[12] = 0.0f;
    M.m[13] = 0.0f;
    M.m[14] = 0.0f;
    M.m[15] = 1.0f;
    return M;
}

// One drawable piece of a part: a BLAS registered into the shared manager plus
// the part-LOCAL transform it sits at inside that part.
struct PartPlacement {
    BLASHandle handle;
    mm::Mat4   local;
};

// -----------------------------------------------------------------------------
// Load one part via part_asset::load_v2 into a temporary BLASManager, register
// every BLAS entry the part actually draws into the shared manager, and return
// one PartPlacement per draw. Callers cache the result by child_hash.
//
// This used to take entries.front() alone, with the comment "the primary
// drawable geometry". A part has no such thing. The voxel path with no modifier
// stack calls register_triangles once PER CELL PER MATERIAL-MERGE-GROUP
// (script_host.cpp, cell_size = 1 m), so anything spanning more than one cell or
// group is many co-equal entries; only a modifier stack (which accumulates all
// cells into region_acc before meshing) or a direct-triangle part yields one.
// Measured on the shipping ForestFloor variants with
// tests/tileset_ao_normal_probe.cpp, which prints the per-entry split:
//
//   Pebble seed=0 size=2.0  8 entries, tris 3+11+105+131+1+3+...  -> traced 3 of 496
//   Pebble seed=3 size=2.0  4 entries, tris 84+134+78+164         -> traced 84 of 460
//   Twig   seed=0 size=4.0  8 entries, tris 14+16+14+16+8+18+...  -> traced 14 of 112
//   Rock   seed=0           1 entry (modifier stack)              -> unaffected
//   Leaf                    1 entry (direct fan triangles)        -> unaffected
//
// so the ortho primary pass and the AO pass were tracing a fragment of every
// pebble and twig: most of the litter's albedo/normal/height/AO never reached
// the atlas, while settle/placement reasoned about the part's full bounds.
//
// The placement transform comes from the part's OWN TLAS draw records rather
// than being assumed identity. part_asset_v2.cpp serializes the full record list
// (blas_index, material_id, 16-float transform) and publish_common_body replays
// it, so a record may carry a non-identity part-local transform and one entry may
// be drawn more than once. Reading the records keeps both cases right; assuming
// identity would silently misplace any part that used them.
// -----------------------------------------------------------------------------
static bool load_part_placements(const std::string& cache_dir, uint64_t child_hash,
                                 BLASManager& shared,
                                 std::vector<PartPlacement>& out,
                                 std::string& err)
{
    const std::string rel  = part_asset::cache_path_resolved(child_hash);
    const std::string path = cache_dir + "/" + rel;

    BLASManager tmp_blas;
    TLASManager tmp_tlas(16);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;

    if (!part_asset::load_v2(path, child_hash, tmp_blas, tmp_tlas, children, lods)) {
        std::ostringstream ss;
        ss << "assemble_torus_bvh: failed to load part 0x" << std::hex << child_hash
           << " from " << path;
        err = ss.str();
        return false;
    }

    const auto& entries = tmp_blas.get_entries();
    if (entries.empty()) {
        std::ostringstream ss;
        ss << "assemble_torus_bvh: part 0x" << std::hex << child_hash
           << " has no BLAS entries";
        err = ss.str();
        return false;
    }

    // register_prebuilt does not deduplicate, so map each part-local handle to
    // the shared handle and register on first use — a record list that draws one
    // entry twice must not upload its triangles twice.
    std::unordered_map<BLASHandle, BLASHandle> local_to_shared;
    auto adopt = [&](BLASHandle local, BLASHandle& shared_handle) -> bool {
        auto it = local_to_shared.find(local);
        if (it != local_to_shared.end()) { shared_handle = it->second; return true; }

        const BLASManager::BLASEntry* found = nullptr;
        for (const auto& ep : entries) {
            if (ep->handle == local) { found = ep.get(); break; }
        }
        if (!found || !found->bvh) {
            std::ostringstream ss;
            ss << "assemble_torus_bvh: part 0x" << std::hex << child_hash
               << " draws BLAS handle " << std::dec << local
               << " which has no entry with a BVH";
            err = ss.str();
            return false;
        }
        const BVH* b = found->bvh.get();
        shared_handle = shared.register_prebuilt(
            found->triangles.data(),
            found->tri_extra.empty() ? nullptr : found->tri_extra.data(),
            (int)found->triangles.size(),
            b->bvhNode, b->nodesUsed, b->triIdx,
            found->hash, /*ref_count*/ 1);
        if (shared_handle == INVALID_BLAS_HANDLE) {
            std::ostringstream ss;
            ss << "assemble_torus_bvh: register_prebuilt returned INVALID for part 0x"
               << std::hex << child_hash;
            err = ss.str();
            return false;
        }
        local_to_shared.emplace(local, shared_handle);
        return true;
    };

    const auto& recs = tmp_tlas.get_draw_records();
    if (!recs.empty()) {
        out.reserve(recs.size());
        for (const auto& r : recs) {
            BLASHandle h = INVALID_BLAS_HANDLE;
            if (!adopt(r.blas_handle, h)) return false;
            out.push_back(PartPlacement{h, r.transform});
        }
        return true;
    }

    // No serialized draw records (publish_common_body only replays them when the
    // file carried instances). Fall back to every entry at identity, which is
    // where the per-cell path puts them anyway — better than dropping the part.
    out.reserve(entries.size());
    for (const auto& ep : entries) {
        BLASHandle h = INVALID_BLAS_HANDLE;
        if (!adopt(ep->handle, h)) return false;
        out.push_back(PartPlacement{h, mm::identity()});
    }
    return true;
}

// -----------------------------------------------------------------------------
// Public entry point.
// -----------------------------------------------------------------------------
bool assemble_torus_bvh(const SettledTorus& settled, const BakeInputs& inputs,
                         BLASManager& blas, TLASManager& tlas, std::string& err)
{
    // 1. Build the base heightfield BLAS and add it as TLAS instance 0.
    BLASHandle base_handle = INVALID_BLAS_HANDLE;
    if (!build_base_blas(settled, blas, base_handle, err))
        return false;

    tlas.load_identity();
    tlas.draw(base_handle, (uint32_t)settled.base.material);

    // 2. Validate every pose and load each distinct part once. Loading is split
    //    out ahead of drawing because a part contributes as many TLAS instances
    //    as it has placements, and TLASManager::draw silently refuses past its
    //    ceiling (printf only) — so the total has to be known before the first
    //    draw rather than discovered by losing geometry.
    std::unordered_map<uint64_t, std::vector<PartPlacement>> hash_to_placements;

    for (size_t i = 0; i < settled.instances.size(); ++i) {
        const SettledInstance& si = settled.instances[i];

        // Quaternion normalization guard.
        float qmag = std::sqrt(si.pose.qx*si.pose.qx + si.pose.qy*si.pose.qy
                              + si.pose.qz*si.pose.qz + si.pose.qw*si.pose.qw);
        if (std::fabs(qmag - 1.0f) > 1e-3f) {
            std::ostringstream ss;
            ss << "assemble_torus_bvh: instance " << i
               << " (hash 0x" << std::hex << si.child_hash
               << ") has non-normalized quaternion |q|=" << qmag;
            err = ss.str();
            return false;
        }

        if (hash_to_placements.find(si.child_hash) == hash_to_placements.end()) {
            std::vector<PartPlacement> placements;
            if (!load_part_placements(inputs.parts_cache_dir, si.child_hash,
                                      blas, placements, err))
                return false;
            hash_to_placements.emplace(si.child_hash, std::move(placements));
        }
    }

    size_t total_instances = 1;  // the base heightfield drawn above
    for (const SettledInstance& si : settled.instances)
        total_instances += hash_to_placements[si.child_hash].size();
    tlas.ensure_instance_capacity((int)total_instances);

    // 3. Emit one TLAS instance per (settled instance x part placement). The
    //    placement transform is part-local and the settle transform is
    //    world-from-part, so the composite is world <- part <- placement:
    //    load_matrix(M) then multiply_matrix(local), which TLASManager evaluates
    //    as current = current * arg in its own row-major convention (no manual
    //    composition, so no chance of transposing it here).
    for (const SettledInstance& si : settled.instances) {
        const mm::Mat4 M = mat4_from_pose_scale(si.pose, si.scale);
        for (const PartPlacement& p : hash_to_placements[si.child_hash]) {
            tlas.push_matrix();
            tlas.load_matrix(M);
            tlas.multiply_matrix(p.local);
            // material 0 → per-triangle material from TriEx (pack_material_w
            // semantics). Deliberately NOT the part record's own material_id:
            // script_host stores a merge-GROUP id there, and this value is only
            // consulted as the fallback for triangles whose TriEx materialId is
            // negative.
            tlas.draw(p.handle, /*material*/ 0);
            tlas.pop_matrix();
        }
    }

    if (tlas.get_draw_record_count() != (int)total_instances) {
        std::ostringstream ss;
        ss << "assemble_torus_bvh: recorded " << tlas.get_draw_record_count()
           << " TLAS instances, expected " << total_instances
           << " — geometry was dropped";
        err = ss.str();
        return false;
    }

    // 4. Build the TLAS (CPU SAH BVH over all instances).
    tlas.build(blas);
    return true;
}

} // namespace tileset
