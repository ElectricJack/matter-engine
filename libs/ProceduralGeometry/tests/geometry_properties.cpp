#include "geometry_properties.h"
#include <matter/geometry/geometry.h>
#if MATTER_GEOMETRY_EXPECT_MODIFIERS
#include "mesh_simplifier.hpp"
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace geometry_properties {
namespace d = matter::contracts;
namespace s = matter::services;
namespace g = matter::geometry;
namespace {
struct Random {
    uint32_t state;
    uint32_t next() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
    unsigned pick(unsigned count) { return next() % count; }
    float real(float low, float high) { return low + (high-low)*float(next() & 65535)/65535; }
};
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
bool same(d::Vec3 a,d::Vec3 b) { return a.x==b.x && a.y==b.y && a.z==b.z; }
void near(double a, double b, double tolerance, const char *message) {
    require(std::isfinite(a) && std::isfinite(b) && std::abs(a-b) <= tolerance, message);
}
const s::JobEnvelope job{"geometry.properties", "1", 17};
s::EvaluationOptions options() {
    s::EvaluationOptions o;
    o.budget.operations = 20000000;
    return o;
}
template<class T> void success(const s::JobResult<T> &r) {
    if (!r.ok()) {
        std::string message = "unexpected failure";
        for (const auto &e : r.diagnostics()) message += ": " + std::string(d::code_name(e.code)) + " " + e.message;
        throw std::runtime_error(message);
    }
    require(bool(r.value()) && r.envelope() == job, "success ownership/envelope");
}
template<class T> void failure(const s::JobResult<T> &r, d::ErrorCode code) {
    require(!r.ok() && !r.value(), "failure published partial output");
    require(r.envelope() == job, "failure envelope changed");
    require(std::any_of(r.diagnostics().begin(), r.diagnostics().end(), [&](const auto &e) { return e.code == code; }),
        "missing expected failure diagnostic");
}
std::string encoded(const d::AssetDocument &doc) {
    std::string bytes; require(d::encode(doc, bytes).ok(), "asset encoding"); return bytes;
}
d::RecipeDocument recipe() {
    d::RecipeDocument r; r.source.id={"geometry.properties"}; r.source.revision="1";
    r.required_capabilities={{"matter.recipe.core",1},{"matter.material.pbr",1}};
    return r;
}
d::RecipeNode node(std::string operation, d::Value::Object parameters, std::string id="shape") {
    d::RecipeNode n; n.id={std::move(id)}; n.operation=std::move(operation); n.parameters=std::move(parameters); return n;
}
d::RecipeDocument single(d::RecipeNode n) {
    auto r=recipe();
    if(n.operation != "primitive.box" && n.operation != "primitive.sphere")
        r.required_capabilities.push_back({"matter.operation."+n.operation,1});
    r.outputs={n.id}; r.nodes={std::move(n)}; return r;
}
s::AssetResult evaluate(const d::RecipeDocument &r) {
    return g::PrimitiveGeometry({0.5f}).evaluate(job,r,{},options(),{});
}
// Contracts use triangle soup. Weld only for this independent topology oracle:
// positions within 2e-5 m account for the trig seam in rounded primitives.
// Fixtures are metres in scale and have edges far larger than that tolerance.
void closed(const d::MeshPrototype &mesh) {
    using Key=std::tuple<long long,long long,long long>;
    std::map<Key,uint32_t> ids;
    std::vector<d::Vec3> points;
    std::vector<uint32_t> welded;
    constexpr double cell=2e-5;
    for(auto p:mesh.positions) {
        const long long x=std::llround(p.x/cell), y=std::llround(p.y/cell), z=std::llround(p.z/cell);
        uint32_t id=uint32_t(points.size());
        bool found=false;
        for(int dx=-1;dx<=1 && !found;++dx) for(int dy=-1;dy<=1 && !found;++dy) for(int dz=-1;dz<=1 && !found;++dz) {
            auto it=ids.find({x+dx,y+dy,z+dz});
            if(it!=ids.end() && mm::length(mm::sub(points[it->second],p))<cell) { id=it->second; found=true; }
        }
        if(!found) { ids.emplace(Key{x,y,z},id); points.push_back(p); }
        welded.push_back(id);
    }
    std::map<std::pair<uint32_t,uint32_t>,std::pair<unsigned,int>> edges;
    std::map<uint32_t,std::vector<std::pair<uint32_t,uint32_t>>> links;
    double volume=0;
    for(size_t t=0;t<mesh.indices.size();t+=3) {
        uint32_t v[3]={welded.at(mesh.indices[t]),welded.at(mesh.indices[t+1]),welded.at(mesh.indices[t+2])};
        require(v[0]!=v[1] && v[1]!=v[2] && v[2]!=v[0], "welded degenerate triangle");
        for(int k=0;k<3;++k) {
            auto a=v[k],b=v[(k+1)%3]; auto &e=edges[std::minmax(a,b)];
            ++e.first; e.second += a<b ? 1 : -1;
            links[a].push_back({v[(k+1)%3],v[(k+2)%3]});
        }
        auto a=mesh.positions[mesh.indices[t]], b=mesh.positions[mesh.indices[t+1]], c=mesh.positions[mesh.indices[t+2]];
        volume += double(mm::dot(a,mm::cross(b,c)))/6;
    }
    // Edge incidence alone misses two closed sheets touching at one vertex.
    // Every vertex link must be one connected cycle, with degree two.
    for(const auto &entry:links) {
        std::map<uint32_t,std::vector<uint32_t>> adjacency;
        for(auto edge:entry.second) { adjacency[edge.first].push_back(edge.second); adjacency[edge.second].push_back(edge.first); }
        for(const auto &neighbor:adjacency) require(neighbor.second.size()==2,"nonmanifold vertex link degree");
        std::vector<uint32_t> pending{adjacency.begin()->first}; std::set<uint32_t> seen;
        while(!pending.empty()) {
            auto v=pending.back(); pending.pop_back();
            if(seen.insert(v).second) for(auto neighbor:adjacency.at(v)) pending.push_back(neighbor);
        }
        require(seen.size()==adjacency.size(),"disconnected nonmanifold vertex link");
    }
    require(!edges.empty(), "closed fixture is empty");
    for(const auto &entry:edges) if(entry.second.first!=2 || entry.second.second!=0) {
        const auto a=points[entry.first.first], b=points[entry.first.second];
        throw std::runtime_error("mesh edge incidence="+std::to_string(entry.second.first)+" orientation="+std::to_string(entry.second.second)+
            " at ("+std::to_string(a.x)+","+std::to_string(a.y)+","+std::to_string(a.z)+") -> ("+
            std::to_string(b.x)+","+std::to_string(b.y)+","+std::to_string(b.z)+")");
    }
    require(volume>0, "closed mesh winding is inward");
}
void valid(const s::AssetResult &r) {
    success(r); require(s::validate(*r.value()).ok(), "published asset contract");
    for(const auto &m:r.value()->document.meshes) {
        require(!m.indices.empty() && m.indices.size()%3==0, "mesh triangle structure");
        for(auto p:m.positions) {
            require(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z), "nonfinite vertex");
            require(p.x>=m.bounds.min.x && p.x<=m.bounds.max.x && p.y>=m.bounds.min.y && p.y<=m.bounds.max.y &&
                p.z>=m.bounds.min.z && p.z<=m.bounds.max.z, "mesh bounds miss vertex");
        }
        for(auto n:m.normals) near(mm::length(n),1,2e-4,"unit normal");
        for(auto i:m.indices) require(i<m.positions.size(), "invalid index");
    }
}
void repeat(const d::RecipeDocument &r, const s::AssetResult &a) {
    auto b=evaluate(r); valid(b);
    require(encoded(a.value()->document)==encoded(b.value()->document), "evaluation is nondeterministic");
}
void primitives(Random &rng) {
    const unsigned kind=rng.pick(6), segments=4+rng.pick(13);
    const float radius=rng.real(0.3f,1.5f), height=rng.real(0.5f,3.0f);
    d::RecipeNode n;
    d::Vec3 size{rng.real(0.5f,3),rng.real(0.5f,3),rng.real(0.5f,3)};
    if(kind==0) n=node("primitive.box",{{"size",d::Value::Array{size.x,size.y,size.z}}});
    else if(kind==1) n=node("primitive.sphere",{{"radius",radius},{"segments",int(segments)}});
    else {
        const char *ops[]={"primitive.line","primitive.cylinder","primitive.cone","primitive.capsule"};
        d::Value::Object p{{"a",d::Value::Array{0,0,0}},{"b",d::Value::Array{0,0,height}},{"segments",int(segments)}};
        if(kind==5) { p["radius"]=radius; p["rings"]=2+int(rng.pick(4)); }
        else { p["radius0"]=kind==4 && segments%2 ? 0 : radius; p["radius1"]=kind==4 && segments%2==0 ? 0 : radius; }
        n=node(ops[kind-2],std::move(p));
    }
    auto r=single(n); auto result=evaluate(r); valid(result); repeat(r,result);
    const auto &mesh=result.value()->document.meshes.at(0); closed(mesh);
    if(kind==0) {
        near(mesh.bounds.min.x,-size.x/2,1e-6,"box min"); near(mesh.bounds.max.z,size.z/2,1e-6,"box max");
    } else if(kind==1) {
        for(auto p:mesh.positions) near(mm::length(p),radius,2e-6,"sphere radius");
    } else {
        for(auto p:mesh.positions) {
            require(std::hypot(p.x,p.y)<=radius+2e-6, "axial primitive radius bound");
            require(p.z >= (kind==5?-radius:0)-2e-6 && p.z <= height+(kind==5?radius:0)+2e-6, "axial primitive length bound");
        }
    }
    // Affine baking must remain atomic for a rejected transform and preserve
    // closed winding even under a mirror/nonuniform scale.
    d::MeshPrototype baked;
    auto transform=mm::multiply(mm::translation({rng.real(-2,2),rng.real(-2,2),rng.real(-2,2)}),
        mm::scale({-rng.real(0.5f,2),rng.real(0.5f,2),rng.real(0.5f,2)}));
    require(g::bake_placement(mesh,transform,baked).ok(), "affine bake rejected"); closed(baked);
    const auto prior=baked.positions;
    require(!g::bake_placement(mesh,mm::scale({0,1,1}),baked).ok() && baked.positions.size()==prior.size() &&
        std::equal(baked.positions.begin(),baked.positions.end(),prior.begin(),[](auto a,auto b){return same(a,b);}), "failed bake mutated destination");
    // One invalid parameter among a successful prefix must reject the whole job.
    auto bad=n;
    auto &p=std::get<d::Value::Object>(bad.parameters.data);
    if(kind==0) p["size"]=d::Value::Array{-1,size.y,size.z};
    else p["segments"]=2;
    r.nodes.push_back(bad); r.nodes.back().id={"bad"}; r.outputs.push_back({"bad"});
    failure(evaluate(r),d::ErrorCode::invalid_input);
}
double profile_area(const std::vector<d::Vec2> &p) {
    double sum=0; for(size_t i=0;i<p.size();++i) sum+=double(p[i].x)*p[(i+1)%p.size()].y-double(p[i].y)*p[(i+1)%p.size()].x;
    return std::abs(sum)/2;
}
void profiles(Random &rng) {
    // Radially ordered simple polygons include concavity, either winding and
    // disjoint holes. Dyadic rectangles make hole cap coverage reproducible.
    g::Profile p;
    bool hole=rng.pick(3)==0;
    if(hole) {
        float x=1+float(rng.pick(8))/8, y=1+float(rng.pick(8))/8;
        p.outer={{-x,-y},{x,-y},{x,y},{-x,y}};
        p.holes={{{-0.25f,-0.25f},{0.25f,-0.25f},{0.25f,0.25f},{-0.25f,0.25f}}};
    } else {
        unsigned count=3+rng.pick(8);
        for(unsigned i=0;i<count;++i) {
            double angle=6.283185307179586*double(i)/count; float radius=rng.real(0.6f,1.4f);
            p.outer.push_back({radius*float(std::cos(angle)),radius*float(std::sin(angle))});
        }
    }
    if(rng.pick(2)) std::reverse(p.outer.begin(),p.outer.end());
    if(hole && rng.pick(2)) std::reverse(p.holes[0].begin(),p.holes[0].end());
    float depth=rng.real(0.5f,2);
    bool core_extrude=rng.pick(2)!=0;
    auto build=[&](bool fill) {
        if(core_extrude && !fill) {
            d::Value::Array points,holes;
            for(auto v:p.outer) points.push_back(d::Value::Array{v.x,v.y});
            for(const auto &h:p.holes) {
                d::Value::Array contour;
                for(auto v:h) contour.push_back(d::Value::Array{v.x,v.y});
                holes.push_back(std::move(contour));
            }
            auto shape=node(hole ? "profile.contours" : "profile.polygon",{{"points",points}},"profile");
            if(hole) std::get<d::Value::Object>(shape.parameters.data)["holes"]=std::move(holes);
            shape.output_type=d::ProductType::profile;
            auto extrusion=node("profile.extrude",{{"depth",depth}},"extrusion");
            extrusion.inputs={{"profile",d::InputKind::node,shape.id,d::ProductType::profile}};
            auto r=recipe();
            if(hole) r.required_capabilities.push_back({"matter.operation.profile.contours",1});
            r.nodes={extrusion,shape}; r.outputs={extrusion.id};
            return evaluate(r);
        }
        g::Session session(recipe().source);
        require(session.retain({"profile"},p).ok(), "retain valid profile");
        if(!fill) require(session.extrude({"extrusion"},{{0,0,0},{0,0,depth}}).ok(), "extrude valid profile");
        return session.finish(job,options());
    };
    double area=profile_area(p.outer); for(const auto &h:p.holes) area-=profile_area(h);
    auto flat=build(true); valid(flat); double actual=0;
    const auto &cap=flat.value()->document.meshes.at(0);
    for(size_t t=0;t<cap.indices.size();t+=3) {
        auto a=cap.positions[cap.indices[t]], b=cap.positions[cap.indices[t+1]], c=cap.positions[cap.indices[t+2]];
        auto normal=mm::cross(mm::sub(b,a),mm::sub(c,a));
        require(normal.z>0,"cap winding"); actual+=normal.z/2;
    }
    near(actual,area,1e-6*std::max(1.0,area),"profile filled area");
    auto result=build(false); valid(result); closed(result.value()->document.meshes.at(0));
    auto again=build(false); valid(again);
    require(encoded(result.value()->document)==encoded(again.value()->document), "extrusion is nondeterministic");
    near(result.value()->document.meshes.at(0).bounds.max.z,depth,1e-6,"extrusion depth");
    g::Session bad(recipe().source);
    bad.emit(node("primitive.box",{{"size",d::Value::Array{1,1,1}}}));
    p.outer.insert(p.outer.begin()+1,p.outer[0]);
    bad.retain({"bad"},p);
    failure(bad.finish(job,options()),d::ErrorCode::invalid_input);
}
void csg(Random &rng) {
    auto r=recipe(); r.required_capabilities.push_back({"matter.operation.solid.ordered",1});
    float radius=rng.real(0.55f,1.0f), cut=rng.real(0.1f,0.4f);
    auto a=node("primitive.sphere",{{"radius",radius},{"segments",8}},"a");
    auto b=node("primitive.box",{{"size",d::Value::Array{2,3,3}}},"b");
    auto placed=node("placement.affine",{},"cutter");
    placed.inputs={{"mesh",d::InputKind::node,b.id,d::ProductType::mesh}};
    placed.transform=mm::translation({cut+1,0,0});
    auto solid=node("solid.ordered",{{"operations",d::Value::Array{"union","subtract"}},
        {"spacing",0.5},{"smoothing",0}},"solid");
    solid.inputs={{"a",d::InputKind::node,a.id,d::ProductType::mesh},{"b",d::InputKind::node,placed.id,d::ProductType::mesh}};
    r.nodes={a,b,placed,solid}; r.outputs={solid.id};
    auto result=evaluate(r); valid(result); repeat(r,result);
    const auto &mesh=result.value()->document.meshes.at(0); closed(mesh);
    // Linear marching-cubes interpolation lies inside the sphere's convex
    // analytic bounds and exactly represents the planar subtraction face.
    for(auto p:mesh.positions) { require(mm::length(p)<=radius+2e-5f,"CSG sphere bound"); require(p.x<=cut+2e-5f,"ordered subtraction bound"); }
    std::swap(r.nodes.back().inputs[0],r.nodes.back().inputs[1]);
    auto reversed=evaluate(r); valid(reversed);
    require(encoded(result.value()->document)!=encoded(reversed.value()->document),"ordered CSG operands were reordered");
    // A late union of the original restores exactly its scalar zero set.
    r.nodes.back()=solid;
    r.nodes.back().inputs.push_back({"restore",d::InputKind::node,a.id,d::ProductType::mesh});
    std::get<d::Value::Object>(r.nodes.back().parameters.data)["operations"]=d::Value::Array{"union","subtract","union"};
    auto restored=evaluate(r); valid(restored); closed(restored.value()->document.meshes.at(0));
    require(restored.value()->document.meshes[0].bounds.max.x>cut,"late union failed to restore sphere");
    r.nodes.back().inputs.resize(1);
    std::get<d::Value::Object>(r.nodes.back().parameters.data)["operations"]=d::Value::Array{"subtract"};
    auto empty=evaluate(r); valid(empty);
    require(empty.value()->document.meshes.empty() && empty.value()->document.instances.empty(),"opening subtract stage isn't empty");
    r.nodes.back()=solid;
    std::get<d::Value::Object>(r.nodes.back().parameters.data)["spacing"]=0;
    failure(evaluate(r),d::ErrorCode::invalid_input);
    // Smooth grouped union uses a second sphere, and expands the zero set by
    // at most k*log(2) beyond the hard union's conservative coordinate bounds.
    float small=rng.real(0.4f,0.7f), smoothing=rng.real(0.05f,0.2f);
    r.nodes[1]=node("primitive.sphere",{{"radius",small},{"segments",8}},"b");
    r.nodes[2].transform=mm::translation({0.3f,0,0});
    r.nodes.back()=solid;
    r.nodes.back().parameters=d::Value::Object{{"operations",d::Value::Array{"union","union"}},
        {"spacing",0.5},{"smoothing",smoothing}};
    auto smooth=evaluate(r); valid(smooth); closed(smooth.value()->document.meshes.at(0));
    float extent=std::max(radius,0.3f+small)+smoothing*float(std::log(2.0));
    for(auto p:smooth.value()->document.meshes[0].positions)
        require(std::abs(p.x)<=extent+2e-5 && std::abs(p.y)<=extent+2e-5 && std::abs(p.z)<=extent+2e-5,"smoothed union escaped analytic bounds");
    r.nodes.back().operation="solid.csg";
    r.nodes.back().parameters=d::Value::Object{{"operator","subtract"}};
    r.nodes.back().inputs[1].reference=a.id;
    auto difference=evaluate(r); valid(difference);
    require(difference.value()->document.meshes.empty(),"core A-A CSG subtraction isn't empty");
}
void queries(Random &rng) {
    d::Vec3 half{rng.real(0.5f,2),rng.real(0.5f,2),rng.real(0.5f,2)};
    auto r=single(node("primitive.box",{{"size",d::Value::Array{half.x*2,half.y*2,half.z*2}}}));
    auto asset=evaluate(r); valid(asset); const auto &mesh=asset.value()->document.meshes.at(0);
    s::QueryBatch batch;
    for(unsigned i=0;i<24;++i) {
        s::SurfaceQuery q; q.prototype=mesh.id;
        q.position={rng.real(-3,3)*half.x,rng.real(-3,3)*half.y,rng.real(-3,3)*half.z};
        batch.samples.push_back(q);
    }
    for(unsigned i=0;i<12;++i) {
        s::SurfaceQuery q; q.prototype=mesh.id; q.kind=s::QueryKind::ray_surface;
        q.position={rng.real(-0.8f,0.8f)*half.x,half.y+rng.real(0.5f,3),rng.real(-0.8f,0.8f)*half.z};
        q.direction={0,-rng.real(0.1f,10),0}; batch.samples.push_back(q);
    }
    g::PrimitiveGeometry service;
    auto result=service.query(job,*asset.value(),batch,options(),{}); success(result);
    auto again=service.query(job,*asset.value(),batch,options(),{}); success(again);
    require(result.value()->samples.size()==batch.samples.size(),"batch changed length");
    for(size_t i=0;i<batch.samples.size();++i) {
        const auto &q=batch.samples[i]; const auto &hit=result.value()->samples[i], &other=again.value()->samples[i];
        require(hit.hit && hit.triangle<mesh.indices.size()/3,"missing/invalid query hit");
        require(hit.hit==other.hit && hit.triangle==other.triangle && same(hit.position,other.position) &&
            same(hit.normal,other.normal) && hit.distance_metres==other.distance_metres,"query nondeterminism");
        auto p=q.position;
        d::Vec3 nearest{std::clamp(p.x,-half.x,half.x),std::clamp(p.y,-half.y,half.y),std::clamp(p.z,-half.z,half.z)};
        double expected=mm::length(mm::sub(p,nearest));
        if(q.kind==s::QueryKind::ray_surface) { nearest={p.x,half.y,p.z}; expected=p.y-half.y; }
        else if(expected==0) expected=std::min({half.x-std::abs(p.x),half.y-std::abs(p.y),half.z-std::abs(p.z)});
        near(hit.distance_metres,expected,3e-5,"analytic box query distance");
        near(mm::length(mm::sub(hit.position,p)),expected,3e-5,"query metric distance");
        near(mm::length(hit.normal),1,2e-5,"query unit normal");
        require(std::abs(hit.position.x)<=half.x+2e-5 && std::abs(hit.position.y)<=half.y+2e-5 && std::abs(hit.position.z)<=half.z+2e-5 &&
            std::min({std::abs(std::abs(hit.position.x)-half.x),std::abs(std::abs(hit.position.y)-half.y),std::abs(std::abs(hit.position.z)-half.z)})<2e-5,
            "query point isn't on the analytic box surface");
        auto one=service.query(job,*asset.value(),s::QueryBatch{{q}},options(),{}); success(one);
        require(same(one.value()->samples[0].position,hit.position) && one.value()->samples[0].triangle==hit.triangle,"batch differs from scalar query");
    }
    auto limited=options(); limited.budget.queries=batch.samples.size()-1;
    failure(service.query(job,*asset.value(),batch,limited,{}),d::ErrorCode::resource_limit);
    batch.samples.back().prototype={"missing"};
    failure(service.query(job,*asset.value(),batch,options(),{}),d::ErrorCode::missing_dependency);
}
#if MATTER_GEOMETRY_EXPECT_MODIFIERS
MeshIndexed octasphere(unsigned levels, float radius) {
    MeshIndexed m;
    m.positions={make_float3(radius,0,0),make_float3(-radius,0,0),make_float3(0,radius,0),
        make_float3(0,-radius,0),make_float3(0,0,radius),make_float3(0,0,-radius)};
    m.indices={0,2,4,2,1,4,1,3,4,3,0,4,2,0,5,1,2,5,3,1,5,0,3,5};
    for(unsigned l=0;l<levels;++l) {
        std::map<std::pair<uint32_t,uint32_t>,uint32_t> mid;
        auto midpoint=[&](uint32_t a,uint32_t b) {
            auto key=std::minmax(a,b); auto it=mid.find(key); if(it!=mid.end()) return it->second;
            auto p=m.positions[a]+m.positions[b]; p=p*(radius/std::sqrt(dot(p,p)));
            uint32_t index=uint32_t(m.positions.size()); m.positions.push_back(p); mid[key]=index; return index;
        };
        std::vector<uint32_t> indices;
        for(size_t t=0;t<m.indices.size();t+=3) {
            auto a=m.indices[t], b=m.indices[t+1], c=m.indices[t+2];
            auto ab=midpoint(a,b), bc=midpoint(b,c), ca=midpoint(c,a);
            indices.insert(indices.end(),{a,ab,ca,b,bc,ab,c,ca,bc,ab,bc,ca});
        }
        m.indices=std::move(indices);
    }
    return m;
}
d::MeshPrototype neutral(const MeshIndexed &m) {
    d::MeshPrototype out; out.indices=m.indices;
    for(auto p:m.positions) out.positions.push_back({p.x,p.y,p.z});
    return out;
}
bool same(const MeshIndexed &a,const MeshIndexed &b) {
    if(a.indices!=b.indices || a.positions.size()!=b.positions.size()) return false;
    for(size_t i=0;i<a.positions.size();++i) if(a.positions[i].x!=b.positions[i].x ||
        a.positions[i].y!=b.positions[i].y || a.positions[i].z!=b.positions[i].z) return false;
    return true;
}
void modifiers(Random &rng) {
    auto input=octasphere(2,rng.real(0.5f,2)); auto original=input;
    float epsilon=rng.real(0.05f,0.4f);
    auto zero=decimate_to_error(input,0,false);
    require(zero.indices.size()==input.indices.size(),"zero-error LOD removed curved triangles");
    auto lod=decimate_to_error(input,epsilon,false), again=decimate_to_error(input,epsilon,false);
    require(same(lod,again) && same(input,original),"LOD nondeterminism/input mutation");
    require(!lod.indices.empty() && lod.indices.size()<=input.indices.size(),"LOD grows/empties mesh");
    closed(neutral(lod));
    // Open terrain patch: production LOD must preserve every outer rim
    // vertex exactly, independent of the requested collapse error.
    MeshIndexed grid;
    for(int y=0;y<5;++y) for(int x=0;x<5;++x)
        grid.positions.push_back(make_float3(float(x),float(y),0.01f*float(x*y*(4-x)*(4-y))));
    for(uint32_t y=0;y<4;++y) for(uint32_t x=0;x<4;++x) {
        uint32_t a=y*5+x; grid.indices.insert(grid.indices.end(),{a,a+1,a+5,a+1,a+6,a+5});
    }
    auto coarse=decimate_to_error(grid,epsilon,true);
    require(coarse.indices.size()<=grid.indices.size(),"terrain LOD grows mesh");
    for(auto p:grid.positions) if(p.x==0 || p.x==4 || p.y==0 || p.y==4)
        require(std::any_of(coarse.positions.begin(),coarse.positions.end(),[&](auto q){return p.x==q.x && p.y==q.y && p.z==q.z;}),"terrain LOD moved/deleted boundary");
    d::Value::Array positions,normals,uv;
    for(auto i:input.indices) {
        auto p=input.positions[i]; auto n=mm::normalize(d::Vec3{p.x,p.y,p.z});
        positions.push_back(d::Value::Array{p.x,p.y,p.z}); normals.push_back(d::Value::Array{n.x,n.y,n.z}); uv.push_back(d::Value::Array{0.25,0.75});
    }
    auto r=single(node("mesh.triangles",{{"positions",positions},{"normals",normals},{"uv0",uv},{"mode",0}}));
    auto asset=evaluate(r); valid(asset); auto before=encoded(asset.value()->document);
    auto id=asset.value()->document.meshes.at(0).id.value;
    d::Value simplify=d::Value::Object{{"operation","mesh.simplify"},{"ratio",rng.real(0.4f,0.9f)}};
    d::Value smooth=d::Value::Object{{"operation","mesh.smooth"},{"iterations",1+int(rng.pick(3))}};
    d::Value::Array stack=rng.pick(2)?d::Value::Array{simplify,smooth}:d::Value::Array{smooth,simplify};
    s::ProcessingRequest request;
    request.required_capabilities={{"matter.operation.mesh.simplify",1},{"matter.operation.mesh.smooth",1}};
    request.settings=d::Value::Object{{"regions",d::Value::Array{d::Value::Object{
        {"prototypes",d::Value::Array{id}},{"modifiers",stack}}}}};
    g::PrimitiveGeometry service;
    auto result=service.process(job,*asset.value(),request,options(),{}); valid(result);
    auto repeated=service.process(job,*asset.value(),request,options(),{}); valid(repeated);
    require(encoded(result.value()->document)==encoded(repeated.value()->document),"modifier nondeterminism");
    require(before==encoded(asset.value()->document),"modifier mutated owned input");
    const auto &out=result.value()->document.meshes[0]; closed(out);
    require(out.indices.size()<=input.indices.size(),"simplify increased triangle count");
    require(out.id==asset.value()->document.meshes[0].id && out.submeshes[0].material==asset.value()->document.meshes[0].submeshes[0].material,"modifier lost identities");
    for(auto p:out.uv_sets.at(0)) { near(p.x,0.25,2e-6,"modifier UV u"); near(p.y,0.75,2e-6,"modifier UV v"); }
    auto limited=options(); limited.budget.operations=1;
    failure(service.process(job,*asset.value(),request,limited,{}),d::ErrorCode::resource_limit);
}
#endif
}
void run(unsigned group,uint32_t seed) {
    Random random{seed ? seed : 0x9e3779b9u};
    switch(group) {
    case 0: primitives(random); break;
    case 1: profiles(random); break;
    case 2: csg(random); break;
    case 3: queries(random); break;
#if MATTER_GEOMETRY_EXPECT_MODIFIERS
    case 4: modifiers(random); break;
#endif
    default: throw std::runtime_error("unknown property group");
    }
}
void fuzz(const uint8_t *bytes,size_t size) {
    if(size==0) return;
    uint32_t seed=2166136261u;
    // Every byte influences a bounded case; no untrusted length controls an
    // allocation or grid extent. Large inputs cannot monopolize the harness.
    for(size_t i=1;i<std::min(size,size_t(4096));++i) seed=(seed^bytes[i])*16777619u;
    constexpr unsigned groups=MATTER_GEOMETRY_EXPECT_MODIFIERS ? 5 : 4;
    run(bytes[0]%groups,seed);
}
}
