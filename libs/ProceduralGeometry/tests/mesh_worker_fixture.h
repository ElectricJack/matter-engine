#pragma once
#include "sphere_fixture.h"
#include <iostream>
#include <stdexcept>
namespace worker_fixture {
namespace d=matter::contracts;
namespace s=matter::services;
namespace g=matter::geometry;
inline void check(bool b,const char *why) { if(!b) throw std::runtime_error(why); }
inline s::JobEnvelope job{"worker.fixture","1",1};
inline const d::Capability cap{"matter.operation.mesh.retopology",1};
inline bool advertised(const g::PrimitiveGeometry &g) {
    for(const auto &c:g.capabilities()) if(c.name==cap.name && c.version==1) return true;
    return false;
}
inline s::AssetProduct product() {
    d::RecipeDocument recipe; recipe.source.id={"worker.fixture"}; recipe.source.revision="1";
    recipe.required_capabilities={{"matter.recipe.core",1},{"matter.operation.mesh.triangles",1},{"matter.material.pbr",1}};
    d::MaterialDescription material; material.id={"clay"}; recipe.materials={material};
    d::RecipeNode node; node.id={"sphere"}; node.operation="mesh.triangles"; node.materials={{"clay"}};
    d::Value::Array positions,normals,uvs;
    for(auto p:geometry_fixture::sphere_corners(2)) {
        positions.push_back(d::Value::Array{p.x,p.y,p.z}); normals.push_back(d::Value::Array{p.x,p.y,p.z});
        uvs.push_back(d::Value::Array{0.25,0.75});
    }
    node.parameters=d::Value::Object{{"positions",positions},{"normals",normals},{"uv0",uvs},{"mode",0}};
    recipe.nodes={node}; recipe.outputs={node.id};
    auto result=g::PrimitiveGeometry{}.evaluate(job,recipe,{},{},{});
    check(result.ok(),"sphere fixture evaluation"); return *result.value();
}
inline s::ProcessingRequest request(const s::AssetProduct &p) {
    s::ProcessingRequest r; r.required_capabilities={cap};
    d::Value::Object region{
        {"prototypes",d::Value::Array{p.document.meshes[0].id.value}},
        {"modifiers",d::Value::Array{d::Value::Object{{"operation","mesh.retopology"}}}}};
    r.settings=d::Value::Object{{"regions",d::Value::Array{std::move(region)}}};
    return r;
}
inline d::Value::Object &parameters(s::ProcessingRequest &r) {
    auto &regions=std::get<d::Value::Array>(std::get<d::Value::Object>(r.settings.data).at("regions").data);
    auto &mods=std::get<d::Value::Array>(std::get<d::Value::Object>(regions[0].data).at("modifiers").data);
    return std::get<d::Value::Object>(mods[0].data);
}
inline void failed(const s::AssetResult &r,d::ErrorCode code) {
    check(!r.ok() && !r.value(),"failure published a partial asset");
    for(const auto &e:r.diagnostics()) if(e.code==code) return;
    for(const auto &e:r.diagnostics()) std::cerr<<e.message<<'\n';
    throw std::runtime_error("wrong failure code");
}
inline s::EvaluationOptions options() {
    s::EvaluationOptions o; o.budget.working_bytes=1024ull*1024*1024;
    o.budget.execution_microseconds=60*1000000ull; return o;
}
}
