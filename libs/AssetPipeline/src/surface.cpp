#include "surface_internal.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace matter::asset {
namespace detail {
namespace {
bool finite(data::Vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool unit(float v) { return std::isfinite(v) && v >= 0 && v <= 1; }
bool color(data::Vec3 v) { return finite(v) && v.x >= 0 && v.y >= 0 && v.z >= 0; }
bool ldr(data::Vec3 v) { return v.x <= 1 && v.y <= 1 && v.z <= 1; }
uint8_t quantize(float v) { return uint8_t(std::lround(std::clamp(v, 0.f, 1.f) * 255)); }
uint8_t srgb(float v) {
  return quantize(v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f);
}
void rgb(Bytes &b, data::Vec3 v) {
  b.insert(b.end(), {srgb(v.x), srgb(v.y), srgb(v.z), 255});
}
void floats(Bytes &b, data::Vec3 v) { scalar(b, v.x); scalar(b, v.y); scalar(b, v.z); }
data::Value native_value(const NativeSurfaceDescription &n) {
  auto wrap = [](data::WrapMode w) { return w == data::WrapMode::clamp ? "clamp" : w == data::WrapMode::mirror ? "mirror" : "repeat"; };
  return data::Value::Object{
      {"version", n.version}, {"width", n.width}, {"height", n.height},
      {"material", n.material.value}, {"base_color", n.base_color.value},
      {"orm", n.orm.value}, {"normal", n.normal.value}, {"emissive", n.emissive.value},
      {"height_metres", n.height_metres.value}, {"coverage", n.coverage.value},
      {"height_min_metres", n.height_min_metres}, {"height_max_metres", n.height_max_metres},
      {"source_tape", n.source_tape ? data::Value(n.source_tape->value) : data::Value{}},
      {"source_version", n.source_version}, {"wrap_u", wrap(n.wrap_u)}, {"wrap_v", wrap(n.wrap_v)},
      {"min_filter", n.min_filter == data::FilterMode::linear ? "linear" : "nearest"},
      {"mag_filter", n.mag_filter == data::FilterMode::linear ? "linear" : "nearest"}};
}
} // namespace

uint64_t surface_preflight(const SurfaceSettings &s, size_t samples,
                           const services::EvaluationOptions &o,
                           const std::vector<data::Capability> &available) {
  checked(services::validate(o.budget));
  checked(services::require_capabilities(s.required_capabilities, available));
  checked(services::require_capabilities(o.algorithms, available));
  need(s.output == SurfaceOutput::pbr_rgba8 || s.output == SurfaceOutput::native_float32,
       "unknown surface output", E::unsupported_capability);
  const uint64_t count = uint64_t(s.width) * s.height;
  need(count && count == samples, "surface raster dimensions do not match samples");
  need(count <= o.budget.operations && count <= SIZE_MAX / sizeof(SurfaceTexel),
       "surface raster exceeds sample budget", E::resource_limit);
  const uint64_t bytes = s.output == SurfaceOutput::native_float32 ? 53 : 16;
  need(count <= o.budget.output_bytes / bytes &&
           count <= o.validation.limits.max_resource_bytes / bytes &&
           count <= o.budget.working_bytes / (bytes + sizeof(SurfaceTexel)),
       "surface raster exceeds byte budget", E::resource_limit);
  // Validate identity, provenance, sampler and capability opt-in before allocating.
  data::AssetDocument probe;
  probe.id = s.asset_id; probe.provenance = s.provenance;
  data::MaterialDescription material; material.id = s.material_id;
  probe.materials.push_back(material);
  probe.resources.push_back({{"probe.bytes"}, data::sha256(""), data::ScalarType::u8, 4, 1, 4});
  data::TextureDescription t;
  t.id = {"probe.texture"}; t.resource = probe.resources.front().id;
  t.wrap_u = s.wrap_u; t.wrap_v = s.wrap_v;
  t.min_filter = s.min_filter; t.mag_filter = s.mag_filter;
  probe.textures.push_back(t);
  probe.required_capabilities = {{"matter.asset.core", 1}, {"matter.material.pbr", 1}};
  if (s.output == SurfaceOutput::native_float32)
    probe.required_capabilities.push_back({native_surface_capability, 1});
  checked(data::validate(probe, o.validation));
  return count;
}

SurfaceResult finish_surface(const services::JobEnvelope &job, const SurfaceImage &image,
                             const services::EvaluationOptions &o, services::JobMonitor &monitor,
                             services::ResourceUsage usage, const SurfaceOrigin &origin) {
  const auto &s = image.settings;
  const auto count = image.texels.size();
  const bool native = s.output == SurfaceOutput::native_float32;
  const uint64_t bytes = uint64_t(count) * (native ? 53 : 16) + (native ? origin.tape.size() : 0);
  need(bytes <= o.budget.output_bytes && bytes <= o.validation.limits.max_resource_bytes,
       "surface payload exceeds budget", E::resource_limit);
  usage.working_bytes = std::max(usage.working_bytes, bytes + uint64_t(count) * sizeof(SurfaceTexel));
  checked(monitor.checkpoint(usage));
  std::array<Bytes, 6> channels;
  for (size_t i = 0; i < (native ? 6u : 4u); ++i)
    channels[i].reserve(count * (native ? (i < 4 ? 12 : i == 4 ? 4 : 1) : 4));
  float height_min = origin.height_min, height_max = origin.height_max;
  for (size_t i = 0; i < count; ++i) {
    if (!(i % 256)) checked(monitor.checkpoint());
    const auto &t = image.texels[i];
    need(color(t.base_color) && color(t.emissive) && unit(t.orm.x) && unit(t.orm.y) && unit(t.orm.z) &&
             finite(t.normal) && std::abs(double(t.normal.x)*t.normal.x + double(t.normal.y)*t.normal.y + double(t.normal.z)*t.normal.z - 1) <= 1e-4 &&
             std::isfinite(t.height_metres) && t.coverage <= 1, "invalid surface texel");
    if (!native) {
      need(t.coverage == 1 && t.height_metres == 0 && t.orm.x == 1 && ldr(t.base_color) && ldr(t.emissive),
           "PBR rgba8 cannot preserve coverage, relief, occlusion or HDR color; request native_float32",
           E::unsupported_capability);
      rgb(channels[0], t.base_color);
      channels[1].insert(channels[1].end(), {255, quantize(t.orm.y), quantize(t.orm.z), 255});
      channels[2].insert(channels[2].end(), {quantize(t.normal.x*.5f+.5f), quantize(t.normal.y*.5f+.5f), quantize(t.normal.z*.5f+.5f), 255});
      rgb(channels[3], t.emissive);
    } else {
      floats(channels[0], t.base_color); floats(channels[1], t.orm);
      floats(channels[2], t.normal); floats(channels[3], t.emissive);
      scalar(channels[4], t.height_metres); channels[5].push_back(t.coverage);
      height_min = std::min(height_min, t.height_metres);
      height_max = std::max(height_max, t.height_metres);
    }
  }
  data::AssetDocument document;
  document.id = s.asset_id; document.provenance = s.provenance;
  document.required_capabilities = {{"matter.asset.core", 1}, {"matter.material.pbr", 1}};
  data::MaterialDescription material; material.id = s.material_id;
  services::ResolvedInputs resources;
  auto resource = [&](const char *suffix, Bytes b, data::ScalarType type, uint32_t components, uint64_t elements) {
    data::ResourceDescription r{{s.material_id.value + suffix}, data::sha256(view(b)), type, components, elements, b.size()};
    document.resources.push_back(r);
    resources.resources.push_back({r, std::make_shared<const Bytes>(std::move(b))});
    return r.id;
  };
  std::optional<NativeSurfaceDescription> description;
  if (native) {
    NativeSurfaceDescription n;
    n.width = s.width; n.height = s.height; n.material = s.material_id;
    n.wrap_u = s.wrap_u; n.wrap_v = s.wrap_v;
    n.min_filter = s.min_filter; n.mag_filter = s.mag_filter;
    n.base_color = resource(".linear", std::move(channels[0]), data::ScalarType::f32, 3, count);
    n.orm = resource(".orm", std::move(channels[1]), data::ScalarType::f32, 3, count);
    n.normal = resource(".normal", std::move(channels[2]), data::ScalarType::f32, 3, count);
    n.emissive = resource(".emissive", std::move(channels[3]), data::ScalarType::f32, 3, count);
    n.height_metres = resource(".height", std::move(channels[4]), data::ScalarType::f32, 1, count);
    n.coverage = resource(".coverage", std::move(channels[5]), data::ScalarType::u8, 1, count);
    n.height_min_metres = height_min; n.height_max_metres = height_max;
    n.source_version = origin.source_version;
    if (!origin.tape.empty())
      n.source_tape = resource(".tape", Bytes(origin.tape.begin(), origin.tape.end()), data::ScalarType::u8, 1, origin.tape.size());
    document.required_capabilities.push_back({native_surface_capability, 1});
    document.extensions.push_back({{native_surface_capability, 1}, true, native_value(n)});
    description = std::move(n);
  } else {
    auto texture = [&](const char *suffix, Bytes b, data::ColorSpace space) {
      data::TextureDescription t;
      t.id = {s.material_id.value + suffix}; t.resource = resource((std::string(suffix) + ".bytes").c_str(), std::move(b), data::ScalarType::u8, 4, count);
      t.width = s.width; t.height = s.height; t.color_space = space;
      t.wrap_u = s.wrap_u; t.wrap_v = s.wrap_v; t.min_filter = s.min_filter; t.mag_filter = s.mag_filter;
      document.textures.push_back(t); return t.id;
    };
    material.base_color.texture = texture(".color", std::move(channels[0]), data::ColorSpace::srgb);
    material.metallic_roughness_texture = texture(".mr", std::move(channels[1]), data::ColorSpace::linear);
    material.normal_texture = texture(".normal", std::move(channels[2]), data::ColorSpace::linear);
    material.emissive_texture = texture(".emissive", std::move(channels[3]), data::ColorSpace::srgb);
    material.metallic = 1; material.roughness = 1; material.emissive = {1, 1, 1};
  }
  document.materials.push_back(material);
  usage.operations += count;
  checked(monitor.report({1, "surface.complete"}));
  auto result = services::finish_asset(job, std::move(document), o, monitor, usage, std::move(resources));
  if (!result.ok()) return SurfaceResult::failure(job, result.diagnostics());
  return SurfaceResult::success(job, {*result.value(), std::move(description)});
}
} // namespace detail

SurfaceResult bake_surface(const services::JobEnvelope &job, const SurfaceImage &image,
                           const services::EvaluationOptions &o, const services::JobControl &control) {
  return detail::boundary<SurfaceResult>(job, [&] {
    services::JobMonitor monitor(o.budget, control);
    detail::checked(monitor.checkpoint());
    detail::surface_preflight(image.settings, image.texels.size(), o, {{surface_bake_capability, 1}});
    return detail::finish_surface(job, image, o, monitor, {});
  });
}
} // namespace matter::asset
