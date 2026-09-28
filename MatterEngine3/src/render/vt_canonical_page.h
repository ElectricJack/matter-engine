#pragma once

// A planar interior can evaluate material coordinates independently of the
// receiver's triangle dimensions and packed chart location. Boundaries retain
// the ordinary projection/dilation path. This is NOT arbitrary periodic remap:
// only equal physical sampling grids and equal source inputs receive equal keys.
#include "vt_finite_sources.h"
#include "vt_types.h"
#include <set>

namespace vt {
struct VtCanonicalPage {
    std::array<float,4> origin{}, du{}, dv{}, normal{};
    std::vector<uint32_t> candidates;
    VtMaterialPixelKey key{};
};

inline bool vt_canonical_page(const VtFillRequest& request, uint32_t chart_id,
                              VtPageHeight height, VtCanonicalPage& out) {
    const auto* ctx=request.part();
    if(!ctx || !request.atlas || chart_id>=request.atlas->charts.size() ||
       !ctx->positions || !ctx->normals || !ctx->indices || ctx->surface_lane_count ||
       !ctx->surface_tape_text || ctx->surface_material_count!=1 || !ctx->surface_materials ||
       request.mip>=8) return false;
    const auto& atlas=*request.atlas;
    const auto& chart=atlas.charts[chart_id];
    if(chart.tri_count!=2 || chart.first_tri>atlas.tri_order.size() || atlas.tri_order.size()-chart.first_tri<2 ||
       !(chart.texels_per_meter>0) || !std::isfinite(chart.texels_per_meter)) return false;
    const auto dot=[](const float* a,const float* b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    const float* axes[2]={chart.tangent,chart.bitangent};
    for(unsigned k=0;k<3;++k) if(!std::isfinite(chart.origin[k]) || !std::isfinite(axes[0][k]) || !std::isfinite(axes[1][k])) return false;
    if(std::abs(dot(axes[0],axes[0])-1)>1e-6f || std::abs(dot(axes[1],axes[1])-1)>1e-6f ||
       std::abs(dot(axes[0],axes[1]))>1e-6f) return false;
    uint32_t source_id=0, receiver_material=0;
    float uv[6][2],lo[2]={INFINITY,INFINITY},hi[2]={-INFINITY,-INFINITY};
    const float* p0=nullptr;const float* n0=nullptr;
    for(unsigned i=0;i<6;++i) {
        const uint32_t tri=atlas.tri_order[chart.first_tri+i/3];
        if(tri>=ctx->triangle_count) return false;
        const uint32_t vertex=ctx->indices[tri*3+i%3];
        if(vertex>=ctx->vertex_count) return false;
        const float* p=ctx->positions+vertex*3;const float* n=ctx->normals+vertex*3;
        if(!i) {p0=p;n0=n;source_id=ctx->finite_source_ids?ctx->finite_source_ids[vertex]:0;}
        uint32_t material=ctx->material_ids?ctx->material_ids[vertex]:ctx->dominant_material;
        if(material==0xffffffffu)
            material=ctx->dominant_material!=0xffffffffu?ctx->dominant_material:0u;
        material&=255u;
        if(!i) receiver_material=material;
        // A source may select its layers by categorical receiver material.
        // A shared rectangle must have one identity, as well as one frame.
        if(material!=receiver_material) return false;
        if(ctx->finite_source_ids && ctx->finite_source_ids[vertex]!=source_id) return false;
        for(unsigned k=0;k<3;++k)
            if(!std::isfinite(p[k]) || !std::isfinite(n[k]) || std::abs(n[k]-n0[k])>1e-7f) return false;
        for(unsigned a=0;a<2;++a) {uv[i][a]=dot(p,axes[a]);lo[a]=std::min(lo[a],uv[i][a]);hi[a]=std::max(hi[a],uv[i][a]);}
        float delta[3]={p[0]-p0[0],p[1]-p0[1],p[2]-p0[2]};
        if(std::abs(dot(delta,n0))>1e-6f) return false;
    }
    if(std::abs(dot(n0,n0)-1)>1e-6f || std::abs(dot(n0,axes[0]))>1e-6f ||
       std::abs(dot(n0,axes[1]))>1e-6f || hi[0]<=lo[0] || hi[1]<=lo[1]) return false;
    // Two triangles must tile the rectangle exactly, sharing its diagonal.
    // A bounding box alone would incorrectly admit holes or disjoint pieces.
    uint32_t masks[2]{};
    for(unsigned i=0;i<6;++i) {
        unsigned corner=0;
        for(unsigned a=0;a<2;++a) {
            if(uv[i][a]==hi[a]) corner|=1u<<a;
            else if(uv[i][a]!=lo[a]) return false;
        }
        if(masks[i/3]&(1u<<corner)) return false;
        masks[i/3]|=1u<<corner;
    }
    const auto diagonal=masks[0]&masks[1];
    if((masks[0]|masks[1])!=15 || (diagonal!=6 && diagonal!=9)) return false;
    const float scale=float(1u<<request.mip),footprint=scale/chart.texels_per_meter;
    if(!std::isfinite(footprint) || footprint<=0) return false;
    float q[2];
    for(unsigned a=0;a<2;++a) {
        const float first=(float((a?request.page_y:request.page_x)*128)-4.f+.5f)*scale;
        q[a]=(first-float(a?chart.rect_y:chart.rect_x)-float(chart_atlas::kChartGutterTexels))/chart.texels_per_meter+dot(chart.origin,axes[a]);
        // Include every stored gutter texel and a small coverage guard.
        if(q[a]<lo[a]+footprint*.01f || q[a]+135.f*footprint>hi[a]-footprint*.01f) return false;
    }
    VtCanonicalPage result;
    for(unsigned k=0;k<3;++k) {
        result.origin[k]=p0[k]+axes[0][k]*(q[0]-dot(p0,axes[0]))+axes[1][k]*(q[1]-dot(p0,axes[1]));
        result.du[k]=axes[0][k]*footprint;result.dv[k]=axes[1][k]*footprint;result.normal[k]=n0[k];
    }
    result.origin[3]=1;result.normal[3]=footprint;
    const auto* sources=ctx->finite_sources.get();
    if(sources) {
        if(!source_id) return false;
        if(sources->receivers.empty()) {
            if(source_id>sources->bindings.size()) return false;
            result.candidates.push_back(source_id-1);
        } else {
            if(source_id>sources->receivers.size()) return false;
            const auto& r=sources->receivers[source_id-1];
            float low[2]={INFINITY,INFINITY},high[2]={-INFINITY,-INFINITY};
            for(unsigned corner=0;corner<4;++corner) {
                float delta[3];for(unsigned k=0;k<3;++k) delta[k]=result.origin[k]+(corner&1?135.f:0.f)*result.du[k]+(corner&2?135.f:0.f)*result.dv[k]-r.origin_datum[k];
                if(std::abs(dot(delta,r.n)-r.origin_datum[3])>r.u[3] || dot(n0,r.n)<r.n[3]) return false;
                for(unsigned a=0;a<2;++a) {float x=dot(delta,a?r.v:r.u);low[a]=std::min(low[a],x);high[a]=std::max(high[a],x);}
            }
            uint32_t first[2],last[2];
            for(unsigned a=0;a<2;++a) {
                if(!r.levels[a] || r.domain[a+2]<=0) return false;
                const auto cell=[&](float x){return uint32_t(std::clamp(std::floor((x-r.domain[a])/r.domain[a+2]*float(r.levels[a])),0.f,float(r.levels[a]-1)));};
                first[a]=cell(low[a]-.5f*footprint);last[a]=cell(high[a]+.5f*footprint);
            }
            if(uint64_t(last[0]-first[0]+1)*(last[1]-first[1]+1)>256) return false;
            std::set<uint32_t> ids;uint32_t visited=0;
            for(uint32_t y=first[1];y<=last[1];++y) for(uint32_t x=first[0];x<=last[0];++x) {
                const size_t index=size_t(r.levels[2])+size_t(y)*r.levels[0]+x;
                if(index>=sources->lookup.size()) return false;
                const auto& cell=sources->lookup[index];
                if(cell[0]>sources->lookup.size() || cell[1]>sources->lookup.size()-cell[0] || (visited+=cell[1])>4096) return false;
                for(uint32_t i=0;i<cell[1];++i) {
                    const uint32_t id=sources->lookup[cell[0]+i][0];
                    if(id>=sources->bindings.size()) return false;
                    ids.insert(id);if(ids.size()>128) return false;
                }
            }
            // Cell membership is a conservative query accelerator, not a
            // material dependency. Resizing the receiver changes cell pitch
            // and may gather extra stamps that cannot affect this page.
            // Every finite mip has at least one texel, so half the larger of
            // the entire source span and query footprint bounds ALL possible
            // filter support. Keep uncertain/touching cases with an FP guard.
            // Coverage derivatives run only after nonzero centre coverage;
            // a source with zero coverage everywhere cannot affect them.
            for(uint32_t id:ids) {
                const auto& source=sources->bindings[id];
                bool outside=false;
                for(unsigned a=0;a<2;++a) {
                    const float* axis=a?source.v:source.u;
                    double low=INFINITY,high=-INFINITY,magnitude=1;
                    for(unsigned corner=0;corner<4;++corner) {
                        double projected=0,scale_bound=1;
                        for(unsigned k=0;k<3;++k) {
                            const double p=double(result.origin[k])+(corner&1?135.:0.)*result.du[k]+
                                (corner&2?135.:0.)*result.dv[k];
                            projected+=(p-source.origin_datum[k])*axis[k];
                            scale_bound+=(std::abs(p)+std::abs(double(source.origin_datum[k])))*std::abs(double(axis[k]));
                        }
                        low=std::min(low,projected);high=std::max(high,projected);
                        magnitude=std::max(magnitude,scale_bound);
                    }
                    const double span=source.domain[a+2],start=source.domain[a];
                    const double support=.5*std::max(span,double(footprint));
                    const double guard=1e-5*(magnitude+std::abs(start)+span+support);
                    outside|=high<start-support-guard || low>start+span+support+guard;
                }
                if(!outside) result.candidates.push_back(id);
            }
        }
    }
    uint64_t h0=14695981039346656037ull,h1=0x9e3779b97f4a7c15ull;
    const auto bytes=[&](const void* input,size_t count){
        const auto* p=static_cast<const uint8_t*>(input);
        for(size_t i=0;i<count;++i){h0=(h0^p[i])*1099511628211ull;h1=(h1+p[i]+1)*0x9e3779b185ebca87ull;h1^=h1>>29;}
    };
    const uint32_t version=2;bytes(&version,sizeof(version));
    for(const auto* row:{&result.origin,&result.du,&result.dv,&result.normal}) bytes(row->data(),sizeof(*row));
    const uint64_t text_size=std::strlen(ctx->surface_tape_text);bytes(&text_size,sizeof(text_size));
    bytes(ctx->surface_tape_text,size_t(text_size));
    bytes(ctx->surface_local_to_world,sizeof(ctx->surface_local_to_world));
    bytes(&ctx->surface_world_anchored,sizeof(ctx->surface_world_anchored));
    bytes(ctx->surface_materials,sizeof(uint32_t));bytes(&height,sizeof(height));
    bytes(&receiver_material,sizeof(receiver_material));
    const uint32_t source_mode=!sources?0:sources->receivers.empty()?1:2;
    bytes(&source_mode,sizeof(source_mode));
    if(sources) {
        bytes(&sources->payload_hash,sizeof(sources->payload_hash));
        // Canonical iteration uses this same page-wide ordered list in GLSL.
        for(uint32_t id:result.candidates) bytes(&sources->bindings[id],sizeof(VtGpuFiniteSource));
    }
    result.key={h0,h1};out=std::move(result);return true;
}
} // namespace vt
