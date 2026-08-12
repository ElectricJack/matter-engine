#pragma once
// seam_weld.h — the runtime seam welder: build the crossing band on one shared
// plane from the two ACTUAL drawn tiles' exported boundary records.
//
// Design: docs/volumetric-sectors-design-2026-08-10.md §4.1 (M0-WP3).
//
// WHY THIS IS PURE GEOMETRY. Nothing here knows what a sector, a part, a bake or
// a frame is. It takes two `seam::WeldSide` lookups and a tangential region and
// emits a triangle soup. That is deliberate on two counts:
//
//   1. It is testable in isolation against synthetic `FaceRecord`s (see
//      tests/seam_weld_tests.cpp) — the whole point of taking seams out of the
//      bake was to stop their correctness depending on streaming state.
//   2. **Corners and edges are not special cases.** A plane edge near a tile's
//      tangential border has cells belonging to a DIAGONAL neighbour tile. The
//      welder never resolves those itself; it asks the caller's closure. The
//      engine, which sees the whole sector map, answers. That is what makes
//      tile-corner and tile-edge adjacency "just more weld cases, baked
//      nowhere" (§4.1) instead of the 2^k patch explosion that killed the
//      previous baked-variant revision.
//
// So: no matter_engine.h, no raylib, no Vulkan, no flecs, no QuickJS. Standard
// library plus seam_boundary.h. Keep it that way.
//
// ---------------------------------------------------------------------------
// The algorithm — dual contouring's faceProc, at one level of difference
// ---------------------------------------------------------------------------
//
// Two tiles share a plane perpendicular to one axis. The plane carries a lattice
// of tangential points `(A, B)` (axes named by `seam::face_tangent_axes`, which
// both sides agree on without negotiating). A **plane edge** runs from `(A,B)` to
// `(A+1,B)` (along `a`) or from `(A,B)` to `(A,B+1)` (along `b`). Such an edge is
// shared by exactly four cells — two on each side of the plane:
//
//     a-edge at (A,B):  cells (A, B-1) and (A, B),  one per side.
//     b-edge at (A,B):  cells (A-1, B) and (A, B),  one per side.
//
// If the densities at the edge's two endpoints have different signs, the surface
// crosses it, and the seam quad is those four cells' surface-nets vertices in
// cyclic order. **Enumeration happens at the FINE resolution.** On the fine side
// the cells are looked up directly; on the coarse side at `floor_div2` of the
// fine tangential indices — and the two coarse cells frequently collapse to the
// same cell, in which case the quad degenerates to a triangle. *That collapse is
// the 2:1 fan.* There is no separate fan code path, and no code that knows the
// word "corner".
//
// Endpoint signs come from the FINE side's `corner_signs`: the coarse side has no
// sample at odd fine lattice points, so it cannot be asked. Any cell adjacent to
// a crossing edge necessarily has a sign change and therefore appears in the
// sparse record, so 4 bits per entry replace a dense (n+1)^2 sign image. Two
// cells sharing an endpoint must agree about its sign; disagreement is a bug and
// is counted in `WeldStats::sign_conflicts` rather than silently resolved.
//
// **Honest residue (§4.1), and the collapsed cap.** §4.1 offers two responses to
// a crossing with no landing site — "emits a collapsed cap or drops the sliver".
// The first revision of this file dropped in every case, and M0-WP6's integration
// scans showed why that was too blunt: the residue was not sub-fine-voxel as §4.1
// predicted, it was a full ~1-voxel column (7-14 m at L2/3 and L3/4), i.e. the
// same width as the strip this whole design exists to remove, merely confined to
// one column instead of running the seam's length. So the two cases are now split
// by whether anything is left to land ON:
//
//   ONE coarse corner missing -> CAP. Collapse the quad onto the coarse corner
//     that IS present: emit a triangle from the two fine vertices plus that one.
//     This synthesizes nothing — the surviving corner is a record vertex — and it
//     is not a new code path: it is the same degeneration the 2:1 fan already
//     performs when floor_div2 maps two fine cells onto one coarse cell, reached
//     by absence instead of by index aliasing. Counted in `tris` (it emits one
//     triangle, like any other collapse) and broken out in `caps`.
//
//   BOTH coarse corners missing -> still dropped, counted in
//     `missing_coarse_pair`. This means the surface enters and leaves within a
//     single coarse cell without flipping any of its corner signs — a feature
//     genuinely smaller than a coarse voxel. There is no coarse vertex anywhere
//     to cap onto. It is not closable HERE at all, and the reason is worth being
//     precise about, because the obvious next move is wrong: a boundary-loop fan
//     among the FINE vertices would close the FINE mesh's opening and leave the
//     identical void, since the hole is on the coarse side of the plane and the
//     coarse tile has no surface anywhere near it. M0-WP6 measured the damage —
//     four heightfield scans, one gapped row of 31 each, 0.75-0.88 fine voxels.
//     So it stays dropped and counted here, and is closed one layer out by the
//     OVERLAP BAND (`WeldSide::band`, below).
//
// ---------------------------------------------------------------------------
// The overlap band (M0-WP7) — the second layer, and why it is not in the mesh
// ---------------------------------------------------------------------------
//
// The fan above is a STITCH: every position it writes is one of the two tiles'
// own vertices, so where it fires it is exact. The band is an OVERLAP: the fine
// tile's surface, extended one coarse voxel back past its -x/-z border, drawn on
// top of the coarse tile's. Where the coarse side has no vertex the stitch has
// nothing to work with and the overlap is all there is; where it does, the two
// coexist. They close different failures and neither subsumes the other, which
// is why this file EMITS BOTH rather than choosing (see `weld_face`).
//
// The band cannot simply live in the tile's mesh, and that constraint is the
// whole reason it arrives here. At an EQUAL-level seam the two tiles already
// meet bitwise through shared samples, and the extra geometry would be duplicate
// coplanar surface — the exact thing the retired `edge_mask` gate existed to
// prevent, and the reason that gate (and with it a neighbour guess in the bake
// identity) survived as long as it did. Deciding it HERE costs nothing: the
// welder is handed the pair actually on screen, so "is this seam cross-level"
// stops being a guess and becomes a fact. Sample unconditionally at bake time,
// decide at draw time — the same move that took the seam out of the bake, one
// level deeper.
//
// `WeldSide::band` is null by default and `weld_face` then behaves exactly as it
// did before this existed. When it is set, the FINE side's band is emitted iff
// the pair is cross-level, which is enforced structurally: the equal-rung early
// return below precedes all emission. The caller is responsible for setting it
// only on the face being welded — the mesher populates bands on kFaceNegX and
// kFaceNegZ only, so there is never a band on the +side face of a plane anyway.
//
//   A FINE corner missing -> dropped, counted in `missing_fine`. Not a residue at
//     all: it contradicts the sparse-signs argument in seam_boundary.h unless the
//     cell simply belongs to a tile the caller's lookup does not cover, in which
//     case the fix is the lookup, not the welder. Capping here would be wrong —
//     it would stretch geometry across ground a tile is about to occupy.
//
// The welder never invents a vertex in any of these branches: every position it
// writes is one of the two records' own vertices. Do not chase watertightness
// past `missing_coarse_pair`.
//
// **The cap's winding needs no new argument.** The cyclic order of the four
// corners is fixed by the four CELLS and the endpoint signs — not by which
// vertices happen to exist. Substituting the present coarse corner for the
// absent one leaves that sequence in place and merely duplicates one entry, so
// the polygon degenerates from a quad to a triangle with no reordering, and the
// survivor is the quad's orientation restricted to a sub-polygon. The
// substitution therefore happens BEFORE the `flip` swap (which is indexed by
// cell position and would otherwise act on the wrong slot), after which the cap
// is bit-for-bit the aliasing path. The suite cross-checks capped triangles
// against the averaged vertex normals anyway.
//
// ---------------------------------------------------------------------------
// Winding, derived (not guessed)
// ---------------------------------------------------------------------------
//
// terrain_mesher.cpp's `emit_quad` (:398-462) is the authority, because the weld
// band has to match the tiles' own triangles. Reading its three call sites:
//
//   For a lattice edge along axis E at lattice point L, the mesher orders the
//   four cells (Lu-1,Lv-1), (Lu,Lv-1), (Lu,Lv), (Lu-1,Lv) over the two other
//   axes (u,v) taken in INCREASING AXIS ORDER, triangulates (0,1,2)+(0,2,3), and
//   flips (swaps 1 and 3) when
//       E=0 (x): a <= 0     E=1 (y): a > 0      E=2 (z): a <= 0
//   where `a` is the density at L, the edge's LOW endpoint along E.
//
// The y case looks like an exception and is not. Take (u,v) instead as
// `u = (E+1)%3, v = (E+2)%3` — always a cyclic rotation of (x,y,z), hence a
// RIGHT-handed frame (u,v,E). In that frame the cell order above runs
// counterclockwise seen from +E, so its right-hand normal points along +E. The
// mesher's increasing-axis-order choice equals this frame for E=0 and E=2 and is
// its TRANSPOSE for E=1 — one orientation reversal, exactly matching the one
// inverted flip. So the single rule underneath all three is:
//
//     Wind the quad so its normal points from SOLID to AIR along E.
//     Density > 0 is solid, so: normal along +E iff the low-E endpoint is solid.
//
// Applied to a seam plane with normal axis N and tangential axes (a,b): for an
// a-edge, E = a_axis; for a b-edge, E = b_axis. Working out `u = (E+1)%3,
// v = (E+2)%3` for all six (N, edge-direction) combinations gives, with the
// welder's own base order [(T-1,neg), (T,neg), (T,pos), (T-1,pos)] where T is the
// OTHER tangential index and neg/pos are the two sides of the plane:
//
//     a-edge:  base order is right-handed  <=>  N != 1
//     b-edge:  base order is right-handed  <=>  N == 1
//
// (The disagreeing cases are the same four cells in the transposed frame, which
// is the base order reversed up to a cyclic rotation — i.e. exactly one flip.)
// Hence
//     reverse_frame = (a-edge ? N == 1 : N != 1)
//     flip          = reverse_frame XOR (low endpoint is air)
// which is what `weld_face` computes. `seam_weld_tests.cpp` cross-checks the
// result against the averaged vertex normals on all three axes and both
// fine-side choices, because a derivation that is only on paper is a guess.

#include "seam_boundary.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace seam {

// One side of a shared plane: everything the welder needs to resolve a cell to a
// vertex, without knowing how many tiles are behind it.
//
// `at` is called with GLOBAL tangential cell indices at THIS side's own rung and
// returns the vertex that cell produced, or null if it produced none / is not
// resident / is not covered by any drawn tile. The normal-axis cell layer is
// implicit: the side already knows whether it is the plane's negative or positive
// neighbour (`FaceRecord::cell_layer`), so the welder never names it.
//
// The engine backs this with a lookup over its whole sector map, which is how a
// plane edge whose cells straddle a tile-tile border gets answered without the
// welder having any notion of tiles. `side_from_record` below is the trivial
// one-tile case, used by tests and by callers that have already narrowed it down.
//
// CONTRACT — `at` must return STABLE, CANONICAL pointers:
//   * stable: the pointee must outlive the `weld_face` call. Returning a pointer
//     into a temporary built inside the closure is a use-after-free.
//   * canonical: two calls naming the SAME cell must return the SAME pointer.
// The second half is load-bearing, not hygiene. The 2:1 collapse — the thing that
// turns a quad into the fan's triangle — is detected by comparing the four
// resolved pointers for equality, because two fine cells sharing one coarse cell
// resolve through `floor_div2` to one lookup. A closure that copied its result
// into a per-call buffer would hand back distinct pointers for one cell, the
// collapse would go undetected, and every fan would emit a zero-area triangle
// instead of degenerating cleanly. `FaceRecord::find` satisfies both halves by
// returning `&verts[i]` into the record's own storage; an engine-side lookup that
// merges several tiles must do likewise (index into the records, never into a
// merged temporary).
struct WeldSide {
    int rung = 0;   // finer = larger, per rung_voxel()
    std::function<const BoundaryVert*(int64_t a, int64_t b)> at;

    // This side's overlap band for the face being welded, or null (M0-WP7).
    // Emitted verbatim by `weld_face` when this side is the FINE one and the
    // pair is cross-level; ignored entirely otherwise. Null keeps `weld_face`
    // bit-for-bit what it was before the band existed, so a caller that has not
    // been taught about bands is not silently changed by their arrival.
    //
    // Must outlive the `weld_face` call, like `at`'s pointees. Unlike `at`, this
    // is a single tile's geometry rather than a lookup over the map: a band is
    // finished triangles covering one tile's own border strip, so a side that
    // spans several tiles carries the band of the tile whose region is being
    // welded, and the per-tile passes partition the plane between them.
    const OverlapBand* band = nullptr;
};

// Per-face weld accounting. `missing_landing` is the §4.1 residue counter the M0
// acceptance criteria are written against (§6): it must be reported, not hidden.
//
// FOUR top-level buckets partition `crossings`: quads, tris, missing_landing,
// degenerate. `caps`, `missing_coarse_pair` and `missing_fine` are BREAKDOWNS of
// two of those, not buckets of their own — adding them to the sum double-counts.
// The partition is kept exactly four wide on purpose: seam_integration_tests.cpp
// asserts `crossings == quads + tris + missing + degen` across every weld it
// performs, and a residue counter whose arithmetic has to be re-learned each time
// it gains a case is a counter nobody will trust.
struct WeldStats {
    int crossings       = 0;  // plane edges found with a sign change
    int quads           = 0;  // crossings that emitted two triangles
    int tris            = 0;  // crossings that emitted one
    int missing_landing = 0;  // crossings that emitted nothing for want of a vertex
    int sign_conflicts  = 0;  // two cells disagreed about a shared endpoint's sign
    int degenerate      = 0;  // four corners resolved but < 3 distinct -> nothing emitted

    // Breakdown of `tris`: how many of them were reached by ABSENCE of one coarse
    // corner (the collapsed cap) rather than by floor_div2 index aliasing.
    // caps <= tris.
    int caps = 0;

    // Breakdown of `missing_landing`; these two sum to it exactly.
    int missing_coarse_pair = 0;  // both coarse corners absent -> sub-coarse-voxel
                                  // feature; what the overlap band covers instead
    int missing_fine        = 0;  // a fine corner absent -> the lookup's problem

    // Triangles copied verbatim from the fine side's overlap band (M0-WP7).
    // DELIBERATELY OUTSIDE the four-way partition: a band triangle is not a
    // crossing and answers to no plane edge, so folding it into `tris` would
    // break `crossings == accounted()` for no gain. Reported separately.
    int band_tris = 0;

    // crossings == quads + tris + missing_landing + degenerate, always.
    // (`band_tris` is not part of this sum -- see above.)
    int accounted() const { return quads + tris + missing_landing + degenerate; }
};

// Triangle soup, deliberately the same layout convention as
// terrain_mesher::MaterialBucket so the engine can push a weld through the same
// path as a tile mesh: 9 floats of position and 9 of normal per triangle.
struct WeldBucket {
    uint32_t material = 0;
    std::vector<float> positions;  // 9 floats per triangle (3 verts x xyz)
    std::vector<float> normals;    // 9 floats per triangle
};

// Positions are emitted relative to `origin_*`, which the caller sets before
// calling (default 0 = world-absolute). Boundary records carry world `double`
// positions precisely so the welder can rebase once, here, instead of inheriting
// the tile-local float cancellation that bites at 10 km out; a weld spans two
// tiles with two different origins, so it needs its own.
// Per-triangle PROVENANCE, for diagnosis only (null in production).
//
// The winding gate (seam_integration [8]) can see that a triangle came out
// reversed and nothing about WHY. Every hypothesis about the y-normal residue
// -- the frame table, caps, the 2:1 collapse -- had to be tested by perturbing
// the welder and re-running, which answers "is this the cause" but never "what
// distinguishes the failures". These four bits make the failures GROUPABLE, so
// a correlation is one run rather than one run per guess.
//
// Deliberately a side-channel rather than a field on WeldBucket: production
// never allocates it, and the geometry arrays stay exactly what they were.
struct WeldProvenance {
    static constexpr uint8_t kAlongA = 1u << 0;  // edge runs along the a axis
    static constexpr uint8_t kSolidLo = 1u << 1; // s0 == 1 (solid at the low end)
    static constexpr uint8_t kCapped = 1u << 2;  // one coarse corner duplicated
    static constexpr uint8_t kFlip   = 1u << 3;  // q1/q3 were swapped
    static constexpr uint8_t kSecond = 1u << 4;  // the (0,2,3) half of the quad
    // One entry per emitted FAN triangle. Band triangles are not recorded --
    // they are the mesher's own output, copied verbatim, with no fan decisions
    // behind them.
    //
    // ADDRESSED BY (material, index within that material's bucket), not by
    // emission order. Emission interleaves materials while a reader walks the
    // mesh bucket-major, so an emission-ordered vector would line up only for a
    // single-material weld -- and would then silently mis-attribute on exactly
    // the multi-material seams most worth inspecting.
    std::vector<uint8_t>  flags;
    std::vector<uint32_t> material;
    std::vector<uint32_t> index_in_bucket;

    void add(uint8_t f, uint32_t mat, size_t idx) {
        flags.push_back(f);
        material.push_back(mat);
        index_in_bucket.push_back(uint32_t(idx));
    }
};

struct WeldMesh {
    double origin_x = 0, origin_y = 0, origin_z = 0;
    std::vector<WeldBucket> buckets;
    // Optional and unowned. Set by tests; nothing in the engine writes it.
    WeldProvenance* provenance = nullptr;

    size_t triangle_count() const {
        size_t n = 0;
        for (const WeldBucket& b : buckets) n += b.positions.size() / 9;
        return n;
    }
    // Empty the geometry but KEEP every bucket's storage. `weld_face` clears the
    // caller's mesh on entry, and the engine reuses one scratch mesh across the
    // whole weld pool -- a `buckets.clear()` here would drop the inner vectors'
    // capacity and re-heap-allocate positions/normals on every call, once per
    // anchor run per face pair per publish. The outer vector keeps its buckets
    // (a handful of materials, reused across welds); only the float arrays are
    // rewound. `triangle_count()` reads `positions.size()`, so an emptied bucket
    // correctly counts zero and a stale material tag on it is harmless --
    // `bucket_for` matches on material and would have found it anyway.
    void clear_geometry() {
        for (WeldBucket& b : buckets) { b.positions.clear(); b.normals.clear(); }
    }
};

// Emit the crossing band on one shared plane.
//
//   face_axis            : the plane's normal axis, 0=x 1=y 2=z (seam::face_axis()).
//   neg / pos            : the sides at normal-axis cell layers below and above
//                          the plane. Their rungs may differ by at most one.
//   region_a0..region_b1 : half-open bounds, in FINE-rung tangential indices, on
//                          the lattice points (A,B) at which edges are ANCHORED.
//                          Both edges anchored at (A,B) are emitted, so the cells
//                          actually touched run one lower: a in [a0-1, a1),
//                          b in [b0-1, b1). Cell index and its min-corner lattice
//                          index are the same integer, so this is equally "the
//                          fine cells to enumerate"; the distinction only matters
//                          when the caller is dividing a plane between passes and
//                          must not double-emit the boundary edges.
//
// `out` is cleared of geometry first (its origin is preserved). Returns false
// with `err` set only on a configuration error: a bad axis, a missing lookup, or
// a rung difference above one. A rung gap of 2+ is REJECTED rather than
// approximated — `restrict_levels` guarantees +/-1 across faces and the fan
// implements exactly one level (§4.4); silently welding a 4:1 face would hide the
// invariant breaking.
//
// Equal rungs emit nothing and return true -- NOT the fan and NOT the band.
// Those pairs already meet bitwise through shared density samples (§4, the
// [1..n] ownership rule), so a fan over them would be redundant and a band over
// them would be duplicate coplanar surface. This early return is the whole of
// the equal-level suppression rule, and it is deliberately the FIRST thing that
// happens after the arguments are validated so that no emission path can be
// reached without passing it.
bool weld_face(int face_axis,
               const WeldSide& neg, const WeldSide& pos,
               int64_t region_a0, int64_t region_a1,
               int64_t region_b0, int64_t region_b1,
               WeldMesh& out, WeldStats& stats, std::string& err);

// Convenience: a WeldSide backed by exactly one FaceRecord. `rec` must outlive
// the returned side. The engine's real sides span several tiles; this is for
// tests and for the already-narrowed single-tile case.
WeldSide side_from_record(int rung, const FaceRecord& rec);

} // namespace seam
