#pragma once
#include "vt_periodic_material.h"
#include "vt_canonical_page.h"
#include "vt_chart_gpu.h"

namespace vt {
class VtMaterialModule;
struct VtReceiverMaterialState;

// Authoring is independent of the material producer. The frame is in receiver
// local metres; its origin is module UV (0,0), before the optional phase.
// This version maps planar charts. Curved receivers need a surface-coordinate
// field; projecting them onto an arbitrary plane is deliberately rejected.
struct VtReceiverMaterialChart {
    uint32_t chart = 0;
    std::shared_ptr<const VtMaterialModule> module;
    gpu_meshing::FaceFrame frame;
    std::array<float,2> phase{};
    std::array<float,2> u_range_m{-1e20f,1e20f}; // finite edge treatments outside this interval
    float datum_m = 0;
};

// std430, shared with vt_common.glsl. One table per receiver parameterization,
// never one per page. A zero binding leaves that chart on its finite material.
struct VtReceiverMaterialGpu {
    uint32_t binding[4]{}; // slot, generation, float bits of unwrapped U min/max
    float uv_u[4]{}; // atlas UV -> module U, affine offset, LOD bias
    float uv_v[4]{}; // atlas UV -> module V, affine offset, signed height datum
    float normal_xy[4]{1,0,0,1}; // row-major module -> receiver canonical XY
    float metrics[4]{}; // module period (metres), common height min/range
};
static_assert(sizeof(VtReceiverMaterialGpu)==80, "vt_common.glsl mapping layout");

// Conservative whole-stored-page proof, including filter gutters. Partial
// charts, finite ends, holes and uncertain geometry keep their full producer.
// The canonical helper proves a solid planar rectangle; its pixel identity is
// deliberately irrelevant here because module and receiver page phases differ.
inline bool vt_receiver_material_page(const VtPartSnapshot& receiver,
    const std::vector<VtReceiverMaterialGpu>& mappings,
    uint32_t mip,uint32_t px,uint32_t py) {
    if(!receiver.geometry || mappings.empty() || mip>=8) return false;
    const auto& atlas=receiver.geometry->atlas;
    std::vector<uint32_t> candidates;
    vt_page_candidate_charts(atlas,px,py,mip,candidates);
    if(candidates.size()!=1 || candidates[0]>=mappings.size()) return false;
    const uint32_t chart=candidates[0];const auto& mapping=mappings[chart];
    if(!mapping.binding[0]) return false;
    const auto& ctx=receiver.context;
    if(ctx.surface_material_count!=1 || !ctx.surface_materials) return false;
    const uint32_t carrier=ctx.surface_materials[0];
    if(carrier>255 || (!ctx.material_ids && ctx.dominant_material!=carrier)) return false;
    VtFillRequest request;request.atlas=&atlas;request.part_context=&ctx;
    request.mip=uint16_t(mip);request.page_x=uint16_t(px);request.page_y=uint16_t(py);
    VtCanonicalPage covered;
    if(!vt_canonical_page(request,chart,{},covered)) return false;
    if(ctx.material_ids) {
        const auto& c=atlas.charts[chart];
        for(uint32_t t=0;t<c.tri_count;++t) for(uint32_t k=0;k<3;++k)
            if(ctx.material_ids[ctx.indices[atlas.tri_order[c.first_tri+t]*3+k]]!=carrier) return false;
    }
    float bounds[2];std::memcpy(bounds,mapping.binding+2,sizeof(bounds));
    if(!std::isfinite(bounds[0]) || !std::isfinite(bounds[1]) || bounds[0]>=bounds[1]) return false;
    const double scale=double(1u<<mip);
    for(uint32_t corner=0;corner<4;++corner) {
        const double u=(double(px)*128+(corner&1?132:-4))*scale/atlas.atlas_w;
        const double v=(double(py)*128+(corner&2?132:-4))*scale/atlas.atlas_h;
        const double x=u*mapping.uv_u[0]+v*mapping.uv_u[1]+mapping.uv_u[2];
        const double guard=1e-5*(1+std::abs(u*mapping.uv_u[0])+std::abs(v*mapping.uv_u[1])+std::abs(double(mapping.uv_u[2])));
        if(!std::isfinite(x) || x-guard<bounds[0] || x+guard>bounds[1]) return false;
    }
    return true;
}

inline bool vt_receiver_height_range(const VtPartSnapshot& input,
    std::array<float,2>& range,std::string& error) {
    terrain_field::SurfaceProgram program;
    if (!input.surface || !terrain_field::SurfaceProgram::parse(input.surface->tape_text,program,error) ||
        program.source.version!=1) {
        error="receiver material mapping requires a direct-source finite fallback";return false;
    }
    range={program.source.height_min,program.source.height_max};
    if (input.context.finite_sources) {
        range[0]=std::min(range[0],input.context.finite_sources->height_min_m-.00005f);
        range[1]=std::max(range[1],input.context.finite_sources->height_max_m+.00005f);
    }
    return std::isfinite(range[0]) && std::isfinite(range[1]) && range[1]>=range[0];
}

inline bool vt_receiver_material_chart(const VtPartSnapshot& receiver,
    const VtPeriodicDomain& domain,const VtReceiverMaterialChart& mapping,
    VtReceiverMaterialGpu& out,std::string& error) {
    const auto& atlas=receiver.geometry->atlas;
    const auto fail=[&](const char* reason){error=reason;return false;};
    if (mapping.chart>=atlas.charts.size() || !vt_valid_periodic_domain(domain))
        return fail("invalid receiver chart or module domain");
    VtPeriodicDomain frame=domain;
    const matter::Float3 axes[]={mapping.frame.origin_m,mapping.frame.u,mapping.frame.v,mapping.frame.n};
    float* rows[]={frame.origin,frame.u,frame.v,frame.n};
    for(int i=0;i<4;++i){rows[i][0]=axes[i].x;rows[i][1]=axes[i].y;rows[i][2]=axes[i].z;}
    if (!vt_valid_periodic_domain(frame) || !std::isfinite(mapping.datum_m) ||
        !std::isfinite(mapping.phase[0]) || !std::isfinite(mapping.phase[1]) ||
        !std::isfinite(mapping.u_range_m[0]) || !std::isfinite(mapping.u_range_m[1]) ||
        !(mapping.u_range_m[1]>mapping.u_range_m[0]))
        return fail("receiver material frame must be finite, rigid and right-handed");
    const auto dot=[](const float* a,const float* b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    const auto& chart=atlas.charts[mapping.chart];
    if (!(chart.texels_per_meter>0) || !std::isfinite(chart.texels_per_meter) ||
        !chart.tri_count || size_t(chart.first_tri)+chart.tri_count>atlas.tri_order.size())
        return fail("invalid receiver chart geometry");
    // The inverse chart projection below is only exact for an orthonormal
    // planar chart. Check both the chart axes and the actual mesh/vertex UVs.
    if (std::abs(dot(chart.tangent,chart.tangent)-1)>1e-4f ||
        std::abs(dot(chart.bitangent,chart.bitangent)-1)>1e-4f ||
        std::abs(dot(chart.tangent,chart.bitangent))>1e-4f ||
        std::abs(dot(chart.tangent,frame.n))>1e-4f ||
        std::abs(dot(chart.bitangent,frame.n))>1e-4f)
        return fail("receiver material mapping requires an orthonormal planar chart");
    const auto& geo=*receiver.geometry;
    for(size_t i=chart.first_tri;i<size_t(chart.first_tri)+chart.tri_count;++i) {
        const size_t tri=atlas.tri_order[i];
        if(tri*3+2>=geo.indices.size())return fail("receiver triangle outside mesh");
        for(int c=0;c<3;++c) {
            const size_t v=geo.indices[tri*3+c];
            if(v*3+2>=geo.positions.size() || v*3+2>=geo.normals.size() || v*2+1>=geo.surface_uvs.size())
                return fail("receiver mapping requires positions, normals and chart UVs");
            const float* p=&geo.positions[v*3];const float* n=&geo.normals[v*3];
            float q[3],f[3];
            for(int k=0;k<3;++k){q[k]=p[k]-chart.origin[k];f[k]=p[k]-frame.origin[k];}
            // Chart origins store the in-plane UV offset; atlas packing may
            // leave their normal component at zero. Only mesh points must
            // lie on the authored plane. The projection ignores q dot n.
            if(std::abs(dot(f,frame.n))>.00005f ||
                !std::isfinite(dot(n,n)) || std::abs(dot(n,n)-1)>1e-4f || dot(n,frame.n)<.9999f)return fail("receiver chart is not on the material projection plane");
            const float uv[]={float((chart.rect_x+4+dot(q,chart.tangent)*chart.texels_per_meter)/atlas.atlas_w),
                              float((chart.rect_y+4+dot(q,chart.bitangent)*chart.texels_per_meter)/atlas.atlas_h)};
            for(int a=0;a<2;++a)if(!std::isfinite(uv[a]) || std::abs(geo.surface_uvs[v*2+a]-uv[a])>1e-5f)
                return fail("receiver vertex UV disagrees with its chart metric");
        }
    }
    VtReceiverMaterialGpu result;
    float origin[3];
    for(int k=0;k<3;++k)origin[k]=chart.origin[k]-frame.origin[k]-
        (chart.tangent[k]*(chart.rect_x+4)+chart.bitangent[k]*(chart.rect_y+4))/chart.texels_per_meter;
    for(int a=0;a<2;++a) {
        const float* axis=a?frame.v:frame.u;float* row=a?result.uv_v:result.uv_u;
        row[0]=dot(chart.tangent,axis)*atlas.atlas_w/(chart.texels_per_meter*domain.period[a]);
        row[1]=dot(chart.bitangent,axis)*atlas.atlas_h/(chart.texels_per_meter*domain.period[a]);
        row[2]=dot(origin,axis)/domain.period[a]+mapping.phase[a];
        result.metrics[a]=domain.period[a];
    }
    // Largest singular value of receiver-texel -> module-texel transform.
    // Scalar LOD cannot express anisotropy; this conservatively filters the
    // widest transformed footprint, including rotated mappings.
    const double a=result.uv_u[0]*domain.width/atlas.atlas_w,b=result.uv_u[1]*domain.width/atlas.atlas_h;
    const double c=result.uv_v[0]*domain.height/atlas.atlas_w,d=result.uv_v[1]*domain.height/atlas.atlas_h;
    const double s=a*a+b*b+c*c+d*d,det=a*d-b*c;
    result.uv_u[3]=float(std::log2(std::sqrt(.5*(s+std::sqrt(std::max(0.0,s*s-4*det*det))))));
    result.uv_v[3]=mapping.datum_m;
    for(int i=0;i<2;++i) {
        const float bound=mapping.u_range_m[i]/domain.period[0]+mapping.phase[0];
        if(!std::isfinite(bound))return fail("receiver material U interval overflow");
        std::memcpy(&result.binding[2+i],&bound,4);
    }
    const auto normal_frame=[&](const float* n,float* t,float* b) {
        const float seed[]={std::abs(n[0])>.999f?0.f:1.f,0.f,std::abs(n[0])>.999f?1.f:0.f};
        const float projection=dot(seed,n);
        for(int k=0;k<3;++k)t[k]=seed[k]-n[k]*projection;
        const float length=std::sqrt(dot(t,t));for(int k=0;k<3;++k)t[k]/=length;
        b[0]=n[1]*t[2]-n[2]*t[1];b[1]=n[2]*t[0]-n[0]*t[2];b[2]=n[0]*t[1]-n[1]*t[0];
    };
    float mt[3],mb[3],rt[3],rb[3];normal_frame(domain.n,mt,mb);normal_frame(frame.n,rt,rb);
    for(int column=0;column<2;++column) {
        const float* m=column?mb:mt;float rotated[3];
        for(int k=0;k<3;++k)rotated[k]=frame.u[k]*dot(m,domain.u)+frame.v[k]*dot(m,domain.v)+frame.n[k]*dot(m,domain.n);
        result.normal_xy[column]=dot(rotated,rt);result.normal_xy[2+column]=dot(rotated,rb);
    }
    const float* groups[]={result.uv_u,result.uv_v,result.normal_xy,result.metrics};
    for(const auto* group:groups)for(int k=0;k<4;++k)if(!std::isfinite(group[k]))return fail("receiver material transform overflow");
    out=result;error.clear();return true;
}
} // namespace vt
