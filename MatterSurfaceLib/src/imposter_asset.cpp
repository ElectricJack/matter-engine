#include "../include/imposter_asset.h"
#include "../include/mesh_simplifier.hpp"
#include "raylib.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include <cstdlib>
#include <sys/stat.h>
#include <map>
#include <array>

namespace {
template <class T> void put(std::vector<uint8_t>& b, const T& v){
    const uint8_t* p=reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(),p,p+sizeof(T));
}
void put_bytes(std::vector<uint8_t>& b, const void* d, size_t n){
    const uint8_t* p=static_cast<const uint8_t*>(d); b.insert(b.end(),p,p+n);
}
void ensure_parent_dir(const std::string& path){
    auto pos=path.find_last_of('/'); if(pos==std::string::npos) return;
#ifdef _WIN32
    mkdir(path.substr(0,pos).c_str());
#else
    mkdir(path.substr(0,pos).c_str(), 0755);
#endif
}
struct Reader {
    const uint8_t* p; const uint8_t* end; bool ok=true;
    template <class T> T get(){ T v{}; if(p+sizeof(T)>end){ok=false;return v;} std::memcpy(&v,p,sizeof(T)); p+=sizeof(T); return v; }
    const uint8_t* take(size_t n){ if(p+n>end){ok=false;return nullptr;} const uint8_t* r=p; p+=n; return r; }
};
} // namespace

namespace imposter_asset {

uint64_t compute_imp_hash(const ImpGenParams& p) {
    return part_asset::fnv1a64(&p, sizeof(p)) ^ static_cast<uint64_t>(kFormatVersion);
}

std::string cache_path(uint64_t hash) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string("imposters/") + buf + ".imp";
}

bool save(const std::string& path, const ImposterAsset& a, uint64_t imp_hash) {
    std::vector<uint8_t> body;
    put_bytes(body, a.bounds_min, 3*sizeof(float));
    put_bytes(body, a.bounds_max, 3*sizeof(float));
    put<float>(body, a.max_disp);
    put<float>(body, a.parallax_radius);
    put<uint32_t>(body, a.atlas_w);
    put<uint32_t>(body, a.atlas_h);
    put<uint32_t>(body, static_cast<uint32_t>(a.disp_bits));
    put<uint32_t>(body, static_cast<uint32_t>(a.verts.size()));
    put_bytes(body, a.verts.data(), a.verts.size()*sizeof(CageVert));
    put<uint32_t>(body, static_cast<uint32_t>(a.tris.size()));
    put_bytes(body, a.tris.data(), a.tris.size()*sizeof(CageTri));
    put<uint32_t>(body, static_cast<uint32_t>(a.disp.size()));
    put_bytes(body, a.disp.data(), a.disp.size());
    put<uint32_t>(body, static_cast<uint32_t>(a.color.size()));
    put_bytes(body, a.color.data(), a.color.size());
    put<uint32_t>(body, static_cast<uint32_t>(a.tri_chart.size()));
    put_bytes(body, a.tri_chart.data(), a.tri_chart.size()*sizeof(uint32_t));
    put<uint32_t>(body, static_cast<uint32_t>(a.triid.size()));
    put_bytes(body, a.triid.data(), a.triid.size());

    const uint64_t content_hash = part_asset::fnv1a64(body.data(), body.size());
    std::vector<uint8_t> head;
    put<uint32_t>(head, kMagic);
    put<uint32_t>(head, kFormatVersion);
    put<uint64_t>(head, imp_hash);
    put<uint64_t>(head, a.source_part_hash);
    put<uint32_t>(head, static_cast<uint32_t>(sizeof(CageVert)));
    put<uint32_t>(head, static_cast<uint32_t>(sizeof(CageTri)));
    put<uint64_t>(head, content_hash);

    ensure_parent_dir(path);
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(head.data(),1,head.size(),f)==head.size() &&
              std::fwrite(body.data(),1,body.size(),f)==body.size();
    std::fclose(f);
    if (!ok) { std::remove(tmp.c_str()); return false; }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool load(const std::string& path, uint64_t expected_imp_hash,
          uint64_t expected_source_hash, ImposterAsset& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f,0,SEEK_END); long sz=std::ftell(f); std::fseek(f,0,SEEK_SET);
    if (sz < 44) { std::fclose(f); return false; }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    bool read_ok = std::fread(buf.data(),1,buf.size(),f)==buf.size();
    std::fclose(f);
    if (!read_ok) return false;

    Reader r{ buf.data(), buf.data()+buf.size() };
    const uint32_t magic   = r.get<uint32_t>();
    const uint32_t version = r.get<uint32_t>();
    const uint64_t ihash   = r.get<uint64_t>();
    const uint64_t shash   = r.get<uint64_t>();
    const uint32_t s_vert  = r.get<uint32_t>();
    const uint32_t s_tri   = r.get<uint32_t>();
    const uint64_t content = r.get<uint64_t>();
    if (!r.ok) return false;
    if (magic != kMagic)               return false;
    if (version != kFormatVersion)     return false;
    if (s_vert != sizeof(CageVert))    return false;
    if (s_tri  != sizeof(CageTri))     return false;
    if (ihash != expected_imp_hash)    return false;
    if (shash != expected_source_hash) return false;
    if (part_asset::fnv1a64(r.p, static_cast<size_t>(r.end - r.p)) != content) return false;

    out = ImposterAsset{};
    out.source_part_hash = shash;
    const uint8_t* bmin = r.take(3*sizeof(float));
    const uint8_t* bmax = r.take(3*sizeof(float));
    if (!r.ok) return false;
    std::memcpy(out.bounds_min, bmin, 3*sizeof(float));
    std::memcpy(out.bounds_max, bmax, 3*sizeof(float));
    out.max_disp = r.get<float>();
    out.parallax_radius = r.get<float>();
    out.atlas_w = r.get<uint32_t>();
    out.atlas_h = r.get<uint32_t>();
    out.disp_bits = static_cast<int>(r.get<uint32_t>());
    const uint32_t vc = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* vp = r.take(vc*sizeof(CageVert));
    if (!r.ok) return false;
    out.verts.resize(vc); std::memcpy(out.verts.data(), vp, vc*sizeof(CageVert));
    const uint32_t tc = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* tp = r.take(tc*sizeof(CageTri));
    if (!r.ok) return false;
    out.tris.resize(tc); std::memcpy(out.tris.data(), tp, tc*sizeof(CageTri));
    const uint32_t dc = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* dp = r.take(dc);
    if (!r.ok) return false;
    out.disp.assign(dp, dp+dc);
    const uint32_t cc = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* cp = r.take(cc);
    if (!r.ok) return false;
    out.color.assign(cp, cp+cc);
    const uint32_t tcc = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* tcp = r.take(tcc*sizeof(uint32_t));
    if (!r.ok) return false;
    out.tri_chart.resize(tcc); std::memcpy(out.tri_chart.data(), tcp, tcc*sizeof(uint32_t));
    const uint32_t idc = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* idp = r.take(idc);
    if (!r.ok) return false;
    out.triid.assign(idp, idp+idc);
    return true;
}

static float3 v3(float x,float y,float z){ return make_float3(x,y,z); }
static float3 sub3(float3 a,float3 b){ return make_float3(a.x-b.x,a.y-b.y,a.z-b.z); }
static float3 cross3(float3 a,float3 b){ return make_float3(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x); }
static float3 norm3(float3 a){ float l=sqrtf(a.x*a.x+a.y*a.y+a.z*a.z); return l>1e-12f?make_float3(a.x/l,a.y/l,a.z/l):make_float3(0,0,0); }

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
        return make_float3(positions[vi*3+0],positions[vi*3+1],positions[vi*3+2]); };

    // Mesh centroid for outward orientation.
    float3 centroid = make_float3(0,0,0);
    for (int t=0;t<triCount;++t) for (int k=0;k<3;++k){ float3 p=vpos(t*3+k);
        centroid=make_float3(centroid.x+p.x,centroid.y+p.y,centroid.z+p.z); }
    float invn = (triCount>0) ? 1.0f/(float)(triCount*3) : 0.0f;
    centroid=make_float3(centroid.x*invn,centroid.y*invn,centroid.z*invn);

    // Outward per-face normals.
    std::vector<float3> fn(triCount);
    for (int t=0;t<triCount;++t){
        float3 p0=vpos(t*3+0),p1=vpos(t*3+1),p2=vpos(t*3+2);
        float3 n=cross3(sub3(p1,p0),sub3(p2,p0));
        float3 fc=make_float3((p0.x+p1.x+p2.x)/3-centroid.x,
                              (p0.y+p1.y+p2.y)/3-centroid.y,
                              (p0.z+p1.z+p2.z)/3-centroid.z);
        if (n.x*fc.x+n.y*fc.y+n.z*fc.z < 0.0f) n=make_float3(-n.x,-n.y,-n.z);
        fn[t]=norm3(n);
    }

    const float coneCos = cosf(coneDeg * 3.14159265358979f / 180.0f);
    std::vector<int> cid(triCount, -1);
    nCharts = 0;
    std::vector<int> stack;
    for (int seed=0; seed<triCount; ++seed) {
        if (cid[seed] != -1) continue;
        int c = nCharts++;
        cid[seed] = c;
        float3 sumN = fn[seed];               // running (unnormalized) chart normal
        stack.clear(); stack.push_back(seed);
        while (!stack.empty()) {
            int t = stack.back(); stack.pop_back();
            for (int e=0;e<3;++e) {
                int nb = adj[t].nbr[e];
                if (nb < 0 || cid[nb] != -1) continue;
                float3 avg = norm3(sumN);
                if (fn[nb].x*avg.x + fn[nb].y*avg.y + fn[nb].z*avg.z >= coneCos) {
                    cid[nb] = c;
                    sumN = make_float3(sumN.x+fn[nb].x, sumN.y+fn[nb].y, sumN.z+fn[nb].z);
                    stack.push_back(nb);
                }
            }
        }
    }
    return cid;
}

void plane_basis(const float n[3], float T[3], float B[3]) {
    float3 N = norm3(make_float3(n[0],n[1],n[2]));
    float3 up = (fabsf(N.z) < 0.9f) ? make_float3(0,0,1) : make_float3(1,0,0);
    float3 t = norm3(cross3(up, N));
    float3 b = cross3(N, t);     // already unit (N,t orthonormal)
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
        int w = (int)ceilf(charts[i].w*scale)+2*pad;
        int h = (int)ceilf(charts[i].h*scale)+2*pad;
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
    scale = (float)sqrt(0.55 * (double)atlasW * (double)atlasH / area);
    for (int attempt=0; attempt<24; ++attempt) {
        if (shelf_pack(charts, atlasW, atlasH, pad, scale, placements)) return true;
        scale *= 0.85f;
    }
    return false;
}

bool build_cage(const std::vector<Tri>& part_tris, const ImpGenParams& p,
                uint64_t source_part_hash, ImposterAsset& out) {
    if (part_tris.empty() || p.atlasW <= 0 || p.atlasH <= 0) return false;

    Mesh cage{};
    std::vector<float3> vn;
    auto getv = [&](int i){ return make_float3(cage.vertices[i*3+0],cage.vertices[i*3+1],cage.vertices[i*3+2]); };

    if (std::getenv("MSL_IMPOSTER_CUBE")) {
        // DEBUG cage: a slightly oversized axis-aligned cube around the part bounds.
        // 12 tris -> a 4x4 atlas with huge cells, and clean per-face outward normals,
        // so this isolates UV/orientation bake bugs from simplified-mesh normal noise.
        // Toggle with MSL_IMPOSTER_CUBE; remove once the bake pipeline is validated.
        float pmin[3]={1e30f,1e30f,1e30f}, pmax[3]={-1e30f,-1e30f,-1e30f};
        auto acc=[&](float3 v){ pmin[0]=fminf(pmin[0],v.x);pmin[1]=fminf(pmin[1],v.y);pmin[2]=fminf(pmin[2],v.z);
                                pmax[0]=fmaxf(pmax[0],v.x);pmax[1]=fmaxf(pmax[1],v.y);pmax[2]=fmaxf(pmax[2],v.z); };
        for (const Tri& t : part_tris){ acc(t.vertex0); acc(t.vertex1); acc(t.vertex2); }
        float ext = fmaxf(pmax[0]-pmin[0], fmaxf(pmax[1]-pmin[1], pmax[2]-pmin[2]));
        float m = 0.05f*ext + 1e-3f; // small oversize margin
        float lo[3]={pmin[0]-m,pmin[1]-m,pmin[2]-m}, hi[3]={pmax[0]+m,pmax[1]+m,pmax[2]+m};
        float3 c[8] = {
            make_float3(lo[0],lo[1],lo[2]), make_float3(hi[0],lo[1],lo[2]),
            make_float3(hi[0],hi[1],lo[2]), make_float3(lo[0],hi[1],lo[2]),
            make_float3(lo[0],lo[1],hi[2]), make_float3(hi[0],lo[1],hi[2]),
            make_float3(hi[0],hi[1],hi[2]), make_float3(lo[0],hi[1],hi[2]),
        };
        struct Face { int a,b,c,d; float3 n; };
        Face faces[6] = {
            {1,2,6,5, make_float3( 1, 0, 0)}, // +X
            {0,4,7,3, make_float3(-1, 0, 0)}, // -X
            {3,7,6,2, make_float3( 0, 1, 0)}, // +Y
            {0,1,5,4, make_float3( 0,-1, 0)}, // -Y
            {4,5,6,7, make_float3( 0, 0, 1)}, // +Z
            {0,3,2,1, make_float3( 0, 0,-1)}, // -Z
        };
        const int nv=36, ntr=12;
        cage.vertexCount=nv; cage.triangleCount=ntr;
        cage.vertices=(float*)MemAlloc(sizeof(float)*3*nv);
        cage.indices=(unsigned short*)MemAlloc(sizeof(unsigned short)*3*ntr);
        vn.assign(nv, make_float3(0,0,0));
        int vi=0;
        auto emit=[&](float3 ppos, float3 n){ cage.vertices[vi*3+0]=ppos.x;cage.vertices[vi*3+1]=ppos.y;cage.vertices[vi*3+2]=ppos.z;
                                              vn[vi]=n; cage.indices[vi]=(unsigned short)vi; ++vi; };
        for (const Face& f : faces){
            emit(c[f.a],f.n); emit(c[f.b],f.n); emit(c[f.c],f.n);
            emit(c[f.a],f.n); emit(c[f.c],f.n); emit(c[f.d],f.n);
        }
    } else {
        // Non-indexed input mesh: 3 corners per triangle, simplifier welds internally.
        Mesh in{};
        in.triangleCount = (int)part_tris.size();
        in.vertexCount   = in.triangleCount * 3;
        in.vertices = (float*)MemAlloc(sizeof(float)*3*in.vertexCount);
        for (int t=0;t<in.triangleCount;++t) {
            const Tri& tr = part_tris[t];
            const float3 c[3] = { tr.vertex0, tr.vertex1, tr.vertex2 };
            for (int k=0;k<3;++k){ in.vertices[(t*3+k)*3+0]=c[k].x; in.vertices[(t*3+k)*3+1]=c[k].y; in.vertices[(t*3+k)*3+2]=c[k].z; }
        }

        // Cap the cage triangle count so atlas cells stay large enough to hold a
        // usable per-triangle heightfield. With ~grid=ceil(sqrt(nt)) cells packed
        // into atlasW texels, a 97k-tri cage gives 3-texel cells that are almost
        // entirely padding gutter (near-zero coverage). Clamp the ratio so the cage
        // never exceeds maxCageTris regardless of how dense the source part is.
        float ratio = p.cageRatio;
        if (p.maxCageTris > 0 && (int)part_tris.size() > p.maxCageTris) {
            float cap = (float)p.maxCageTris / (float)part_tris.size();
            if (cap < ratio) ratio = cap;
        }
        SimplifyOptions opt; opt.target_ratio = ratio; opt.lock_boundary = false;
        cage = simplify_mesh(in, opt, nullptr);
        MemFree(in.vertices);
        if (cage.vertexCount == 0 || cage.triangleCount == 0) {
            if (cage.vertices) MemFree(cage.vertices);
            if (cage.normals)  MemFree(cage.normals);
            if (cage.indices)  MemFree(cage.indices);
            return false;
        }

        // Compute mesh centroid to orient face normals outward.
        float3 centroid = make_float3(0,0,0);
        for (int i=0;i<cage.vertexCount;++i) {
            float3 pp = getv(i);
            centroid = make_float3(centroid.x+pp.x, centroid.y+pp.y, centroid.z+pp.z);
        }
        float inv = 1.0f / (float)cage.vertexCount;
        centroid = make_float3(centroid.x*inv, centroid.y*inv, centroid.z*inv);

        vn.assign(cage.vertexCount, make_float3(0,0,0));
        for (int t=0;t<cage.triangleCount;++t) {
            int i0=cage.indices[t*3+0], i1=cage.indices[t*3+1], i2=cage.indices[t*3+2];
            float3 p0=getv(i0), p1=getv(i1), p2=getv(i2);
            float3 fn = cross3(sub3(p1,p0), sub3(p2,p0));
            // Orient face normal to point away from mesh centroid.
            float3 fc = make_float3((p0.x+p1.x+p2.x)/3-(centroid.x),
                                    (p0.y+p1.y+p2.y)/3-(centroid.y),
                                    (p0.z+p1.z+p2.z)/3-(centroid.z));
            float dot = fn.x*fc.x + fn.y*fc.y + fn.z*fc.z;
            if (dot < 0.0f) { fn = make_float3(-fn.x,-fn.y,-fn.z); }
            vn[i0]=make_float3(vn[i0].x+fn.x,vn[i0].y+fn.y,vn[i0].z+fn.z);
            vn[i1]=make_float3(vn[i1].x+fn.x,vn[i1].y+fn.y,vn[i1].z+fn.z);
            vn[i2]=make_float3(vn[i2].x+fn.x,vn[i2].y+fn.y,vn[i2].z+fn.z);
        }
        for (auto& n : vn) n = norm3(n);
    }

    // ---- Chart pipeline (replaces per-triangle grid packing) ----
    const int nt = cage.triangleCount;

    std::vector<TriAdj> adj = build_adjacency(cage.vertices, cage.indices, nt);
    int nCharts = 0;
    std::vector<int> chartOf = segment_charts(cage.vertices, cage.indices, nt, adj,
                                              p.chartConeDeg, nCharts);

    // Inflated emitted position per (triangle, corner) using the smoothed normal `vn`.
    auto inflated = [&](int t,int k)->float3 {
        int vi = cage.indices[t*3+k];
        float3 pos = getv(vi), n = vn[vi];
        return make_float3(pos.x+n.x*p.inflation, pos.y+n.y*p.inflation, pos.z+n.z*p.inflation);
    };

    // Per-chart outward average normal (re-accumulated from oriented face normals) and
    // centroid of inflated corners.
    float3 meshC = make_float3(0,0,0);
    for (int t=0;t<nt;++t) for (int k=0;k<3;++k){ float3 q=inflated(t,k);
        meshC=make_float3(meshC.x+q.x,meshC.y+q.y,meshC.z+q.z); }
    if (nt>0){ float invN=1.0f/(float)(nt*3); meshC=make_float3(meshC.x*invN,meshC.y*invN,meshC.z*invN); }

    std::vector<float3> chartSumN(nCharts, make_float3(0,0,0));
    std::vector<float3> chartCsum(nCharts, make_float3(0,0,0));
    std::vector<int>    chartCcnt(nCharts, 0);
    for (int t=0;t<nt;++t){
        float3 p0=inflated(t,0),p1=inflated(t,1),p2=inflated(t,2);
        float3 fnv=cross3(sub3(p1,p0),sub3(p2,p0));
        float3 fc=make_float3((p0.x+p1.x+p2.x)/3-meshC.x,(p0.y+p1.y+p2.y)/3-meshC.y,(p0.z+p1.z+p2.z)/3-meshC.z);
        if (fnv.x*fc.x+fnv.y*fc.y+fnv.z*fc.z<0.0f) fnv=make_float3(-fnv.x,-fnv.y,-fnv.z);
        fnv=norm3(fnv);
        int c=chartOf[t];
        chartSumN[c]=make_float3(chartSumN[c].x+fnv.x,chartSumN[c].y+fnv.y,chartSumN[c].z+fnv.z);
        for (int k=0;k<3;++k){ float3 q=inflated(t,k);
            chartCsum[c]=make_float3(chartCsum[c].x+q.x,chartCsum[c].y+q.y,chartCsum[c].z+q.z);
            chartCcnt[c]++; }
    }

    // Per-chart basis + centroid.
    std::vector<float3> chartT(nCharts), chartB(nCharts), chartO(nCharts);
    for (int c=0;c<nCharts;++c){
        float3 N=norm3(chartSumN[c]); float nn[3]={N.x,N.y,N.z}, T[3],B[3];
        plane_basis(nn,T,B);
        chartT[c]=make_float3(T[0],T[1],T[2]); chartB[c]=make_float3(B[0],B[1],B[2]);
        float inv = chartCcnt[c]>0 ? 1.0f/(float)chartCcnt[c] : 0.0f;
        chartO[c]=make_float3(chartCsum[c].x*inv,chartCsum[c].y*inv,chartCsum[c].z*inv);
    }

    // Pass 1: project every corner into its chart's plane; track per-chart 2D bbox.
    std::vector<float> pu((size_t)nt*3), pv((size_t)nt*3);
    std::vector<float> cMinU(nCharts,1e30f), cMinV(nCharts,1e30f),
                       cMaxU(nCharts,-1e30f), cMaxV(nCharts,-1e30f);
    for (int t=0;t<nt;++t){ int c=chartOf[t];
        for (int k=0;k<3;++k){
            float3 q=inflated(t,k), d=sub3(q,chartO[c]);
            float u=d.x*chartT[c].x+d.y*chartT[c].y+d.z*chartT[c].z;
            float v=d.x*chartB[c].x+d.y*chartB[c].y+d.z*chartB[c].z;
            pu[t*3+k]=u; pv[t*3+k]=v;
            cMinU[c]=fminf(cMinU[c],u); cMinV[c]=fminf(cMinV[c],v);
            cMaxU[c]=fmaxf(cMaxU[c],u); cMaxV[c]=fmaxf(cMaxV[c],v);
        }
    }

    // Pack chart rects.
    std::vector<ChartRect> rects(nCharts);
    for (int c=0;c<nCharts;++c){
        float w=fmaxf(cMaxU[c]-cMinU[c],1e-5f), h=fmaxf(cMaxV[c]-cMinV[c],1e-5f);
        rects[c]=ChartRect{cMinU[c],cMinV[c],w,h};
    }
    const int pad=2;
    float scale=1.0f; std::vector<ChartPlacement> placements;
    if (!pack_charts(rects, p.atlasW, p.atlasH, pad, scale, placements)) {
        MemFree(cage.vertices); if (cage.normals) MemFree(cage.normals); MemFree(cage.indices); return false;
    }

    // ---- Emit ----
    out = ImposterAsset{};
    out.source_part_hash = source_part_hash;
    out.atlas_w=(uint32_t)p.atlasW; out.atlas_h=(uint32_t)p.atlasH;
    out.disp_bits=p.dispBits; out.max_disp=p.inflation;
    out.verts.reserve(nt*3); out.tris.reserve(nt); out.tri_chart.reserve(nt);

    const float aw=(float)p.atlasW, ah=(float)p.atlasH;
    float bmin[3]={1e30f,1e30f,1e30f}, bmax[3]={-1e30f,-1e30f,-1e30f};
    for (int t=0;t<nt;++t){
        int c=chartOf[t];
        for (int k=0;k<3;++k){
            float3 ip=inflated(t,k); int vi=cage.indices[t*3+k]; float3 n=vn[vi];
            float u=(placements[c].ox+pad+(pu[t*3+k]-cMinU[c])*scale)/aw;
            float v=(placements[c].oy+pad+(pv[t*3+k]-cMinV[c])*scale)/ah;
            CageVert cv; cv.px=ip.x; cv.py=ip.y; cv.pz=ip.z;
            cv.nx=n.x; cv.ny=n.y; cv.nz=n.z; cv.u=u; cv.v=v;
            out.verts.push_back(cv);
            bmin[0]=fminf(bmin[0],ip.x); bmin[1]=fminf(bmin[1],ip.y); bmin[2]=fminf(bmin[2],ip.z);
            bmax[0]=fmaxf(bmax[0],ip.x); bmax[1]=fmaxf(bmax[1],ip.y); bmax[2]=fmaxf(bmax[2],ip.z);
        }
        out.tris.push_back({(uint32_t)(t*3),(uint32_t)(t*3+1),(uint32_t)(t*3+2)});
        out.tri_chart.push_back((uint32_t)c);
    }
    for (int i=0;i<3;++i){ out.bounds_min[i]=bmin[i]; out.bounds_max[i]=bmax[i]; }
    float ext=fmaxf(bmax[0]-bmin[0],fmaxf(bmax[1]-bmin[1],bmax[2]-bmin[2]));
    out.parallax_radius=ext*6.0f;

    MemFree(cage.vertices); if (cage.normals) MemFree(cage.normals); MemFree(cage.indices);
    return true;
}

bool bake_displacement_cpu(const std::vector<Tri>& part_tris, ImposterAsset& out) {
    if (part_tris.empty() || out.tris.empty() || out.atlas_w==0 || out.atlas_h==0) return false;

    // BVH over the part (BvhMesh owns a copy so the BVH's lifetime is self-contained).
    BvhMesh mesh{};
    mesh.triCount=(int)part_tris.size();
    mesh.tri=(Tri*)MALLOC64(sizeof(Tri)*mesh.triCount);
    for (int i=0;i<mesh.triCount;++i) mesh.tri[i]=part_tris[i];
    BVH bvh(&mesh);

    const int W=(int)out.atlas_w, H=(int)out.atlas_h;
    const int bytes=out.disp_bits/8;
    const int nt=(int)out.tris.size();

    std::vector<float> dist((size_t)W*H, -1.0f);
    out.triid.assign((size_t)W*H*2, 0xFF);   // default uint16 0xFFFF
    auto set_id=[&](int px,uint16_t id){ memcpy(&out.triid[(size_t)px*2], &id, 2); };

    // Rasterize each cage triangle into its packed UV region; cast an inward ray
    // per covered texel and record the part-surface hit distance + owning triangle.
    for (int t=0;t<nt;++t){
        const CageTri& tr=out.tris[t];
        const CageVert& A=out.verts[tr.i0]; const CageVert& B=out.verts[tr.i1]; const CageVert& C=out.verts[tr.i2];
        float ax=A.u*W, ay=A.v*H, bx=B.u*W, by=B.v*H, cx=C.u*W, cy=C.v*H;
        int x0=(int)floorf(fminf(ax,fminf(bx,cx))), x1=(int)ceilf(fmaxf(ax,fmaxf(bx,cx)));
        int y0=(int)floorf(fminf(ay,fminf(by,cy))), y1=(int)ceilf(fmaxf(ay,fmaxf(by,cy)));
        if (x0<0)x0=0; if (y0<0)y0=0; if (x1>W)x1=W; if (y1>H)y1=H;
        float area=(bx-ax)*(cy-ay)-(cx-ax)*(by-ay);
        if (fabsf(area)<1e-9f) continue;
        float invArea=1.0f/area;
        for (int y=y0;y<y1;++y) for (int x=x0;x<x1;++x){
            float px=x+0.5f, py=y+0.5f;
            float wA=((bx-px)*(cy-py)-(cx-px)*(by-py))*invArea;   // area(B,C,P)/area -> weight of A
            float wB=((cx-px)*(ay-py)-(ax-px)*(cy-py))*invArea;   // area(C,A,P)/area -> weight of B
            float wC=1.0f-wA-wB;
            if (wA<0||wB<0||wC<0) continue;            // outside this triangle
            float3 pos=make_float3(wA*A.px+wB*B.px+wC*C.px,
                                   wA*A.py+wB*B.py+wC*C.py,
                                   wA*A.pz+wB*B.pz+wC*C.pz);
            float3 n=norm3(make_float3(wA*A.nx+wB*B.nx+wC*C.nx,
                                       wA*A.ny+wB*B.ny+wC*C.ny,
                                       wA*A.nz+wB*B.nz+wC*C.nz));
            float3 dir=make_float3(-n.x,-n.y,-n.z);
            BVHRay ray; ray.O=pos; ray.D=dir; ray.rD=make_float3(1.0f/dir.x,1.0f/dir.y,1.0f/dir.z);
            ray.hit.t=1e30f;
            bvh.Intersect(ray,0);
            int idx=y*W+x;
            if (ray.hit.t<1e29f && ray.hit.t>0.0f){
                dist[idx]=ray.hit.t;
                set_id(idx,(uint16_t)t);
            }
        }
    }

    // Displacement spans the FULL imposter volume, not a thin surface shell:
    // disp 0 = the inward ray hit right at the cage face, disp 1 = it hit near
    // the far side of the volume. max_disp is therefore the cage's depth (largest
    // bbox extent), so the relief march walks the whole interior. Never below the
    // cage inflation (the guaranteed minimum shell).
    float ext[3]={ out.bounds_max[0]-out.bounds_min[0],
                   out.bounds_max[1]-out.bounds_min[1],
                   out.bounds_max[2]-out.bounds_min[2] };
    float full_depth=fmaxf(ext[0],fmaxf(ext[1],ext[2]));
    out.max_disp=fmaxf(out.max_disp,full_depth);
    if (const char* e=std::getenv("MSL_IMP_SHELL")) out.max_disp=(float)atof(e); // absolute override

    // Normalize every inward hit across the full depth + write coverage. A texel
    // is covered iff its ray hit the part anywhere inside the volume; a true miss
    // (ray escaped, or never rasterized) is coverage 0. Hits past max_disp clamp.
    out.disp.assign((size_t)W*H*bytes,0);
    out.color.assign((size_t)W*H*4,0);
    for (int i=0;i<W*H;++i){
        float d=dist[i];
        if (d<0.0f){ out.color[i*4+3]=0; continue; }
        float nrm=d/out.max_disp; if(nrm>1.0f)nrm=1.0f; if(nrm<0.0f)nrm=0.0f;
        if (bytes==2){ uint16_t v=(uint16_t)(nrm*65535.0f+0.5f); memcpy(&out.disp[(size_t)i*2],&v,2); }
        else         { out.disp[i]=(uint8_t)(nrm*255.0f+0.5f); }
        out.color[i*4+3]=255;
    }

    FREE64(mesh.tri);
    return true;
}

void dilate_atlas(ImposterAsset& a, int passes) {
    if (a.color.empty() || a.atlas_w==0 || a.atlas_h==0) return;
    const int W=(int)a.atlas_w, H=(int)a.atlas_h;
    const int bytes = a.disp_bits/8;
    const bool haveDisp = (!a.disp.empty() && (int)a.disp.size() == W*H*bytes);
    auto dispGet = [&](const std::vector<uint8_t>& d, int i)->float {
        if (bytes==2) { uint16_t v; memcpy(&v,&d[(size_t)i*2],2); return (float)v; }
        return (float)d[i];
    };
    auto dispSet = [&](std::vector<uint8_t>& d, int i, float v){
        if (bytes==2) { uint16_t u=(uint16_t)(v+0.5f); memcpy(&d[(size_t)i*2],&u,2); }
        else          { d[i]=(uint8_t)(v+0.5f); }
    };
    // Dilate color AND displacement together. Leaving disp=0 in the gutter makes
    // bilinear filtering ramp toward "surface at the cage face" at silhouettes,
    // which the relief march reads as a false near crossing (speckle). Flooding
    // real neighbor depths outward keeps the filtered edge consistent; the
    // coverage>0.5 gate then clips the silhouette cleanly.
    for (int pass=0; pass<passes; ++pass) {
        std::vector<uint8_t> cov(W*H);
        for (int i=0;i<W*H;++i) cov[i]=a.color[i*4+3];
        std::vector<uint8_t> nextC = a.color;
        std::vector<uint8_t> nextD = a.disp;
        const int dx[8]={-1,1,0,0,-1,-1,1,1}, dy[8]={0,0,-1,1,-1,1,-1,1};
        for (int y=0;y<H;++y) for (int x=0;x<W;++x) {
            int i=y*W+x;
            if (cov[i]!=0) continue; // already covered: keep
            int rs=0,gs=0,bs=0,n=0; float ds=0.0f;
            for (int k=0;k<8;++k) {
                int nx=x+dx[k], ny=y+dy[k];
                if (nx<0||ny<0||nx>=W||ny>=H) continue;
                int j=ny*W+nx;
                if (cov[j]==0) continue;
                rs+=a.color[j*4+0]; gs+=a.color[j*4+1]; bs+=a.color[j*4+2];
                if (haveDisp) ds+=dispGet(a.disp, j);
                ++n;
            }
            if (n>0) {
                nextC[i*4+0]=(uint8_t)(rs/n); nextC[i*4+1]=(uint8_t)(gs/n); nextC[i*4+2]=(uint8_t)(bs/n);
                nextC[i*4+3]=1; // mark as "filled gutter" so the next pass can spread further
                if (haveDisp) dispSet(nextD, i, ds/(float)n);
            }
        }
        a.color.swap(nextC);
        if (haveDisp) a.disp.swap(nextD);
    }
    // Reset the temporary fill markers (1) back to 0 so coverage stays {0,255}.
    for (int i=0;i<W*H;++i) if (a.color[i*4+3]==1) a.color[i*4+3]=0;
}

std::vector<float> pack_cage_uvs_bvh_order(const ImposterAsset& a,
                                           const uint32_t* triIdx, int nTris) {
    // Chart UV rects from tri_chart + emitted UVs.
    int nCharts=0; for (uint32_t c : a.tri_chart) nCharts=std::max(nCharts,(int)c+1);
    std::vector<float> lo((size_t)nCharts*2, 1e30f), hi((size_t)nCharts*2,-1e30f);
    for (int t=0;t<(int)a.tris.size();++t){
        int c=(int)a.tri_chart[t]; const CageTri& tr=a.tris[t];
        const CageVert* vs[3]={&a.verts[tr.i0],&a.verts[tr.i1],&a.verts[tr.i2]};
        for (auto* v : vs){
            lo[c*2]=fminf(lo[c*2],v->u); lo[c*2+1]=fminf(lo[c*2+1],v->v);
            hi[c*2]=fmaxf(hi[c*2],v->u); hi[c*2+1]=fmaxf(hi[c*2+1],v->v);
        }
    }
    std::vector<float> buf((size_t)nTris*3*4, 0.0f);
    auto setpx=[&](int row,int i,float x,float y,float z,float w){
        size_t o=((size_t)row*nTris + i)*4; buf[o]=x; buf[o+1]=y; buf[o+2]=z; buf[o+3]=w; };
    for (int i=0;i<nTris;++i){
        int cageTri=(int)triIdx[i]; const CageTri& tr=a.tris[cageTri];
        const CageVert& v0=a.verts[tr.i0]; const CageVert& v1=a.verts[tr.i1]; const CageVert& v2=a.verts[tr.i2];
        int c=(int)a.tri_chart[cageTri];
        setpx(0,i, v0.u,v0.v, lo[c*2],   lo[c*2+1]);     // uv0 + chartLo
        setpx(1,i, v1.u,v1.v, hi[c*2],   hi[c*2+1]);     // uv1 + chartHi
        setpx(2,i, v2.u,v2.v, (float)cageTri, 0.0f);     // uv2 + cageTriId
    }
    return buf;
}

std::vector<float> pack_cage_tri_data(const ImposterAsset& a) {
    const int nt=(int)a.tris.size();
    std::vector<float> buf((size_t)nt*6*4, 0.0f);
    auto setpx=[&](int row,int tri,float x,float y,float z,float w){
        size_t o=((size_t)row*nt + tri)*4; buf[o]=x; buf[o+1]=y; buf[o+2]=z; buf[o+3]=w; };
    for (int t=0;t<nt;++t){
        const CageTri& tr=a.tris[t];
        const CageVert& v0=a.verts[tr.i0]; const CageVert& v1=a.verts[tr.i1]; const CageVert& v2=a.verts[tr.i2];
        setpx(0,t, v0.px,v0.py,v0.pz, 0.0f);
        setpx(1,t, v1.px,v1.py,v1.pz, 0.0f);
        setpx(2,t, v2.px,v2.py,v2.pz, 0.0f);
        setpx(3,t, v0.u,v0.v, 0.0f, 0.0f);
        setpx(4,t, v1.u,v1.v, 0.0f, 0.0f);
        setpx(5,t, v2.u,v2.v, 0.0f, 0.0f);
    }
    return buf;
}

} // namespace imposter_asset
