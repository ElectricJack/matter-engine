#include <matter/geometry/geometry.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <new>
#ifdef _MSC_VER
#include <malloc.h>
#endif
namespace {
long fail_at=-1;
size_t allocations=0;
void checkpoint() {
    ++allocations;
    if(fail_at==0) { fail_at=-1; throw std::bad_alloc(); }
    if(fail_at>0) --fail_at;
}
void *allocate(size_t n) { checkpoint(); if(void *p=std::malloc(n?n:1)) return p; throw std::bad_alloc(); }
void *aligned(size_t n,size_t alignment) {
    checkpoint();
#ifdef _MSC_VER
    if(void *p=_aligned_malloc(n?n:1,alignment)) return p;
#else
    void *p=nullptr; if(posix_memalign(&p,alignment,n?n:1)==0) return p;
#endif
    throw std::bad_alloc();
}
void aligned_free(void *p) {
#ifdef _MSC_VER
    _aligned_free(p);
#else
    std::free(p);
#endif
}
}
void *operator new(size_t n) { return allocate(n); }
void *operator new[](size_t n) { return allocate(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p,size_t) noexcept { std::free(p); }
void operator delete[](void *p,size_t) noexcept { std::free(p); }
void *operator new(size_t n,std::align_val_t a) { return aligned(n,size_t(a)); }
void *operator new[](size_t n,std::align_val_t a) { return aligned(n,size_t(a)); }
void operator delete(void *p,std::align_val_t) noexcept { aligned_free(p); }
void operator delete[](void *p,std::align_val_t) noexcept { aligned_free(p); }
void operator delete(void *p,size_t,std::align_val_t) noexcept { aligned_free(p); }
void operator delete[](void *p,size_t,std::align_val_t) noexcept { aligned_free(p); }
namespace d=matter::contracts;
namespace s=matter::services;
// Fails every allocation position of one repeatable invocation in turn. Each
// must fail closed with resource_limit and no value; at least one must reach
// the late publication path; a final unfailed run must recover.
template<class Run> int exercise(Run run,const char *publication,const char *label) {
    // Warm standard-library locale/formatting caches before measuring a
    // repeatable invocation; their one-time allocations cannot be replayed.
    auto warmup=run();
    if(!warmup.ok()) return 1;
    allocations=0;
    auto baseline=run();
    size_t count=allocations;
    if(!baseline.ok()) return 1;
    size_t publication_failures=0;
    for(size_t i=0;i<count;++i) {
        fail_at=long(i);
        auto result=run();
        bool injected=fail_at<0;
        fail_at=-1;
        if(!injected || result.ok() || result.value()) {
            std::cerr<<"allocation "<<i<<" of "<<count<<": injected="<<injected<<", ok="<<result.ok()<<'\n'; return 2;
        }
        bool diagnosed=false;
        for(const auto &error:result.diagnostics()) {
            if(error.code==d::ErrorCode::resource_limit) diagnosed=true;
            if(error.message==publication) ++publication_failures;
        }
        if(!diagnosed) {
            std::cerr<<"missing resource_limit at allocation "<<i<<'\n';
            for(const auto &error:result.diagnostics()) std::cerr<<d::code_name(error.code)<<": "<<error.message<<'\n';
            return 3;
        }
    }
    if(!publication_failures) { std::cerr<<"late publication allocation path not exercised\n"; return 5; }
    auto recovered=run();
    if(!recovered.ok()) return 4;
    std::cout<<label<<": "<<count<<" allocation failure positions, "<<publication_failures
        <<" late publication diagnoses, no partial results, recovery passed\n";
    return 0;
}
int main(int argc,char **argv) {
    const std::string mode=argc>1?argv[1]:"";
    if(!mode.empty() && mode!="--csg" && mode!="--query" && mode!="--process") {
        std::cerr<<"unknown mode "<<mode<<" (expected --csg, --query or --process)\n"; return 6;
    }
    matter::geometry::PrimitiveGeometry geometry;
    d::RecipeDocument recipe; recipe.source.id={"allocation.fixture"};
    recipe.required_capabilities={{"matter.recipe.core",1}};
    d::RecipeNode profile; profile.id={"profile"}; profile.operation="profile.polygon";
    profile.output_type=d::ProductType::profile;
    profile.parameters=d::Value::Object{{"points",d::Value::Array{
        d::Value::Array{-0.5,-0.5},d::Value::Array{0.5,-0.5},
        d::Value::Array{0.5,0.5},d::Value::Array{-0.5,0.5}}}};
    d::RecipeNode sweep; sweep.id={"extrusion"}; sweep.operation="profile.extrude";
    sweep.inputs={{"profile",d::InputKind::node,profile.id,d::ProductType::profile}};
    sweep.parameters=d::Value::Object{{"depth",2}};
    recipe.nodes={profile,sweep}; recipe.outputs={sweep.id};
    d::RecipeNode a; a.id={"a"}; a.operation="primitive.sphere";
    a.parameters=d::Value::Object{{"radius",0.04},{"segments",8}};
    if(mode=="--csg") {
        auto b=a; b.id={"b"};
        d::RecipeNode solid; solid.id={"solid"}; solid.operation="solid.csg";
        solid.parameters=d::Value::Object{{"operator","union"}};
        solid.inputs={{"a",d::InputKind::node,a.id,d::ProductType::mesh},{"b",d::InputKind::node,b.id,d::ProductType::mesh}};
        recipe.nodes={a,b,solid}; recipe.outputs={solid.id};
    } else if(mode=="--query") {
        // Twelve triangles still split into several index leaves; contract
        // validation of every replayed product dominates the allocation count.
        d::RecipeNode box; box.id={"box"}; box.operation="primitive.box";
        box.parameters=d::Value::Object{{"size",d::Value::Array{2,2,2}}};
        recipe.nodes={box}; recipe.outputs={box.id};
    } else if(mode=="--process") {
        // The coarsest sphere still welds into a closed surface the simplifier can collapse.
        auto &parameters=std::get<d::Value::Object>(a.parameters.data);
        parameters["radius"]=1; parameters["segments"]=4;
        recipe.nodes={a}; recipe.outputs={a.id};
    }
    s::JobEnvelope job{"allocation.fixture","1",1};
    s::ResolvedInputs inputs; s::EvaluationOptions options; s::JobControl control;
    if(mode.empty() || mode=="--csg")
        return exercise([&] { return geometry.evaluate(job,recipe,inputs,options,control); },
            "geometry publication allocation failed",mode.empty()?"GEOM-ALLOCATION":"GEOM-CSG-ALLOCATION");
    // Query and processing consume an already published product, built once
    // outside the measured invocation.
    auto source=geometry.evaluate(job,recipe,inputs,options,control);
    if(!source.ok()) return 1;
    const s::AssetProduct product=*source.value();
    const auto prototype=product.document.meshes.at(0).id;
    if(mode=="--query") {
        s::QueryBatch batch;
        for(int i=0;i<8;++i) {
            s::SurfaceQuery q; q.prototype=prototype; q.position={float(i%3)-1,3,float(i%2)};
            q.kind=i%2?s::QueryKind::ray_surface:s::QueryKind::closest_surface; batch.samples.push_back(q);
        }
        return exercise([&] { return geometry.query(job,product,batch,options,control); },
            "geometry query publication allocation failed","GEOM-QUERY-ALLOCATION");
    }
    s::ProcessingRequest request;
    request.required_capabilities={{"matter.operation.mesh.simplify",1},{"matter.operation.mesh.smooth",1}};
    request.settings=d::Value::Object{{"regions",d::Value::Array{d::Value::Object{
        {"prototypes",d::Value::Array{prototype.value}},
        {"modifiers",d::Value::Array{
            d::Value::Object{{"operation","mesh.simplify"},{"ratio",0.5}},
            d::Value::Object{{"operation","mesh.smooth"},{"iterations",1}}}}}}}};
    return exercise([&] { return geometry.process(job,product,request,options,control); },
        "geometry modifier publication allocation failed","GEOM-MODIFIER-ALLOCATION");
}
