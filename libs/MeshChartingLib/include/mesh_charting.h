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
namespace mesh_charting {

// Per-triangle neighbor across edge slots (i0,i1)=0, (i1,i2)=1, (i2,i0)=2; -1 = boundary.
struct TriAdj { int nbr[3]; };

// Build triangle adjacency. Vertices are welded by EXACT position first.
std::vector<TriAdj> build_adjacency(const float* positions, const unsigned short* indices,
                                    int triCount);
// 32-bit index overload (identical semantics).
std::vector<TriAdj> build_adjacency(const float* positions, const unsigned int* indices,
                                    int triCount);

// Region-grow charts by normal-cone (coneDeg must be < 90). Returns per-triangle chart id.
std::vector<int> segment_charts(const float* positions, const unsigned short* indices,
                                int triCount, const std::vector<TriAdj>& adj,
                                float coneDeg, int& nCharts);
// 32-bit index overload (identical semantics).
std::vector<int> segment_charts(const float* positions, const unsigned int* indices,
                                int triCount, const std::vector<TriAdj>& adj,
                                float coneDeg, int& nCharts);

// Area-weighted, outward-oriented (same centroid rule as segment_charts)
// average face normal per chart. Returned flat: nCharts * 3 floats,
// normalized; a degenerate chart falls back to +Y.
std::vector<float> chart_average_normals(const float* positions, const unsigned int* indices,
                                         int triCount, const std::vector<int>& chartOfTri,
                                         int nCharts);

// Orthonormal basis (T,B) spanning the plane with normal n.
void plane_basis(const float n[3], float T[3], float B[3]);

struct ChartRect  { float minU, minV, w, h; };
struct ChartPlacement { int ox, oy; };

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
