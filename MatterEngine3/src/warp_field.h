#pragma once

// Warped ground field (VT Phase 2) — per-sector ARAP parameterisation of the
// terrain surface, the runtime coordinate for live-generated ground content
// and its POM march.
//
// Design: docs/superpowers/specs/2026-08-01-decal-ground-compositing-design.md
// (§4, the warped ground field). One sentence: a per-sector, bake-time
// (u, v) parameterisation of the rung-0 surface mesh, stored per vertex,
// continuous across the whole sector by construction and across sector
// borders by a deterministic shared-chain pin rule — plus, at the CORNERS
// where two chains meet and no single chain owns the vertex, a shared-corner
// pin rule (the corner's own anchored world XZ, which each chain is then
// fitted to terminate at) — so a seam cannot exist;
// the price is smooth low-frequency stretch where the surface is
// intrinsically curved (§2.4 measures that kind of error as statistically
// invisible on stochastic content).
//
// The solve runs at sector STAGE time (part_store::stage_from_snapshot,
// beside the terrain ladder re-bake), deterministically: fixed iteration
// counts, fixed order, unthreaded per solve — identical input produces a
// byte-identical field. Sector parts are transient, so the field deploys on
// next stage with no version break and no content-key change (the live
// coordinate is not a page-content input).
//
// What this is NOT (§4.4): it does not address page storage (chart atlas UVs
// unchanged), does not change page content identity, does not exist on props,
// and does not feed placement grids. It is exactly one thing: the runtime
// coordinate for live ground content and its march.

#include "tri.h"  // Tri, float2/float3 (SpatialQueryLib)

#include <cstdint>
#include <vector>

namespace warp_field {

// ---------------------------------------------------------------------------
// Solve options. Defaults are the production configuration the §4.3 gates are
// measured against; tests use the same values so a gate pass means something.
// ---------------------------------------------------------------------------
struct SolveOptions {
    // World-space anchor added to sector-local XZ: (tx * sector_size,
    // tz * sector_size). Border pins are functions of WORLD chain coordinates
    // (anchor + local), which is what makes two neighbouring sectors compute
    // identical pins for their bitwise-shared border chain — and what anchors
    // the warp lattice to world space so the Wang pattern is continuous across
    // sector borders. Without a known anchor callers should not solve (the
    // field would be locally consistent but border-discontinuous).
    double anchor_x = 0.0;
    double anchor_z = 0.0;
    // Sector footprint in metres (border-band classification uses it).
    float sector_size = 64.0f;
    // Local voxel size: the mesher's border cell rows are one voxel wide, so
    // the border classification bands are [-voxel, ~0] and [S - voxel, S].
    float voxel = 2.0f;

    // Local/global ARAP iteration counts. Fixed (never adaptive) for
    // determinism and timing stability.
    int arap_iterations = 8;
    // Conjugate-gradient budgets. The INIT solve carries the large
    // low-frequency motion (an extreme wall must spread ~5x over a ~150-cell
    // graph diameter, which costs ~sqrt(kappa) ~ hundreds of CG iterations);
    // the per-ARAP global solves are warm-started and need far less. Both
    // are caps; the tolerance may stop earlier, which is still deterministic
    // for identical input.
    int init_cg_iterations = 800;
    int cg_iterations = 200;
    float cg_tolerance = 1e-6f;
    // Fold-relax post-pass cap (local Laplacian relaxation applied only to
    // folded triangles' free vertices; accepted only while the fold count
    // strictly decreases).
    int fold_relax_passes = 20;
};

// Per-welded-vertex output. gu/gv are the rows of the 3D->uv Jacobian
// averaged from incident triangles (area-weighted) — kept as full float3
// vectors here; the oct-tangent + f16-scale packing happens at evaluate()
// against the *render* vertex's own shading normal.
//
// EXCEPT on border-chain vertices, where the incident fan is one-sided and the
// average would be a one-sided difference that the neighbouring sector takes
// from the other side. Those get a frame derived from the shared chain
// instead, so the frame is border-continuous for the same reason the uv is —
// see apply_chain_frames() in warp_field.cpp. It matters because the frame is
// a shading BASIS downstream (gbuffer.frag), not just a march scale.
struct VertexField {
    float2 uv;        // world-anchored metres
    float3 gu, gv;    // d(u)/d(pos), d(v)/d(pos)
};

// Distortion metrics over the solved field, area-weighted over non-skirt,
// non-degenerate triangles. stretch = sigma1, compress = 1/sigma2,
// aniso = sigma1/sigma2 of the per-triangle 3D->uv map's singular values;
// delta_j = Frobenius norm of the difference between the two adjacent
// triangles' 2x3 Jacobians across each interior manifold edge (weighted by
// the pair's summed area). folds = triangles whose uv winding opposes their
// 3D winding.
struct FieldStats {
    uint32_t verts = 0, tris = 0, solved_tris = 0;
    uint32_t folds = 0;
    float stretch_p50 = 0.f, stretch_p95 = 0.f, stretch_max = 0.f;
    float compress_p50 = 0.f, compress_p95 = 0.f;
    float aniso_p50 = 0.f, aniso_p95 = 0.f;
    float dj_p50 = 0.f, dj_p95 = 0.f;
    double solve_ms = 0.0;
};

// A solved field: the welded solve mesh, per-vertex uv + Jacobian rows, and a
// uniform-grid triangle index for evaluate(). Positions are SECTOR-LOCAL
// (exactly the bitwise Tri corner values); uv is world-anchored.
struct Field {
    bool valid = false;
    std::vector<float3> positions;   // welded, first-appearance order
    std::vector<uint32_t> indices;   // 3 per triangle (non-skirt, non-degenerate)
    std::vector<VertexField> verts;  // parallel to positions
    FieldStats stats;

    // Uniform XZ grid over the solve mesh for nearest-triangle queries.
    float grid_min_x = 0.f, grid_min_z = 0.f;
    float grid_cell = 4.0f;
    int grid_nx = 0, grid_nz = 0;
    std::vector<uint32_t> grid_offsets;  // grid_nx*grid_nz + 1
    std::vector<uint32_t> grid_tris;     // triangle ids
};

// Solve the field for one sector mesh (triangle soup, sector-local coords).
// skirt_mask (optional, parallel to tris): 1 = skirt triangle, excluded from
// the solve domain (legacy pre-2026-07-30 artifacts only; new sectors have no
// skirts). Returns false (out.valid == false) when the mesh has no usable
// terrain triangles — callers fail soft to the unwarped path.
// Deterministic: identical input (bytes) => byte-identical Field.
bool solve(const Tri* tris, size_t tri_count, const uint8_t* skirt_mask,
           const SolveOptions& opts, Field& out);

// Compute the metrics of the *identity* world-XZ map (uv := anchored world
// XZ) over the same mesh — the shipped-addressing baseline the warp field is
// measured against. Same weighting and definitions as Field::stats.
FieldStats world_xz_stats(const Tri* tris, size_t tri_count,
                          const uint8_t* skirt_mask, const SolveOptions& opts);

// Evaluate the field at `count` render vertices (positions: 3 floats each,
// sector-local — the same space the solve ran in; normals: 3 floats each,
// the vertex SHADING normal the fragment shader will interpolate).
//
// Writes per vertex:
//   out_uv[2i+0..1]  = warp uv (world-anchored metres, f32)
//   out_frame[2i+0]  = octahedral-encoded tangent T (2 x f16 packed)
//   out_frame[2i+1]  = (su, sv) as 2 x f16: grad_u = su * T,
//                      grad_v = sv * (N x T) in the shader's frozen frame
//
// Vertices whose position welds exactly onto a solve vertex (bitwise) take
// its value directly (every rung-0 vertex does); everything else — coarser
// ladder rungs, skirt verts — takes the nearest solve triangle's barycentric
// interpolation. An empty/invalid field writes uv = 0 and su = 0, which the
// shader reads as "no warp" (fail-soft to world-XZ addressing).
void evaluate(const Field& field, const float* positions, const float* normals,
              size_t count, float* out_uv, uint32_t* out_frame);

// Border-chain pin computation, exposed for the §4.3 shared-border gate: the
// pins for every side-classified border chain of the mesh, as (welded vertex
// index, pinned uv) pairs in deterministic order. Both sectors of a
// neighbouring pair must produce identical uv values for the shared chain.
struct BorderPin {
    uint32_t vertex;
    float2 uv;
};
// Extracts the welded mesh + pins without running the solve (cheap).
bool border_pins(const Tri* tris, size_t tri_count, const uint8_t* skirt_mask,
                 const SolveOptions& opts, std::vector<float3>& out_positions,
                 std::vector<BorderPin>& out_pins);

// Process-wide monotonic census (same discipline as lod_bake::ladder_census):
// always accumulating, readers take deltas. Rendered by [stream.warp] beside
// [stream.ladder]; see matter_engine.cpp.
struct WarpCensus {
    uint64_t sectors = 0;      // solves attempted
    uint64_t solved = 0;       // solves that produced a valid field
    uint64_t solve_us = 0;     // warp_field::solve time
    uint64_t evaluate_us = 0;  // evaluate() over all rung meshes
    uint64_t tris = 0;         // solve-domain triangles
    uint64_t folds = 0;        // folded triangles left after fold-relax
};
WarpCensus warp_census();
void warp_census_add_evaluate_us(uint64_t us);

}  // namespace warp_field
