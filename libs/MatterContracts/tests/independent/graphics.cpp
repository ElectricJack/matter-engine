#include <matter/contracts/graphics.h>
#include <iostream>

// Graphics-only public consumer. No generator factory or implementation links.
int main() {
  namespace mc = matter::contracts;
  namespace ms = matter::services;
  mc::AssetDocument a; a.id = {"imported.triangle"}; a.provenance.id = {"importer"};
  a.required_capabilities = {{"matter.asset.core", 1}, {"matter.material.pbr", 1}};
  a.bounds = {{0,0,0}, {1,1,0}};
  mc::MaterialDescription material; material.id = {"surface"}; a.materials = {material};
  mc::MeshPrototype m; m.id = {"triangle"}; m.bounds = a.bounds;
  m.positions = {{0,0,0}, {1,0,0}, {0,1,0}}; m.indices = {0,1,2};
  m.submeshes = {{0,3,{"surface"}}}; a.meshes = {m};
  ms::UploadRequest upload{std::make_shared<const mc::AssetDocument>(std::move(a)), {}, {}};
  if (!ms::validate(ms::AssetProduct{*upload.asset, upload.resources}).ok()) return 1;
  ms::SceneUpdate scene; scene.scene_revision = 1;
  // A fixture handle describes identity only; no device resource is allocated.
  ms::GraphicsHandle handle{1,1,1};
  scene.upsert = {{{"one"}, {"triangle"}, handle, {}}, {{"two"}, {"triangle"}, handle, {}}};
  if (scene.upsert[0].id == scene.upsert[1].id || scene.upsert[0].prototype != scene.upsert[1].prototype) return 1;
  if (ms::require_capabilities({{"unknown.representation",1}}, upload.asset->required_capabilities).ok()) return 1;
  std::cout << "PASS GATE-C2-GRAPHICS imported/synthetic data; no rendering performed\n";
}
