#include "bridge.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace mc = matter::contracts;
namespace ms = matter::services;
void check(bool ok, const char *id) {
  if (!ok) { std::cerr << "FAIL " << id << '\n'; std::exit(1); }
}
void rejected(const mc::ValidationResult &r, mc::ErrorCode code, const char *id) {
  bool found = false;
  for (const auto &d : r.diagnostics) found |= d.code == code;
  check(!r.ok() && found, id);
}
int main() {
  using E = mc::ErrorCode;
  auto recipe = gate::recipe();
  std::string wire;
  auto primitive = recipe; primitive.nodes = {recipe.nodes[1]}; primitive.outputs = {{"body"}};
  check(mc::encode(primitive, wire).ok(), "GATE-C1-PRIMITIVE");
  auto extruded = recipe;
  mc::RecipeNode profile, extrusion;
  profile.id = {"profile"}; profile.operation = "profile.polygon"; profile.output_type = mc::ProductType::profile;
  profile.parameters = mc::Value::Object{{"points", mc::Value::Array{
      mc::Value::Array{0,0}, mc::Value::Array{1,0}, mc::Value::Array{0,1}}}};
  extrusion.id = {"extrude"}; extrusion.operation = "profile.extrude";
  extrusion.parameters = mc::Value::Object{{"depth", 2}}; extrusion.materials = {{"paint"}};
  extrusion.inputs = {{"profile", mc::InputKind::node, {"profile"}, mc::ProductType::profile}};
  extruded.nodes = {profile, extrusion}; extruded.outputs = {{"extrude"}};
  check(mc::encode(extruded, wire).ok(), "GATE-C1-EXTRUDE");
  check(mc::encode(recipe, wire).ok(), "GATE-C1-CSG");
  mc::RecipeDocument decoded;
  check(mc::decode_recipe(wire, decoded).ok() && decoded.nodes[0].inputs[0].reference.value == "body" &&
        decoded.nodes[0].inputs[1].reference.value == "hole", "GATE-C1-CSG");
  mc::ContentDigest original, reordered;
  check(mc::content_digest(recipe, original).ok(), "GATE-C1-CANONICAL");
  std::swap(decoded.nodes[0].inputs[0], decoded.nodes[0].inputs[1]);
  check(mc::content_digest(decoded, reordered).ok() && original.hex != reordered.hex, "GATE-C1-CSG");
  decoded = recipe; decoded.metadata = mc::Value::Object{{"note", "editor-only"}};
  check(mc::content_digest(decoded, reordered).ok() && original.hex == reordered.hex, "GATE-C1-CANONICAL");
  mc::Value numeric;
  check(mc::parse_json("{\"z\":-0,\"a\":0.1}", numeric).ok() && mc::canonical_json(numeric, wire).ok() &&
        wire == "{\"a\":0.1000000000000000055511151231257827021181583404541015625,\"z\":0}", "GATE-C1-CANONICAL");
  auto a = gate::asset(); mc::AssetDocument round;
  check(mc::encode(a, wire).ok() && mc::decode_asset(wire, round).ok(), "GATE-C1-ASSEMBLY");
  check(round.meshes.size() == 1 && round.assemblies[0].instances.size() == 2 &&
        round.assemblies[0].instances[0].prototype == round.assemblies[0].instances[1].prototype &&
        round.assemblies[0].instances[0].id != round.assemblies[0].instances[1].id, "GATE-C1-ASSEMBLY");
  check(round.meshes[0].submeshes[0].material.value == "paint" &&
        round.materials[0].base_color.factor.z == 0.75f, "GATE-C1-MATERIAL");
  auto matrix = round.assemblies[0].instances[1].local_transform;
  auto point = mm::transform_point(matrix, {1, 2, 3});
  check(point.x == 1 && point.y == 8 && point.z == 13 && mc::reverses_winding(matrix), "GATE-C1-AFFINE");
  auto normal = mc::transform_normal(matrix, {1, 1, 0});
  check(normal && std::abs(normal->x / normal->y + 1.5f) < 1e-6f, "GATE-C1-AFFINE");
  decoded = recipe; decoded.nodes[0].inputs[0].reference = {"missing"};
  rejected(mc::validate(decoded), E::missing_dependency, "GATE-C1-MALFORMED");
  mc::ValidationOptions limited; limited.limits.max_nodes = 1;
  rejected(mc::validate(recipe, limited), E::resource_limit, "GATE-C1-EXHAUSTION");

  // Exact typed bytes are constructed by the caller, independently of the callee.
  recipe.nodes.clear(); recipe.source_map.clear(); recipe.outputs = {{"mesh"}};
  ms::ResolvedInputs inputs;
  auto resource = [&](const char *id, mc::ScalarType type, uint32_t components, std::vector<uint8_t> bytes) {
    auto owner = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    auto digest = mc::sha256(std::string_view(reinterpret_cast<const char *>(owner->data()), owner->size()));
    mc::ResourceDescription d{{id}, digest, type, components, owner->size() / (4 * components), owner->size()};
    recipe.resources.push_back(d); inputs.resources.push_back({d, owner});
  };
  resource("positions", mc::ScalarType::f32, 3, std::vector<uint8_t>(36));
  resource("indices", mc::ScalarType::u32, 1, {0,0,0,0, 1,0,0,0, 2,0,0,0});
  mc::RecipeNode mesh; mesh.id = {"mesh"}; mesh.operation = "mesh.input"; mesh.materials = {{"paint"}};
  mesh.inputs = {{"positions", mc::InputKind::resource, {"positions"}, mc::ProductType::mesh},
                 {"indices", mc::InputKind::resource, {"indices"}, mc::ProductType::mesh}};
  recipe.nodes = {mesh};
  check(ms::validate(recipe, inputs).ok(), "GATE-C2-RESOLVED-INDEX");
  inputs.resources.pop_back(); recipe.resources.pop_back();
  resource("indices", mc::ScalarType::u32, 1, {0,0,0,0, 1,0,0,0, 3,0,0,0});
  recipe.source_map["mesh"] = {"fixture://caller",7,3};
  auto bad_index = ms::validate(recipe, inputs);
  rejected(bad_index, E::invalid_input, "GATE-C2-RESOLVED-INDEX");
  check(bad_index.diagnostics.back().node_id == "mesh" && bad_index.diagnostics.back().source &&
        bad_index.diagnostics.back().source->uri == "fixture://caller" &&
        bad_index.diagnostics.back().source->line == 7, "GATE-C2-RESOLVED-INDEX");
  inputs.resources.pop_back(); recipe.resources.pop_back();
  resource("indices", mc::ScalarType::u32, 1, {0,0,0,0, 1,0,0,0, 0,1,0,0});
  rejected(ms::validate(recipe, inputs), E::invalid_input, "GATE-C2-RESOLVED-INDEX");
  inputs.resources[1].bytes = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0});
  rejected(ms::validate(recipe, inputs), E::invalid_input, "GATE-C2-RESOLVED-LENGTH");

  ms::JobEnvelope job{"gate-job", "revision-1", 1}; ms::EvaluationOptions options;
  auto result = gate::complete(job, options, {});
  check(result.ok(), "GATE-C2-OWNER");
  ms::AssetOwner owner = ms::asset_owner(result);
  std::weak_ptr<const ms::AssetProduct> weak = result.value();
  result = ms::AssetResult::failure(job, E::cancelled, "fixture");
  check(!weak.expired(), "GATE-C2-OWNER"); owner.reset();
  check(weak.expired(), "GATE-C2-OWNER");
  ms::CancellationSource cancellation; ms::JobControl control;
  control.cancellation = cancellation.token();
  control.progress = [&](const ms::Progress &) { cancellation.cancel(); };
  result = gate::complete(job, options, control);
  check(!result.value(), "GATE-C2-CANCEL");
  rejected({result.diagnostics()}, E::cancelled, "GATE-C2-CANCEL");
  options.budget.vertices = 2; result = gate::complete(job, options, {});
  check(!result.value(), "GATE-C2-BUDGET");
  rejected({result.diagnostics()}, E::resource_limit, "GATE-C2-BUDGET");

  ms::TerrainRequest request; request.job = job;
  request.field = {{"field"}, "fixture://field", "1", mc::sha256("field"), {}};
  request.region = {"chunk-0"}; request.origin_metres = {1e9, 0, -1e9};
  request.sample_domain = {{0, 0, 0}, {2, 2, 2}}; request.cell_counts = {2, 2, 2};
  request.required_capabilities = {{"matter.terrain.generate", 1}, {"gate.boundary", 1}};
  request.boundaries = {{ms::BoundaryFace::positive_x, {1,0,0}, 1, 2, {"gate.boundary", 1}, {}}};
  auto capabilities = request.required_capabilities;
  auto product = gate::terrain(request);
  check(ms::validate(product, request, capabilities).ok(), "GATE-C2-TERRAIN");
  std::shared_ptr<const ms::TerrainProduct> current;
  auto completed = ms::TerrainResult::success(job, product);
  check(ms::admit_terrain(completed, request, capabilities, current).ok(), "GATE-C2-TERRAIN");
  auto old = current; ++request.job.generation_epoch;
  request.cell_counts = {1,1,1}; request.detail_level = 1;
  request.boundaries[0].sample_spacing_metres = 2;
  rejected(ms::admit_terrain(completed, request, capabilities, current), E::stale_result, "GATE-C2-LATE");
  check(current == old, "GATE-C2-LATE");
  capabilities[1].version = 2;
  rejected(ms::validate(request, capabilities), E::unsupported_capability, "GATE-C2-CAPS");
  auto extension = a; mc::TerrainProduct terrain_data;
  terrain_data.region = {"chunk-0"}; terrain_data.origin_metres = {1e9,0,-1e9};
  terrain_data.prototypes = {{"face"}};
  terrain_data.boundaries = {{{1,0,0}, "gate.boundary.v1", 1, {}}};
  extension.terrain = terrain_data;
  extension.required_capabilities.push_back({"matter.asset.terrain",1});
  rejected(mc::validate(extension), E::unsupported_capability, "GATE-C1-TERRAIN-EXTENSION");
  mc::ValidationOptions terrain_options;
  terrain_options.supported_capabilities.push_back({"matter.asset.terrain",1});
  check(mc::encode(extension, wire, terrain_options).ok() && mc::decode_asset(wire, round, terrain_options).ok() &&
        round.terrain && round.terrain->origin_metres[0] == 1e9, "GATE-C1-TERRAIN-EXTENSION");
  std::cout << "PASS GATE native C1/C2 caller/callee (synthetic boundary fixtures only)\n";
}
