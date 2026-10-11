#include <matter/geometry/geometry.h>
#include <cstdlib>
#include <iostream>
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
int main() {
    namespace d=matter::contracts;
    namespace s=matter::services;
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
    s::JobEnvelope job{"allocation.fixture","1",1};
    s::ResolvedInputs inputs; s::EvaluationOptions options; s::JobControl control;
    // Warm standard-library locale/formatting caches before measuring a
    // repeatable invocation; their one-time allocations cannot be replayed.
    auto warmup=geometry.evaluate(job,recipe,inputs,options,control);
    if(!warmup.ok()) return 1;
    allocations=0;
    auto baseline=geometry.evaluate(job,recipe,inputs,options,control);
    size_t count=allocations;
    if(!baseline.ok()) return 1;
    size_t publication_failures=0;
    for(size_t i=0;i<count;++i) {
        fail_at=long(i);
        auto result=geometry.evaluate(job,recipe,inputs,options,control);
        bool injected=fail_at<0;
        fail_at=-1;
        if(!injected || result.ok() || result.value()) {
            std::cerr<<"allocation "<<i<<" of "<<count<<": injected="<<injected<<", ok="<<result.ok()<<'\n'; return 2;
        }
        bool diagnosed=false;
        for(const auto &error:result.diagnostics()) {
            if(error.code==d::ErrorCode::resource_limit) diagnosed=true;
            if(error.message=="geometry publication allocation failed") ++publication_failures;
        }
        if(!diagnosed) {
            std::cerr<<"missing resource_limit at allocation "<<i<<'\n';
            for(const auto &error:result.diagnostics()) std::cerr<<d::code_name(error.code)<<": "<<error.message<<'\n';
            return 3;
        }
    }
    if(!publication_failures) { std::cerr<<"late publication allocation path not exercised\n"; return 5; }
    auto recovered=geometry.evaluate(job,recipe,inputs,options,control);
    if(!recovered.ok()) return 4;
    std::cout<<"GEOM-ALLOCATION: "<<count<<" allocation failure positions, "<<publication_failures
        <<" late publication diagnoses, no partial assets, recovery passed\n";
}
