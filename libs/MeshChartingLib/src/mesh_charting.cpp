// libs/MeshChartingLib/src/mesh_charting.cpp
//
// The whole implementation of MeshChartingLib. See ../include/mesh_charting.h
// for the public contract, the shared input conventions (flat float3
// positions, triCount * 3 indices, per-triangle chart ids) and the caveats
// callers need; this file documents how the pieces work.
//
// Layout
// ------
// - An anonymous namespace holds a minimal float3 type and the two
//   index-width-templated cores. build_adjacency and segment_charts each have
//   a 16-bit and a 32-bit public overload that forward to the SAME template,
//   so the two widths cannot drift apart — 32-bit exists because sector
//   meshes exceed 64k vertices (WP-A, 2026-07-29).
// - Two independent shelf packers follow: shelf_pack/pack_charts work in
//   texels with a scale search, shelf_pack_pages/pack_charts_paged work in
//   whole pages with a width search. They share the tallest-first shelf idea
//   and nothing else.
// - projection_distortion closes the file and stands alone.
//
// Determinism
// -----------
// Bake output depends on these results being reproducible, so the file is
// written to avoid every common source of run-to-run variation: welded vertex
// ids come from first-encounter order and the hash maps are only ever probed,
// never iterated; both packers sort with an explicit tie-break because
// std::sort is not stable; and no floating-point value is compared for
// equality except against the documented degeneracy epsilons.
//
// No global or static mutable state anywhere, so independent calls on
// independent data may run concurrently — bake workers rely on this.
#include "../include/mesh_charting.h"
#include <map>
#include <array>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace mesh_charting {
namespace {
// A private three-float vector and the handful of operations this file needs.
// It exists so MeshChartingLib stays a leaf: SpatialQueryLib's float3 is the
// engine's SIMD interchange type, and depending on it would pull the geometry
// library into a lib that is deliberately dependency-free. Deliberately not
// exported — nothing in the public header mentions it.
//
// Note norm3's degeneracy behaviour: a vector shorter than 1e-12 normalizes
// to exactly (0,0,0) rather than producing NaNs. Callers below test for that
// zero and substitute a fallback.
struct float3c { float x,y,z; };
static float3c v3(float x,float y,float z){ return {x,y,z}; }
static float3c sub3(float3c a,float3c b){ return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static float3c cross3(float3c a,float3c b){ return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
static float dot3(float3c a,float3c b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static float3c norm3(float3c a){ float l=std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z); return l>1e-12f?float3c{a.x/l,a.y/l,a.z/l}:float3c{0,0,0}; }

// Bit-exact position key for welding (12 bytes of float bits). Deterministic:
// welded ids are assigned in first-encounter (corner-scan) order, and the maps
// below are only ever probed, never iterated.
struct PosKey { uint32_t b[3]; };
struct PosKeyHash {
    size_t operator()(const PosKey& k) const {
        uint64_t h = 1469598103934665603ull;
        for (int i = 0; i < 3; ++i) { h ^= k.b[i]; h *= 1099511628211ull; }
        return (size_t)h;
    }
};
struct PosKeyEq {
    bool operator()(const PosKey& a, const PosKey& b) const {
        return a.b[0]==b.b[0] && a.b[1]==b.b[1] && a.b[2]==b.b[2];
    }
};

// Two hash passes, both O(triCount): weld corners to positional vertex ids,
// then match edges by the sorted pair of welded ids packed into a uint64 key.
// The `seen` map records the FIRST (triangle, edge slot) to claim each edge
// and links the second one to it symmetrically.
//
// Consequence for non-manifold input: a third triangle on the same edge finds
// the stale first claimant still in the map and re-links against it,
// clobbering the earlier pairing on one side only. The result is an
// asymmetric graph rather than an error — nothing here detects or reports the
// condition, so callers must supply manifold meshes.
//
// The 16- and 32-bit public overloads both instantiate this template, so the
// two index widths are guaranteed to behave identically.
template <typename IndexT>
std::vector<TriAdj> build_adjacency_impl(const float* positions, const IndexT* indices,
                                         int triCount) {
    // Weld corners by exact position -> welded vertex id.
    std::unordered_map<PosKey,int,PosKeyHash,PosKeyEq> weld;
    weld.reserve((size_t)triCount * 2);
    auto wid = [&](size_t corner)->int {
        const size_t vi = (size_t)indices[corner];
        PosKey k;
        std::memcpy(k.b, positions + vi*3, sizeof k.b);
        auto it = weld.find(k);
        if (it != weld.end()) return it->second;
        int id = (int)weld.size(); weld.emplace(k, id); return id;
    };

    std::vector<TriAdj> adj(triCount);
    for (auto& a : adj) { a.nbr[0]=a.nbr[1]=a.nbr[2]=-1; }

    // edge (sorted welded id pair) -> first (tri, edgeSlot) that claimed it.
    std::unordered_map<uint64_t, std::pair<int,int>> seen;
    seen.reserve((size_t)triCount * 3);
    for (int t=0;t<triCount;++t) {
        int w[3] = { wid((size_t)t*3+0), wid((size_t)t*3+1), wid((size_t)t*3+2) };
        for (int e=0;e<3;++e) {
            int a=w[e], b=w[(e+1)%3];
            const uint64_t key = (a<b) ? ((uint64_t)(uint32_t)a<<32 | (uint32_t)b)
                                       : ((uint64_t)(uint32_t)b<<32 | (uint32_t)a);
            auto it = seen.find(key);
            if (it == seen.end()) {
                seen.emplace(key, std::make_pair(t,e));
            } else {
                int ot = it->second.first, oe = it->second.second;
                adj[t].nbr[e]  = ot;
                adj[ot].nbr[oe] = t;
            }
        }
    }
    return adj;
}

// Three phases: mesh centroid, per-face outward normals, then a greedy
// depth-first flood fill (explicit `stack`, no recursion, so deep meshes
// cannot blow the C stack).
//
// "Outward" is defined against the mesh centroid, not against winding order —
// a face normal is flipped whenever it points back toward the centroid. This
// makes the segmentation independent of how the source mesh was wound, but it
// is only correct for roughly star-shaped geometry; chart_average_normals
// repeats the identical rule so the two agree regardless.
//
// The cone test compares a candidate's normal against the chart's RUNNING
// average (`sumN` accumulated unnormalized, normalized fresh at each test),
// so a chart's acceptance cone drifts as it grows and the partition depends
// on seed order — seeds are taken in ascending triangle index, which is what
// makes the result reproducible. coneCos is precomputed from coneDeg, which
// the header requires to be under 90 degrees; at or past 90 the cosine turns
// non-positive and the test starts admitting back-facing neighbours.
//
// Every triangle is assigned, so the returned vector contains no -1 and
// nCharts is the exact number of charts created.
template <typename IndexT>
std::vector<int> segment_charts_impl(const float* positions, const IndexT* indices,
                                     int triCount, const std::vector<TriAdj>& adj,
                                     float coneDeg, int& nCharts) {
    auto vpos = [&](size_t corner){ const size_t vi=(size_t)indices[corner];
        return v3(positions[vi*3+0],positions[vi*3+1],positions[vi*3+2]); };

    // Mesh centroid for outward orientation.
    float3c centroid = v3(0,0,0);
    for (int t=0;t<triCount;++t) for (int k=0;k<3;++k){ float3c p=vpos((size_t)t*3+k);
        centroid=v3(centroid.x+p.x,centroid.y+p.y,centroid.z+p.z); }
    float invn = (triCount>0) ? 1.0f/(float)(triCount*3) : 0.0f;
    centroid=v3(centroid.x*invn,centroid.y*invn,centroid.z*invn);

    // Outward per-face normals.
    std::vector<float3c> fn(triCount);
    for (int t=0;t<triCount;++t){
        float3c p0=vpos((size_t)t*3+0),p1=vpos((size_t)t*3+1),p2=vpos((size_t)t*3+2);
        float3c n=cross3(sub3(p1,p0),sub3(p2,p0));
        float3c fc=v3((p0.x+p1.x+p2.x)/3-centroid.x,
                      (p0.y+p1.y+p2.y)/3-centroid.y,
                      (p0.z+p1.z+p2.z)/3-centroid.z);
        if (n.x*fc.x+n.y*fc.y+n.z*fc.z < 0.0f) n=v3(-n.x,-n.y,-n.z);
        fn[t]=norm3(n);
    }

    const float coneCos = std::cos(coneDeg * 3.14159265358979f / 180.0f);
    std::vector<int> cid(triCount, -1);
    nCharts = 0;
    std::vector<int> stack;
    for (int seed=0; seed<triCount; ++seed) {
        if (cid[seed] != -1) continue;
        int c = nCharts++;
        cid[seed] = c;
        float3c sumN = fn[seed];               // running (unnormalized) chart normal
        stack.clear(); stack.push_back(seed);
        while (!stack.empty()) {
            int t = stack.back(); stack.pop_back();
            for (int e=0;e<3;++e) {
                int nb = adj[t].nbr[e];
                if (nb < 0 || cid[nb] != -1) continue;
                float3c avg = norm3(sumN);
                if (fn[nb].x*avg.x + fn[nb].y*avg.y + fn[nb].z*avg.z >= coneCos) {
                    cid[nb] = c;
                    sumN = v3(sumN.x+fn[nb].x, sumN.y+fn[nb].y, sumN.z+fn[nb].z);
                    stack.push_back(nb);
                }
            }
        }
    }
    return cid;
}
} // namespace

std::vector<TriAdj> build_adjacency(const float* positions, const unsigned short* indices,
                                    int triCount) {
    return build_adjacency_impl(positions, indices, triCount);
}
std::vector<TriAdj> build_adjacency(const float* positions, const unsigned int* indices,
                                    int triCount) {
    return build_adjacency_impl(positions, indices, triCount);
}

std::vector<int> segment_charts(const float* positions, const unsigned short* indices,
                                int triCount, const std::vector<TriAdj>& adj,
                                float coneDeg, int& nCharts) {
    return segment_charts_impl(positions, indices, triCount, adj, coneDeg, nCharts);
}
std::vector<int> segment_charts(const float* positions, const unsigned int* indices,
                                int triCount, const std::vector<TriAdj>& adj,
                                float coneDeg, int& nCharts) {
    return segment_charts_impl(positions, indices, triCount, adj, coneDeg, nCharts);
}

std::vector<float> chart_average_normals(const float* positions, const unsigned int* indices,
                                         int triCount, const std::vector<int>& chartOfTri,
                                         int nCharts) {
    std::vector<float> out((size_t)std::max(nCharts,0) * 3, 0.0f);
    if (nCharts <= 0) return out;
    auto vpos = [&](size_t corner){ const size_t vi=(size_t)indices[corner];
        return v3(positions[vi*3+0],positions[vi*3+1],positions[vi*3+2]); };

    // Same outward-orientation rule as segment_charts (mesh centroid).
    float3c centroid = v3(0,0,0);
    for (int t=0;t<triCount;++t) for (int k=0;k<3;++k){ float3c p=vpos((size_t)t*3+k);
        centroid=v3(centroid.x+p.x,centroid.y+p.y,centroid.z+p.z); }
    float invn = (triCount>0) ? 1.0f/(float)(triCount*3) : 0.0f;
    centroid=v3(centroid.x*invn,centroid.y*invn,centroid.z*invn);

    std::vector<float3c> acc((size_t)nCharts, v3(0,0,0));
    for (int t=0;t<triCount;++t) {
        const int c = ((size_t)t < chartOfTri.size()) ? chartOfTri[t] : -1;
        if (c < 0 || c >= nCharts) continue;
        float3c p0=vpos((size_t)t*3+0),p1=vpos((size_t)t*3+1),p2=vpos((size_t)t*3+2);
        float3c n=cross3(sub3(p1,p0),sub3(p2,p0));   // magnitude = 2*area (area weighting)
        float3c fc=v3((p0.x+p1.x+p2.x)/3-centroid.x,
                      (p0.y+p1.y+p2.y)/3-centroid.y,
                      (p0.z+p1.z+p2.z)/3-centroid.z);
        if (dot3(n,fc) < 0.0f) n=v3(-n.x,-n.y,-n.z);
        acc[c]=v3(acc[c].x+n.x, acc[c].y+n.y, acc[c].z+n.z);
    }
    for (int c=0;c<nCharts;++c) {
        float3c n = norm3(acc[c]);
        if (n.x==0.0f && n.y==0.0f && n.z==0.0f) n = v3(0,1,0);   // degenerate chart
        out[(size_t)c*3+0]=n.x; out[(size_t)c*3+1]=n.y; out[(size_t)c*3+2]=n.z;
    }
    return out;
}

void plane_basis(const float n[3], float T[3], float B[3]) {
    float3c N = norm3(v3(n[0],n[1],n[2]));
    float3c up = (std::fabs(N.z) < 0.9f) ? v3(0,0,1) : v3(1,0,0);
    float3c t = norm3(cross3(up, N));
    float3c b = cross3(N, t);     // already unit (N,t orthonormal)
    T[0]=t.x; T[1]=t.y; T[2]=t.z;
    B[0]=b.x; B[1]=b.y; B[2]=b.z;
}

// One packing attempt at a fixed `scale`. Rects are placed left to right into
// horizontal shelves, opening a new shelf when the row is full; tallest-first
// ordering keeps each shelf's wasted height down. `pad` is applied on BOTH
// sides of every rect, so the recorded ox/oy is the padded box's corner and
// two charts' content ends up at least 2*pad texels apart.
//
// Returns false as soon as anything does not fit, leaving `out` partially
// written — pack_charts treats that as "retry smaller", so `out` is only
// meaningful when this returns true.
static bool shelf_pack(const std::vector<ChartRect>& charts, int atlasW, int atlasH,
                       int pad, float scale, std::vector<ChartPlacement>& out) {
    const int n = (int)charts.size();
    out.assign(n, ChartPlacement{0,0});
    // Pack tallest-first for tighter shelves; remember original indices.
    std::vector<int> order(n); for (int i=0;i<n;++i) order[i]=i;
    std::sort(order.begin(), order.end(), [&](int a,int b){
        return charts[a].h > charts[b].h; });
    int cursorX=0, shelfY=0, shelfH=0;
    for (int oi=0; oi<n; ++oi) {
        int i = order[oi];
        int w = (int)std::ceil(charts[i].w*scale)+2*pad;
        int h = (int)std::ceil(charts[i].h*scale)+2*pad;
        if (w>atlasW || h>atlasH) return false;
        if (cursorX + w > atlasW) { shelfY += shelfH; cursorX = 0; shelfH = 0; }
        if (shelfY + h > atlasH) return false;
        out[i].ox = cursorX; out[i].oy = shelfY;
        cursorX += w; if (h>shelfH) shelfH = h;
    }
    return true;
}

// Binary-search-free scale fit: guess a scale from the total chart area
// assuming a 55% fill, then shrink 15% per failed attempt. Monotonic
// downward, so it never overshoots into an over-scaled atlas, but equally it
// never grows the guess — a run that succeeds on attempt 0 may be leaving
// resolution on the table. 24 attempts bottom out around 2% of the initial
// guess, well past anything usable, so exhausting them means the input is
// unpackable rather than merely awkward.
bool pack_charts(const std::vector<ChartRect>& charts, int atlasW, int atlasH, int pad,
                 float& scale, std::vector<ChartPlacement>& placements) {
    if (charts.empty() || atlasW<=0 || atlasH<=0) return false;
    double area = 0.0;
    for (const auto& c : charts) area += (double)std::max(c.w,1e-6f) * std::max(c.h,1e-6f);
    if (area <= 0.0) return false;
    // Initial guess assumes 55% fill; iterate down if packing overflows.
    scale = (float)std::sqrt(0.55 * (double)atlasW * (double)atlasH / area);
    for (int attempt=0; attempt<24; ++attempt) {
        if (shelf_pack(charts, atlasW, atlasH, pad, scale, placements)) return true;
        scale *= 0.85f;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Page-aligned packing
// ---------------------------------------------------------------------------

// Shelf pack of page-aligned blocks (all sizes in PAGES) into a fixed width;
// returns the used height in pages (0 = a block was wider than the atlas).
// Note the overloaded 0 return: it means "a block was wider than the atlas",
// but an empty block list would also return 0 (shelfY + shelfH with nothing
// placed). pack_charts_paged rejects the empty case before calling, so the
// ambiguity is unreachable there; any new caller must do the same.
static int shelf_pack_pages(const std::vector<std::pair<int,int>>& blocks, // (wPages,hPages)
                            const std::vector<int>& order, int widthPages,
                            std::vector<std::pair<int,int>>& out /* (x,y) pages */) {
    const int n = (int)blocks.size();
    out.assign(n, {0,0});
    int cursorX=0, shelfY=0, shelfH=0;
    for (int oi=0; oi<n; ++oi) {
        const int i = order[oi];
        const int w = blocks[i].first, h = blocks[i].second;
        if (w > widthPages) return 0;
        if (cursorX + w > widthPages) { shelfY += shelfH; cursorX = 0; shelfH = 0; }
        out[i] = {cursorX, shelfY};
        cursorX += w; if (h > shelfH) shelfH = h;
    }
    return shelfY + shelfH;
}

bool pack_charts_paged(const std::vector<PagedChartSize>& charts,
                       int page_texels, int gutter_texels, int max_atlas_dim,
                       int& atlas_w, int& atlas_h,
                       std::vector<PagedChartPlacement>& placements) {
    atlas_w = atlas_h = 0;
    placements.clear();
    const int n = (int)charts.size();
    if (n == 0 || page_texels <= 0 || gutter_texels < 0 || max_atlas_dim < page_texels)
        return false;
    const int maxPages = max_atlas_dim / page_texels;

    // Block sizes in pages: content + 2*gutter rounded up to the page grid.
    std::vector<std::pair<int,int>> blocks(n);
    long long totalPages = 0;
    int widest = 1;
    for (int i=0;i<n;++i) {
        const int cw = std::max(charts[i].content_w, 1);
        const int ch = std::max(charts[i].content_h, 1);
        const int bw = (cw + 2*gutter_texels + page_texels - 1) / page_texels;
        const int bh = (ch + 2*gutter_texels + page_texels - 1) / page_texels;
        if (bw > maxPages || bh > maxPages) return false;
        blocks[i] = {bw, bh};
        totalPages += (long long)bw * bh;
        if (bw > widest) widest = bw;
    }

    // Tallest-first, index tie-break (std::sort is not stable; the explicit
    // tie-break keeps the pack deterministic).
    std::vector<int> order(n); for (int i=0;i<n;++i) order[i]=i;
    std::sort(order.begin(), order.end(), [&](int a,int b){
        if (blocks[a].second != blocks[b].second) return blocks[a].second > blocks[b].second;
        return a < b; });

    int w0 = (int)std::ceil(std::sqrt((double)totalPages));
    if (w0 < widest) w0 = widest;
    if (w0 > maxPages) w0 = maxPages;

    std::vector<std::pair<int,int>> pos;
    for (int wPages = w0; wPages <= maxPages; ++wPages) {
        const int hPages = shelf_pack_pages(blocks, order, wPages, pos);
        if (hPages > 0 && hPages <= maxPages) {
            atlas_w = wPages * page_texels;
            atlas_h = hPages * page_texels;
            placements.resize(n);
            for (int i=0;i<n;++i) {
                placements[i].x = pos[i].first  * page_texels;
                placements[i].y = pos[i].second * page_texels;
                placements[i].w = blocks[i].first  * page_texels;
                placements[i].h = blocks[i].second * page_texels;
            }
            return true;
        }
    }
    return false;
}

// `worst` starts at 1.0 (isometric) and only ever rises, so a subset in which
// every triangle is skipped as degenerate returns 1.0 — a perfect score, not
// an error signal. Callers that care about coverage must count the triangles
// themselves. Triangle indices in `tri_list` outside [0, triCount) are
// skipped rather than treated as an error.
float projection_distortion(const float* positions, const unsigned int* indices,
                            int triCount, const int* tri_list, int tri_list_count,
                            const float T[3], const float B[3]) {
    const float3c Tv = v3(T[0],T[1],T[2]);
    const float3c Bv = v3(B[0],B[1],B[2]);
    const int count = tri_list ? tri_list_count : triCount;
    float worst = 1.0f;
    for (int k=0;k<count;++k) {
        const int t = tri_list ? tri_list[k] : k;
        if (t < 0 || t >= triCount) continue;
        const size_t i0=(size_t)indices[(size_t)t*3+0], i1=(size_t)indices[(size_t)t*3+1],
                     i2=(size_t)indices[(size_t)t*3+2];
        const float3c p0=v3(positions[i0*3],positions[i0*3+1],positions[i0*3+2]);
        const float3c p1=v3(positions[i1*3],positions[i1*3+1],positions[i1*3+2]);
        const float3c p2=v3(positions[i2*3],positions[i2*3+1],positions[i2*3+2]);
        const float3c e1=sub3(p1,p0), e2=sub3(p2,p0);
        const float3c nrm=cross3(e1,e2);
        const float twoArea = std::sqrt(dot3(nrm,nrm));
        const float e1len = std::sqrt(dot3(e1,e1));
        if (twoArea <= 1e-12f || e1len <= 1e-12f) continue;   // degenerate: skip
        // Intrinsic orthonormal frame (u,v) of the triangle's plane.
        const float3c u = v3(e1.x/e1len, e1.y/e1len, e1.z/e1len);
        const float3c w = v3(nrm.x/twoArea, nrm.y/twoArea, nrm.z/twoArea);
        const float3c v = cross3(w,u);
        // 2x2 Jacobian of (u,v) -> (T,B) projection.
        const float j11=dot3(u,Tv), j12=dot3(v,Tv);
        const float j21=dot3(u,Bv), j22=dot3(v,Bv);
        const float a=j11*j11+j21*j21, b=j11*j12+j21*j22, c=j12*j12+j22*j22;
        const float tr=a+c;
        float disc=(a-c)*(a-c)+4.0f*b*b;
        disc = disc>0.0f ? std::sqrt(disc) : 0.0f;
        const float s2max=0.5f*(tr+disc), s2min=0.5f*(tr-disc);
        float ratio;
        if (s2min <= 1e-12f) ratio = 1e6f;                    // near-perpendicular
        else ratio = std::sqrt(s2max/s2min);
        if (ratio > worst) worst = ratio;
    }
    return worst;
}

} // namespace mesh_charting
