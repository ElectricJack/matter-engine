#pragma once

#include "vt_snapshot.h"
#include "vt_surface_tape.h"
#include <cmath>
#include <limits>

namespace vt {

inline uint32_t vt_periodic_tail_mip(const VtPeriodicDomain& d) {
    uint32_t mip=0;
    while(mip<7 && (std::max(d.width>>mip,1u)>64 || std::max(d.height>>mip,1u)>64))++mip;
    return mip;
}

inline bool vt_valid_periodic_domain(const VtPeriodicDomain& d) {
    if(d.version!=1 || !d.width || !d.height || d.width>8192 || d.height>8192) return false;
    // Every supported mip must repeat at the same BC block phase. A one- or
    // two-texel period also divides the four-texel block and is safe.
    for(uint32_t mip=0;mip<=vt_periodic_tail_mip(d);++mip)
        for(uint32_t size:{std::max(d.width>>mip,1u),std::max(d.height>>mip,1u)})
            if(size>2 && size%4)return false;
    for(float x:d.origin) if(!std::isfinite(x)) return false;
    for(float x:d.period) if(!(x>0) || !std::isfinite(x)) return false;
    const auto dot=[](const float* a,const float* b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    for(const auto* axis:{d.u,d.v,d.n}) {
        for(int k=0;k<3;++k)if(!std::isfinite(axis[k]))return false;
        if(std::abs(dot(axis,axis)-1)>1e-4f)return false;
    }
    const float cross[]={d.u[1]*d.v[2]-d.u[2]*d.v[1],d.u[2]*d.v[0]-d.u[0]*d.v[2],d.u[0]*d.v[1]-d.u[1]*d.v[0]};
    return std::abs(dot(d.u,d.v))<=1e-4f && dot(cross,d.n)>=.9999f;
}

inline bool vt_make_periodic_domain(gpu_meshing::FaceFrame frame,
    std::array<float,2> period,float texels_per_m,VtPeriodicDomain& out,std::string& error) {
    if(!(texels_per_m>0) || !std::isfinite(texels_per_m)) {error="invalid periodic material density";return false;}
    VtPeriodicDomain d;d.version=1;
    const matter::Float3 axes[]={frame.origin_m,frame.u,frame.v,frame.n};
    float* rows[]={d.origin,d.u,d.v,d.n};
    for(int i=0;i<4;++i){rows[i][0]=axes[i].x;rows[i][1]=axes[i].y;rows[i][2]=axes[i].z;}
    for(int i=0;i<2;++i) {
        const double pixels=std::ceil(double(period[i])*texels_per_m);
        if(!(pixels>=1 && pixels<=8192)) {error="periodic material extent/density exceeds 1..8192 texels";return false;}
        d.period[i]=period[i];(i?d.height:d.width)=uint32_t(pixels);
    }
    // Round density upward without rescaling physical geometry. Larger axes
    // need multiples of 4*2^tail; shorter ones can reach the valid 2/1-texel
    // cases earlier. This preserves NPOT domains where the mip chain permits.
    for(;;) {
        const uint32_t tail=vt_periodic_tail_mip(d),alignment=4u<<tail;
        for(uint32_t* size:{&d.width,&d.height}) {
            if(*size<alignment) {
                uint32_t rounded=1;while(rounded<*size)rounded*=2;*size=rounded;
            } else *size=(*size+alignment-1)/alignment*alignment;
        }
        // Rounding just above a tail boundary (e.g. 259 -> 272) can add
        // another mip. Re-align for that mip rather than accepting its seam.
        if(vt_periodic_tail_mip(d)==tail)break;
    }
    if(!vt_valid_periodic_domain(d)){error="periodic material requires a rigid right-handed frame";return false;}
    out=d;error.clear();return true;
}

// Repeat source REFERENCES, never pixels. The conservative support covers the
// widest module mip plus finite-source mip filtering and its normal derivative.
// A brick crossing the repeat cut stays one physical brick in each repetition.
// Explicit budgets fail without changing the caller's previous complete result.
inline bool vt_periodic_source_catalog(const VtPeriodicDomain& d,
    const std::vector<VtFiniteSourceBinding>& sources,
    std::shared_ptr<const VtFiniteSources>& out,std::string& error) {
    if(!vt_valid_periodic_domain(d)){error="invalid periodic source domain";return false;}
    if(sources.empty()){out.reset();error.clear();return true;}
    const uint32_t mip=vt_periodic_tail_mip(d);
    const double support=std::max(double(d.period[0])/std::max(d.width>>mip,1u),
                                  double(d.period[1])/std::max(d.height>>mip,1u));
    std::vector<VtFiniteSourceBinding> repeated;
    VtFiniteReceiver receiver;
    const auto f3=[](const float* p){return matter::Float3{p[0],p[1],p[2]};};
    receiver.frame={f3(d.origin),f3(d.u),f3(d.v),f3(d.n)};
    receiver.domain={0,0,d.period[0],d.period[1]};
    for(const auto& source:sources) {
        if(!source.stamp){error="missing periodic source payload";return false;}
        const auto& f=source.frame;const auto& sd=source.stamp->domain;
        const float facing=f.n.x*d.n[0]+f.n.y*d.n[1]+f.n.z*d.n[2];
        const float depth=(d.origin[0]-f.origin_m.x)*f.n.x+
            (d.origin[1]-f.origin_m.y)*f.n.y+(d.origin[2]-f.origin_m.z)*f.n.z;
        if(!(facing>=.9999f) || !std::isfinite(depth) || !std::isfinite(source.datum_m) ||
           std::abs(depth-source.datum_m)>.00005f) {
            error="periodic source projection must match its module plane and datum";return false;
        }
        double lo[2]={INFINITY,INFINITY},hi[2]={-INFINITY,-INFINITY};
        for(int corner=0;corner<4;++corner) {
            const double u=sd[0]+((corner&1)?1.5:-.5)*sd[2];
            const double v=sd[1]+((corner&2)?1.5:-.5)*sd[3];
            const double p[]={f.origin_m.x+u*f.u.x+v*f.v.x-d.origin[0],
                f.origin_m.y+u*f.u.y+v*f.v.y-d.origin[1],f.origin_m.z+u*f.u.z+v*f.v.z-d.origin[2]};
            for(int a=0;a<2;++a) {
                const float* axis=a?d.v:d.u;
                const double q=p[0]*axis[0]+p[1]*axis[1]+p[2]*axis[2];
                lo[a]=std::min(lo[a],q);hi[a]=std::max(hi[a],q);
            }
        }
        int first[2],last[2];
        for(int a=0;a<2;++a) {
            const double guard=1e-5*(1+std::abs(lo[a])+std::abs(hi[a])+d.period[a]);
            const double begin=std::ceil((-support-guard-hi[a])/d.period[a]);
            const double end=std::floor((d.period[a]+support+guard-lo[a])/d.period[a]);
            if(!std::isfinite(begin)||!std::isfinite(end)||begin<-1000000||end>1000000||end<begin||end-begin>24576) {
                error="periodic source repetition exceeds bounded support";return false;
            }
            first[a]=int(begin);last[a]=int(end);
        }
        const size_t count=size_t(last[0]-first[0]+1)*size_t(last[1]-first[1]+1);
        if(count>24576-repeated.size()){error="periodic source references exceed 24576";return false;}
        for(int y=first[1];y<=last[1];++y)for(int x=first[0];x<=last[0];++x) {
            auto b=source;
            const double u=double(x)*d.period[0],v=double(y)*d.period[1];
            b.frame.origin_m.x=float(f.origin_m.x+u*d.u[0]+v*d.v[0]);
            b.frame.origin_m.y=float(f.origin_m.y+u*d.u[1]+v*d.v[1]);
            b.frame.origin_m.z=float(f.origin_m.z+u*d.u[2]+v*d.v[2]);
            receiver.sources.push_back(uint32_t(repeated.size()));repeated.push_back(std::move(b));
        }
    }
    return vt_make_finite_sources(repeated,out,error,{receiver});
}

// Complete immutable producer inputs, reusable by any number of receivers. It
// can enter ordinary VT residency under context.variant_hash without owning a
// drawn mesh. Publication and module/receiver lifetime binding remain separate.
inline bool vt_make_periodic_material(const VtPeriodicDomain& d,
    const std::vector<VtFiniteSourceBinding>& sources,const std::string& base_program,
    uint32_t carrier,std::shared_ptr<const VtPartSnapshot>& out,std::string& error) {
    if(!vt_valid_periodic_domain(d) || carrier>255){error="invalid periodic material domain/carrier";return false;}
    terrain_field::SurfaceProgram program;VtSurfaceTapePack packed;
    if(!terrain_field::SurfaceProgram::parse(base_program,program,error))return false;
    if(program.source.version!=1 || program.uses_world_inputs() ||
       !vt_pack_surface_tape(program,false,packed) || packed.scan.count || packed.weight_reg_count!=1) {
        error="periodic material requires one local direct source without receiver field lanes";return false;
    }
    std::shared_ptr<const VtFiniteSources> catalog;
    if(!vt_periodic_source_catalog(d,sources,catalog,error))return false;
    uint64_t hash=vt_finite_hash_word(14695981039346656037ull,0x504552494f440001ull);
    for(uint64_t word:{uint64_t(d.width),uint64_t(d.height),uint64_t(carrier),program.hash(),catalog?catalog->content_hash:0})
        hash=vt_finite_hash_word(hash,word);
    for(const float* row:{d.origin,d.u,d.v,d.n,d.period}) {
        const int n=row==d.period?2:3;
        for(int k=0;k<n;++k){uint32_t bits;std::memcpy(&bits,row+k,4);hash=vt_finite_hash_word(hash,bits);}
    }
    float positions[12],normals[12],uv[8];
    const uint32_t indices[]={0,1,2,0,2,3},ids[]={1,1,1,1};const uint8_t weights[]={255,255,255,255};
    for(int i=0;i<4;++i) {
        const float u=(i==1||i==2)?1.f:0.f,v=i>=2?1.f:0.f;
        uv[i*2]=u;uv[i*2+1]=v;
        for(int k=0;k<3;++k){positions[i*3+k]=d.origin[k]+u*d.period[0]*d.u[k]+v*d.period[1]*d.v[k];normals[i*3+k]=d.n[k];}
    }
    chart_atlas::ChartAtlasRung atlas;atlas.atlas_w=d.width;atlas.atlas_h=d.height;
    atlas.tri_order={0,1};atlas.charts.resize(1);
    auto& chart=atlas.charts[0];chart.rect_w=d.width;chart.rect_h=d.height;chart.tri_count=2;
    chart.texels_per_meter=std::max(d.width/d.period[0],d.height/d.period[1]);
    std::copy(d.origin,d.origin+3,chart.origin);std::copy(d.u,d.u+3,chart.tangent);std::copy(d.v,d.v+3,chart.bitangent);
    VtPartContext ctx;ctx.variant_hash=hash;ctx.rung_count=1;ctx.atlas=&atlas;
    ctx.positions=positions;ctx.normals=normals;ctx.surface_uvs=uv;ctx.vertex_count=4;
    ctx.indices=indices;ctx.triangle_count=2;ctx.dominant_material=carrier;
    ctx.surface_weights=weights;ctx.surface_materials=&carrier;ctx.surface_material_count=1;
    ctx.surface_tape_text=base_program.c_str();ctx.surface_tape_hash=hash;
    ctx.finite_sources=catalog;ctx.finite_source_ids=catalog?ids:nullptr;ctx.periodic=d;
    out=VtPartSnapshot::capture(atlas,ctx);error.clear();return true;
}
} // namespace vt
