#pragma once
#include <matter/contracts/contracts.h>
namespace fixtures {
namespace mc = matter::contracts;
inline mc::RecipeDocument recipe() {
  mc::RecipeDocument d;
  d.source.id = {"fixture.ordered-csg"};
  d.source.uri = "fixture://ordered-csg";
  d.source.revision = "1";
  d.required_capabilities = {{"matter.recipe.core", 1},
                             {"matter.material.pbr", 1}};
  d.parameters = mc::Value::Object{{"width", 0.1}, {"enabled", true}};
  d.seed = UINT64_C(0x0123456789abcdef);
  mc::MaterialDescription material;
  material.id = {"clay"};
  material.base_color.factor = {0.75f, 0.25f, 0.125f, 1};
  material.roughness = 0.8f;
  d.materials = {material};
  mc::RecipeNode box;
  box.id = {"outer"};
  box.operation = "primitive.box";
  box.parameters = mc::Value::Object{{"size", mc::Value::Array{2, 2, 2}}};
  box.materials = {{"clay"}};
  mc::RecipeNode sphere;
  sphere.id = {"cutter"};
  sphere.operation = "primitive.sphere";
  sphere.parameters = mc::Value::Object{{"radius", 1}, {"segments", 16}};
  mc::RecipeNode csg;
  csg.id = {"cut"};
  csg.operation = "solid.csg";
  csg.parameters = mc::Value::Object{{"operator", "subtract"}};
  csg.inputs = {
      {"operand", mc::InputKind::node, {"outer"}, mc::ProductType::mesh},
      {"operand", mc::InputKind::node, {"cutter"}, mc::ProductType::mesh}};
  csg.materials = {{"clay"}};
  d.nodes = {csg, box, sphere};
  d.outputs = {{"cut"}};
  d.source_map = {{"cut", {"fixture://ordered-csg", 12, 3}}};
  return d;
}
inline mc::RecipeDocument extrusion() {
  auto d = recipe();
  d.source.id = {"fixture.extruded-prop"};
  mc::RecipeNode p;
  p.id = {"profile"};
  p.operation = "profile.polygon";
  p.output_type = mc::ProductType::profile;
  p.parameters = mc::Value::Object{
      {"points",
       mc::Value::Array{mc::Value::Array{0, 0}, mc::Value::Array{1, 0},
                        mc::Value::Array{1, 1}, mc::Value::Array{0, 1}}}};
  mc::RecipeNode e;
  e.id = {"extrude"};
  e.operation = "profile.extrude";
  e.parameters = mc::Value::Object{{"depth", 2}};
  e.inputs = {
      {"profile", mc::InputKind::node, {"profile"}, mc::ProductType::profile}};
  e.materials = {{"clay"}};
  d.nodes = {p, e};
  d.outputs = {{"extrude"}};
  d.source_map.clear();
  return d;
}
inline mc::AssetDocument asset() {
  mc::AssetDocument d;
  d.id = {"fixture.repeated-assembly"};
  d.provenance.id = {"fixture.generator"};
  d.provenance.uri = "fixture://generator";
  d.provenance.revision = "1";
  d.required_capabilities = {{"matter.asset.core", 1},
                             {"matter.material.pbr", 1}};
  d.bounds = {{-10, -10, -10}, {10, 10, 10}};
  mc::MaterialDescription material;
  material.id = {"clay"};
  d.materials = {material};
  mc::MeshPrototype mesh;
  mesh.id = {"triangle"};
  mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
  mesh.tangents = {{1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}};
  mesh.uv_sets = {{{0, 0}, {1, 0}, {0, 1}}};
  mesh.indices = {0, 1, 2};
  mesh.submeshes = {{0, 3, {"clay"}}};
  mesh.bounds = {{0, 0, 0}, {1, 1, 0}};
  d.meshes = {mesh};
  mc::Instance a;
  a.id = {"child.a"};
  a.prototype = {"triangle"};
  mc::Instance b = a;
  b.id = {"child.b"};
  b.parent = a.id;
  b.local_transform = mm::translation({2, 0, 0});
  d.assemblies = {{{"pair"}, {a, b}}};
  mc::Instance first;
  first.id = {"placed.a"};
  first.prototype = {"pair"};
  first.target = mc::InstanceTarget::assembly;
  mc::Instance second = first;
  second.id = {"placed.b"};
  second.local_transform =
      mm::multiply(mm::translation({3, 2, 1}), mm::scale({-2, 3, 4}));
  d.instances = {first, second};
  return d;
}
inline mc::ValidationOptions optional_options() {
  mc::ValidationOptions o;
  for (const char *name : {"matter.asset.attachments", "matter.asset.lod",
                           "matter.asset.collision", "matter.asset.animation",
                           "matter.asset.terrain"})
    o.supported_capabilities.push_back({name, 1});
  return o;
}
inline mc::AssetDocument optional_asset() {
  auto d = asset();
  auto o = optional_options();
  d.required_capabilities = o.supported_capabilities;
  d.attachments = {{{"socket"}, {"placed.a"}, {}}};
  d.lods = {{{"triangle"}, 0.01f}};
  mc::CollisionDescription collision;
  collision.id = {"collider"};
  collision.instance = {"placed.a"};
  collision.prototype = mc::SemanticId{"triangle"};
  d.collisions = {collision};
  mc::Rig rig;
  rig.id = {"rig"};
  rig.joints = {{{"root"}, -1, {}}};
  d.rigs = {rig};
  mc::SkinWeights skin;
  skin.prototype = {"triangle"};
  skin.rig = {"rig"};
  skin.joints = {{{0, 0, 0, 0}}, {{0, 0, 0, 0}}, {{0, 0, 0, 0}}};
  skin.weights = {{1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}};
  d.skins = {skin};
  mc::AnimationClip clip;
  clip.id = {"idle"};
  clip.rig = {"rig"};
  clip.duration_seconds = 1;
  mc::AnimationTrack track;
  track.joint = {"root"};
  mc::AnimationKey a, b;
  b.time_seconds = 1;
  b.translation = {0, 1, 0};
  track.keys = {a, b};
  clip.tracks = {track};
  d.animations = {clip};
  mc::TerrainProduct terrain;
  terrain.region = {"chunk.0"};
  terrain.origin_metres = {1000000000, 0, -1000000000};
  terrain.prototypes = {{"triangle"}};
  mc::TerrainBoundary boundary;
  boundary.neighbor = {1, 0, 0};
  boundary.policy = "fixture.shared-edge.v1";
  terrain.boundaries = {boundary};
  d.terrain = terrain;
  return d;
}
} // namespace fixtures
