#include "finite_surface_stamp.h"
#include "part_asset.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <cstdio>

namespace surface_stamp {
namespace {
using namespace gpu_meshing;
constexpr std::size_t max_bytes=128u*1024u*1024u;
bool fail(Error &e, ErrorCode code, const char *text) { e={code,text}; return false; }
bool current(const FaceMaterialJob &j,const BuildControl &c,Error &e) {
    if (c.cancelled && c.cancelled()) return fail(e,ErrorCode::Cancelled,"stamp preparation cancelled");
    if (c.generation_is_current && !c.generation_is_current(j.geometry_job.source.generation))
        return fail(e,ErrorCode::StaleGeneration,"stamp generation stale");
    return true;
}
void accumulate(Channels &a,const Channels &b,float w) {
    for (int i=0;i<4;++i) {
        a.albedo_coverage[i]+=b.albedo_coverage[i]*w;
        a.orm_height[i]+=b.orm_height[i]*w;
        a.normal_detail[i]+=b.normal_detail[i]*w;
        a.geometric_reserved[i]+=b.geometric_reserved[i]*w;
    }
}
bool valid(const FaceMaterialTexel &t,const FaceMaterialPatch &p,const FaceTexel &g) {
    if (t.coverage!=g.coverage || !std::isfinite(t.detail_height_m)) return false;
    float norm=0;
    for (float n:{t.normal_uvn.x,t.normal_uvn.y,t.normal_uvn.z}) {
        if (!std::isfinite(n) || (!t.coverage && n!=0)) return false;
        norm+=n*n;
    }
    for (int c=0;c<3;++c) if (!std::isfinite(t.albedo[c]) || t.albedo[c]<0 ||
        t.albedo[c]>terrain_field::kSurfaceTintMax || !std::isfinite(t.orm[c]) ||
        t.orm[c]<0 || t.orm[c]>1 || (!t.coverage && (t.albedo[c]!=0 || t.orm[c]!=0))) return false;
    return t.coverage ? std::abs(norm-1)<.001f && t.detail_height_m>=p.detail_min_m &&
        t.detail_height_m<=p.detail_max_m : t.detail_height_m==0;
}
Channels filtered_level(const Stamp &s,std::size_t level,float u,float v,float footprint) {
    const auto &l=s.levels[level];
    // Box integration with a minimum width of one source pixel is exactly
    // bilinear at magnification. Beyond the final mip it still attenuates a
    // finite source by its actual covered fraction of the receiver footprint.
    const float hx=.5f*std::max(1.f/l.width,footprint/s.domain[2]);
    const float hy=.5f*std::max(1.f/l.height,footprint/s.domain[3]);
    if (!std::isfinite(hx) || !std::isfinite(hy)) return {};
    const float x0=std::max(0.f,(u-hx)*l.width),x1=std::min(float(l.width),(u+hx)*l.width);
    const float y0=std::max(0.f,(v-hy)*l.height),y1=std::min(float(l.height),(v+hy)*l.height);
    Channels result;
    if (x1<=x0 || y1<=y0) return result;
    // LOD selection bounds the overlaps to at most 3x3 source pixels (1x1
    // after the last mip). Clip in float before integer conversion.
    for (std::uint32_t y=std::uint32_t(y0);y<std::uint32_t(std::ceil(y1));++y)
        for (std::uint32_t x=std::uint32_t(x0);x<std::uint32_t(std::ceil(x1));++x) {
            const float wx=(std::min(x1,float(x+1))-std::max(x0,float(x)))/(2*hx*l.width);
            const float wy=(std::min(y1,float(y+1))-std::max(y0,float(y)))/(2*hy*l.height);
            accumulate(result,s.pixels[l.offset+std::size_t(y)*l.width+x],wx*wy);
        }
    return result;
}
void normalize(float *n) {
    const float d=n[0]*n[0]+n[1]*n[1]+n[2]*n[2];
    if (d<1e-20f) { n[0]=n[1]=0; n[2]=1; return; }
    const float inv=1/std::sqrt(d);
    for (int i=0;i<3;++i) n[i]*=inv;
}
// Continue the normal-offset field from the nearest covered filter boundary.
// Switching to a wider filter at exactly zero coverage changes the offset
// discontinuously: inverse iteration can bounce across a chipped silhouette.
// Color/coverage still sample the original finite image at the solved point.
Channels boundary_offset(const Stamp& s,float u,float v,float pu,float pv) {
    if (!std::isfinite(u)||!std::isfinite(v)||u<s.domain[0]-2*pu||v<s.domain[1]-2*pv||
        u>s.domain[0]+s.domain[2]+2*pu||v>s.domain[1]+s.domain[3]+2*pv) return {};
    const auto& level=s.levels.front();
    const int x=int(std::floor((u-s.domain[0])/pu)),y=int(std::floor((v-s.domain[1])/pv));
    float best=std::numeric_limits<float>::max(),qu=0,qv=0;
    for(int by=std::max(0,y-2);by<=std::min(int(level.height)-1,y+2);++by)
        for(int bx=std::max(0,x-2);bx<=std::min(int(level.width)-1,x+2);++bx) {
            if(s.pixels[level.offset+size_t(by)*level.width+bx].albedo_coverage[3]<=0) continue;
            const float cu=s.domain[0]+(bx+.5f)*pu,cv=s.domain[1]+(by+.5f)*pv;
            // A bilinear texel's support extends one pitch from its centre.
            // Step slightly inside that support to evaluate its limiting value.
            const float a=std::clamp(u,cu-pu*.9999f,cu+pu*.9999f);
            const float b=std::clamp(v,cv-pv*.9999f,cv+pv*.9999f);
            const float distance=(a-u)*(a-u)+(b-v)*(b-v);
            if(distance<best){best=distance;qu=a;qv=b;}
        }
    return best==std::numeric_limits<float>::max()?Channels{}:sample(s,qu,qv,0);
}
bool build_mips(Stamp &s,const FaceMaterialJob &j,const BuildControl &control,Error &e) {
        for (std::size_t level=1;level<s.levels.size();++level) {
            const auto &a=s.levels[level-1],&b=s.levels[level];
            // Exact area overlap preserves the whole domain for odd sizes.
            // Dropping the final row/column would change a physical brick's
            // coverage and material average as it recedes.
            const double scale_x=double(a.width)/b.width,scale_y=double(a.height)/b.height;
            for (std::uint32_t y=0;y<b.height;++y) {
                if (!current(j,control,e)) return false;
                const double y0=y*scale_y,y1=(y+1)*scale_y;
                for (std::uint32_t x=0;x<b.width;++x) {
                    if (!(x&1023) && !current(j,control,e)) return false;
                    const double x0=x*scale_x,x1=(x+1)*scale_x;
                    auto &dst=s.pixels[b.offset+std::size_t(y)*b.width+x]; dst={};
                    for (std::uint32_t sy=std::uint32_t(y0);sy<std::min(a.height,std::uint32_t(std::ceil(y1)));++sy)
                        for (std::uint32_t sx=std::uint32_t(x0);sx<std::min(a.width,std::uint32_t(std::ceil(x1)));++sx) {
                            const double weight=(std::min(x1,double(sx+1))-std::max(x0,double(sx)))*
                                (std::min(y1,double(sy+1))-std::max(y0,double(sy)))/(scale_x*scale_y);
                            accumulate(dst,s.pixels[a.offset+std::size_t(sy)*a.width+sx],float(weight));
                        }
                }
            }
        }
    return true;
}
} // namespace
static_assert(sizeof(Channels)==64 && sizeof(Level)==16,"finite stamp std430 ABI");
bool prepare(const gpu_meshing::FaceMaterialJob &j,const gpu_meshing::FaceMaterialPatch &material,
             std::shared_ptr<const Stamp> &out,gpu_meshing::Error &e,const gpu_meshing::BuildControl &control) {
    using namespace gpu_meshing;
    try {
        PreparedFaceMaterial expected;
        if (!prepare_face_material(j,expected,e,control)) return false;
        const auto &m=expected.metadata;
        if (material.geometry_digest!=m.geometry_digest || material.recipe_digest!=m.recipe_digest ||
            material.width!=m.width || material.height!=m.height || material.footprint_m!=m.footprint_m ||
            material.detail_min_m!=m.detail_min_m || material.detail_max_m!=m.detail_max_m ||
            material.texels.size()!=j.geometry->texels.size())
            return fail(e,ErrorCode::ArtifactFailure,"stamp material/geometry identity mismatch");
        auto s=std::make_shared<Stamp>();
        const auto &g=*j.geometry;
        s->frame=g.frame;
        s->domain[0]=g.u_min_m; s->domain[1]=g.v_min_m;
        s->domain[2]=g.u_max_m-g.u_min_m; s->domain[3]=g.v_max_m-g.v_min_m;
        s->height_min_m=g.height_min_m; s->height_max_m=g.height_max_m;
        s->detail_min_m=m.detail_min_m; s->detail_max_m=m.detail_max_m;
        s->geometry_digest=g.recipe_digest; s->material_digest=m.recipe_digest;
        const std::uint64_t words[]={filter_version,s->geometry_digest,s->material_digest};
        std::uint8_t bytes[sizeof(words)];
        for (std::size_t i=0;i<3;++i) for (unsigned b=0;b<8;++b) bytes[i*8+b]=std::uint8_t(words[i]>>(b*8));
        s->content_digest=part_asset::fnv1a64(bytes,sizeof(bytes));
        std::size_t count=0;
        for (std::uint32_t w=m.width,h=m.height;;w=std::max(1u,w/2),h=std::max(1u,h/2)) {
            const std::size_t area=std::size_t(w)*h;
            if (area>max_bytes/sizeof(Channels)-count)
                return fail(e,ErrorCode::LimitExceeded,"finite stamp exceeds preparation memory limit");
            s->levels.push_back({std::uint32_t(count),w,h,0}); count+=area;
            if (w==1 && h==1) break;
        }
        s->pixels.resize(count);
        for (std::size_t i=0;i<material.texels.size();++i) {
            if (!(i&1023) && !current(j,control,e)) return false;
            const auto &t=material.texels[i]; const auto &gt=g.texels[i];
            if (!valid(t,material,gt)) return fail(e,ErrorCode::ArtifactFailure,"invalid stamp material texel");
            if (!t.coverage) continue;
            auto &p=s->pixels[i];
            std::copy(t.albedo,t.albedo+3,p.albedo_coverage); p.albedo_coverage[3]=1;
            std::copy(t.orm,t.orm+3,p.orm_height); p.orm_height[1]*=p.orm_height[1]; p.orm_height[3]=gt.height_m;
            p.normal_detail[0]=t.normal_uvn.x; p.normal_detail[1]=t.normal_uvn.y;
            p.normal_detail[2]=t.normal_uvn.z; p.normal_detail[3]=t.detail_height_m;
            p.geometric_reserved[0]=gt.normal_uvn.x; p.geometric_reserved[1]=gt.normal_uvn.y;
            p.geometric_reserved[2]=gt.normal_uvn.z;
        }
        if (!build_mips(*s,j,control,e)) return false;
        if (!current(j,control,e)) return false;
        out=std::move(s); return true;
    } catch (const std::bad_alloc &) { return fail(e,ErrorCode::LimitExceeded,"stamp allocation failed"); }
}
bool prepare_projected(const gpu_meshing::FaceMaterialJob &j,const gpu_meshing::FaceMaterialPatch &material,
                       std::shared_ptr<const Stamp> &out,gpu_meshing::Error &e,const gpu_meshing::BuildControl &control) {
    using namespace gpu_meshing;
    try {
        std::shared_ptr<const Stamp> raw;
        if (!prepare(j,material,raw,e,control)) return false;
        if (raw->pixels.size()>max_bytes/(2*sizeof(Channels)))
            return fail(e,ErrorCode::LimitExceeded,"projected stamp working set exceeds limit");
        auto s=std::make_shared<Stamp>(*raw);
        const auto &base=s->levels.front();
        const float pu=s->domain[2]/base.width,pv=s->domain[3]/base.height;
        const float tolerance=std::max(1e-8f,std::min(.000001f,.001f*std::min(pu,pv)));
        // A normal offset is a lateral warp as well as a depth change. Solve
        // q + detail(q)*geometric_normal.xy(q) = destination_uv, then read ALL
        // channels at q. This is exact for a tilted plane + constant offset;
        // simply adding either detail or detail*N.z at destination_uv is not.
        bool covered=false;
        s->height_projection=1; s->projection_error_m=0;
        s->height_min_m=std::numeric_limits<float>::max();
        s->height_max_m=-std::numeric_limits<float>::max();
        for (std::uint32_t y=0;y<base.height;++y) for (std::uint32_t x=0;x<base.width;++x) {
            const std::size_t index=std::size_t(y)*base.width+x;
            if (!(index&255) && !current(j,control,e)) return false;
            const float u=s->domain[0]+(x+.5f)*pu,v=s->domain[1]+(y+.5f)*pv;
            float qu=u,qv=v;
            Channels p; bool converged=false;
            for (unsigned iteration=0;iteration<24;++iteration) {
                p=sample(*raw,qu,qv,0);
                auto warp=p;
                // Extend only the offset field into a one-pixel transparent
                // fringe so a positive normal offset may expand the silhouette.
                // Final color/coverage still come from the ordinary finite sample.
                if (warp.albedo_coverage[3]==0) warp=boundary_offset(*raw,qu,qv,pu,pv);
                const float du=warp.normal_detail[3]*warp.geometric_reserved[0];
                const float dv=warp.normal_detail[3]*warp.geometric_reserved[1];
                const float residual=std::max(std::abs(qu+du-u),std::abs(qv+dv-v));
                if (residual<=tolerance) {
                    s->projection_error_m=std::max(s->projection_error_m,residual);
                    converged=true; break;
                }
                qu=u-du; qv=v-dv;
            }
            if (!converged) {
                char message[256];std::snprintf(message,sizeof(message),
                    "source normal-offset projection did not converge: pixel=%u,%u size=%u,%u uv=%.9g,%.9g q=%.9g,%.9g coverage=%.9g detail=%.9g",
                    x,y,base.width,base.height,u,v,qu,qv,p.albedo_coverage[3],p.normal_detail[3]);
                e={ErrorCode::ArtifactFailure,message};return false;
            }
            auto &dst=s->pixels[index]; dst={};
            const float coverage=p.albedo_coverage[3];
            if (coverage<=0) continue;
            covered=true;
            p.orm_height[3]+=p.normal_detail[3]*p.geometric_reserved[2];
            p.normal_detail[3]=0;
            s->height_min_m=std::min(s->height_min_m,p.orm_height[3]);
            s->height_max_m=std::max(s->height_max_m,p.orm_height[3]);
            p.orm_height[1]*=p.orm_height[1];
            for (int c=0;c<4;++c) {
                dst.albedo_coverage[c]=c==3?coverage:p.albedo_coverage[c]*coverage;
                dst.orm_height[c]=p.orm_height[c]*coverage;
                dst.normal_detail[c]=p.normal_detail[c]*coverage;
                dst.geometric_reserved[c]=p.geometric_reserved[c]*coverage;
            }
        }
        if (!covered) s->height_min_m=s->height_max_m=0;
        s->detail_min_m=s->detail_max_m=0;
        // Domain-separate the new representation from the raw two-axis source.
        const std::uint64_t words[]={0x70726f6a7374616dull,2,raw->content_digest};
        std::uint8_t bytes[sizeof(words)];
        for (std::size_t i=0;i<3;++i) for (unsigned b=0;b<8;++b) bytes[i*8+b]=std::uint8_t(words[i]>>(8*b));
        s->content_digest=part_asset::fnv1a64(bytes,sizeof(bytes));
        if (!build_mips(*s,j,control,e) || !current(j,control,e)) return false;
        out=std::move(s); return true;
    } catch (const std::bad_alloc &) { return fail(e,ErrorCode::LimitExceeded,"projected stamp allocation failed"); }
}
Channels sample(const Stamp &s,float u_m,float v_m,float footprint_m) {
    Channels result;
    if (s.levels.empty() || !std::isfinite(u_m) || !std::isfinite(v_m) ||
        !std::isfinite(footprint_m) || footprint_m<0) return result;
    const float u=(u_m-s.domain[0])/s.domain[2],v=(v_m-s.domain[1])/s.domain[3];
    if (!std::isfinite(u) || !std::isfinite(v)) return result;
    const auto &base=s.levels.front();
    const float pitch=std::min(s.domain[2]/base.width,s.domain[3]/base.height);
    const float lod=std::min(float(s.levels.size()-1),std::log2(std::max(1.f,footprint_m/pitch)));
    const std::size_t lo=std::size_t(lod),hi=std::min(lo+1,s.levels.size()-1);
    const float f=lod-float(lo);
    accumulate(result,filtered_level(s,lo,u,v,footprint_m),1-f);
    if (f>0) accumulate(result,filtered_level(s,hi,u,v,footprint_m),f);
    const float coverage=result.albedo_coverage[3];
    if (coverage<=0) return {};
    for (int c=0;c<4;++c) {
        if (c<3) result.albedo_coverage[c]/=coverage;
        result.orm_height[c]/=coverage; result.normal_detail[c]/=coverage;
        result.geometric_reserved[c]/=coverage;
    }
    result.orm_height[1]=std::sqrt(std::max(0.f,result.orm_height[1]));
    normalize(result.normal_detail); normalize(result.geometric_reserved);
    return result;
}
} // namespace surface_stamp
