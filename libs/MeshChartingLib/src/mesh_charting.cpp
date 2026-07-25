#include "../include/mesh_charting.h"
#include <map>
#include <array>
#include <vector>
#include <algorithm>
#include <cmath>

namespace mesh_charting {
namespace {
struct float3c { float x,y,z; };
static float3c v3(float x,float y,float z){ return {x,y,z}; }
static float3c sub3(float3c a,float3c b){ return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static float3c cross3(float3c a,float3c b){ return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
static float3c norm3(float3c a){ float l=std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z); return l>1e-12f?float3c{a.x/l,a.y/l,a.z/l}:float3c{0,0,0}; }
} // namespace

std::vector<TriAdj> build_adjacency(const float* positions, const unsigned short* indices,
                                    int triCount) {
    // Weld corners by exact position -> welded vertex id.
    std::map<std::array<float,3>,int> weld;
    auto wid = [&](int corner)->int {
        int vi = indices[corner];
        std::array<float,3> k{ positions[vi*3+0], positions[vi*3+1], positions[vi*3+2] };
        auto it = weld.find(k);
        if (it != weld.end()) return it->second;
        int id = (int)weld.size(); weld.emplace(k, id); return id;
    };

    std::vector<TriAdj> adj(triCount);
    for (auto& a : adj) { a.nbr[0]=a.nbr[1]=a.nbr[2]=-1; }

    // edge (sorted welded id pair) -> first (tri, edgeSlot) that claimed it.
    std::map<std::pair<int,int>, std::pair<int,int>> seen;
    for (int t=0;t<triCount;++t) {
        int w[3] = { wid(t*3+0), wid(t*3+1), wid(t*3+2) };
        for (int e=0;e<3;++e) {
            int a=w[e], b=w[(e+1)%3];
            std::pair<int,int> key = (a<b) ? std::make_pair(a,b) : std::make_pair(b,a);
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

std::vector<int> segment_charts(const float* positions, const unsigned short* indices,
                                int triCount, const std::vector<TriAdj>& adj,
                                float coneDeg, int& nCharts) {
    auto vpos = [&](int corner){ int vi=indices[corner];
        return v3(positions[vi*3+0],positions[vi*3+1],positions[vi*3+2]); };

    // Mesh centroid for outward orientation.
    float3c centroid = v3(0,0,0);
    for (int t=0;t<triCount;++t) for (int k=0;k<3;++k){ float3c p=vpos(t*3+k);
        centroid=v3(centroid.x+p.x,centroid.y+p.y,centroid.z+p.z); }
    float invn = (triCount>0) ? 1.0f/(float)(triCount*3) : 0.0f;
    centroid=v3(centroid.x*invn,centroid.y*invn,centroid.z*invn);

    // Outward per-face normals.
    std::vector<float3c> fn(triCount);
    for (int t=0;t<triCount;++t){
        float3c p0=vpos(t*3+0),p1=vpos(t*3+1),p2=vpos(t*3+2);
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

void plane_basis(const float n[3], float T[3], float B[3]) {
    float3c N = norm3(v3(n[0],n[1],n[2]));
    float3c up = (std::fabs(N.z) < 0.9f) ? v3(0,0,1) : v3(1,0,0);
    float3c t = norm3(cross3(up, N));
    float3c b = cross3(N, t);     // already unit (N,t orthonormal)
    T[0]=t.x; T[1]=t.y; T[2]=t.z;
    B[0]=b.x; B[1]=b.y; B[2]=b.z;
}

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

} // namespace mesh_charting
