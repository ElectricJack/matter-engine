#include "../include/voxel_imposter.h"
#include "../include/part_asset.h"   // fnv1a64
#include "../include/material_registry.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
// tlas_manager.hpp and blas_manager.hpp are already pulled in via voxel_imposter.h

namespace {
// ---- I/O helpers ----
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

inline void sub(float r[3],const float a[3],const float b[3]){ r[0]=a[0]-b[0];r[1]=a[1]-b[1];r[2]=a[2]-b[2]; }
inline void cross(float r[3],const float a[3],const float b[3]){ r[0]=a[1]*b[2]-a[2]*b[1]; r[1]=a[2]*b[0]-a[0]*b[2]; r[2]=a[0]*b[1]-a[1]*b[0]; }
inline float dot(const float a[3],const float b[3]){ return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline bool plane_box_overlap(const float n[3],float d,const float h[3]){
    float vmin[3],vmax[3];
    for(int q=0;q<3;++q){ if(n[q]>0){vmin[q]=-h[q];vmax[q]=h[q];}else{vmin[q]=h[q];vmax[q]=-h[q];} }
    if(dot(n,vmin)+d>0) return false;
    if(dot(n,vmax)+d>=0) return true;
    return false;
}
} // namespace

namespace voxel_imposter {

bool choose_grid_dims(const float lo[3], const float hi[3],
                      int maxDim, int& nx, int& ny, int& nz) {
    float ex = hi[0]-lo[0], ey = hi[1]-lo[1], ez = hi[2]-lo[2];
    float maxExtent = std::max(ex, std::max(ey, ez));
    if (maxExtent <= 0.0f || maxDim < 1) return false;
    float v = maxExtent / (float)maxDim;
    auto dim = [&](float e){ int d = (int)std::ceil(e / v); return std::max(1, std::min(maxDim, d)); };
    nx = dim(ex); ny = dim(ey); nz = dim(ez);
    return true;
}

bool tri_box_overlap(const float bc[3], const float bh[3],
                     const float V0[3], const float V1[3], const float V2[3]) {
    float v0[3],v1[3],v2[3]; sub(v0,V0,bc); sub(v1,V1,bc); sub(v2,V2,bc);
    float e0[3],e1[3],e2[3]; sub(e0,v1,v0); sub(e1,v2,v1); sub(e2,v0,v2);
    float fex,fey,fez,p0,p1,p2,rad,minv,maxv;
    (void)p2;
    #define AXISTEST_X(a,b,fa,fb,va,vb) \
        p0=a*va[1]-b*va[2]; p1=a*vb[1]-b*vb[2]; \
        minv=p0<p1?p0:p1; maxv=p0<p1?p1:p0; rad=fa*bh[1]+fb*bh[2]; \
        if(minv>rad||maxv<-rad) return false;
    #define AXISTEST_Y(a,b,fa,fb,va,vb) \
        p0=-a*va[0]+b*va[2]; p1=-a*vb[0]+b*vb[2]; \
        minv=p0<p1?p0:p1; maxv=p0<p1?p1:p0; rad=fa*bh[0]+fb*bh[2]; \
        if(minv>rad||maxv<-rad) return false;
    #define AXISTEST_Z(a,b,fa,fb,va,vb) \
        p0=a*va[0]-b*va[1]; p1=a*vb[0]-b*vb[1]; \
        minv=p0<p1?p0:p1; maxv=p0<p1?p1:p0; rad=fa*bh[0]+fb*bh[1]; \
        if(minv>rad||maxv<-rad) return false;
    fex=std::fabs(e0[0]);fey=std::fabs(e0[1]);fez=std::fabs(e0[2]);
    AXISTEST_X(e0[2],e0[1],fez,fey,v0,v2); AXISTEST_Y(e0[2],e0[0],fez,fex,v0,v2); AXISTEST_Z(e0[1],e0[0],fey,fex,v1,v2);
    fex=std::fabs(e1[0]);fey=std::fabs(e1[1]);fez=std::fabs(e1[2]);
    AXISTEST_X(e1[2],e1[1],fez,fey,v0,v2); AXISTEST_Y(e1[2],e1[0],fez,fex,v0,v2); AXISTEST_Z(e1[1],e1[0],fey,fex,v0,v1);
    fex=std::fabs(e2[0]);fey=std::fabs(e2[1]);fez=std::fabs(e2[2]);
    AXISTEST_X(e2[2],e2[1],fez,fey,v0,v1); AXISTEST_Y(e2[2],e2[0],fez,fex,v0,v1); AXISTEST_Z(e2[1],e2[0],fey,fex,v1,v2);
    #undef AXISTEST_X
    #undef AXISTEST_Y
    #undef AXISTEST_Z
    auto mm=[&](float a,float b,float c,float&mn,float&mx){ mn=mx=a; if(b<mn)mn=b; if(b>mx)mx=b; if(c<mn)mn=c; if(c>mx)mx=c; };
    mm(v0[0],v1[0],v2[0],minv,maxv); if(minv>bh[0]||maxv<-bh[0]) return false;
    mm(v0[1],v1[1],v2[1],minv,maxv); if(minv>bh[1]||maxv<-bh[1]) return false;
    mm(v0[2],v1[2],v2[2],minv,maxv); if(minv>bh[2]||maxv<-bh[2]) return false;
    float n[3]; cross(n,e0,e1); float d=-dot(n,v0);
    return plane_box_overlap(n,d,bh);
}

void oct_encode(const float n[3], uint8_t out[2]) {
    double ax=std::fabs((double)n[0])+std::fabs((double)n[1])+std::fabs((double)n[2]);
    double x=n[0]/ax, y=n[1]/ax;
    if (n[2] < 0.0f) {
        double ox=(1.0-std::fabs(y))*(x>=0?1.0:-1.0);
        double oy=(1.0-std::fabs(x))*(y>=0?1.0:-1.0);
        x=ox; y=oy;
    }
    auto q=[](double v){ v=0.5*(v+1.0); v=v<0?0:(v>1?1:v); return (uint8_t)(v*255.0+0.5); };
    out[0]=q(x); out[1]=q(y);
}
void oct_decode(const uint8_t in[2], float n[3]) {
    float x=in[0]/255.0f*2.0f-1.0f, y=in[1]/255.0f*2.0f-1.0f;
    float z=1.0f-std::fabs(x)-std::fabs(y);
    if (z<0.0f) { float ox=(1.0f-std::fabs(y))*(x>=0?1.0f:-1.0f);
                  float oy=(1.0f-std::fabs(x))*(y>=0?1.0f:-1.0f); x=ox; y=oy; }
    float l=std::sqrt(x*x+y*y+z*z); if(l<1e-12f)l=1.0f;
    n[0]=x/l; n[1]=y/l; n[2]=z/l;
}

std::vector<FlatTri> flatten_part_triangles_mat(const BLASManager& blas, const TLASManager& tlas) {
    std::vector<FlatTri> out;
    const auto& recs = tlas.get_draw_records();
    for (const auto& r : recs) {
        const BLASManager::BLASEntry* e = blas.get_entry(r.blas_handle);
        if (!e) continue;
        const float* m = r.transform.m; // row-major 4x4
        auto xf = [&](float3 p) {
            return make_float3(
                m[0]*p.x + m[1]*p.y + m[2]*p.z + m[3],
                m[4]*p.x + m[5]*p.y + m[6]*p.z + m[7],
                m[8]*p.x + m[9]*p.y + m[10]*p.z + m[11]);
        };
        for (size_t i = 0; i < e->triangles.size(); ++i) {
            const Tri& t = e->triangles[i];
            FlatTri f{};
            f.v0 = xf(t.vertex0); f.v1 = xf(t.vertex1); f.v2 = xf(t.vertex2);
            if (i < e->tri_extra.size()) {
                f.materialId = e->tri_extra[i].materialId;
                const float4& tn = e->tri_extra[i].tint;
                f.tint[0] = tn.x; f.tint[1] = tn.y; f.tint[2] = tn.z; f.tint[3] = tn.w;
            } else {
                f.materialId = static_cast<int>(r.material_id);
                f.tint[0] = 1.0f; f.tint[1] = 1.0f; f.tint[2] = 1.0f; f.tint[3] = 0.0f;
            }
            out.push_back(f);
        }
    }
    return out;
}

bool bake_voxels(const std::vector<FlatTri>& tris, const VoxGenParams& p,
                 uint64_t source_part_hash, VoxelImposter& out) {
    if (tris.empty() || p.maxDim < 1) return false;
    float lo[3]={1e30f,1e30f,1e30f}, hi[3]={-1e30f,-1e30f,-1e30f};
    auto grow=[&](const float3& v){ lo[0]=std::min(lo[0],v.x);lo[1]=std::min(lo[1],v.y);lo[2]=std::min(lo[2],v.z);
                                    hi[0]=std::max(hi[0],v.x);hi[1]=std::max(hi[1],v.y);hi[2]=std::max(hi[2],v.z); };
    for (auto& t:tris){ grow(t.v0); grow(t.v1); grow(t.v2); }
    int nx,ny,nz;
    if (!choose_grid_dims(lo,hi,p.maxDim,nx,ny,nz)) return false;

    out = VoxelImposter{};
    out.source_part_hash = source_part_hash;
    for (int i=0;i<3;++i){ out.bounds_min[i]=lo[i]; out.bounds_max[i]=hi[i]; }
    out.nx=nx; out.ny=ny; out.nz=nz;
    const size_t N=(size_t)nx*ny*nz;
    out.coverage.assign(N,0); out.albedo.assign(N*3,0); out.normal.assign(N*2,0);

    std::vector<float> wsum(N,0.0f), nacc(N*3,0.0f), aacc(N*3,0.0f);
    float cell[3]={ (hi[0]-lo[0])/std::max(1,nx), (hi[1]-lo[1])/std::max(1,ny), (hi[2]-lo[2])/std::max(1,nz) };
    for (int a=0;a<3;++a) if (cell[a]<=0.0f) cell[a]=1e-6f;
    float half[3]={cell[0]*0.5f,cell[1]*0.5f,cell[2]*0.5f};

    for (const FlatTri& t : tris) {
        const float V0[3]={t.v0.x,t.v0.y,t.v0.z}, V1[3]={t.v1.x,t.v1.y,t.v1.z}, V2[3]={t.v2.x,t.v2.y,t.v2.z};
        float tlo[3]={std::min(V0[0],std::min(V1[0],V2[0])),std::min(V0[1],std::min(V1[1],V2[1])),std::min(V0[2],std::min(V1[2],V2[2]))};
        float thi[3]={std::max(V0[0],std::max(V1[0],V2[0])),std::max(V0[1],std::max(V1[1],V2[1])),std::max(V0[2],std::max(V1[2],V2[2]))};
        int x0=std::max(0,(int)std::floor((tlo[0]-lo[0])/cell[0])), x1=std::min(nx-1,(int)std::floor((thi[0]-lo[0])/cell[0]));
        int y0=std::max(0,(int)std::floor((tlo[1]-lo[1])/cell[1])), y1=std::min(ny-1,(int)std::floor((thi[1]-lo[1])/cell[1]));
        int z0=std::max(0,(int)std::floor((tlo[2]-lo[2])/cell[2])), z1=std::min(nz-1,(int)std::floor((thi[2]-lo[2])/cell[2]));
        float e1[3]={V1[0]-V0[0],V1[1]-V0[1],V1[2]-V0[2]}, e2[3]={V2[0]-V0[0],V2[1]-V0[1],V2[2]-V0[2]};
        float fn[3]={e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]};
        float area2=std::sqrt(fn[0]*fn[0]+fn[1]*fn[1]+fn[2]*fn[2]);
        float w=std::max(1e-6f, 0.5f*area2);
        float un[3]={fn[0],fn[1],fn[2]}; if(area2>1e-12f){un[0]/=area2;un[1]/=area2;un[2]/=area2;}
        const MaterialDef* md=MaterialRegistryGet(t.materialId);
        float al[3]={md->albedo[0],md->albedo[1],md->albedo[2]};
        if (t.tint[3]>0.0f){ for(int k=0;k<3;++k) al[k]=al[k]*(1.0f-t.tint[3])+t.tint[k]*t.tint[3]; }
        for (int z=z0;z<=z1;++z) for (int y=y0;y<=y1;++y) for (int x=x0;x<=x1;++x) {
            float bc[3]={lo[0]+(x+0.5f)*cell[0], lo[1]+(y+0.5f)*cell[1], lo[2]+(z+0.5f)*cell[2]};
            if (!tri_box_overlap(bc,half,V0,V1,V2)) continue;
            size_t vi=(size_t)out.voxel_index(x,y,z);
            wsum[vi]+=w;
            for(int k=0;k<3;++k){ nacc[vi*3+k]+=un[k]*w; aacc[vi*3+k]+=al[k]*w; }
        }
    }
    for (size_t vi=0;vi<N;++vi) {
        if (wsum[vi] <= 0.0f) continue;
        out.coverage[vi]=255;
        float inv=1.0f/wsum[vi];
        for(int k=0;k<3;++k) out.albedo[vi*3+k]=(uint8_t)std::min(255.0f,std::max(0.0f, aacc[vi*3+k]*inv*255.0f+0.5f));
        float nn[3]={nacc[vi*3]*inv, nacc[vi*3+1]*inv, nacc[vi*3+2]*inv};
        float l=std::sqrt(nn[0]*nn[0]+nn[1]*nn[1]+nn[2]*nn[2]); if(l<1e-12f){nn[0]=0;nn[1]=0;nn[2]=1;l=1;} else {nn[0]/=l;nn[1]/=l;nn[2]/=l;}
        oct_encode(nn,&out.normal[vi*2]);
    }
    (void)p.coverThresh;
    return true;
}

bool dda_first_hit(const float o[3], const float d[3],
                   int nx,int ny,int nz, const std::vector<uint8_t>& cov,
                   int& hitX,int& hitY,int& hitZ, float& tHit) {
    int dim[3]={nx,ny,nz};
    // Clip the ray to the unit box [0,1]^3 and advance the origin to the entry
    // point so a ray starting outside the box does not snap onto a boundary
    // voxel row (parity with the shader voxelMarch).
    float tEnterBox=-1e30f, tExitBox=1e30f;
    for (int a=0;a<3;++a){
        if (std::fabs(d[a])<1e-12f){
            if (o[a]<0.0f||o[a]>1.0f) return false; // parallel and outside slab
            continue;
        }
        float inv=1.0f/d[a];
        float t0=(0.0f-o[a])*inv, t1=(1.0f-o[a])*inv;
        if (t0>t1){ float tmp=t0; t0=t1; t1=tmp; }
        tEnterBox=std::max(tEnterBox,t0);
        tExitBox=std::min(tExitBox,t1);
    }
    if (tExitBox < std::max(tEnterBox,0.0f)) return false;
    float tStart=std::max(tEnterBox,0.0f);
    float p[3]={o[0]+d[0]*tStart, o[1]+d[1]*tStart, o[2]+d[2]*tStart};
    int vx[3];
    for (int a=0;a<3;++a){ int c=(int)std::floor(p[a]*dim[a]); vx[a]=std::max(0,std::min(dim[a]-1,c)); }
    int step[3]; float tMax[3], tDelta[3];
    for (int a=0;a<3;++a){
        if (std::fabs(d[a])<1e-12f){ step[a]=0; tMax[a]=1e30f; tDelta[a]=1e30f; continue; }
        step[a]=d[a]>0?1:-1;
        float cellSize=1.0f/dim[a];
        float voxelBoundary=(vx[a]+(step[a]>0?1:0))*cellSize;
        tMax[a]=(voxelBoundary-p[a])/d[a];
        tDelta[a]=cellSize/std::fabs(d[a]);
    }
    auto idx=[&](int x,int y,int z){ return (size_t)((z*ny+y)*nx+x); };
    if (cov[idx(vx[0],vx[1],vx[2])]>0){ hitX=vx[0];hitY=vx[1];hitZ=vx[2]; tHit=tStart; return true; }
    for (int guard=0; guard < (nx+ny+nz)*2; ++guard) {
        int axis = (tMax[0]<tMax[1]) ? (tMax[0]<tMax[2]?0:2) : (tMax[1]<tMax[2]?1:2);
        vx[axis]+=step[axis];
        if (vx[axis]<0||vx[axis]>=dim[axis]) return false;
        float tEnter=tStart+tMax[axis];
        tMax[axis]+=tDelta[axis];
        if (cov[idx(vx[0],vx[1],vx[2])]>0){ hitX=vx[0];hitY=vx[1];hitZ=vx[2]; tHit=tEnter; return true; }
    }
    return false;
}

// ---- Serialization -------------------------------------------------------

uint64_t compute_vox_hash(const VoxGenParams& p) {
    return part_asset::fnv1a64(&p, sizeof(p)) ^ static_cast<uint64_t>(kFormatVersion);
}

std::string cache_path(uint64_t hash) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string("imposters/") + buf + ".vxi";
}

bool save(const std::string& path, const VoxelImposter& v, uint64_t vox_hash) {
    // Build body
    std::vector<uint8_t> body;
    put_bytes(body, v.bounds_min, 3*sizeof(float));
    put_bytes(body, v.bounds_max, 3*sizeof(float));
    put<int32_t>(body, static_cast<int32_t>(v.nx));
    put<int32_t>(body, static_cast<int32_t>(v.ny));
    put<int32_t>(body, static_cast<int32_t>(v.nz));
    put<uint32_t>(body, static_cast<uint32_t>(v.coverage.size()));
    put_bytes(body, v.coverage.data(), v.coverage.size());
    put<uint32_t>(body, static_cast<uint32_t>(v.albedo.size()));
    put_bytes(body, v.albedo.data(), v.albedo.size());
    put<uint32_t>(body, static_cast<uint32_t>(v.normal.size()));
    put_bytes(body, v.normal.data(), v.normal.size());

    const uint64_t content_hash = part_asset::fnv1a64(body.data(), body.size());

    // Build header
    std::vector<uint8_t> head;
    put<uint32_t>(head, kMagic);
    put<uint32_t>(head, kFormatVersion);
    put<uint64_t>(head, vox_hash);
    put<uint64_t>(head, v.source_part_hash);
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

bool load(const std::string& path, uint64_t expected_vox_hash,
          uint64_t expected_source_hash, VoxelImposter& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f,0,SEEK_END); long sz=std::ftell(f); std::fseek(f,0,SEEK_SET);
    // Minimum header size: magic(4)+version(4)+vox_hash(8)+source_hash(8)+content_hash(8) = 32
    if (sz < 32) { std::fclose(f); return false; }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    bool read_ok = std::fread(buf.data(),1,buf.size(),f)==buf.size();
    std::fclose(f);
    if (!read_ok) return false;

    Reader r{ buf.data(), buf.data()+buf.size() };
    const uint32_t magic   = r.get<uint32_t>();
    const uint32_t version = r.get<uint32_t>();
    const uint64_t vhash   = r.get<uint64_t>();
    const uint64_t shash   = r.get<uint64_t>();
    const uint64_t content = r.get<uint64_t>();
    if (!r.ok)                          return false;
    if (magic   != kMagic)              return false;
    if (version != kFormatVersion)      return false;
    if (vhash   != expected_vox_hash)   return false;
    if (shash   != expected_source_hash)return false;
    if (part_asset::fnv1a64(r.p, static_cast<size_t>(r.end - r.p)) != content) return false;

    VoxelImposter tmp_out{};
    tmp_out.source_part_hash = shash;
    const uint8_t* bmin = r.take(3*sizeof(float));
    const uint8_t* bmax = r.take(3*sizeof(float));
    if (!r.ok) return false;
    std::memcpy(tmp_out.bounds_min, bmin, 3*sizeof(float));
    std::memcpy(tmp_out.bounds_max, bmax, 3*sizeof(float));
    tmp_out.nx = static_cast<int>(r.get<int32_t>());
    tmp_out.ny = static_cast<int>(r.get<int32_t>());
    tmp_out.nz = static_cast<int>(r.get<int32_t>());
    if (!r.ok) return false;

    const uint32_t cov_n = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* cov_p = r.take(cov_n);
    if (!r.ok) return false;
    tmp_out.coverage.assign(cov_p, cov_p+cov_n);

    const uint32_t alb_n = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* alb_p = r.take(alb_n);
    if (!r.ok) return false;
    tmp_out.albedo.assign(alb_p, alb_p+alb_n);

    const uint32_t nrm_n = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* nrm_p = r.take(nrm_n);
    if (!r.ok) return false;
    tmp_out.normal.assign(nrm_p, nrm_p+nrm_n);

    // The content hash only proves the body is self-consistent, not that the
    // dims agree with the blob sizes. Consumers index coverage/albedo/normal by
    // nx*ny*nz, so a crafted file with large dims but short blobs would drive
    // out-of-bounds reads downstream. Reject any mismatch here.
    if (tmp_out.nx <= 0 || tmp_out.ny <= 0 || tmp_out.nz <= 0) return false;
    const uint64_t n = static_cast<uint64_t>(tmp_out.nx) * tmp_out.ny * tmp_out.nz;
    if (cov_n != n || alb_n != n * 3 || nrm_n != n * 2) return false;

    out = std::move(tmp_out);
    return true;
}

} // namespace voxel_imposter
