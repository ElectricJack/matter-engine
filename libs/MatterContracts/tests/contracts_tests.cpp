#include "fixtures.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace mc = matter::contracts;
int checks = 0;
void check(bool ok, const char *message) {
  ++checks;
  if (!ok) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}
void passes(const mc::ValidationResult &r, const char *name) {
  if (!r.ok())
    for (const auto &d : r.diagnostics)
      std::cerr << mc::code_name(d.code) << ": " << d.message << '\n';
  check(r.ok(), name);
}
void rejects(const mc::ValidationResult &r, mc::ErrorCode code,
             const char *name) {
  check(!r.ok(), name);
  bool found = false;
  for (const auto &d : r.diagnostics)
    found |= d.code == code;
  check(found, "expected stable diagnostic code");
}
int main() {
  using E = mc::ErrorCode;
  auto recipe = fixtures::recipe();
  auto asset = fixtures::asset();
  passes(mc::validate(recipe), "ordered-csg-v1");
  passes(mc::validate(fixtures::extrusion()), "extruded-prop-v1");
  passes(mc::validate(asset), "repeated-assembly-v1");
  std::string wire;
  passes(mc::encode(recipe, wire), "recipe encode");
  mc::RecipeDocument round;
  passes(mc::decode_recipe(wire, round), "recipe decode");
  std::string again;
  passes(mc::encode(round, again), "recipe reencode");
  check(wire == again, "exact recipe round trip");
  check(round.nodes.front().id.value == "cut",
        "forward refs and authored node order preserved");
  check(round.seed == UINT64_C(0x0123456789abcdef), "64-bit seed preserved");
  round.source.legacy_hashes = {
      {"matter.source.v1", UINT64_MAX},
      {"matter.resolved.v1", UINT64_C(0x8000000000000001)}};
  passes(mc::encode(round, wire), "legacy namespace encoding");
  mc::RecipeDocument legacy;
  passes(mc::decode_recipe(wire, legacy), "legacy namespace decoding");
  check(legacy.source.legacy_hashes[0].value == UINT64_MAX,
        "legacy full-width hash retained");
  passes(mc::encode(asset, wire), "asset encode");
  mc::AssetDocument asset_round;
  passes(mc::decode_asset(wire, asset_round), "asset decode");
  passes(mc::encode(asset_round, again), "asset reencode");
  check(wire == again, "exact asset round trip");
  check(asset_round.meshes.size() == 1 && asset_round.assemblies.size() == 1 &&
            asset_round.instances.size() == 2,
        "shared prototypes retained");
  check(asset_round.instances[1].local_transform.m[3] == 3 &&
            asset_round.instances[1].local_transform.m[0] == -2,
        "matrix convention round trip");
  auto optional = fixtures::optional_asset();
  rejects(mc::validate(optional), E::unsupported_capability,
          "optional runtime capability matrix");
  auto opts = fixtures::optional_options();
  passes(mc::validate(optional, opts), "typed-products-v1");
  passes(mc::encode(optional, wire, opts), "typed products encode");
  passes(mc::decode_asset(wire, asset_round, opts), "typed products decode");
  passes(mc::encode(asset_round, again, opts), "typed products reencode");
  check(again == wire, "typed products exact round trip");
  auto r = recipe;
  r.nodes[1].id = r.nodes[0].id;
  rejects(mc::validate(r), E::invalid_input, "duplicate IDs");
  r = recipe;
  r.nodes[1].id = {"invalid id"};
  rejects(mc::validate(r), E::invalid_input, "invalid ID syntax");
  r = recipe;
  r.nodes[0].inputs[0].reference = {"gone"};
  rejects(mc::validate(r), E::missing_dependency, "missing node reference");
  r = recipe;
  r.nodes[0].inputs[0].reference = {"cut"};
  auto cycle = mc::validate(r);
  rejects(cycle, E::cycle, "recipe cycle");
  bool mapped = false;
  for (const auto &d : cycle.diagnostics)
    if (d.code == E::cycle)
      mapped = d.source.has_value() && !d.dependency_path.empty();
  check(mapped, "cycle path and source mapping retained");
  r = recipe;
  r.nodes[0].inputs[0].type = mc::ProductType::profile;
  rejects(mc::validate(r), E::invalid_input, "typed input mismatch");
  r = recipe;
  r.nodes[0].operation = "vendor.displace";
  rejects(mc::validate(r), E::unsupported_capability, "unknown operation");
  r = recipe;
  r.required_capabilities.push_back({"vendor.runtime", 1});
  rejects(mc::validate(r), E::unsupported_capability,
          "unknown required capability");
  r = recipe;
  r.nodes[0].operation_version = 2;
  rejects(mc::validate(r), E::unsupported_version, "operation version");
  r = recipe;
  r.schema_version.major = 2;
  rejects(mc::validate(r), E::unsupported_version, "major schema version");
  r = recipe;
  r.rng_algorithm = "xoshiro128";
  rejects(mc::validate(r), E::invalid_input, "explicit RNG version");
  r = recipe;
  r.nodes[0].materials = {{"missing"}};
  rejects(mc::validate(r), E::missing_dependency, "recipe material reference");
  r = recipe;
  r.parameters =
      mc::Value::Object{{"bad", std::numeric_limits<double>::infinity()}};
  rejects(mc::validate(r), E::invalid_input, "nonfinite parameter");
  r = recipe;
  r.nodes[0].transform = mm::scale({1, 2, 1});
  rejects(mc::validate(r), E::unsupported_capability,
          "CSG nonuniform transform");
  r = recipe;
  r.nodes[0].transform = mm::scale({-1, 1, 1});
  rejects(mc::validate(r), E::unsupported_capability, "CSG mirrored transform");
  r = recipe;
  r.nodes[0].transform = mm::scale(0);
  rejects(mc::validate(r), E::invalid_input, "singular transform");
  r = recipe;
  r.nodes[0].transform.m[12] = 1;
  rejects(mc::validate(r), E::invalid_input, "perspective transform");
  r = recipe;
  mc::ResourceDescription resource;
  resource.id = {"buffer"};
  resource.content = mc::sha256("fixture");
  resource.scalar_type = mc::ScalarType::f32;
  resource.components = 4;
  resource.element_count = UINT64_MAX;
  resource.byte_length = 0;
  r.resources = {resource};
  rejects(mc::validate(r), E::invalid_input,
          "resource multiplication overflow");
  r = recipe;
  resource.element_count = 3;
  resource.byte_length = 47;
  r.resources = {resource};
  rejects(mc::validate(r), E::invalid_input, "resource length mismatch");
  resource.byte_length = 48;
  r.resources = {resource};
  passes(mc::validate(r), "typed-resource-v1");
  mc::ValidationOptions small;
  small.limits.max_resource_bytes = 47;
  rejects(mc::validate(r, small), E::resource_limit,
          "resource exhaustion fixture");
  small = {};
  small.limits.max_nodes = 2;
  rejects(mc::validate(recipe, small), E::resource_limit, "node budget");
  small = {};
  small.limits.max_depth = 0;
  rejects(mc::validate(recipe, small), E::resource_limit, "dependency depth");
  auto a = asset;
  a.meshes[0].indices[2] = 99;
  rejects(mc::validate(a), E::invalid_input, "bad index");
  a = asset;
  a.meshes[0].indices.pop_back();
  rejects(mc::validate(a), E::invalid_input, "triangle length");
  a = asset;
  a.meshes[0].normals.pop_back();
  rejects(mc::validate(a), E::invalid_input, "attribute length");
  a = asset;
  a.meshes[0].positions[0].x = std::numeric_limits<float>::quiet_NaN();
  rejects(mc::validate(a), E::invalid_input, "nonfinite float32 position");
  a = asset;
  a.meshes[0].winding = mc::Winding::clockwise;
  rejects(mc::validate(a), E::invalid_input, "wrong winding declaration");
  a = asset;
  a.meshes[0].indices = {0, 0, 1};
  rejects(mc::validate(a), E::invalid_input, "degenerate triangle");
  a = asset;
  a.meshes[0].submeshes[0].material = {"gone"};
  rejects(mc::validate(a), E::missing_dependency, "asset material reference");
  a = asset;
  a.instances[0].parent = mc::SemanticId{"placed.b"};
  a.instances[1].parent = mc::SemanticId{"placed.a"};
  rejects(mc::validate(a), E::cycle, "instance parent cycle");
  a = asset;
  a.assemblies[0].instances[0].prototype = {"pair"};
  a.assemblies[0].instances[0].target = mc::InstanceTarget::assembly;
  rejects(mc::validate(a), E::cycle, "assembly cycle");
  a = asset;
  a.assemblies[0].instances[0].parent = mc::SemanticId{"placed.a"};
  rejects(mc::validate(a), E::missing_dependency,
          "assembly scope cannot refer to root parent");
  a = optional;
  a.rigs[0].joints[0].parent = 0;
  rejects(mc::validate(a, opts), E::cycle, "rig cycle");
  a = optional;
  a.skins[0].weights[0].x = 0.5f;
  rejects(mc::validate(a, opts), E::invalid_input, "skin normalization");
  a = optional;
  a.animations[0].tracks[0].keys[1].time_seconds = 0;
  rejects(mc::validate(a, opts), E::invalid_input, "animation ordering");
  a = optional;
  a.terrain->boundaries[0].sample_spacing_metres = 0;
  rejects(mc::validate(a, opts), E::invalid_input, "terrain spacing");
  auto matrix =
      mm::multiply(mm::translation({10, 20, 30}), mm::scale({-2, 3, 4}));
  auto point = mm::transform_point(matrix, {1, 2, 3});
  check(point.x == 8 && point.y == 26 && point.z == 42,
        "parent * local column-vector convention");
  check(mc::reverses_winding(matrix), "mirrored winding correction flag");
  auto normal = mc::transform_normal(matrix, {1, 1, 0});
  check(normal && normal->x < 0 && normal->y > 0 &&
            std::abs(normal->x / normal->y + 1.5f) < 1e-5,
        "inverse-transpose nonuniform normal");
  check(!mc::transform_normal(mm::scale(0), {1, 0, 0}),
        "invalid normal transform returns no value");
  mc::ContentDigest d1, d2;
  passes(mc::content_digest(recipe, d1), "recipe SHA digest");
  r = recipe;
  std::swap(r.nodes[0].inputs[0], r.nodes[0].inputs[1]);
  passes(mc::content_digest(r, d2), "reordered CSG digest");
  check(d1.hex != d2.hex, "CSG input order affects content identity");
  r = recipe;
  std::swap(r.nodes[1], r.nodes[2]);
  passes(mc::content_digest(r, d2), "recipe order digest");
  check(d1.hex != d2.hex, "authored node order affects identity");
  r = recipe;
  r.metadata = mc::Value::Object{{"comment", "changed"}};
  r.source_map["cut"].line = 99;
  r.extensions = {
      {{"vendor.editor", 99}, false, mc::Value::Object{{"label", "test"}}}};
  passes(mc::content_digest(r, d2), "optional metadata extension");
  check(d1.hex == d2.hex, "optional metadata and source map are nonsemantic");
  r.extensions[0].required = true;
  rejects(mc::validate(r), E::unsupported_capability,
          "unknown required extension fails");
  check(mc::sha256("").hex ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "SHA empty known answer");
  check(mc::sha256("abc").hex ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA abc known answer");
  check(mc::sha256(std::string(1000000, 'a')).hex ==
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        "SHA million-a multiblock known answer");
  mc::Rng rng(1);
  check(rng.next_u64() == UINT64_C(0x910a2dec89025cc1),
        "SplitMix64 seed-one vector");
  mc::Rng zero(0), mapped_zero(UINT64_C(0x9e3779b97f4a7c15));
  for (int i = 0; i < 32; ++i)
    check(zero.next_u64() == mapped_zero.next_u64(),
          "native zero seed remapping");
  mc::Value value;
  passes(mc::parse_json("{\"z\":-0,\"a\":0.1}", value), "JSON parse");
  passes(mc::canonical_json(value, wire), "canonical numbers");
  check(wire ==
            "{\"a\":0.1000000000000000055511151231257827021181583404541015625,"
            "\"z\":0}",
        "exact decimal profile");
  for (const char *malformed : {"{\"a\":1,\"a\":2}", "01", "[1,]", "1e9999",
                                "\"\\ud800\"", "true false"})
    rejects(mc::parse_json(malformed, value), E::invalid_input,
            "malformed JSON fixture");
  rejects(mc::parse_json(std::string("\"") + char(0xff) + "\"", value),
          E::invalid_input, "invalid UTF-8");
  passes(mc::parse_json("\"\\ud83d\\ude00\"", value), "surrogate pair");
  mc::ValidationLimits limits;
  limits.max_depth = 1;
  rejects(mc::parse_json("[[[0]]]", value, limits), E::resource_limit,
          "JSON depth limit");
  limits = {};
  limits.max_document_bytes = 2;
  rejects(mc::canonical_json(mc::Value::Array{1, 2}, wire, limits),
          E::resource_limit, "canonical byte limit");
  mc::RecipeDocument unchanged = recipe;
  rejects(mc::decode_recipe("{}", unchanged), E::invalid_input,
          "invalid decoder envelope");
  check(unchanged.source.id == recipe.source.id,
        "failed decode leaves output untouched");
  passes(mc::encode(recipe, wire), "encode strict schema fixture");
  mc::Value envelope;
  passes(mc::parse_json(wire, envelope), "parse strict schema fixture");
  std::get<mc::Value::Object>(envelope.data)["surprise"] = 1;
  passes(mc::canonical_json(envelope, wire), "unknown field encode");
  rejects(mc::decode_recipe(wire, unchanged), E::invalid_input,
          "unknown schema fields rejected");
  // Shared DAG with a route longer than a previously cached DFS route.
  r = fixtures::extrusion();
  r.nodes[1].id = {"a"};
  r.nodes[1].inputs = {
      {"profile", mc::InputKind::node, {"profile"}, mc::ProductType::profile}};
  mc::RecipeNode p;
  p.id = {"z"};
  p.operation = "placement.affine";
  p.inputs = {{"mesh", mc::InputKind::node, {"a"}, mc::ProductType::mesh}};
  r.nodes.push_back(p);
  r.outputs = {{"z"}};
  small = {};
  small.limits.max_depth = 1;
  rejects(mc::validate(r, small), E::resource_limit,
          "DAG longest depth not hidden by memoization");
  r = recipe;
  std::get<mc::Value::Object>(r.nodes[1].parameters.data)["ignored_option"] =
      true;
  rejects(mc::validate(r), E::invalid_input,
          "unknown core parameter cannot silently alter semantics");
  r = recipe;
  r.required_capabilities.pop_back();
  rejects(mc::validate(r), E::invalid_input,
          "PBR capability declaration required");
  r = recipe;
  r.source_map.clear();
  mc::ResourceDescription positions;
  positions.id = {"positions"};
  positions.content = mc::sha256("positions");
  positions.scalar_type = mc::ScalarType::f32;
  positions.components = 3;
  positions.element_count = 3;
  positions.byte_length = 36;
  mc::ResourceDescription indices = positions;
  indices.id = {"indices"};
  indices.content = mc::sha256("indices");
  indices.scalar_type = mc::ScalarType::u32;
  indices.components = 1;
  indices.byte_length = 12;
  r.resources = {positions, indices};
  mc::RecipeNode input_mesh;
  input_mesh.id = {"explicit"};
  input_mesh.operation = "mesh.input";
  input_mesh.materials = {{"clay"}};
  input_mesh.inputs = {
      {"positions",
       mc::InputKind::resource,
       {"positions"},
       mc::ProductType::mesh},
      {"indices", mc::InputKind::resource, {"indices"}, mc::ProductType::mesh}};
  r.nodes = {input_mesh};
  r.outputs = {{"explicit"}};
  passes(mc::validate(r), "explicit-mesh-v1");
  passes(mc::encode(r, wire), "explicit mesh resource encode");
  passes(mc::decode_recipe(wire, round), "explicit mesh resource decode");
  passes(mc::encode(round, again), "explicit mesh resource reencode");
  check(wire == again && round.resources[0].byte_length == 36,
        "typed buffer lengths survive exact round trip");
  auto explicit_mesh = r;
  r.resources[1].scalar_type = mc::ScalarType::f32;
  rejects(mc::validate(r), E::invalid_input,
          "typed index buffer must be uint32");
  r = explicit_mesh;
  r.nodes[0].inputs.pop_back();
  rejects(mc::validate(r), E::invalid_input,
          "positions alone are not explicit indexed mesh");
  r = recipe;
  r.source_map.clear();
  r.asset_dependencies = {{{"child"}, mc::sha256("child-version-one"), {}}};
  mc::RecipeNode assembly;
  assembly.id = {"shared-child"};
  assembly.operation = "assembly.reference";
  assembly.output_type = mc::ProductType::assembly;
  assembly.inputs = {
      {"assembly", mc::InputKind::asset, {"child"}, mc::ProductType::assembly}};
  r.nodes = {assembly};
  r.outputs = {{"shared-child"}};
  passes(mc::validate(r), "assembly-recipe-v1");
  passes(mc::encode(r, wire), "pinned assembly dependency encode");
  passes(mc::decode_recipe(wire, round), "pinned assembly dependency decode");
  check(round.asset_dependencies[0].content.hex ==
            r.asset_dependencies[0].content.hex,
        "dependency digest round trip");
  passes(mc::content_digest(r, d1), "pinned assembly dependency digest");
  r.asset_dependencies[0].content = mc::sha256("child-version-two");
  passes(mc::content_digest(r, d2), "changed assembly dependency digest");
  check(d1.hex != d2.hex, "dependency content affects recipe identity");
  r.asset_dependencies[0].content.hex = "not-a-digest";
  rejects(mc::validate(r), E::invalid_input,
          "assembly dependency requires SHA-256");
  a = asset;
  positions.id = {"image"};
  positions.scalar_type = mc::ScalarType::u8;
  positions.components = 4;
  positions.element_count = 1;
  positions.byte_length = 4;
  a.resources = {positions};
  mc::TextureDescription texture;
  texture.id = {"normal-map"};
  texture.resource = {"image"};
  a.textures = {texture};
  a.materials[0].normal_texture = mc::SemanticId{"normal-map"};
  passes(mc::validate(a), "texture-material-v1");
  passes(mc::encode(a, wire), "texture description encode");
  passes(mc::decode_asset(wire, asset_round), "texture description decode");
  check(asset_round.textures[0].pixel_format == "rgba8" &&
            asset_round.textures[0].width == 1,
        "texture format and dimensions round trip");
  a.textures[0].color_space = mc::ColorSpace::srgb;
  rejects(mc::validate(a), E::invalid_input,
          "normal map requires explicit linear color space");
  a = asset;
  a.meshes[0].submeshes[0].first_index = UINT32_MAX;
  rejects(mc::validate(a), E::invalid_input, "submesh addition does not wrap");
  std::cout << "ALL PASS: " << checks
            << " contract checks; fixtures ordered-csg-v1, extruded-prop-v1, "
               "repeated-assembly-v1, typed-products-v1, malformed-v1, "
               "exhaustion-v1\n";
}
