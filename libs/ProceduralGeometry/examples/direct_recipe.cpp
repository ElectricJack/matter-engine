#include <matter/geometry/geometry.h>
#include <iostream>
int main() {
    namespace d=matter::contracts;
    d::RecipeDocument recipe;
    recipe.source.id={"example.extruded-prop"}; recipe.source.revision="1";
    recipe.required_capabilities={{"matter.recipe.core",1},{"matter.material.pbr",1}};
    d::MaterialDescription material; material.id={"clay"}; material.base_color.factor={0.75f,0.25f,0.125f,1};
    recipe.materials={material};
    d::RecipeNode profile; profile.id={"profile"}; profile.operation="profile.polygon";
    profile.output_type=d::ProductType::profile;
    profile.parameters=d::Value::Object{{"points",d::Value::Array{
        d::Value::Array{-0.5,-0.5},d::Value::Array{0.5,-0.5},d::Value::Array{0.5,0.5},d::Value::Array{-0.5,0.5}}}};
    d::RecipeNode extrude; extrude.id={"prop"}; extrude.operation="profile.extrude";
    extrude.parameters=d::Value::Object{{"depth",2}}; extrude.materials={{"clay"}};
    extrude.inputs={{"profile",d::InputKind::node,profile.id,d::ProductType::profile}};
    d::RecipeNode a; a.id={"placed.a"}; a.operation="placement.affine";
    a.inputs={{"mesh",d::InputKind::node,extrude.id,d::ProductType::mesh}};
    d::RecipeNode b=a; b.id={"placed.b"}; b.transform=mm::multiply(mm::translation({3,0,0}),mm::scale({-2,3,1}));
    recipe.nodes={b,extrude,a,profile}; recipe.outputs={a.id,b.id};
    matter::geometry::PrimitiveGeometry geometry;
    auto result=geometry.evaluate({"example","1",1},recipe,{}, {},{});
    if(!result.ok()) {
        for(const auto &error:result.diagnostics()) std::cerr<<d::code_name(error.code)<<": "<<error.message<<'\n';
        return 1;
    }
    const auto &asset=result.value()->document;
    if(asset.meshes.size()!=1 || asset.instances.size()!=2 || asset.meshes[0].indices.size()!=36) return 2;
    std::cout<<"GEOM-DIRECT-RECIPE: 1 shared prototype, 2 instances, 12 triangles\n";
}
