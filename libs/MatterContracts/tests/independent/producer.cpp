#include "bridge.h"

namespace gate {
namespace mc = matter::contracts;
namespace ms = matter::services;
mc::RecipeDocument recipe() {
  mc::RecipeDocument r;
  r.source.id = {"gate.prop"};
  r.source.revision = "1";
  r.required_capabilities = {{"matter.recipe.core", 1}, {"matter.material.pbr", 1}};
  mc::MaterialDescription m;
  m.id = {"paint"};
  m.base_color.factor = {0.25f, 0.5f, 0.75f, 1};
  m.roughness = 0.5f;
  r.materials = {m};
  mc::RecipeNode outer, cutter, cut;
  outer.id = {"body"}; outer.operation = "primitive.box";
  outer.parameters = mc::Value::Object{{"size", mc::Value::Array{2, 3, 4}}};
  outer.materials = {{"paint"}};
  cutter.id = {"hole"}; cutter.operation = "primitive.sphere";
  cutter.parameters = mc::Value::Object{{"radius", 0.5}, {"segments", 8}};
  cut.id = {"cut"}; cut.operation = "solid.csg";
  cut.parameters = mc::Value::Object{{"operator", "subtract"}};
  cut.inputs = {{"operand", mc::InputKind::node, {"body"}, mc::ProductType::mesh},
                {"operand", mc::InputKind::node, {"hole"}, mc::ProductType::mesh}};
  cut.materials = {{"paint"}};
  r.nodes = {cut, outer, cutter}; r.outputs = {{"cut"}};
  return r;
}
mc::AssetDocument asset() {
  mc::AssetDocument a;
  a.id = {"gate.hand-built"}; a.provenance.id = {"gate.synthetic"};
  a.required_capabilities = {{"matter.asset.core", 1}, {"matter.material.pbr", 1}};
  a.bounds = {{-10, -10, -10}, {10, 10, 10}};
  a.materials = recipe().materials;
  mc::MeshPrototype m;
  m.id = {"face"}; m.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  m.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
  m.tangents = {{1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}};
  m.indices = {0, 1, 2}; m.submeshes = {{0, 3, {"paint"}}};
  m.bounds = {{0, 0, 0}, {1, 1, 0}}; a.meshes = {m};
  mc::Instance first; first.id = {"first"}; first.prototype = {"face"};
  mc::Instance second = first; second.id = {"second"}; second.parent = first.id;
  second.local_transform = mm::multiply(mm::translation({3, 2, 1}), mm::scale({-2, 3, 4}));
  a.assemblies = {{{"pair"}, {first, second}}};
  mc::Instance root; root.id = {"root"}; root.prototype = {"pair"};
  root.target = mc::InstanceTarget::assembly; a.instances = {root};
  return a;
}
ms::AssetResult complete(const ms::JobEnvelope &job, const ms::EvaluationOptions &options,
                         const ms::JobControl &control) {
  // Boundary/ownership fake: returns hand-built data, never evaluates a recipe.
  ms::JobMonitor monitor(options.budget, control);
  auto r = monitor.report({0.5, "hand-built fixture"});
  if (!r.ok()) return ms::AssetResult::failure(job, r.diagnostics);
  return ms::finish_asset(job, asset(), options, monitor, {});
}
ms::TerrainProduct terrain(const ms::TerrainRequest &request) {
  ms::TerrainProduct p;
  p.job = request.job; p.field_content = *request.field.content;
  p.region = request.region; p.region_key = request.region_key;
  p.origin_metres = request.origin_metres; p.cell_counts = request.cell_counts;
  p.detail_level = request.detail_level; p.boundaries = request.boundaries;
  auto a = asset(); p.bounds = a.bounds;
  p.geometry = std::make_shared<const mc::AssetDocument>(std::move(a));
  return p;
}
}
