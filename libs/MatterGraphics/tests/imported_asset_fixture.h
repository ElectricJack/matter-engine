#pragma once
#include <matter/contracts/contracts.h>

// Generate the imported contracts fixture without an asset/source resolver.
// Keep the repeated, parented, mirrored and nonuniform placements used by both
// facade and native raster regression guards, including the JSON decode seam.
inline matter::contracts::ValidationResult imported_asset_fixture(
    matter::contracts::AssetDocument &decoded) {
  namespace mc = matter::contracts;
  mc::AssetDocument asset;
  asset.id = {"fixture.repeated-assembly"};
  asset.provenance = {{"fixture.generator"}, "fixture://generator", "1", {}, {}};
  asset.required_capabilities = {{"matter.asset.core", 1}, {"matter.material.pbr", 1}};
  asset.bounds = {{-10, -10, -10}, {10, 10, 10}};
  mc::MeshPrototype mesh;
  mesh.id = {"triangle"};
  mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
  mesh.tangents = {{1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}};
  mesh.uv_sets = {{{0, 0}, {1, 0}, {0, 1}}};
  mesh.indices = {0, 1, 2};
  mesh.submeshes = {{0, 3, {"clay"}}};
  mesh.bounds = {{0, 0, 0}, {1, 1, 0}};
  asset.meshes = {mesh};
  mc::MaterialDescription material;
  material.id = {"clay"};
  material.roughness = 1;
  asset.materials = {material};
  mc::Instance child;
  child.id = {"child.a"}; child.prototype = mesh.id;
  auto parented = child;
  parented.id = {"child.b"}; parented.parent = child.id;
  parented.local_transform = mm::translation({2, 0, 0});
  asset.assemblies = {{{"pair"}, {child, parented}}};
  mc::Instance placed;
  placed.id = {"placed.a"}; placed.prototype = {"pair"};
  placed.target = mc::InstanceTarget::assembly;
  auto mirrored = placed;
  mirrored.id = {"placed.b"};
  mirrored.local_transform = mm::multiply(mm::translation({3, 2, 1}), mm::scale({-2, 3, 4}));
  asset.instances = {placed, mirrored};
  std::string json;
  auto result = mc::encode(asset, json);
  return result.ok() ? mc::decode_asset(json, decoded) : result;
}
