#include <matter/graphics/composition.h>
#include <algorithm>
#include <cmath>

namespace matter::graphics {
namespace {
using E = data::ErrorCode;
data::ValidationResult fail(E code, const char *message) {
  return {{{data::Severity::error, code, message, {}, {}, {}}}};
}
bool finite(float f) { return std::isfinite(f) && std::abs(f) <= 1e12f; }
bool finite(data::Vec3 v) { return finite(v.x) && finite(v.y) && finite(v.z); }
bool unit(float f) { return finite(f) && f >= 0 && f <= 1; }
bool color(data::Vec3 v) { return finite(v) && v.x >= 0 && v.y >= 0 && v.z >= 0; }
bool direction(data::Vec3 v) {
  return finite(v) && double(v.x)*v.x + double(v.y)*v.y + double(v.z)*v.z > 1e-12;
}
bool finite(Rgba v) { return std::all_of(v.begin(), v.end(), [](float f) { return finite(f); }); }
}
std::vector<data::Capability> composition_capabilities() {
  return {{"matter.graphics.composition", 1}, {"matter.graphics.local-light-shading", 1},
          {"matter.graphics.shadow-product", 1}, {"matter.graphics.gi-product", 1},
          {"matter.graphics.environment-product", 1}, {"matter.graphics.volume-product", 1},
          {"matter.graphics.native-subsurface", 1}, {"matter.graphics.native-transmission", 1},
          {"matter.graphics.display-transform", 1}};
}
data::ValidationResult shading_material(const data::MaterialDescription &m, ShadingMaterial &out) {
  if (m.base_color.texture || m.metallic_roughness_texture || m.normal_texture || m.emissive_texture)
    return fail(E::unsupported_capability, "Texture evaluation belongs to the material/G-buffer producer");
  const auto c = m.base_color.factor;
  if (!unit(c.x) || !unit(c.y) || !unit(c.z) || c.w != 1 || !unit(m.roughness) ||
      !unit(m.metallic) || !color(m.emissive) || !finite(m.normal_scale) || m.normal_scale < 0)
    return fail(E::invalid_input, "Invalid opaque PBR material");
  ShadingMaterial result;
  result.base_roughness = {c.x, c.y, c.z, m.roughness};
  result.metal_opacity_spec_coat[0] = m.metallic;
  result.emission_strength = {m.emissive.x, m.emissive.y, m.emissive.z, 1};
  out = result; return {};
}
data::ValidationResult validate_composition(const CompositionInputs &in, CompositionLimits limits) {
  const auto caps = composition_capabilities();
  if (in.required_capabilities.size() > caps.size())
    return fail(E::unsupported_capability, "Too many required composition capabilities");
  for (const auto &c : in.required_capabilities)
    if (std::none_of(caps.begin(), caps.end(), [&](const auto &s) { return c.name == s.name && c.version == s.version; }))
      return fail(E::unsupported_capability, "Unsupported required composition capability/version");
  if (in.materials.empty() || in.materials.size() > limits.max_materials ||
      in.materials.size() >= 0x80000000ull || in.local_lights.size() > limits.max_lights ||
      in.local_lights.size() > UINT32_MAX)
    return fail(E::resource_limit, "Composition material/light budget exceeded");
  uint64_t bytes = 0;
  auto charge = [&](uint64_t n) {
    if (n > limits.max_input_bytes - bytes) return false;
    bytes += n; return true;
  };
  if (!charge(uint64_t(in.materials.size()) * sizeof(ShadingMaterial)) ||
      !charge(uint64_t(in.local_lights.size()) * 64))
    return fail(E::resource_limit, "Composition input byte budget exceeded");
  const std::array<const FloatImage *, 16> images{{
      &in.albedo, &in.normal, &in.orm, &in.depth, &in.visibility, &in.diffuse, &in.specular,
      &in.transmission, &in.local_direct, &in.integrated_volume, &in.environment.sky_view,
      &in.environment.irradiance_sh, &in.environment.cloud_optical_depth[0],
      &in.environment.cloud_optical_depth[1], &in.environment.cloud_optical_depth[2],
      &in.environment.cloud_optical_depth[3]}};
  for (size_t i = 0; i < images.size(); ++i) {
    const auto &im = *images[i];
    const bool volume = i == 9 || i >= 12;
    const uint32_t max_dim = volume ? limits.max_volume_dimension : limits.max_dimension;
    if (!im.width || !im.height || !im.depth || im.width > max_dim || im.height > max_dim ||
        im.depth > (volume ? max_dim : 1u))
      return fail(E::resource_limit, "Invalid composition image dimensions");
    // Divide before multiplying: caller supplied ceilings are not trusted.
    const uint64_t xy = uint64_t(im.width) * im.height;
    if (xy > UINT64_MAX / im.depth || xy * im.depth > UINT64_MAX / sizeof(Rgba))
      return fail(E::resource_limit, "Composition image size overflow");
    const uint64_t count = xy * im.depth;
    if (count != im.pixels.size()) return fail(E::invalid_input, "Composition image payload length mismatch");
    if (!charge(count * sizeof(Rgba))) return fail(E::resource_limit, "Composition input byte budget exceeded");
  }
  const auto w = in.albedo.width, h = in.albedo.height;
  for (const auto *im : {&in.normal, &in.orm, &in.depth})
    if (im->width != w || im->height != h) return fail(E::invalid_input, "G-buffer dimensions must match");
  const uint64_t pixels = uint64_t(w) * h;
  if (in.identity.width != w || in.identity.height != h || in.identity.pixels.size() != pixels)
    return fail(E::invalid_input, "Identity dimensions/payload must match G-buffer");
  if (!charge(pixels * sizeof(in.identity.pixels[0]))) return fail(E::resource_limit, "Identity byte budget exceeded");
  // Outputs plus upload/readback allocations are bounded separately by the backend.
  if (in.diffuse.width > w || in.diffuse.height > h)
    return fail(E::invalid_input, "Diffuse product must be full or reduced resolution");
  if (in.environment.irradiance_sh.width != 3 || in.environment.irradiance_sh.height != 3)
    return fail(E::invalid_input, "Sky irradiance requires nine radiance SH coefficients");
  for (size_t i = 0; i < images.size(); ++i)
    for (auto p : images[i]->pixels)
      if (!finite(p)) return fail(E::invalid_input, "Nonfinite or unbounded image value");
  for (auto p : in.albedo.pixels) if (!unit(p[0]) || !unit(p[1]) || !unit(p[2]))
    return fail(E::invalid_input, "Albedo must be linear RGB in [0,1]");
  for (auto p : in.orm.pixels) if (!unit(p[0]) || !unit(p[1]) || !unit(p[2]))
    return fail(E::invalid_input, "Roughness/metallic/AO must be in [0,1]");
  for (auto p : in.depth.pixels) if (!unit(p[0])) return fail(E::invalid_input, "Depth must be reversed-Z in [0,1]");
  for (auto p : in.visibility.pixels) if (!unit(p[0]) || !unit(p[1]) || !unit(p[2]))
    return fail(E::invalid_input, "Shadow visibility must be RGB in [0,1]");
  for (auto p : in.transmission.pixels) if (!unit(p[3])) return fail(E::invalid_input, "Transmission coverage must be in [0,1]");
  for (auto p : in.integrated_volume.pixels) if (!unit(p[3])) return fail(E::invalid_input, "Volume transmittance must be in [0,1]");
  for (auto id : in.identity.pixels) if ((id[0] & 0x7fffffffu) >= in.materials.size())
    return fail(E::invalid_input, "Identity references missing material");
  for (const auto &m : in.materials) {
    for (auto lane : {m.base_roughness, m.metal_opacity_spec_coat, m.specular_tint_coat_roughness,
                     m.emission_strength, m.transmission, m.absorption_pad, m.scattering, m.scattering_shape})
      if (!finite(lane)) return fail(E::invalid_input, "Invalid material table value");
    if ((m.flags_misc[0] & ~1u) || m.flags_misc[1] || m.flags_misc[2] || m.flags_misc[3])
      return fail(E::unsupported_capability, "Unsupported composition material flags");
    if (!unit(m.transmission[0]) || m.transmission[1] < 1.001f || !unit(m.scattering[3]))
      return fail(E::invalid_input, "Invalid native transmission/scattering parameters");
  }
  const auto &c = in.camera;
  if (!finite(c.position) || !direction(c.forward) || !finite(c.tan_half_fov) || c.tan_half_fov <= 0 ||
      !finite(c.aspect_ratio) || c.aspect_ratio <= 0 || !finite(c.near_metres) || c.near_metres <= 0 ||
      !finite(c.far_metres) || c.far_metres <= c.near_metres || !finite(c.jitter_uv[0]) || !finite(c.jitter_uv[1]))
    return fail(E::invalid_input, "Invalid composition camera");
  const auto &l = in.lighting;
  if (!direction(l.sun_direction) || !finite(l.diffuse_rt_multiplier) || l.diffuse_rt_multiplier < 0 ||
      !finite(l.emission_multiplier) || l.emission_multiplier < 0 || !finite(l.sun_disc_cos_edge) ||
      !finite(l.sun_disc_cos_core) || l.sun_disc_cos_edge < -1 || l.sun_disc_cos_core > 1 ||
      l.sun_disc_cos_edge >= l.sun_disc_cos_core || !finite(in.cloud_top_metres) ||
      !finite(in.exposure_ev) || in.exposure_ev < -6 || in.exposure_ev > 6)
    return fail(E::invalid_input, "Invalid composition lighting/display settings");
  if (in.view < CompositionView::shaded || in.view > CompositionView::local_candidates ||
      (in.local_direct_owner != LocalDirectOwner::raster && in.local_direct_owner != LocalDirectOwner::ray_traced_product))
    return fail(E::invalid_input, "Unknown composition mode");
  for (const auto &light : in.local_lights) {
    if (light.kind != services::LightKind::point && light.kind != services::LightKind::spot)
      return fail(E::unsupported_capability, "Local lights support point/spot; use the sun input for directional lighting");
    if (!finite(light.position) || !direction(light.direction) || !color(light.color) ||
        !finite(light.range_metres) || light.range_metres <= 0 || !finite(light.source_radius_metres) ||
        light.source_radius_metres <= 0 || !finite(light.cos_inner) || !finite(light.cos_outer) ||
        light.cos_outer < -1 || light.cos_inner > 1 || light.cos_inner < light.cos_outer)
      return fail(E::invalid_input, "Invalid local-light input");
  }
  const auto &e = in.environment;
  if (!color(e.direct_sun) || !color(e.sun_disc) || !color(e.sky_scale) || !color(e.irradiance_scale) ||
      !finite(e.cloud_state) || !finite(e.cloud_filter))
    return fail(E::invalid_input, "Invalid environment input");
  for (const auto &matrix : e.cloud_world_to_uvw) for (float value : matrix.m)
    if (!finite(value)) return fail(E::invalid_input, "Invalid cloud transform");
  if ((e.cloud_state[0] != 0 && e.cloud_state[0] != 1) ||
      (e.cloud_state[1] != 0 && e.cloud_state[1] != 1) || (e.cloud_state[2] != 0 && e.cloud_state[2] != 1))
    return fail(E::invalid_input, "Cloud enabled/ping selectors must be 0 or 1");
  return {};
}
} // namespace matter::graphics
