#include <matter/graphics/composition.h>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace mg = matter::graphics;
namespace mc = matter::contracts;
void check(bool ok, const char *id) { if (!ok) { std::cerr << "FAIL " << id << '\n'; std::exit(1); } }
int main() {
  mg::CompositionInputs base;
  check(mg::validate_composition(base).ok(), "COMP-VALID");
  auto rejected = [&](const mg::CompositionInputs &in, mc::ErrorCode code, const char *id) {
    auto r = mg::validate_composition(in);
    check(!r.ok() && r.diagnostics[0].code == code, id);
  };
  auto bad = base; bad.required_capabilities = {{"matter.graphics.ray-tracing-producer", 1}};
  rejected(bad, mc::ErrorCode::unsupported_capability, "COMP-CAPS producer is not a product consumer");
  bad.required_capabilities = {{"matter.graphics.composition", 2}};
  rejected(bad, mc::ErrorCode::unsupported_capability, "COMP-CAPS version");
  bad = base; bad.albedo.width = UINT32_MAX; bad.albedo.height = UINT32_MAX;
  rejected(bad, mc::ErrorCode::resource_limit, "COMP-BUDGET dimensions");
  bad = base; bad.normal.pixels.clear(); rejected(bad, mc::ErrorCode::invalid_input, "COMP-LENGTH");
  bad = base; bad.identity.pixels[0][0] = 1; rejected(bad, mc::ErrorCode::invalid_input, "COMP-IDENTITY");
  bad = base; bad.identity.pixels[0][0] = 0x80000000u;
  check(mg::validate_composition(bad).ok(), "COMP-IMPOSTOR identity flag preserved");
  bad = base; bad.depth.pixels[0][0] = -1; rejected(bad, mc::ErrorCode::invalid_input, "COMP-DEPTH");
  bad = base; bad.visibility.pixels[0][1] = 2; rejected(bad, mc::ErrorCode::invalid_input, "COMP-VISIBILITY");
  bad = base; bad.diffuse.pixels[0][0] = std::numeric_limits<float>::quiet_NaN();
  rejected(bad, mc::ErrorCode::invalid_input, "COMP-FINITE");
  bad = base; bad.camera.forward = {}; rejected(bad, mc::ErrorCode::invalid_input, "COMP-CAMERA zero direction");
  bad = base; bad.camera.far_metres = bad.camera.near_metres; rejected(bad, mc::ErrorCode::invalid_input, "COMP-CAMERA clip interval");
  bad = base; bad.environment.cloud_state[1] = 2; rejected(bad, mc::ErrorCode::invalid_input, "COMP-CLOUD ping");
  bad = base; bad.materials[0].flags_misc[0] = 16;
  rejected(bad, mc::ErrorCode::unsupported_capability, "COMP-WATER requires forward producer");
  bad = base; bad.local_lights.push_back({}); bad.local_lights[0].cos_inner = -1;
  bad.local_lights[0].cos_outer = 1; rejected(bad, mc::ErrorCode::invalid_input, "COMP-SPOT cone order");
  bad = base; bad.local_direct_owner = static_cast<mg::LocalDirectOwner>(9);
  rejected(bad, mc::ErrorCode::invalid_input, "COMP-OWNER enum");
  mg::CompositionLimits limits; limits.max_input_bytes = 32;
  check(!mg::validate_composition(base, limits).ok(), "COMP-BUDGET bytes");
  mc::MaterialDescription material; material.base_color.factor = {0.2f, 0.3f, 0.4f, 1};
  material.metallic = 0.7f; material.roughness = 0.2f; material.emissive = {2, 3, 4};
  mg::ShadingMaterial converted;
  check(mg::shading_material(material, converted).ok() && converted.base_roughness[0] == 0.2f &&
        converted.base_roughness[3] == 0.2f && converted.metal_opacity_spec_coat[0] == 0.7f &&
        converted.emission_strength[1] == 3, "COMP-MATERIAL conversion");
  const auto previous = converted;
  material.normal_texture = mc::SemanticId{"normal"};
  check(!mg::shading_material(material, converted).ok() && converted.emission_strength == previous.emission_strength,
        "COMP-MATERIAL no dropped texture/partial publication");
  std::cout << "ALL PASS COMP-VALID/CAPS/BUDGET/LENGTH/IDENTITY/IMPOSTOR/DEPTH/VISIBILITY/FINITE/CAMERA/CLOUD/WATER/SPOT/OWNER/MATERIAL\n";
}
