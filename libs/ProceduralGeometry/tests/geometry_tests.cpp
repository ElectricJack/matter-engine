#include <matter/geometry/geometry.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace d=matter::contracts;
namespace s=matter::services;
namespace g=matter::geometry;
int checks=0;
void check(bool value,const char *label) { ++checks; if(!value) throw std::runtime_error(label); }
void near(float a,float b,const char *label) { check(std::abs(a-b)<=1e-6f+1e-6f*std::abs(b),label); }
s::JobEnvelope job{"geometry.fixture","1",7};
d::RecipeDocument recipe() {
    d::RecipeDocument r; r.source.id={"geometry.fixture"}; r.source.revision="1";
    r.required_capabilities={{"matter.recipe.core",1},{"matter.material.pbr",1}};
    d::MaterialDescription m; m.id={"clay"}; m.base_color.factor={0.75f,0.25f,0.125f,1}; m.roughness=0.8f;
    r.materials={m}; return r;
}
d::RecipeNode box() {
    d::RecipeNode n; n.id={"box"}; n.operation="primitive.box"; n.materials={{"clay"}};
    n.parameters=d::Value::Object{{"size",d::Value::Array{2,4,6}}}; return n;
}
d::RecipeNode sphere() {
    auto n=box(); n.id={"sphere"}; n.operation="primitive.sphere";
    n.parameters=d::Value::Object{{"radius",1},{"segments",12}}; return n;
}
g::Profile square() { return {{{-0.5f,-0.5f},{0.5f,-0.5f},{0.5f,0.5f},{-0.5f,0.5f}}, {}}; }
s::AssetResult evaluate(d::RecipeDocument r,const s::EvaluationOptions &options={},const s::JobControl &control={}) {
    g::PrimitiveGeometry geometry; return geometry.evaluate(job,r,{},options,control);
}
void ok(const s::AssetResult &r) {
    if(!r.ok()) for(const auto &e:r.diagnostics()) std::cerr<<d::code_name(e.code)<<": "<<e.node_id<<": "<<e.message<<'\n';
    check(r.ok(),"evaluation failed"); check(r.envelope()==job,"job identity");
    check(s::validate(*r.value()).ok(),"published asset validation");
}
void failed(const s::AssetResult &r,d::ErrorCode code) {
    check(!r.ok()&&!r.value(),"failure published partial output");
    bool found=false; for(const auto &e:r.diagnostics()) if(e.code==code) found=true;
    check(found,"expected diagnostic code");
}
void normals(const d::MeshPrototype &mesh) {
    for(size_t i=0;i<mesh.indices.size();i+=3) {
        auto a=mesh.indices[i],b=mesh.indices[i+1],c=mesh.indices[i+2];
        auto face=mm::normalize(mm::cross(mm::sub(mesh.positions[b],mesh.positions[a]),mm::sub(mesh.positions[c],mesh.positions[a])));
        for(auto j:{a,b,c}) { near(mm::length(mesh.normals[j]),1,"unit normal"); check(mm::dot(face,mesh.normals[j])>0,"normal points with winding"); }
    }
}
void primitives() {
    auto r=recipe(); r.nodes={box()}; r.outputs={{"box"}};
    auto result=evaluate(r); ok(result); const auto &mesh=result.value()->document.meshes[0];
    check(mesh.indices.size()==36&&mesh.positions.size()==36,"GEOM-BOX topology");
    near(mesh.bounds.min.x,-1,"box min x"); near(mesh.bounds.max.y,2,"box max y"); near(mesh.bounds.max.z,3,"box max z");
    check(mesh.submeshes[0].material.value=="clay","box material"); normals(mesh);
    r.nodes={sphere()}; r.outputs={{"sphere"}}; result=evaluate(r); ok(result); const auto &sp=result.value()->document.meshes[0];
    check(sp.indices.size()==792,"GEOM-SPHERE 264 triangles"); normals(sp);
    for(size_t i=0;i<sp.positions.size();++i) { near(mm::length(sp.positions[i]),1,"sphere radius"); near(mm::dot(sp.positions[i],sp.normals[i]),1,"sphere smooth normals"); }
    for(const char *operation:{"primitive.line","primitive.cylinder","primitive.cone","primitive.capsule"}) {
        auto n=box(); n.id={operation}; n.operation=operation;
        r=recipe(); r.required_capabilities.push_back({"matter.operation."+n.operation,1});
        n.parameters=d::Value::Object{{"a",d::Value::Array{0,0,0}},{"b",d::Value::Array{0,0,2}},{"segments",16}};
        auto &p=std::get<d::Value::Object>(n.parameters.data);
        if(n.operation=="primitive.capsule") { p["radius"]=0.5; p["rings"]=6; }
        else { p["radius0"]=0.5; p["radius1"]=n.operation=="primitive.cone"?0:0.5; }
        r.nodes={n}; r.outputs={n.id}; result=evaluate(r); ok(result);
        auto triangles=result.value()->document.meshes[0].indices.size()/3;
        check(triangles==(n.operation=="primitive.cone"?32u:n.operation=="primitive.capsule"?384u:64u),"GEOM-ROUND topology");
        const auto &rounded=result.value()->document.meshes[0]; normals(rounded);
        near(rounded.bounds.min.x,-0.5f,"round min x"); near(rounded.bounds.max.y,0.5f,"round max y");
        near(rounded.bounds.min.z,n.operation=="primitive.capsule"?-0.5f:0,"round min z");
        near(rounded.bounds.max.z,n.operation=="primitive.capsule"?2.5f:2,"round max z");
        check(rounded.submeshes[0].material.value=="clay","round material");
        if(n.operation=="primitive.capsule") {
            std::get<d::Value::Object>(r.nodes[0].parameters.data)["b"]=d::Value::Array{0,0,0};
            result=evaluate(r); ok(result);
            check(result.value()->document.meshes[0].indices.size()==1440,"coincident capsule preserves sphere fallback");
        }
    }
    std::cout<<"GEOM-BOX GEOM-SPHERE GEOM-ROUND passed\n";
}
void profiles() {
    g::Session session(recipe().source); check(session.material(recipe().materials[0]).ok(),"material cursor");
    check(session.retain({"profile"},square()).ok(),"retain profile");
    check(session.extrude({"extrusion"},{{0,0,0},{0,0,2}}).ok(),"consume profile");
    auto r=session.finish(job); ok(r); const auto &m=r.value()->document.meshes[0];
    check(m.indices.size()==36,"GEOM-EXTRUDE 12 triangles without flat fill"); normals(m);
    near(m.bounds.min.x,-0.5f,"extrude x min"); near(m.bounds.max.z,2,"extrude z max");
    g::Session fill(recipe().source); fill.retain({"flat"},square()); r=fill.finish(job); ok(r);
    check(r.value()->document.meshes[0].indices.size()==6,"GEOM-FLUSH two triangles");
    for(auto p:r.value()->document.meshes[0].positions) near(p.z,0,"flat plane");
    auto hole=square(); hole.outer={{-2,-2},{2,-2},{2,2},{-2,2}};
    hole.holes={{{-1,-1},{1,-1},{1,1},{-1,1}}};
    g::Session holed(recipe().source); holed.retain({"holed"},hole); holed.extrude({"sweep"},{{0,0,0},{0,0,2}});
    r=holed.finish(job); ok(r); check(r.value()->document.meshes[0].indices.size()==96,"GEOM-HOLES 32 triangles"); normals(r.value()->document.meshes[0]);
    for(auto join:{g::Join::miter,g::Join::bevel,g::Join::round}) {
        g::Session bent(recipe().source); bent.retain({"bent"},square()); bent.extrude({"sweep"},{{0,0,0},{0,0,2},{2,0,2}},join);
        r=bent.finish(job); ok(r); check(r.value()->document.meshes[0].indices.size()==(join==g::Join::miter?60u:84u),"GEOM-JOINS topology");
    }
    std::cout<<"GEOM-EXTRUDE GEOM-FLUSH GEOM-HOLES GEOM-JOINS passed\n";
}
void cursors() {
    g::Session session(recipe().source); session.material(recipe().materials[0]); session.tint({0.5f,1,1,0.5f});
    session.transform(mm::translation({1,2,3})); session.retain({"retained"},square());
    session.transform(mm::translation({10,20,30})); auto other=recipe().materials[0]; other.id={"blue"}; session.material(other); session.tint({1,1,1,0});
    session.extrude({"sweep"},{{0,0,0},{0,0,2}}); session.emit(box()); auto r=session.finish(job); ok(r); const auto &asset=r.value()->document;
    check(asset.instances.size()==2,"GEOM-CURSOR two instances"); near(asset.instances[0].local_transform.m[3],1,"retained transform");
    near(asset.instances[1].local_transform.m[3],10,"current transform"); const auto &id=asset.meshes[0].submeshes[0].material;
    bool matched=false; for(const auto &m:asset.materials) if(m.id==id) { near(m.base_color.factor.x,0.5625f,"retained tint material"); matched=true; }
    check(matched,"retained material exists");
    g::Session second(recipe().source); second.retain({"first"},square()); second.retain({"second"},square()); r=second.finish(job); ok(r);
    check(r.value()->document.meshes.size()==2,"new profile flushes prior one"); std::cout<<"GEOM-CURSOR passed\n";
}
void triangles() {
    for(int mode=0;mode<3;++mode) {
        auto n=box(); n.id={"triangles"}; n.operation="mesh.triangles";
        d::Value::Array positions=mode==1?d::Value::Array{d::Value::Array{0,0,0},d::Value::Array{1,0,0},
            d::Value::Array{0,1,0},d::Value::Array{1,1,0}}:
            d::Value::Array{d::Value::Array{0,0,0},d::Value::Array{1,0,0},d::Value::Array{1,1,0}};
        if(mode==2) positions.push_back(d::Value::Array{0,1,0});
        d::Value::Array shading,uv;
        for(size_t i=0;i<positions.size();++i) { shading.push_back(d::Value::Array{0,0,2}); uv.push_back(d::Value::Array{0.25,0.75}); }
        n.parameters=d::Value::Object{{"positions",positions},{"normals",shading},{"uv0",uv},{"mode",mode}};
        g::Session session(recipe().source); session.retain({"pending"},square()); session.emit(n);
        auto result=session.finish(job); ok(result); const auto &meshes=result.value()->document.meshes;
        check(meshes.size()==2,"triangle shape flushes retained profile");
        check(meshes[1].indices.size()==(mode==0?3u:6u),"GEOM-TRIANGLES topology"); normals(meshes[1]);
        for(auto t:meshes[1].uv_sets[0]) { near(t.x,0.25f,"authored UV x"); near(t.y,0.75f,"authored UV y"); }
    }
    std::cout<<"GEOM-TRIANGLES passed\n";
}
void placements() {
    auto r=recipe(); auto sp=sphere(); d::RecipeNode a; a.id={"placed.a"}; a.operation="placement.affine";
    a.inputs={{"mesh",d::InputKind::node,sp.id,d::ProductType::mesh}};
    auto b=a; b.id={"placed.b"}; b.transform=mm::multiply(mm::translation({4,2,1}),mm::scale({-2,3,4}));
    r.nodes={b,a,sp}; r.outputs={a.id,b.id}; auto result=evaluate(r); ok(result); const auto &asset=result.value()->document;
    check(asset.meshes.size()==1 && asset.instances.size()==2,"GEOM-SHARED prototype sharing");
    check(asset.instances[0].prototype==asset.instances[1].prototype,"shared reference identity"); check(asset.instances[0].id!=asset.instances[1].id,"separate instance identity");
    d::MeshPrototype placed; check(g::bake_placement(asset.meshes[0],b.transform,placed).ok(),"GEOM-AFFINE baking"); normals(placed);
    for(size_t i=0;i<placed.positions.size();++i) {
        auto local=asset.meshes[0].positions[i]; auto expected=mm::normalize(d::Vec3{-local.x/2,local.y/3,local.z/4});
        near(placed.normals[i].x,expected.x,"inverse transpose x"); near(placed.normals[i].y,expected.y,"inverse transpose y"); near(placed.normals[i].z,expected.z,"inverse transpose z");
    }
    near(asset.bounds.max.x,6,"placed world bounds"); auto singular=mm::scale({0,1,1}); auto previous=placed.indices;
    check(!g::bake_placement(asset.meshes[0],singular,placed).ok() && placed.indices==previous,"atomic singular placement failure");
    auto tangent_mesh=asset.meshes[0]; tangent_mesh.tangents.assign(tangent_mesh.positions.size(),{1,0,0,1});
    check(g::bake_placement(tangent_mesh,b.transform,placed).ok(),"affine tangent baking");
    near(placed.tangents[0].x,-1,"tangent vector placement"); near(placed.tangents[0].w,-1,"mirrored tangent handedness");
    std::cout<<"GEOM-SHARED GEOM-AFFINE passed\n";
}
void malformed() {
    g::PrimitiveGeometry service; auto admitted=recipe(); admitted.nodes={box()}; admitted.outputs={{"box"}};
    failed(service.evaluate({"","1",7},admitted,{}, {},{}),d::ErrorCode::invalid_input);
    failed(service.evaluate({"valid","1",0},admitted,{}, {},{}),d::ErrorCode::invalid_input);
    failed(service.evaluate({std::string(1,char(0xff)),"1",7},admitted,{}, {},{}),d::ErrorCode::invalid_input);
    for(auto p:{g::Profile{{{0,0},{2,2},{0,2},{2,0}},{}},g::Profile{{{0,0},{1,0},{1,0},{0,1}},{}},
        g::Profile{{{0,0},{1,0},{2,0}},{}},g::Profile{{{0,0},{1,0},{1,1},{0,1}},{{{2,2},{3,2},{3,3},{2,3}}}}}) {
        g::Session session(recipe().source); session.emit(box()); session.retain({"bad"},p); failed(session.finish(job),d::ErrorCode::invalid_input);
    }
    g::Session path(recipe().source); path.retain({"square"},square()); path.extrude({"bad"},{{0,0,0},{0,0,0}}); failed(path.finish(job),d::ErrorCode::invalid_input);
    g::Session missing(recipe().source); check(!missing.extrude({"bad"},{{0,0,0},{0,0,2}}).ok(),"missing profile diagnostic"); failed(missing.finish(job),d::ErrorCode::invalid_input);
    auto r=recipe(); r.nodes={sphere()}; r.outputs={{"sphere"}};
    s::EvaluationOptions limits; limits.budget.vertices=3; failed(evaluate(r,limits),d::ErrorCode::resource_limit);
    limits={}; limits.budget.working_bytes=1; failed(evaluate(r,limits),d::ErrorCode::resource_limit);
    limits={}; limits.budget.output_bytes=1; failed(evaluate(r,limits),d::ErrorCode::resource_limit);
    r.required_capabilities.push_back({"unknown.geometry",1}); failed(evaluate(r),d::ErrorCode::unsupported_capability);
    r=recipe(); r.nodes={box()}; r.outputs={{"box"}};
    r.extensions={{{"matter.recipe.core",1},true,d::Value::Object{}}};
    failed(evaluate(r),d::ErrorCode::unsupported_capability);
    r=recipe(); r.nodes={box()}; r.outputs={{"box"}}; s::CancellationSource cancellation; cancellation.cancel(); s::JobControl control; control.cancellation=cancellation.token();
    failed(evaluate(r,{},control),d::ErrorCode::cancelled);
    cancellation=s::CancellationSource{}; control.cancellation=cancellation.token(); control.progress=[&](const s::Progress &p){if(p.fraction==1) cancellation.cancel();}; failed(evaluate(r,{},control),d::ErrorCode::cancelled);
    control={}; control.progress=[](const s::Progress &){throw std::runtime_error("callback failure");}; failed(evaluate(r,{},control),d::ErrorCode::backend_failure);
    r.source_map={{"box",{"fixture://bad",4,2}}}; std::get<d::Value::Object>(r.nodes[0].parameters.data)["size"]=d::Value::Array{-1,1,1};
    auto bad=evaluate(r); failed(bad,d::ErrorCode::invalid_input); check(bad.diagnostics()[0].source.has_value(),"source diagnostic");
    std::cout<<"GEOM-MALFORMED GEOM-EXHAUSTION GEOM-CANCEL GEOM-CAPABILITIES passed\n";
}
int main() {
    try { primitives(); profiles(); cursors(); triangles(); placements(); malformed(); std::cout<<checks<<" assertions passed\n"; return 0; }
    catch(const std::exception &e) { std::cerr<<"FAILED: "<<e.what()<<'\n'; return 1; }
}
