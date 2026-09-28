#include "geometry_hierarchy.h"
#include "mesh_simplifier.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <chrono>
#include <cstdlib>
#include "matter/log.h"

namespace geometry {
namespace {
using Point = std::array<float, 3>;
using Edge = std::pair<Point, Point>;
Point point(float3 p) { return {p.x,p.y,p.z}; }
Edge edge(float3 a, float3 b) { auto x=point(a), y=point(b); return x<y?Edge{x,y}:Edge{y,x}; }
struct Side { uint32_t triangle, corner; };
using Edges = std::map<Edge,std::vector<Side>>;
Edges edges(const MeshIndexed& mesh) {
    Edges result;
    for (uint32_t t=0;t<mesh.indices.size()/3;++t) for(uint32_t c=0;c<3;++c)
        result[edge(mesh.positions[mesh.indices[t*3+c]],mesh.positions[mesh.indices[t*3+(c+1)%3]])].push_back({t,c});
    return result;
}
std::map<Edge,size_t> boundary(const MeshIndexed& mesh) {
    std::map<Edge,size_t> result;
    for(const auto& e:edges(mesh)) if(e.second.size()!=2) result[e.first]=e.second.size();
    return result;
}
std::array<float,6> corner(const TriEx& t,uint32_t c) {
    const float2 uv[]={t.uv0,t.uv1,t.uv2};const float3 n[]={t.N0,t.N1,t.N2};const float ao[]={t.ao0,t.ao1,t.ao2};
    return {uv[c].x,uv[c].y,n[c].x,n[c].y,n[c].z,ao[c]};
}
bool continuous(const MeshIndexed& m,Side a,Side b) {
    const auto& x=m.triex[a.triangle];const auto& y=m.triex[b.triangle];
    if(x.materialId!=y.materialId || x.tint.x!=y.tint.x || x.tint.y!=y.tint.y || x.tint.z!=y.tint.z || x.tint.w!=y.tint.w) return false;
    for(uint32_t c=0;c<2;++c) {
        const uint32_t ac=(a.corner+c)%3;
        const auto p=point(m.positions[m.indices[a.triangle*3+ac]]);
        bool matched=false;
        for(uint32_t j=0;j<2;++j) {
            const uint32_t bc=(b.corner+j)%3;
            if(p==point(m.positions[m.indices[b.triangle*3+bc]])) {
                if(corner(x,ac)!=corner(y,bc)) return false;
                matched=true;
            }
        }
        if(!matched) return false;
    }
    return true;
}
MeshIndexed subset(const MeshIndexed& source,const std::vector<uint32_t>& triangles) {
    MeshIndexed result;std::map<Point,uint32_t> vertices;
    for(uint32_t t:triangles) {
        for(uint32_t c=0;c<3;++c) {
            const auto p=source.positions[source.indices[t*3+c]];
            auto inserted=vertices.emplace(point(p),static_cast<uint32_t>(result.positions.size()));
            if(inserted.second) result.positions.push_back(p);
            result.indices.push_back(inserted.first->second);
        }
        result.triex.push_back(source.triex[t]);
    }
    return result;
}
MeshIndexed merge(const MeshIndexed& a,const MeshIndexed& b) {
    MeshIndexed temp=a;
    const uint32_t base=static_cast<uint32_t>(temp.positions.size());
    temp.positions.insert(temp.positions.end(),b.positions.begin(),b.positions.end());
    for(uint32_t i:b.indices) temp.indices.push_back(base+i);
    temp.triex.insert(temp.triex.end(),b.triex.begin(),b.triex.end());
    std::vector<uint32_t> all(temp.indices.size()/3);std::iota(all.begin(),all.end(),0u);
    return subset(temp,all);
}
Bounds bounds(const MeshIndexed& m) {
    Bounds b;
    for(int k=0;k<3;++k) b.lo[k]=b.hi[k]=point(m.positions[m.indices[0]])[k];
    for(auto i:m.indices) for(int k=0;k<3;++k) {
        const float v=point(m.positions[i])[k];b.lo[k]=std::min(b.lo[k],v);b.hi[k]=std::max(b.hi[k],v);
    }
    return b;
}
// Attribute seams constrain simplification, not storage. Combine nearby
// terminal islands without changing any triangle or corner attribute.
bool pack_terminal_roots(Hierarchy& h, uint32_t limit, const std::atomic<bool>* cancel) {
    if (!limit || h.roots.size() < 2) return true;
    std::vector<uint32_t> candidates, roots;
    for (auto id : h.roots) {
        const auto& n=h.nodes[id];
        if(n.children.empty() && n.error==0 && n.mesh.triex.size()<=limit) candidates.push_back(id);
        else roots.push_back(id);
    }
    if(candidates.size()<2)return true;
    Bounds region=h.nodes[candidates.front()].bounds;
    for(auto id:candidates)for(int k=0;k<3;++k){
        region.lo[k]=std::min(region.lo[k],h.nodes[id].bounds.lo[k]);
        region.hi[k]=std::max(region.hi[k],h.nodes[id].bounds.hi[k]);
    }
    const auto morton=[&](uint32_t id){
        uint32_t code=0;
        for(int k=0;k<3;++k){
            const auto& b=h.nodes[id].bounds;
            const double extent=double(region.hi[k])-region.lo[k];
            const double centre=(double(b.lo[k])+b.hi[k])*.5;
            const auto q=extent>0?uint32_t(std::clamp((centre-region.lo[k])/extent,0.,1.)*1023):0u;
            for(uint32_t bit=0;bit<10;++bit)code|=((q>>bit)&1u)<<(3*bit+k);
        }
        return code;
    };
    std::vector<std::pair<uint32_t,uint32_t>> ordered;
    for(auto id:candidates)ordered.emplace_back(morton(id),id);
    std::sort(ordered.begin(),ordered.end());
    std::vector<uint8_t> removed(h.nodes.size(),0);
    uint32_t target=UINT32_MAX;
    for(const auto& item:ordered){
        if(cancel && cancel->load())return false;
        const auto id=item.second;
        if(target==UINT32_MAX || h.nodes[target].mesh.triex.size()+h.nodes[id].mesh.triex.size()>limit){
            target=id;roots.push_back(id);continue;
        }
        auto& dst=h.nodes[target];auto& src=h.nodes[id];
        const auto offset=static_cast<uint32_t>(dst.mesh.positions.size());
        dst.mesh.positions.insert(dst.mesh.positions.end(),src.mesh.positions.begin(),src.mesh.positions.end());
        for(auto index:src.mesh.indices)dst.mesh.indices.push_back(offset+index);
        dst.mesh.triex.insert(dst.mesh.triex.end(),src.mesh.triex.begin(),src.mesh.triex.end());
        dst.receivers.insert(dst.receivers.end(),src.receivers.begin(),src.receivers.end());
        dst.source_triangles+=src.source_triangles;
        for(int k=0;k<3;++k){dst.bounds.lo[k]=std::min(dst.bounds.lo[k],src.bounds.lo[k]);dst.bounds.hi[k]=std::max(dst.bounds.hi[k],src.bounds.hi[k]);}
        removed[id]=1;src=Node{};
    }
    // Remove absorbed nodes as well as their manifest references. Keeping dead
    // leaves would still write hundreds of thousands of unused binary pages.
    std::vector<uint32_t> remap(h.nodes.size(),UINT32_MAX);
    size_t next=0;
    for(size_t i=0;i<h.nodes.size();++i)if(!removed[i]){
        remap[i]=static_cast<uint32_t>(next);
        if(next!=i)h.nodes[next]=std::move(h.nodes[i]);
        ++next;
    }
    h.nodes.resize(next);
    for(auto& node:h.nodes)for(auto& child:node.children)child=remap[child];
    for(auto& root:roots)root=remap[root];
    for(auto root:roots)if(h.nodes[root].children.empty())
        h.nodes[root].boundary_edges=static_cast<uint32_t>(boundary(h.nodes[root].mesh).size());
    h.roots=std::move(roots);
    return true;
}
std::vector<mesh_error::Triangle> triangles(const MeshIndexed& m) {
    std::vector<mesh_error::Triangle> result(m.indices.size()/3);
    for(size_t t=0;t<result.size();++t) for(size_t c=0;c<3;++c) {
        auto p=m.positions[m.indices[t*3+c]];result[t][c]={p.x,p.y,p.z};
    }
    return result;
}
using D=std::array<double,3>;
D sub(D a,D b) {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
double dot(D a,D b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
D dp(float3 p) {return {p.x,p.y,p.z};}
// Closest-point barycentrics, including the three edge and vertex regions.
D weights(D p,D a,D b,D c) {
    auto ab=sub(b,a),ac=sub(c,a),ap=sub(p,a);
    double d1=dot(ab,ap),d2=dot(ac,ap);
    if(d1<=0 && d2<=0) return {1,0,0};
    auto bp=sub(p,b);double d3=dot(ab,bp),d4=dot(ac,bp);
    if(d3>=0 && d4<=d3) return {0,1,0};
    double vc=d1*d4-d3*d2;
    if(vc<=0 && d1>=0 && d3<=0) {double v=d1/(d1-d3);return {1-v,v,0};}
    auto cp=sub(p,c);double d5=dot(ab,cp),d6=dot(ac,cp);
    if(d6>=0 && d5<=d6) return {0,0,1};
    double vb=d5*d2-d1*d6;
    if(vb<=0 && d2>=0 && d6<=0) {double w=d2/(d2-d6);return {1-w,0,w};}
    double va=d3*d6-d5*d4;
    if(va<=0 && d4-d3>=0 && d5-d6>=0) {double w=(d4-d3)/((d4-d3)+(d5-d6));return {0,1-w,w};}
    const double inv=1/(va+vb+vc);return {va*inv,vb*inv,vc*inv};
}
bool attributes(const MeshIndexed& source,MeshIndexed& target,uint64_t& tests,uint64_t limit) {
    std::map<Point,std::array<float,6>> exact;
    for(size_t t=0;t<source.triex.size();++t)for(uint32_t c=0;c<3;++c)
        exact.emplace(point(source.positions[source.indices[t*3+c]]),corner(source.triex[t],c));
    std::vector<std::array<float,6>> values(target.positions.size());
    std::vector<D> queries;
    std::vector<size_t> query_vertices;
    for(size_t v=0;v<target.positions.size();++v) {
        // In particular, preserve border corner attributes bit-for-bit. A
        // nearest triangle can tie at zero distance and otherwise introduce
        // rounding differences that look like new seams at the next level.
        const auto found=exact.find(point(target.positions[v]));
        if(found!=exact.end()){values[v]=found->second;continue;}
        queries.push_back(dp(target.positions[v])); query_vertices.push_back(v);
    }
    if (!queries.empty()) {
        std::vector<uint32_t> nearest; std::string error;
        if (!mesh_error::nearest_triangles(triangles(source),queries,tests,limit,nearest,error)) return false;
        for(size_t q=0;q<queries.size();++q) {
            const size_t t=nearest[q],v=query_vertices[q];
            const D a=dp(source.positions[source.indices[t*3]]),b=dp(source.positions[source.indices[t*3+1]]),c=dp(source.positions[source.indices[t*3+2]]);
            const D w=weights(queries[q],a,b,c);
            const auto x=corner(source.triex[t],0),y=corner(source.triex[t],1),z=corner(source.triex[t],2);
            for(size_t k=0;k<6;++k) values[v][k]=float(w[0]*x[k]+w[1]*y[k]+w[2]*z[k]);
        }
    }
    target.triex.assign(target.indices.size()/3,source.triex.front());
    for(size_t t=0;t<target.triex.size();++t) {
        auto& ex=target.triex[t];float2* uv[]={&ex.uv0,&ex.uv1,&ex.uv2};float3* n[]={&ex.N0,&ex.N1,&ex.N2};float* ao[]={&ex.ao0,&ex.ao1,&ex.ao2};
        for(size_t c=0;c<3;++c) {const auto& v=values[target.indices[t*3+c]];*uv[c]={v[0],v[1]};*n[c]=make_float3(v[2],v[3],v[4]);*ao[c]=v[5];}
    }
    return true;
}
bool project_receivers(const MeshIndexed& source, const MeshIndexed& target,
                       const std::vector<ReceiverCorner>& input,
                       std::vector<ReceiverCorner>& output, uint64_t& tests, uint64_t limit) {
    if (input.empty()) { output.clear(); return true; }
    std::map<Point, ReceiverCorner> exact;
    for (size_t i=0; i<source.indices.size(); ++i) exact.emplace(point(source.positions[source.indices[i]]), input[i]);
    std::vector<ReceiverCorner> values(target.positions.size());
    std::vector<D> queries; std::vector<size_t> vertices;
    for (size_t i=0; i<target.positions.size(); ++i) {
        const auto found=exact.find(point(target.positions[i]));
        if(found!=exact.end()) values[i]=found->second;
        else { vertices.push_back(i); queries.push_back(dp(target.positions[i])); }
    }
    std::vector<uint32_t> nearest; std::string error;
    if(!queries.empty() && !mesh_error::nearest_triangles(triangles(source),queries,tests,limit,nearest,error)) return false;
    for(size_t q=0;q<queries.size();++q) {
        const size_t t=nearest[q];
        const auto w=weights(queries[q],dp(source.positions[source.indices[t*3]]),dp(source.positions[source.indices[t*3+1]]),dp(source.positions[source.indices[t*3+2]]));
        D p{}, n{};
        for(size_t c=0;c<3;++c) for(size_t k=0;k<3;++k) {
            p[k]+=w[c]*point(input[t*3+c].position)[k];
            n[k]+=w[c]*point(input[t*3+c].normal)[k];
        }
        const auto length=std::sqrt(dot(n,n)); if(!(length>1e-12))return false;
        values[vertices[q]]={make_float3(float(p[0]),float(p[1]),float(p[2])),make_float3(float(n[0]/length),float(n[1]/length),float(n[2]/length))};
    }
    output.clear(); output.reserve(target.indices.size());
    for(auto i:target.indices)output.push_back(values[i]);
    return true;
}

}

bool compile(const MeshIndexed& source,const CompileConfig& cfg,Hierarchy& out,std::string& error,const std::atomic<bool>* cancel,const std::vector<ReceiverCorner>* receivers) {
    using Clock = std::chrono::steady_clock;
    const bool profile = std::getenv("MATTER_GEOMETRY_PAGES_PROFILE") != nullptr;
    const auto started = Clock::now();
    const auto ms = [](Clock::time_point t) { return std::chrono::duration<double,std::milli>(Clock::now()-t).count(); };
    double simplify_ms=0, verify_ms=0, attributes_ms=0;
    const auto fail=[&](const char* msg){error=msg;return false;};
    const size_t count=source.indices.size()/3;
    if(!count || source.indices.size()%3 || source.triex.size()!=count || count>cfg.max_source_triangles ||
       source.positions.size()>uint64_t(cfg.max_source_triangles)*3 || !cfg.leaf_triangles ||
       cfg.leaf_triangles>cfg.max_group_triangles || cfg.packed_root_triangles>cfg.max_group_triangles || !(cfg.reduction>0 && cfg.reduction<1) ||
       !cfg.max_output_triangles || count>cfg.max_output_triangles) return fail("invalid geometry compiler input or limits");
    for(auto p:source.positions) for(float x:point(p))
        if(!std::isfinite(x) || std::abs(x)>1e8f) return fail("invalid or excessive local position");
    for(auto i:source.indices) if(i>=source.positions.size()) return fail("geometry index out of range");
    for(size_t t=0;t<count;++t) {
        auto a=dp(source.positions[source.indices[t*3]]),b=dp(source.positions[source.indices[t*3+1]]),c=dp(source.positions[source.indices[t*3+2]]);
        auto ab=sub(b,a),ac=sub(c,a);D cross{ab[1]*ac[2]-ab[2]*ac[1],ab[2]*ac[0]-ab[0]*ac[2],ab[0]*ac[1]-ab[1]*ac[0]};
        if(dot(cross,cross)==0) return fail("degenerate source triangle");
        for(uint32_t k=0;k<3;++k) for(float x:corner(source.triex[t],k)) if(!std::isfinite(x)) return fail("invalid source shading");
        const auto tint=source.triex[t].tint;
        if(!std::isfinite(tint.x)||!std::isfinite(tint.y)||!std::isfinite(tint.z)||!std::isfinite(tint.w)) return fail("invalid source tint");
    }
    if(receivers && !receivers->empty()) {
        if(receivers->size()!=source.indices.size()) return fail("receiver mapping count differs from source corners");
        std::map<Point,std::array<float,6>> mapping;
        for(size_t i=0;i<receivers->size();++i) {
            const auto& receiver=(*receivers)[i]; const auto p=point(receiver.position),n=point(receiver.normal);
            std::array<float,6> value{p[0],p[1],p[2],n[0],n[1],n[2]};
            for(float v:value)if(!std::isfinite(v))return fail("invalid receiver mapping");
            if(dot(dp(receiver.normal),dp(receiver.normal))<1e-12)return fail("zero receiver normal");
            auto found=mapping.emplace(point(source.positions[source.indices[i]]),value);
            if(!found.second && found.first->second!=value)return fail("discontinuous receiver mapping requires separate source assets");
        }
    }
    std::vector<std::vector<uint32_t>> adjacent(count);
    for(const auto& e:edges(source)) if(e.second.size()==2 && continuous(source,e.second[0],e.second[1])) {
        auto a=e.second[0].triangle,b=e.second[1].triangle;adjacent[a].push_back(b);adjacent[b].push_back(a);
    }
    for(auto& neighbors:adjacent) std::sort(neighbors.begin(),neighbors.end());
    Hierarchy result;std::vector<uint32_t> owner(count,UINT32_MAX);
    uint64_t output_triangles=0,attribute_tests=0;
    for(uint32_t seed=0;seed<count;++seed) if(owner[seed]==UINT32_MAX) {
        if(cancel && cancel->load()) return fail("geometry compilation cancelled");
        const auto id=static_cast<uint32_t>(result.nodes.size());
        std::vector<uint32_t> group{seed};owner[seed]=id;
        for(size_t cursor=0;cursor<group.size() && group.size()<cfg.leaf_triangles;++cursor)
            for(auto neighbor:adjacent[group[cursor]]) if(owner[neighbor]==UINT32_MAX && group.size()<cfg.leaf_triangles) {
                owner[neighbor]=id;group.push_back(neighbor);
            }
        Node n;n.mesh=subset(source,group);n.bounds=bounds(n.mesh);n.source_triangles=static_cast<uint32_t>(group.size());
        if(receivers && !receivers->empty()) for(auto t:group)
            n.receivers.insert(n.receivers.end(),receivers->begin()+t*3,receivers->begin()+t*3+3);
        n.boundary_edges=static_cast<uint32_t>(boundary(n.mesh).size());
        output_triangles+=group.size();result.nodes.push_back(std::move(n));
    }
    std::map<uint32_t,std::set<uint32_t>> active;
    for(uint32_t i=0;i<result.nodes.size();++i) active[i];
    for(uint32_t t=0;t<count;++t) for(auto n:adjacent[t]) if(owner[t]!=owner[n]) active[owner[t]].insert(owner[n]);
    while(!active.empty()) {
        std::map<uint32_t,uint32_t> replacement;std::set<uint32_t> consumed;
        for(const auto& entry:active) {
            const auto a=entry.first;if(consumed.count(a)) continue;
            if(cancel && cancel->load()) return fail("geometry compilation cancelled");
            uint32_t b=UINT32_MAX;
            bool mergeable_neighbor=false;
            for(auto neighbor:entry.second) {
                // Boundary/attribute constraints can prevent reduction. Keep
                // bounded independent roots instead of rejecting valid detail.
                if(result.nodes[a].mesh.triex.size()+result.nodes[neighbor].mesh.triex.size()>cfg.max_group_triangles)
                    continue;
                mergeable_neighbor=true;
                if(!consumed.count(neighbor)) {b=neighbor;break;}
            }
            consumed.insert(a);
            if(b==UINT32_MAX) {
                if(!mergeable_neighbor) result.roots.push_back(a);
                else replacement[a]=a;
                continue;
            }
            consumed.insert(b);
            Node n;n.children={a,b};n.source_triangles=result.nodes[a].source_triangles+result.nodes[b].source_triangles;
            if(result.nodes[a].mesh.triex.size()+result.nodes[b].mesh.triex.size()>cfg.max_group_triangles)
                return fail("group exceeded compiler staging triangle limit");
            auto combined=merge(result.nodes[a].mesh,result.nodes[b].mesh);
            auto combined_receivers=result.nodes[a].receivers;
            combined_receivers.insert(combined_receivers.end(),result.nodes[b].receivers.begin(),result.nodes[b].receivers.end());
            if(combined.indices.size()/3>cfg.max_group_triangles) return fail("group exceeded compiler staging triangle limit");
            bool can_simplify=true;
            for(const auto& e:edges(combined)) if(e.second.size()==2 && !continuous(combined,e.second[0],e.second[1])) can_simplify=false;
            // A seam connected around an alternate chart path is retained
            // exactly until the simplifier supports explicit attribute locks.
            auto candidate=combined;
            const auto source_border=boundary(combined);
            n.boundary_edges=static_cast<uint32_t>(source_border.size());
            n.simplification=SimplificationResult::AttributeSeam;
            if(can_simplify) {
                SimplifyOptions options;
                // A boundary-constrained child may exceed its target. Do not
                // carry that excess up by only halving it at each parent: try
                // to return to the group target after interior borders unlock.
                options.target_ratio=std::min(cfg.reduction,float(cfg.leaf_triangles)/float(combined.triex.size()));
                options.lock_boundary=true;options.preserve_locked_edges=true;
                auto step = profile ? Clock::now() : Clock::time_point{};
                candidate=simplify(combined,options);
                if(profile)simplify_ms+=ms(step);
                if(candidate.indices.empty() || candidate.indices.size()>=combined.indices.size()) {
                    n.simplification=SimplificationResult::NoReduction;candidate=combined;
                } else if(boundary(candidate)!=source_border) {
                    n.simplification=SimplificationResult::BorderChanged;candidate=combined;
                } else {
                    n.simplification=SimplificationResult::Reduced;
                    step = profile ? Clock::now() : Clock::time_point{};
                    if(!attributes(combined,candidate,attribute_tests,cfg.max_attribute_tests)) return fail("attribute sampling budget exceeded");
                    if(profile)attributes_ms+=ms(step);
                }
            }
            double added_error=0;
            if(candidate.indices.size()<combined.indices.size()) {
                mesh_error::Bounds measured;
                const auto step = profile ? Clock::now() : Clock::time_point{};
                if(!mesh_error::measure(triangles(combined),triangles(candidate),cfg.verification,measured,error)) return false;
                if(profile)verify_ms+=ms(step);
                added_error=measured.upper;
            }
            if(candidate.indices.size()<combined.indices.size()) {
                if(!project_receivers(combined,candidate,combined_receivers,n.receivers,attribute_tests,cfg.max_attribute_tests))
                    return fail("receiver projection budget exceeded or invalid normal");
            } else n.receivers=std::move(combined_receivers);
            n.error=std::nextafter(std::max(result.nodes[a].error,result.nodes[b].error)+added_error,INFINITY);
            n.mesh=std::move(candidate);n.bounds=bounds(n.mesh);
            for(int k=0;k<3;++k) {
                n.bounds.lo[k]=std::min({n.bounds.lo[k],result.nodes[a].bounds.lo[k],result.nodes[b].bounds.lo[k]});
                n.bounds.hi[k]=std::max({n.bounds.hi[k],result.nodes[a].bounds.hi[k],result.nodes[b].bounds.hi[k]});
            }
            output_triangles+=n.mesh.indices.size()/3;
            if(output_triangles>cfg.max_output_triangles) return fail("geometry output triangle budget exceeded");
            const auto id=static_cast<uint32_t>(result.nodes.size());result.nodes.push_back(std::move(n));replacement[a]=replacement[b]=id;
        }
        std::map<uint32_t,std::set<uint32_t>> next;
        for(const auto& r:replacement) {
            next[r.second];
            for(auto neighbor:active[r.first]) {
                auto found=replacement.find(neighbor);
                if(found!=replacement.end() && found->second!=r.second) next[r.second].insert(found->second);
            }
        }
        active=std::move(next);
    }
    if(output_triangles>cfg.max_output_triangles) return fail("geometry output triangle budget exceeded");
    if(!pack_terminal_roots(result,cfg.packed_root_triangles,cancel))return fail("geometry compilation cancelled");
    if(profile) {
        uint64_t roots_tris=0, stored_tris=0, census[5]{};
        double max_error=0;
        for(const auto& n:result.nodes){stored_tris+=n.mesh.triex.size();++census[static_cast<unsigned>(n.simplification)];}
        for(auto id:result.roots){roots_tris+=result.nodes[id].mesh.triex.size();max_error=std::max(max_error,result.nodes[id].error);}
        MATTER_LOGI("geometry", "compile_profile source=%zu nodes=%zu roots=%zu root_triangles=%llu stored_triangles=%llu leaf=%llu reduced=%llu attribute_seam=%llu border_changed=%llu no_reduction=%llu max_error=%.6f total_ms=%.3f simplify_ms=%.3f verify_ms=%.3f attributes_ms=%.3f",
            count,result.nodes.size(),result.roots.size(),(unsigned long long)roots_tris,(unsigned long long)stored_tris,
            (unsigned long long)census[0],(unsigned long long)census[1],(unsigned long long)census[2],(unsigned long long)census[3],(unsigned long long)census[4],max_error,ms(started),simplify_ms,verify_ms,attributes_ms);
    }
    std::sort(result.roots.begin(),result.roots.end());out=std::move(result);error.clear();return true;
}
} // namespace geometry
