#pragma once
#include "vt_chart_gpu.h"
#include "vt_snapshot.h"
#include "vt_world_receivers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vt {
// Triangle/chart indices refer to the emitted immutable GPU stream, never
// the original mesh order. Local positions keep this reusable across edits.
struct VtSurfaceBoundaryEdge {
    uint32_t triangle=0, chart=0, edge=0;
    std::array<float,3> a{}, b{}, normal{};
};
struct VtSurfaceBoundary {
    std::vector<VtSurfaceBoundaryEdge> edges;
    size_t bytes() const { return sizeof(*this)+edges.capacity()*sizeof(VtSurfaceBoundaryEdge); }
};

// Duplicate/nonmanifold edges are deliberately omitted. A missing within-mesh
// neighbor alone is not evidence that an edge is a genuine open boundary.
inline bool vt_extract_surface_boundary(const std::vector<GpuChart>& charts,
    const std::vector<GpuTriGeometry>& triangles, VtSurfaceBoundary& out) {
    out={};
    struct Candidate { VtSurfaceBoundaryEdge edge; uint32_t count=0; };
    std::map<std::array<float,6>,Candidate> candidates;
    std::vector<uint8_t> seen(triangles.size());
    for (uint32_t c=0;c<charts.size();++c) {
        const auto& chart=charts[c];
        const size_t first=chart.tri_range[0], count=chart.tri_range[1];
        if(first>triangles.size() || count>triangles.size()-first)return false;
        for(size_t i=first;i<first+count;++i) {
            if(seen[i]++)return false;
            const auto& tri=triangles[i];
            const float* p[]={tri.p0,tri.p1,tri.p2};
            double ab[3],ac[3],n[3];
            for(int k=0;k<3;++k) {
                if(!std::isfinite(p[0][k]) || !std::isfinite(p[1][k]) || !std::isfinite(p[2][k]))return false;
                ab[k]=double(p[1][k])-p[0][k];ac[k]=double(p[2][k])-p[0][k];
            }
            n[0]=ab[1]*ac[2]-ab[2]*ac[1];n[1]=ab[2]*ac[0]-ab[0]*ac[2];n[2]=ab[0]*ac[1]-ab[1]*ac[0];
            const double n2=n[0]*n[0]+n[1]*n[1]+n[2]*n[2];
            if(!(n2>1e-20))continue;
            const double orientation=n[0]*(tri.n0[0]+tri.n1[0]+tri.n2[0])+
                n[1]*(tri.n0[1]+tri.n1[1]+tri.n2[1])+n[2]*(tri.n0[2]+tri.n1[2]+tri.n2[2]);
            const double factor=(orientation<0 ? -1 : 1)/std::sqrt(n2);
            for(uint32_t e=0;e<3;++e) {
                if(tri.mat[e+1])continue;
                VtSurfaceBoundaryEdge edge;edge.triangle=uint32_t(i);edge.chart=c;edge.edge=e;
                for(int k=0;k<3;++k){edge.a[k]=p[e][k];edge.b[k]=p[(e+1)%3][k];edge.normal[k]=float(n[k]*factor);}
                auto a=edge.a,b=edge.b;if(b<a)std::swap(a,b);
                std::array<float,6> key{a[0],a[1],a[2],b[0],b[1],b[2]};
                auto& candidate=candidates[key];candidate.edge=edge;++candidate.count;
            }
        }
    }
    size_t count=0;for(const auto& item:candidates)if(item.second.count==1)++count;
    out.edges.reserve(count);
    for(const auto& item:candidates)if(item.second.count==1)out.edges.push_back(item.second.edge);
    std::sort(out.edges.begin(),out.edges.end(),[](const auto& a,const auto& b){
        return std::tie(a.triangle,a.edge)<std::tie(b.triangle,b.edge);
    });
    return true;
}

// A publication lease, captured only after the tail and its geometry exist.
// Holding it keeps the CPU inputs, boundary and device geometry alive. The
// residency current() query must still succeed before admitting a NEW use.
struct VtSurfaceBoundarySource {
    uint32_t slot=0,generation=0;
    uint64_t content_revision=0;
    std::shared_ptr<const VtPartSnapshot> inputs;
    VtDrawGeometry geometry;
    std::shared_ptr<const VtInputSnapshot> material_inputs;
    VtPageMetadata metadata;
};

struct VtSurfaceBoundaryLink {
    uint32_t source_edge=0,target_edge=0;
    double source_begin=0,source_end=0,target_begin=0,target_end=0;
};
struct VtSurfaceBoundaryLimits {
    size_t max_edges=65536,max_candidates=1048576,max_links=4096;
    double tolerance_m=1e-5;
};

// Compile one explicitly authorized owner pair. The caller supplies matching
// nonzero continuity-domain IDs; materials and spatial proximity confer no
// authorization. Target parameters run backwards for opposite edge winding.
// Failure clears all links; bounded work never publishes a partial connection.
inline bool vt_link_surface_boundaries(const VtSurfaceBoundary& source,const float* source_frame,
    uint64_t source_domain,const VtSurfaceBoundary& target,const float* target_frame,
    uint64_t target_domain,std::vector<VtSurfaceBoundaryLink>& out,std::string& error,
    const VtSurfaceBoundaryLimits& limits={}) {
    out.clear();error.clear();
    const auto fail=[&](const char* why){out.clear();error=why;return false;};
    if(!source_domain || source_domain!=target_domain)return fail("surface continuity domains differ or are absent");
    if(!source_frame || !target_frame || !std::isfinite(limits.tolerance_m) ||
       !(limits.tolerance_m>0) || limits.tolerance_m>1e-3)return fail("invalid boundary frame or tolerance");
    if(source.edges.size()>limits.max_edges || target.edges.size()>limits.max_edges)
        return fail("surface boundary edge budget exceeded");
    for(const float* frame:{source_frame,target_frame}) {
        VtWorldReceiverFrame rigid;rigid.references=1;rigid.local_to_world[15]=1;
        std::copy_n(frame,12,rigid.local_to_world.begin());
        if(!rigid.supported())return fail("surface boundary frame must be finite, rigid and right handed");
    }
    using Vec=std::array<double,3>;
    const auto dot=[](const Vec& a,const Vec& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    const auto sub=[](const Vec& a,const Vec& b){return Vec{a[0]-b[0],a[1]-b[1],a[2]-b[2]};};
    const auto transform=[](const std::array<float,3>& p,const float* m,bool normal) {
        Vec v{};for(int k=0;k<3;++k)v[k]=double(m[k*4])*p[0]+double(m[k*4+1])*p[1]+
            double(m[k*4+2])*p[2]+(normal ? 0 : m[k*4+3]);return v;
    };
    struct Edge { Vec a,b,n;uint32_t index; };
    const auto world=[&](const VtSurfaceBoundary& boundary,const float* frame,std::vector<Edge>& edges) {
        edges.reserve(boundary.edges.size());
        for(uint32_t i=0;i<boundary.edges.size();++i) {
            const auto& e=boundary.edges[i];
            Edge w{transform(e.a,frame,false),transform(e.b,frame,false),transform(e.normal,frame,true),i};
            for(int k=0;k<3;++k)if(!std::isfinite(w.a[k]) || !std::isfinite(w.b[k]) || !std::isfinite(w.n[k]))return false;
            if(dot(sub(w.b,w.a),sub(w.b,w.a))<=1e-20 || std::abs(dot(w.n,w.n)-1)>1e-4)return false;
            edges.push_back(w);
        }
        return true;
    };
    std::vector<Edge> a,b;
    if(!world(source,source_frame,a) || !world(target,target_frame,b))return fail("invalid boundary edge geometry");
    if(a.empty() || b.empty())return true;
    // Sweep along the widest source extent, including edge interiors. This
    // permits one coarse edge to match many fine edges; endpoint hashing cannot.
    Vec low=a[0].a,high=low;
    for(const auto& e:a)for(int k=0;k<3;++k){low[k]=std::min({low[k],e.a[k],e.b[k]});high[k]=std::max({high[k],e.a[k],e.b[k]});}
    int axis=0;for(int k=1;k<3;++k)if(high[k]-low[k]>high[axis]-low[axis])axis=k;
    const auto minimum=[&](const Edge& e){return std::min(e.a[axis],e.b[axis]);};
    std::sort(b.begin(),b.end(),[&](const Edge& x,const Edge& y){
        const double left=minimum(x),right=minimum(y);
        return left==right ? x.index<y.index : left<right;
    });
    const double eps=limits.tolerance_m;
    size_t candidates=0;
    for(const auto& s:a) {
        const auto sa=sub(s.b,s.a);const double length2=dot(sa,sa),length=std::sqrt(length2);
        const double end=std::max(s.a[axis],s.b[axis])+eps;
        for(const auto& t:b) {
            if(minimum(t)>end)break;
            if(++candidates>limits.max_candidates)return fail("surface boundary candidate budget exceeded");
            bool overlap=true;
            for(int k=0;k<3;++k)overlap &= std::max(s.a[k],s.b[k])+eps>=std::min(t.a[k],t.b[k]) &&
                std::max(t.a[k],t.b[k])+eps>=std::min(s.a[k],s.b[k]);
            if(!overlap || dot(s.n,t.n)<=-.999)continue;
            const auto tb=sub(t.b,t.a);const double target2=dot(tb,tb);
            if(dot(sa,tb)>=0)continue;
            const auto start=sub(t.a,s.a),finish=sub(t.b,s.a);
            const double u0=dot(start,sa)/length2,u1=dot(finish,sa)/length2;
            Vec d0{},d1{};for(int k=0;k<3;++k){d0[k]=start[k]-u0*sa[k];d1[k]=finish[k]-u1*sa[k];}
            if(dot(d0,d0)>eps*eps || dot(d1,d1)>eps*eps)continue;
            const double first=std::max(0.,std::min(u0,u1)),last=std::min(1.,std::max(u0,u1));
            if((last-first)*length<=eps)continue;
            Vec p0{},p1{};for(int k=0;k<3;++k){p0[k]=s.a[k]+first*sa[k]-t.a[k];p1[k]=s.a[k]+last*sa[k]-t.a[k];}
            if(out.size()>=limits.max_links)return fail("surface boundary link budget exceeded");
            out.push_back({s.index,t.index,first,last,std::clamp(dot(p0,tb)/target2,0.,1.),std::clamp(dot(p1,tb)/target2,0.,1.)});
        }
    }
    std::sort(out.begin(),out.end(),[](const auto& x,const auto& y){
        return std::tie(x.source_edge,x.source_begin,x.target_edge)<std::tie(y.source_edge,y.source_begin,y.target_edge);
    });
    for(size_t i=1;i<out.size();++i)if(out[i-1].source_edge==out[i].source_edge) {
        const auto& e=a[out[i].source_edge];const double length=std::sqrt(dot(sub(e.b,e.a),sub(e.b,e.a)));
        if((out[i-1].source_end-out[i].source_begin)*length>eps)return fail("ambiguous overlapping boundary destinations");
    }
    return true;
}
} // namespace vt
