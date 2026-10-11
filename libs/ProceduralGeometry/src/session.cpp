#include <matter/geometry/geometry.h>
#include <algorithm>
#include <cmath>

namespace matter::geometry {
namespace {
data::Value point(data::Vec2 p) { return data::Value::Array{p.x,p.y}; }
data::Value point(data::Vec3 p) { return data::Value::Array{p.x,p.y,p.z}; }
bool identity(const data::Mat4 &m) {
    data::Mat4 unit; return std::equal(m.m,m.m+16,unit.m);
}
}
Session::Session(data::SourceIdentity source) {
    recipe_.source=std::move(source);
    recipe_.required_capabilities={{"matter.recipe.core",1},{"matter.material.pbr",1}};
    material_.id={"geometry.default"};
}
data::ValidationResult Session::fail(data::ErrorCode code,const char *message) {
    errors_.push_back({data::Severity::error,code,message,{},{},{}});
    return {errors_};
}
void Session::declare(const std::string &op) {
    if(op=="primitive.box" || op=="primitive.sphere" || op=="profile.polygon" || op=="placement.affine") return;
    std::string name="matter.operation."+op;
    if(std::none_of(recipe_.required_capabilities.begin(),recipe_.required_capabilities.end(),
        [&](const auto &c){return c.name==name;})) recipe_.required_capabilities.push_back({name,1});
}
void Session::capture_material(data::RecipeNode &n) {
    auto m=material_;
    auto &c=m.base_color.factor;
    c.x*=1+(tint_.x-1)*tint_.w; c.y*=1+(tint_.y-1)*tint_.w; c.z*=1+(tint_.z-1)*tint_.w;
    // Snapshot identity includes the entire PBR material and the legacy tint's
    // neutral multiplicative conversion, never a renderer palette index.
    data::AssetDocument sample; sample.materials={m};
    std::string canonical;
    auto result=data::canonical_json(data::to_value(sample),canonical);
    if(!result.ok()) { errors_.insert(errors_.end(),result.diagnostics.begin(),result.diagnostics.end()); return; }
    m.id.value+="."+data::sha256(canonical).hex;
    if(std::none_of(recipe_.materials.begin(),recipe_.materials.end(),[&](const auto &v){return v.id==m.id;}))
        recipe_.materials.push_back(m);
    n.materials={m.id};
}
void Session::append(data::RecipeNode n,const data::Mat4 &xf) {
    declare(n.operation);
    auto id=n.id;
    recipe_.nodes.push_back(std::move(n));
    if(!identity(xf)) {
        data::RecipeNode placement; placement.id={id.value+".placement"}; placement.operation="placement.affine";
        placement.inputs={{"mesh",data::InputKind::node,id,data::ProductType::mesh}};
        placement.transform=xf; recipe_.nodes.push_back(placement); id=placement.id;
    }
    recipe_.outputs.push_back(id);
}
data::ValidationResult Session::transform(const data::Mat4 &m) {
    if(!errors_.empty()) return {errors_};
    try {
        auto r=data::validate_transform(m);
        if(!r.ok()) { errors_=r.diagnostics; return r; }
        transform_=m; return {};
    } catch(const std::bad_alloc &) { return fail(data::ErrorCode::resource_limit,"cursor allocation failed"); }
}
data::ValidationResult Session::material(const data::MaterialDescription &m) {
    if(!errors_.empty()) return {errors_};
    try { material_=m; return {}; }
    catch(const std::bad_alloc &) { return fail(data::ErrorCode::resource_limit,"material allocation failed"); }
}
data::ValidationResult Session::tint(data::Vec4 t) {
    if(!errors_.empty()) return {errors_};
    if(!std::isfinite(t.x)||!std::isfinite(t.y)||!std::isfinite(t.z)||!std::isfinite(t.w) ||
        t.x<0||t.y<0||t.z<0||t.w<0||t.w>1)
        return fail(data::ErrorCode::invalid_input,"tint must be finite nonnegative RGB and a blend in [0,1]");
    tint_=t; return {};
}
data::ValidationResult Session::emit(data::RecipeNode n) {
    if(!errors_.empty()) return {errors_};
    try {
        if(n.output_type!=data::ProductType::mesh || !identity(n.transform))
            return fail(data::ErrorCode::invalid_input,"emit requires a local mesh node; use the transform cursor");
        if(n.operation=="mesh.triangles") { auto r=flush(); if(!r.ok()) return r; }
        capture_material(n); if(!errors_.empty()) return {errors_};
        append(std::move(n),transform_); return {};
    } catch(const std::bad_alloc &) { return fail(data::ErrorCode::resource_limit,"emit allocation failed"); }
}
data::ValidationResult Session::retain(data::SemanticId id,const Profile &p) {
    if(!errors_.empty()) return {errors_};
    try {
        auto r=flush(); if(!r.ok()) return r;
        data::RecipeNode n; n.id=std::move(id); n.output_type=data::ProductType::profile;
        n.operation=p.holes.empty()?"profile.polygon":"profile.contours";
        data::Value::Array outer,holes;
        for(auto v:p.outer) outer.push_back(point(v));
        for(const auto &h:p.holes) { data::Value::Array c; for(auto v:h) c.push_back(point(v)); holes.push_back(std::move(c)); }
        data::Value::Object params{{"points",std::move(outer)}};
        if(!p.holes.empty()) params["holes"]=std::move(holes);
        n.parameters=std::move(params); capture_material(n);
        if(!errors_.empty()) return {errors_};
        declare(n.operation); recipe_.nodes.push_back(n);
        retained_=std::move(n); retained_transform_=transform_; return {};
    } catch(const std::bad_alloc &) { return fail(data::ErrorCode::resource_limit,"profile allocation failed"); }
}
data::ValidationResult Session::extrude(data::SemanticId id,const std::vector<data::Vec3> &path,Join join) {
    if(!errors_.empty()) return {errors_};
    if(!retained_) return fail(data::ErrorCode::invalid_input,"extrude requires a retained profile");
    try {
        data::RecipeNode n; n.id=std::move(id); n.operation="profile.sweep";
        n.inputs={{"profile",data::InputKind::node,retained_->id,data::ProductType::profile}};
        n.materials=retained_->materials;
        data::Value::Array points; for(auto v:path) points.push_back(point(v));
        n.parameters=data::Value::Object{{"path",std::move(points)},{"join",int(join)}};
        append(std::move(n),retained_transform_); retained_.reset(); return {};
    } catch(const std::bad_alloc &) { return fail(data::ErrorCode::resource_limit,"extrusion allocation failed"); }
}
data::ValidationResult Session::flush() {
    if(!errors_.empty()) return {errors_};
    if(!retained_) return {};
    try {
        data::RecipeNode fill; fill.id={retained_->id.value+".fill"}; fill.operation="profile.fill";
        fill.inputs={{"profile",data::InputKind::node,retained_->id,data::ProductType::profile}};
        fill.materials=retained_->materials;
        append(std::move(fill),retained_transform_); retained_.reset(); return {};
    } catch(const std::bad_alloc &) { return fail(data::ErrorCode::resource_limit,"profile flush allocation failed"); }
}
jobs::AssetResult Session::finish(const jobs::JobEnvelope &job,const jobs::EvaluationOptions &options,const jobs::JobControl &control) {
    try {
        auto r=flush(); if(!r.ok()) return jobs::AssetResult::failure(job,std::move(r.diagnostics));
        PrimitiveGeometry service;
        return service.evaluate(job,recipe_,{},options,control);
    } catch(const std::bad_alloc &) { return jobs::AssetResult::failure(job,data::ErrorCode::resource_limit,"session allocation failed"); }
    catch(const std::exception &e) { return jobs::AssetResult::failure(job,data::ErrorCode::backend_failure,e.what()); }
}
} // namespace matter::geometry
