#include "../include/imposter_asset.h"
#include "../include/mesh_simplifier.hpp"
#include "raylib.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include <sys/stat.h>

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
    return true;
}

static float3 v3(float x,float y,float z){ return make_float3(x,y,z); }
static float3 sub3(float3 a,float3 b){ return make_float3(a.x-b.x,a.y-b.y,a.z-b.z); }
static float3 cross3(float3 a,float3 b){ return make_float3(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x); }
static float3 norm3(float3 a){ float l=sqrtf(a.x*a.x+a.y*a.y+a.z*a.z); return l>1e-12f?make_float3(a.x/l,a.y/l,a.z/l):make_float3(0,0,0); }

bool build_cage(const std::vector<Tri>& part_tris, const ImpGenParams& p,
                uint64_t source_part_hash, ImposterAsset& out) {
    if (part_tris.empty() || p.atlasW <= 0 || p.atlasH <= 0) return false;

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

    SimplifyOptions opt; opt.target_ratio = p.cageRatio; opt.lock_boundary = false;
    Mesh cage = simplify_mesh(in, opt, nullptr);
    MemFree(in.vertices);
    if (cage.vertexCount == 0 || cage.triangleCount == 0) {
        if (cage.vertices) MemFree(cage.vertices);
        if (cage.normals)  MemFree(cage.normals);
        if (cage.indices)  MemFree(cage.indices);
        return false;
    }

    // Smoothed per-vertex normals on the simplified mesh.
    auto getv = [&](int i){ return make_float3(cage.vertices[i*3+0],cage.vertices[i*3+1],cage.vertices[i*3+2]); };

    // Compute mesh centroid to orient face normals outward.
    float3 centroid = make_float3(0,0,0);
    for (int i=0;i<cage.vertexCount;++i) {
        float3 p = getv(i);
        centroid = make_float3(centroid.x+p.x, centroid.y+p.y, centroid.z+p.z);
    }
    float inv = 1.0f / (float)cage.vertexCount;
    centroid = make_float3(centroid.x*inv, centroid.y*inv, centroid.z*inv);

    std::vector<float3> vn(cage.vertexCount, make_float3(0,0,0));
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

    // Atlas packing grid.
    const int nt = cage.triangleCount;
    int grid = (int)ceilf(sqrtf((float)nt)); if (grid < 1) grid = 1;
    const float cell = (float)p.atlasW / (float)grid; // assume square atlas
    const float pad = 2.0f;
    const float aw = (float)p.atlasW, ah = (float)p.atlasH;

    out = ImposterAsset{};
    out.source_part_hash = source_part_hash;
    out.atlas_w = (uint32_t)p.atlasW;
    out.atlas_h = (uint32_t)p.atlasH;
    out.disp_bits = p.dispBits;
    out.max_disp = p.inflation;
    out.verts.reserve(nt*3);
    out.tris.reserve(nt);

    float bmin[3]={1e30f,1e30f,1e30f}, bmax[3]={-1e30f,-1e30f,-1e30f};
    for (int t=0;t<nt;++t) {
        int idx[3] = { (int)cage.indices[t*3+0], (int)cage.indices[t*3+1], (int)cage.indices[t*3+2] };
        int gx = t % grid, gy = t / grid;
        float cx = gx*cell, cy = gy*cell;
        float uv[3][2] = {
            {(cx+pad)/aw,        (cy+pad)/ah},
            {(cx+cell-pad)/aw,   (cy+pad)/ah},
            {(cx+pad)/aw,        (cy+cell-pad)/ah},
        };
        for (int k=0;k<3;++k) {
            float3 pos = getv(idx[k]);
            float3 n   = vn[idx[k]];
            float3 ip  = make_float3(pos.x + n.x*p.inflation, pos.y + n.y*p.inflation, pos.z + n.z*p.inflation);
            CageVert cv; cv.px=ip.x; cv.py=ip.y; cv.pz=ip.z;
            cv.nx=n.x; cv.ny=n.y; cv.nz=n.z; cv.u=uv[k][0]; cv.v=uv[k][1];
            out.verts.push_back(cv);
            bmin[0]=fminf(bmin[0],ip.x); bmin[1]=fminf(bmin[1],ip.y); bmin[2]=fminf(bmin[2],ip.z);
            bmax[0]=fmaxf(bmax[0],ip.x); bmax[1]=fmaxf(bmax[1],ip.y); bmax[2]=fmaxf(bmax[2],ip.z);
        }
        out.tris.push_back({ (uint32_t)(t*3), (uint32_t)(t*3+1), (uint32_t)(t*3+2) });
    }
    for (int i=0;i<3;++i){ out.bounds_min[i]=bmin[i]; out.bounds_max[i]=bmax[i]; }
    float ext = fmaxf(bmax[0]-bmin[0], fmaxf(bmax[1]-bmin[1], bmax[2]-bmin[2]));
    out.parallax_radius = ext * 6.0f; // #3 hint; tune later

    MemFree(cage.vertices); MemFree(cage.normals); MemFree(cage.indices);
    return true;
}

bool bake_displacement_cpu(const std::vector<Tri>& part_tris, ImposterAsset& out) {
    if (part_tris.empty() || out.tris.empty() || out.atlas_w==0 || out.atlas_h==0) return false;

    // BVH over the part (BvhMesh owns a copy so the BVH's lifetime is self-contained).
    BvhMesh mesh{};
    mesh.triCount = (int)part_tris.size();
    mesh.tri = (Tri*)MALLOC64(sizeof(Tri)*mesh.triCount);
    for (int i=0;i<mesh.triCount;++i) mesh.tri[i] = part_tris[i];
    BVH bvh(&mesh);

    const int W=(int)out.atlas_w, H=(int)out.atlas_h;
    const int nt=(int)out.tris.size();
    int grid=(int)ceilf(sqrtf((float)nt)); if(grid<1) grid=1;
    const float cell=(float)W/(float)grid, pad=2.0f;
    const int bytes = out.disp_bits/8;

    // First pass: cast all covered texels, record raw distances + max.
    std::vector<float> dist((size_t)W*H, -1.0f);
    float observed_max = 1e-6f;
    for (int y=0;y<H;++y) for (int x=0;x<W;++x) {
        int gx=(int)((x+0.5f)/cell), gy=(int)((y+0.5f)/cell);
        int t=gy*grid+gx; if(t<0||t>=nt) continue;
        float fu=((x+0.5f)-(gx*cell+pad))/(cell-2*pad);
        float fv=((y+0.5f)-(gy*cell+pad))/(cell-2*pad);
        if (fu<0||fv<0||fu+fv>1.0f) continue; // gutter (outside the padded right-tri)
        float w1=fu,w2=fv,w0=1.0f-fu-fv;
        const CageVert& A=out.verts[3*t]; const CageVert& B=out.verts[3*t+1]; const CageVert& C=out.verts[3*t+2];
        float3 pos=make_float3(w0*A.px+w1*B.px+w2*C.px, w0*A.py+w1*B.py+w2*C.py, w0*A.pz+w1*B.pz+w2*C.pz);
        float3 n=norm3(make_float3(w0*A.nx+w1*B.nx+w2*C.nx, w0*A.ny+w1*B.ny+w2*C.ny, w0*A.nz+w1*B.nz+w2*C.nz));
        float3 dir=make_float3(-n.x,-n.y,-n.z);
        BVHRay ray; ray.O=pos; ray.D=dir; ray.rD=make_float3(1.0f/dir.x,1.0f/dir.y,1.0f/dir.z);
        ray.hit.t=1e30f;
        bvh.Intersect(ray, 0);
        if (ray.hit.t < 1e29f && ray.hit.t > 0.0f) {
            dist[(size_t)y*W+x]=ray.hit.t;
            if (ray.hit.t>observed_max) observed_max=ray.hit.t;
        }
    }
    out.max_disp = fmaxf(out.max_disp, observed_max);

    // Second pass: normalize + write disp + coverage.
    out.disp.assign((size_t)W*H*bytes, 0);
    out.color.assign((size_t)W*H*4, 0);
    for (int i=0;i<W*H;++i) {
        float d=dist[i];
        if (d<0.0f) { out.color[i*4+3]=0; continue; } // miss/gutter
        float nrm = d/out.max_disp; if(nrm>1.0f) nrm=1.0f; if(nrm<0.0f) nrm=0.0f;
        if (bytes==2) { uint16_t v=(uint16_t)(nrm*65535.0f+0.5f); memcpy(&out.disp[(size_t)i*2], &v, 2); }
        else          { out.disp[i]=(uint8_t)(nrm*255.0f+0.5f); }
        out.color[i*4+3]=255; // coverage
    }

    FREE64(mesh.tri);
    return true;
}

void dilate_atlas(ImposterAsset& a, int passes) {
    if (a.color.empty() || a.atlas_w==0 || a.atlas_h==0) return;
    const int W=(int)a.atlas_w, H=(int)a.atlas_h;
    for (int pass=0; pass<passes; ++pass) {
        std::vector<uint8_t> cov(W*H);
        for (int i=0;i<W*H;++i) cov[i]=a.color[i*4+3];
        std::vector<uint8_t> next = a.color;
        const int dx[8]={-1,1,0,0,-1,-1,1,1}, dy[8]={0,0,-1,1,-1,1,-1,1};
        for (int y=0;y<H;++y) for (int x=0;x<W;++x) {
            int i=y*W+x;
            if (cov[i]!=0) continue; // already covered: keep
            int rs=0,gs=0,bs=0,n=0;
            for (int k=0;k<8;++k) {
                int nx=x+dx[k], ny=y+dy[k];
                if (nx<0||ny<0||nx>=W||ny>=H) continue;
                int j=ny*W+nx;
                if (cov[j]==0) continue;
                rs+=a.color[j*4+0]; gs+=a.color[j*4+1]; bs+=a.color[j*4+2]; ++n;
            }
            if (n>0) {
                next[i*4+0]=(uint8_t)(rs/n); next[i*4+1]=(uint8_t)(gs/n); next[i*4+2]=(uint8_t)(bs/n);
                next[i*4+3]=1; // mark as "filled gutter" so the next pass can spread further
            }
        }
        a.color.swap(next);
    }
    // Reset the temporary fill markers (1) back to 0 so coverage stays {0,255}.
    for (int i=0;i<W*H;++i) if (a.color[i*4+3]==1) a.color[i*4+3]=0;
}

} // namespace imposter_asset
