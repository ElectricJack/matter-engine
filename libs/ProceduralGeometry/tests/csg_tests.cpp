#include "csg.h"
#include "surface.h"
#include <cstdlib>
#include <iostream>
#include <thread>
namespace d=matter::contracts;
namespace s=matter::services;
namespace g=matter::geometry;
namespace impl=matter::geometry::detail;
namespace {
int checks=0;
void check(bool ok,const char *what) { ++checks; if(!ok) throw std::runtime_error(what); }
void near(float a,float b,const char *what,float tolerance=1e-6f) {
    check(std::abs(a-b)<=tolerance+1e-6f*std::abs(b),what);
}
d::RecipeNode sphere(std::string id,float radius) {
    d::RecipeNode n; n.id={id}; n.operation="primitive.sphere";
    n.parameters=d::Value::Object{{"radius",double(radius)},{"segments",12}}; return n;
}
d::RecipeNode box(std::string id,float size) {
    auto n=sphere(id,size); n.operation="primitive.box";
    n.parameters=d::Value::Object{{"size",d::Value::Array{double(size),double(size),double(size)}}}; return n;
}
d::RecipeNode place(std::string id,const d::RecipeNode &input,float x) {
    d::RecipeNode n; n.id={id}; n.operation="placement.affine"; n.transform(0,3)=x;
    n.inputs={{"shape",d::InputKind::node,input.id,d::ProductType::mesh}}; return n;
}
d::RecipeDocument fixture(bool ordered=false,float smoothing=0) {
    d::RecipeDocument r; r.source.id={"csg.fixture"};
    r.required_capabilities={{"matter.recipe.core",1},{"matter.material.pbr",1}};
    d::MaterialDescription material; material.id={"clay"}; r.materials={material};
    auto a=sphere("a",0.83f),b=box("b",1.2f),p=place("cutter",b,1);
    b.parameters=d::Value::Object{{"size",d::Value::Array{1.2,3,3}}};
    d::RecipeNode solid; solid.id={"solid"}; solid.operation=ordered?"solid.ordered":"solid.csg"; solid.materials={{"clay"}};
    solid.inputs={{"a",d::InputKind::node,a.id,d::ProductType::mesh},{"b",d::InputKind::node,p.id,d::ProductType::mesh}};
    solid.parameters=d::Value::Object{{"operator","subtract"}};
    if(ordered) {
        r.required_capabilities.push_back({"matter.operation.solid.ordered",1});
        solid.parameters=d::Value::Object{{"operations",d::Value::Array{"union","subtract"}},{"spacing",0.125},{"smoothing",double(smoothing)}};
    }
    r.nodes={a,b,p,solid}; r.outputs={solid.id}; return r;
}
s::JobEnvelope job{"csg.fixture","v1",42};
s::EvaluationOptions generous() {
    s::EvaluationOptions o; o.budget.operations=50000000; o.budget.execution_microseconds=30000000; return o;
}
s::AssetResult evaluate(const d::RecipeDocument &r,s::EvaluationOptions o=generous(),s::JobControl control={},float spacing=0.125f) {
    return g::PrimitiveGeometry({spacing}).evaluate(job,r,{},o,control);
}
void ok(const s::AssetResult &r) {
    if(!r.ok()) for(const auto &e:r.diagnostics()) std::cerr<<d::code_name(e.code)<<": "<<e.node_id<<": "<<e.message<<'\n';
    check(r.ok(),"CSG evaluation failed"); check(r.envelope()==job,"job identity preserved");
    check(s::validate(*r.value()).ok(),"published asset validates");
}
void failed(const s::AssetResult &r,d::ErrorCode code) {
    check(!r.ok()&&!r.value(),"failure must not publish an asset");
    bool found=false; for(const auto &e:r.diagnostics()) if(e.code==code) found=true;
    if(!found) for(const auto &e:r.diagnostics()) std::cerr<<d::code_name(e.code)<<": "<<e.message<<'\n';
    check(found,"expected failure diagnostic");
}
impl::CsgField lower(const d::RecipeDocument &r,s::JobMonitor &monitor,s::ResourceUsage &usage) {
    std::map<std::string,const d::RecipeNode *> nodes;
    for(const auto &n:r.nodes) nodes.emplace(n.id.value,&n);
    return impl::lower_csg(r.nodes.back(),nodes,{0.125f},monitor,usage);
}
float sample(const d::RecipeDocument &r,d::Vec3 point) {
    s::JobMonitor monitor(generous().budget,{}); s::ResourceUsage usage;
    auto f=lower(r,monitor,usage); std::vector<float> values(f.program.size()),group(f.program.size());
    return impl::sample_csg(f,point,values,group,monitor,usage);
}
void semantics() {
    auto r=fixture(); auto result=evaluate(r); ok(result);
    const auto &mesh=result.value()->document.meshes[0];
    check(result.value()->document.meshes.size()==1,"CSG operands are not independently emitted");
    near(mesh.bounds.max.x,0.4f,"GEOM-CSG analytic cut face",0.02f);
    size_t cut_normals=0;
    for(size_t i=0;i<mesh.positions.size();++i) if(std::abs(mesh.positions[i].x-0.4f)<1e-5f&&
        std::abs(mesh.positions[i].y)<0.2f&&std::abs(mesh.positions[i].z)<0.2f) {
        check(mesh.normals[i].x>0.95f,"cut normal faces cavity"); ++cut_normals;
    }
    check(cut_normals>0,"cut face sampled"); check(mesh.submeshes[0].material.value=="clay","CSG output material");
    check(sample(r,{0,0,0})<0&&sample(r,{0.6f,0,0})>0,"difference membership");
    std::swap(r.nodes.back().inputs[0],r.nodes.back().inputs[1]);
    check(sample(r,{0,0,0})>0&&sample(r,{1.2f,0,0})<0,"difference preserves order");
    r=fixture(); r.nodes[2].transform={};
    r.nodes.back().inputs.push_back({"c",d::InputKind::node,r.nodes[0].id,d::ProductType::mesh});
    std::get<d::Value::Object>(r.nodes.back().parameters.data)["operator"]="intersect";
    check(sample(r,{0.7f,0,0})>0,"core intersection is left-fold, not grouped union");
    r=fixture(true); r.nodes.back().inputs.push_back({"restore",d::InputKind::node,r.nodes[0].id,d::ProductType::mesh});
    std::get<d::Value::Object>(r.nodes.back().parameters.data)["operations"]=d::Value::Array{"union","subtract","union"};
    check(sample(r,{0.6f,0,0})<0,"union after difference restores matter");
    for(const char *opening:{"subtract","intersect"}) {
        r=fixture(true); r.nodes.back().inputs.resize(1);
        std::get<d::Value::Object>(r.nodes.back().parameters.data)["operations"]=d::Value::Array{opening};
        result=evaluate(r); ok(result);
        check(result.value()->document.meshes.empty()&&result.value()->document.instances.empty(),"opening negative stage is empty");
    }
    r=fixture(); auto coarse=evaluate(r); auto fine=evaluate(r,generous(),{},0.0625f); ok(fine);
    check(fine.value()->document.meshes[0].indices.size()>coarse.value()->document.meshes[0].indices.size(),"spacing controls actual lattice");
    auto shared=place("shared",r.nodes.back(),2); r.nodes.push_back(shared); r.outputs.push_back(shared.id);
    result=evaluate(r); ok(result); check(result.value()->document.meshes.size()==1,"CSG prototype shared across placements");
    r=fixture(true); r.nodes.back().inputs.clear(); d::Value::Array operations;
    for(int i=0;i<71;++i) {
        r.nodes.back().inputs.push_back({"stage"+std::to_string(i),d::InputKind::node,r.nodes[0].id,d::ProductType::mesh});
        operations.emplace_back(i%2?"subtract":"union");
    }
    std::get<d::Value::Object>(r.nodes.back().parameters.data)["operations"]=std::move(operations);
    near(sample(r,{0,0,0}),-0.83f,"no legacy 64-stage truncation");
    r=fixture(); r.nodes.back().inputs[1].reference=r.nodes[0].id;
    std::get<d::Value::Object>(r.nodes.back().parameters.data)["operator"]="union";
    result=evaluate(r,generous(),{},0.03f); ok(result);
    check(result.value()->document.meshes[0].positions.size()>65535,"32-bit CPU output exceeds legacy vertex ceiling");
    std::cout<<"GEOM-CSG order, cut face, normals, empty solids, spacing, sharing passed\n";
}
void baseline() {
    // Independently authored native fat brushes; no renderer or DSL setup.
    for(int mode=0;mode<6;++mode) {
        float smoothing=mode==0?0.0f:0.18f;
        float reference=mode==5?0.0625f:1.0f;
        auto r=fixture(true,smoothing); r.nodes[1]=sphere("b",0.71f); r.nodes[2].transform(0,3)=0.45f;
        std::get<d::Value::Object>(r.nodes.back().parameters.data)["operations"]=d::Value::Array{"union","union"};
        FatPrim primitives[3]{};
        for(auto &p:primitives) { p.kind=FAT_PRIM_SPHERE; p.invTransform=mm::to_c(mm::identity()); }
        primitives[0].radius=0.83f; primitives[1].radius=0.71f; primitives[1].invTransform.m[3]=-0.45f;
        CsgStageOp ops[3]={CSG_STAGE_UNION,CSG_STAGE_DIFFERENCE,CSG_STAGE_UNION};
        FieldStages stages{ops,1,nullptr,nullptr,0}; int primitive_count=2;
        auto &parameters=std::get<d::Value::Object>(r.nodes.back().parameters.data);
        parameters["referenceRadius"]=double(reference);
        if(mode>=2&&mode<=4) {
            parameters["operations"]=d::Value::Array{"union",mode==4?"intersect":"subtract"};
            stages.stageCount=2; primitives[1].stage=1;
            if(mode==4) ops[1]=CSG_STAGE_INTERSECTION;
            if(mode==3) {
                auto c=sphere("restore",0.27f); r.nodes.insert(r.nodes.end()-1,c);
                r.nodes.back().inputs.push_back({"restore",d::InputKind::node,c.id,d::ProductType::mesh});
                // Insertion invalidates references into the nodes vector.
                std::get<d::Value::Object>(r.nodes.back().parameters.data)["operations"]=d::Value::Array{"union","subtract","union"};
                primitives[2].radius=0.27f; primitives[2].stage=2; stages.stageCount=3; primitive_count=3;
            }
        }
        auto *scratch=CreateSurfaceScratch(); check(scratch!=nullptr,"native scratch");
        s::JobMonitor monitor(generous().budget,{}); s::ResourceUsage usage; auto f=lower(r,monitor,usage);
        std::vector<float> values(f.program.size()),group(f.program.size());
        for(int z=-6;z<=6;++z) for(int y=-6;y<=6;++y) for(int x=-6;x<=6;++x) {
            d::Vec3 p{x*0.19f,y*0.17f,z*0.13f};
            float expected=ProbeFieldScalar(scratch,nullptr,reference,0,smoothing,&stages,primitives,primitive_count,nullptr,0,0,mm::to_c(p));
            near(impl::sample_csg(f,p,values,group,monitor,usage),expected,"canonical scalar fixture");
        }
        // Exactly matched 32^3 sample lattice, binary-exact spacing .125.
        f.bounds.min={-1.75f,-1.75f,-1.75f}; f.bounds.max={1.625f,1.625f,1.625f};
        auto native=GenerateMeshStaged(scratch,nullptr,reference,0,{{-0.0625f,-0.0625f,-0.0625f},{3.875f,3.875f,3.875f},5},smoothing,
            &stages,primitives,primitive_count,nullptr,0,nullptr,0,0);
        auto mesh=impl::mesh_csg(f,r.nodes.back(),monitor,usage); check(bool(mesh),"new mesh exists");
        check(native.triangleCount>0,"native mesh exists");
        size_t offset=0;
        for(int triangle=0;triangle<native.triangleCount;++triangle) {
            d::Vec3 p[3],n[3];
            for(int corner=0;corner<3;++corner) {
                auto index=native.indices[triangle*3+corner];
                p[corner]={native.vertices[index*3],native.vertices[index*3+1],native.vertices[index*3+2]};
                n[corner]={native.normals[index*3],native.normals[index*3+1],native.normals[index*3+2]};
            }
            if(mm::length(mm::cross(mm::sub(p[1],p[0]),mm::sub(p[2],p[0])))==0) continue;
            check(offset+3<=mesh->positions.size(),"native triangle count upper bound");
            for(int c=0;c<3;++c,++offset) {
                near(mesh->positions[offset].x,p[c].x,"native triangle x");
                near(mesh->positions[offset].y,p[c].y,"native triangle y");
                near(mesh->positions[offset].z,p[c].z,"native triangle z");
                near(mesh->normals[offset].x,n[c].x,"native normal x",0.002f);
                near(mesh->normals[offset].y,n[c].y,"native normal y",0.002f);
                near(mesh->normals[offset].z,n[c].z,"native normal z",0.002f);
            }
        }
        check(offset==mesh->positions.size(),"exact nondegenerate native topology");
        std::free(native.vertices); std::free(native.normals); std::free(native.indices); std::free(native.colors);
        DestroySurfaceScratch(scratch);
    }
    std::cout<<"GEOM-CSG-BASELINE canonical hard/smooth field, matched-lattice topology and normals passed\n";
}
void rejection() {
    using E=d::ErrorCode;
    auto r=fixture();
    for(int axis=0;axis<3;++axis) r.nodes[2].transform(axis,axis)=2;
    ok(evaluate(r));
    r.nodes[2].transform(0,0)=3; failed(evaluate(r),E::unsupported_capability);
    r=fixture(); r.nodes[2].transform(0,0)=-1; failed(evaluate(r),E::unsupported_capability);
    r=fixture(); r.nodes[2].transform(0,1)=0.2f; failed(evaluate(r),E::unsupported_capability);
    r=fixture(); r.nodes[2].transform(0,0)=0; failed(evaluate(r),E::invalid_input);
    r=fixture(); r.required_capabilities.push_back({"unknown.required",1}); failed(evaluate(r),E::unsupported_capability);
    r=fixture(); r.nodes.back().operation_version=2; failed(evaluate(r),E::unsupported_version);
    r=fixture(true); std::get<d::Value::Object>(r.nodes.back().parameters.data)["smoothing"]=-1; failed(evaluate(r),E::invalid_input);
    r=fixture(); r.nodes[1].operation="mesh.triangles";
    r.required_capabilities.push_back({"matter.operation.mesh.triangles",1});
    r.nodes[1].parameters=d::Value::Object{{"mode",0},{"positions",d::Value::Array{
        d::Value::Array{0,0,0},d::Value::Array{1,0,0},d::Value::Array{0,1,0}}}};
    failed(evaluate(r),E::unsupported_capability);
    r=fixture(); failed(evaluate(r,generous(),{},0),E::invalid_input);
    failed(evaluate(r,generous(),{},1e-20f),E::resource_limit);
    for(int limit=0;limit<7;++limit) {
        auto o=generous();
        switch(limit) {
        case 0:o.budget.operations=10;break;case 1:o.budget.working_bytes=100;break;
        case 2:o.budget.vertices=10;break;case 3:o.budget.indices=10;break;
        case 4:o.budget.output_bytes=100;break;case 5:o.budget.dependency_depth=1;break;
        case 6:o.budget.execution_microseconds=1;break;
        }
        failed(evaluate(r,o),E::resource_limit);
    }
    for(const char *stage:{"validate","csg.sample","csg.mesh","csg.normals","complete"}) {
        s::CancellationSource source; s::JobControl control; control.cancellation=source.token();
        bool visited=false; control.progress=[&](const s::Progress &p) { if(p.stage==stage) { visited=true; source.cancel(); } };
        failed(evaluate(r,generous(),control),E::cancelled); check(visited,"cancellation phase reached");
    }
    s::JobControl throwing; throwing.progress=[](const s::Progress &p) { if(p.stage=="csg.mesh") throw std::runtime_error("callback"); };
    failed(evaluate(r,generous(),throwing),E::backend_failure);
    // The signal is raised while a large grid is being sampled, beyond the
    // progress callback itself. Joining occurs even if a failure is unexpected.
    s::CancellationSource source; s::JobControl control; control.cancellation=source.token();
    std::thread cancellation;
    control.progress=[&](const s::Progress &p) { if(p.stage=="csg.sample") cancellation=std::thread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); source.cancel();
    }); };
    auto o=generous(); o.budget.operations=100000000;
    auto cancelled=evaluate(r,o,control,0.02f);
    if(cancellation.joinable()) cancellation.join();
    failed(cancelled,E::cancelled);
    std::cout<<"GEOM-CSG-BOUNDS transforms, caps, versions, budgets and cooperative cancellation passed\n";
}
}
int main() {
    try { semantics(); baseline(); rejection(); std::cout<<checks<<" CSG checks passed\n"; }
    catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
    catch(const impl::Failure &e) { std::cerr<<d::code_name(e.code)<<": "<<e.message<<'\n'; return 2; }
}
