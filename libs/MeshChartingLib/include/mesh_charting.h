#pragma once
#include <vector>

// Reusable mesh-charting / UV-atlas-packing utilities, salvaged from the
// chart-based imposter cage. GL-free and unit-tested. See
// docs/superpowers/specs/2026-06-22-voxel-box-imposter-design.md
namespace mesh_charting {

// Per-triangle neighbor across edge slots (i0,i1)=0, (i1,i2)=1, (i2,i0)=2; -1 = boundary.
struct TriAdj { int nbr[3]; };

// Build triangle adjacency. Vertices are welded by EXACT position first.
std::vector<TriAdj> build_adjacency(const float* positions, const unsigned short* indices,
                                    int triCount);

// Region-grow charts by normal-cone (coneDeg must be < 90). Returns per-triangle chart id.
std::vector<int> segment_charts(const float* positions, const unsigned short* indices,
                                int triCount, const std::vector<TriAdj>& adj,
                                float coneDeg, int& nCharts);

// Orthonormal basis (T,B) spanning the plane with normal n.
void plane_basis(const float n[3], float T[3], float B[3]);

struct ChartRect  { float minU, minV, w, h; };
struct ChartPlacement { int ox, oy; };

// Shelf-pack chart rects into an atlasW x atlasH grid with `pad` gutter texels.
bool pack_charts(const std::vector<ChartRect>& charts, int atlasW, int atlasH, int pad,
                 float& scale, std::vector<ChartPlacement>& placements);

} // namespace mesh_charting
