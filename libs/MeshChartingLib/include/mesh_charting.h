#pragma once
#include <vector>

// Reusable mesh-charting / UV-atlas-packing utilities, salvaged from the
// chart-based imposter cage. GL-free and unit-tested. See
// docs/superpowers/specs/2026-06-22-voxel-box-imposter-design.md
//
// 2026-07-29 (chart-space virtual texturing, WP-A): 32-bit index overloads
// (sector meshes exceed 64k vertices), page-aligned shelf packing for the VT
// atlas, and a per-chart projection-distortion metric. The 16-bit API is
// unchanged (existing consumers/tests).
//
// Where it fits
// -------------
// MeshChartingLib is a leaf: no engine headers, no raylib, no Vulkan, no GL —
// only <vector> and the C++ standard library. Nothing depends on it in the
// other direction either, so it can be built and tested on its own
// (`make -C libs/MeshChartingLib`, `make -C libs/MeshChartingLib/tests run`).
//
// Consumers compile src/mesh_charting.cpp from where it lives rather than
// linking an archive (the repo's no-copies rule). Today that is
// MatterEngine3: MatterEngine3/Makefile pulls the .cpp into the engine
// archive, and MatterEngine3/src/lod_bake.cpp's build_chart_rung() drives the
// full sequence — build_adjacency -> segment_charts ->
// chart_average_normals -> plane_basis (per chart) -> pack_charts_paged — to
// produce a chart-atlas LOD rung. MatterEngine3/tests/chart_atlas_tests.cpp
// covers projection_distortion.
//
// Conventions shared by every function here
// -----------------------------------------
// - `positions` is a flat array of 3 floats per vertex, in whatever space the
//   caller uses (lod_bake passes world-space metres). `indices` holds 3
//   vertex indices per triangle, and its length is triCount * 3. Indices
//   address `positions` by VERTEX, so element k is at positions[k*3 .. +2].
// - A triangle soup (every corner its own vertex) is a valid input:
//   build_adjacency welds by exact position, so connectivity is recovered
//   from the geometry rather than the index buffer.
// - Chart ids run 0..nCharts-1 and are per-TRIANGLE, indexed by triangle
//   number; the chart-id vector returned by segment_charts is the parallel
//   array every later call expects.
// - All packing lengths (page_texels, gutter_texels, max_atlas_dim, pad,
//   atlas dims, placements) are in TEXELS.
//
// Considerations
// --------------
// - These are pure functions over caller-owned buffers. There is no global or
//   static state, so calls are independent and safe to run concurrently on
//   different data — which is what lets bake workers call them off the render
//   thread.
// - Results are deterministic for identical input, by design: welded vertex
//   ids are assigned in first-encounter order, and the packers break sort ties
//   explicitly because std::sort is not stable. Bake output depends on this.
// - Nothing here is in-place; every function returns a fresh std::vector
//   sized by the input, so cost is O(triangles) in both time and allocation.
//   None of them is cheap enough for a per-frame path.
// - No input validation beyond what each function documents. Pointers are
//   assumed non-null and long enough for triCount; a short buffer reads out
//   of bounds.
namespace mesh_charting {

// One entry per triangle, parallel to the input index buffer: nbr[e] is the
// triangle index sharing edge slot e, or -1 when that edge has no partner.
// Plain data, no ownership, cheap to copy. Produced only by build_adjacency
// and consumed only by segment_charts — the two must be given the same mesh,
// since the neighbour values are triangle indices into it.
// Per-triangle neighbor across edge slots (i0,i1)=0, (i1,i2)=1, (i2,i0)=2; -1 = boundary.
struct TriAdj { int nbr[3]; };

// Returns a vector of triCount entries. Welding is bit-exact on the three
// float words of each position — no epsilon — so vertices that merely round
// to the same value stay distinct and the edge between them reads as a
// boundary. That is deliberate: it keeps the result deterministic.
//
// Well defined for manifold meshes only. An edge is matched to the FIRST
// other triangle that claims it; if a third triangle shares the same edge it
// overwrites one side of that pairing, leaving the adjacency asymmetric
// (triangle A points at C while B still points at A). Nothing detects or
// reports this, so non-manifold input yields a silently lopsided graph.
// Build triangle adjacency. Vertices are welded by EXACT position first.
std::vector<TriAdj> build_adjacency(const float* positions, const unsigned short* indices,
                                    int triCount);
// 32-bit index overload (identical semantics).
std::vector<TriAdj> build_adjacency(const float* positions, const unsigned int* indices,
                                    int triCount);

// Greedy flood fill: seeds are visited in ascending triangle order, and a
// neighbour joins the chart when its face normal is within coneDeg of the
// chart's running average normal. Because the average moves as the chart
// grows, the partition depends on triangle ORDER as well as on geometry —
// reorder the mesh and you get a different (still valid) set of charts.
// `nCharts` is an out-parameter; every triangle ends up in exactly one chart,
// so the result has no -1 entries and chart ids are dense in 0..nCharts-1.
//
// Face normals are flipped to point away from the mesh CENTROID rather than
// taken from winding order, so the orientation is only trustworthy for a
// roughly star-shaped mesh; a deeply concave one can have inward-facing
// triangles classified outward. chart_average_normals repeats the same rule,
// so the two stay consistent with each other either way.
//
// `adj` must be the adjacency built from this same mesh. coneDeg is in
// DEGREES and the caller must keep it below 90 (at or beyond 90 the cone test
// admits back-facing neighbours and charts stop being planar-ish).
// Region-grow charts by normal-cone (coneDeg must be < 90). Returns per-triangle chart id.
std::vector<int> segment_charts(const float* positions, const unsigned short* indices,
                                int triCount, const std::vector<TriAdj>& adj,
                                float coneDeg, int& nCharts);
// 32-bit index overload (identical semantics).
std::vector<int> segment_charts(const float* positions, const unsigned int* indices,
                                int triCount, const std::vector<TriAdj>& adj,
                                float coneDeg, int& nCharts);

// 32-bit indices only — there is no 16-bit overload of this one. `chartOfTri`
// is the per-triangle vector segment_charts returned; entries outside
// [0, nCharts) and triangles past the end of that vector are skipped rather
// than treated as an error, so a short or stale chart vector quietly produces
// degenerate charts. Weighting is by triangle area (the un-normalized cross
// product is accumulated), so large triangles dominate a chart's normal.
// Area-weighted, outward-oriented (same centroid rule as segment_charts)
// average face normal per chart. Returned flat: nCharts * 3 floats,
// normalized; a degenerate chart falls back to +Y.
std::vector<float> chart_average_normals(const float* positions, const unsigned int* indices,
                                         int triCount, const std::vector<int>& chartOfTri,
                                         int nCharts);

// n need not be unit length; it is normalized internally. (T, B, n) come out
// right-handed, and the reference axis is chosen by inspecting |n.z| so the
// construction never degenerates — but the resulting rotation about n is
// arbitrary, so do not expect T to line up with anything in particular. It is
// stable for a given n, which is what matters for reproducible bakes. Writes
// 3 floats to each of T and B.
// Orthonormal basis (T,B) spanning the plane with normal n.
void plane_basis(const float n[3], float T[3], float B[3]);

// Input and output of the free-form (non-page-aligned) packer.
//
// ChartRect is a chart's bounding box in its own plane space — the units are
// whatever the caller measured in (lod_bake works in metres), NOT texels;
// pack_charts derives the texels-per-unit `scale` itself. minU/minV are the
// box's low corner and exist so the caller can map a point back into the
// atlas; the packer itself only reads w and h.
//
// ChartPlacement is the atlas position in TEXELS of the PADDED box, i.e. the
// gutter is inside the placement: the chart's content starts at
// (ox + pad, oy + pad) and spans ceil(w * scale) x ceil(h * scale) texels.
// Placements are parallel to the input charts vector.
struct ChartRect  { float minU, minV, w, h; };
struct ChartPlacement { int ox, oy; };

// `scale` is an OUT parameter only (texels per input unit) — whatever the
// caller passes in is overwritten. The search starts from an assumed 55% fill
// and shrinks by 15% per attempt for up to 24 attempts, so it only ever
// scales DOWN: success means everything fit, not that the atlas is tightly
// packed.
//
// Returns false when 24 attempts still overflow the atlas, or for an empty
// chart list or a non-positive atlas dimension. On failure `placements` and
// `scale` hold the last rejected attempt rather than being cleared, so the
// caller must not read them — this differs from pack_charts_paged below,
// which clears its outputs up front.
// Shelf-pack chart rects into an atlasW x atlasH grid with `pad` gutter texels.
bool pack_charts(const std::vector<ChartRect>& charts, int atlasW, int atlasH, int pad,
                 float& scale, std::vector<ChartPlacement>& placements);

// ---------------------------------------------------------------------------
// Page-aligned packing (chart-space virtual texturing).
//
// Each chart occupies a whole number of page_texels x page_texels pages so no
// finest-mip page ever spans two charts. The chart's CONTENT rect (content_w x
// content_h texels) sits inset by gutter_texels from the block origin; the
// block is the content + 2*gutter rounded UP to the page grid. Blocks are
// disjoint, so any two charts' content is >= 2*gutter_texels apart.
// ---------------------------------------------------------------------------
struct PagedChartSize      { int content_w, content_h; };   // texels, gutters excluded
struct PagedChartPlacement { int x, y, w, h; };             // page-aligned block, texels

// Deterministic tallest-first shelf pack of page-aligned blocks. On success
// fills placements (parallel to charts) and the atlas dims (page multiples,
// each <= max_atlas_dim). Fails when any block alone exceeds max_atlas_dim or
// the shelves cannot fit within max_atlas_dim^2.
bool pack_charts_paged(const std::vector<PagedChartSize>& charts,
                       int page_texels, int gutter_texels, int max_atlas_dim,
                       int& atlas_w, int& atlas_h,
                       std::vector<PagedChartPlacement>& placements);

// Projection distortion of the orthographic map onto the plane spanned by
// (T,B) over the given triangle subset: max over triangles of
// sigma_max/sigma_min of the per-triangle 2x2 Jacobian (intrinsic triangle
// frame -> plane). 1.0 = isometric. Degenerate (near-zero-area) triangles are
// skipped; a triangle nearly perpendicular to the plane returns a large value
// (>= 1e6). tri_list may be null to measure all triCount triangles.
float projection_distortion(const float* positions, const unsigned int* indices,
                            int triCount, const int* tri_list, int tri_list_count,
                            const float T[3], const float B[3]);

} // namespace mesh_charting
