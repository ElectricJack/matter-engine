#pragma once
// Hand-built asset moved from the existing native Vulkan fixture.
ms::UploadRequest triangle() {
  mc::AssetDocument a; a.id = {"gpu.synthetic"};
  a.provenance = {{"gpu-fixture"}, "fixture://gpu", "1", {}, {}};
  a.required_capabilities = {{"matter.asset.core", 1}, {"matter.material.pbr", 1}};
  mc::MeshPrototype m; m.id = {"triangle"};
  m.positions = {{-0.1f, -0.1f, 0}, {0.1f, -0.1f, 0}, {0, 0.1f, 0}};
  m.indices = {0, 1, 2}; m.bounds = {{-0.1f, -0.1f, 0}, {0.1f, 0.1f, 0}};
  m.submeshes = {{0, 3, {"paint"}}};
  mc::MaterialDescription material; material.id = {"paint"}; a.materials = {material};
  a.meshes = {m}; a.bounds = m.bounds;
  return {std::make_shared<const mc::AssetDocument>(std::move(a)), {}, {}};
}
