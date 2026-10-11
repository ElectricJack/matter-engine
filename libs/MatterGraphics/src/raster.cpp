#include "raster_validation.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <matter/graphics/raster.h>
#include <set>
#include <tuple>

namespace matter::graphics {
namespace {
using E = data::ErrorCode;
data::ValidationResult fail(E code, const char *message) {
  return {{{data::Severity::error, code, message, {}, {}, {}}}};
}
bool finite(float f) { return std::isfinite(f) && std::abs(f) <= 1e12f; }
bool unit(float f) { return finite(f) && f >= 0 && f <= 1; }
bool finite(data::Vec3 v) { return finite(v.x) && finite(v.y) && finite(v.z); }
bool finite(data::Vec2 v) { return finite(v.x) && finite(v.y); }
bool finite(data::Vec4 v) { return finite(v.x) && finite(v.y) && finite(v.z) && finite(v.w); }
} // namespace
std::vector<data::Capability> raster_capabilities() {
  return {{"matter.graphics.gbuffer-producer", 1},
          {"matter.graphics.impostor-draw", 1},
          {"matter.graphics.raster-alpha-mask", 1}};
}
data::ValidationResult detail::validate_raster_products(const RasterInputs &in, RasterLimits limits,
                                                        bool require_placed_ids, uint64_t *input_bytes) {
  auto valid = services::require_capabilities(in.required_capabilities, raster_capabilities());
  if (!valid.ok())
    return valid;
  if (!in.width || !in.height || in.width > limits.max_dimension || in.height > limits.max_dimension ||
      in.draws.size() > limits.max_draws || in.draws.size() > UINT32_MAX / 5 || in.materials.empty() ||
      in.materials.size() > limits.max_materials || in.materials.size() >= 0x80000000ull ||
      in.textures.size() > limits.max_textures || in.textures.size() > INT32_MAX ||
      in.atlases.size() > limits.max_textures || in.atlases.size() > 65536)
    return fail(E::resource_limit, "Raster extent/draw/material/texture limit exceeded");
  data::Mat4 inverse;
  for (auto f : in.world_to_clip.m)
    if (!finite(f))
      return fail(E::invalid_input, "Invalid raster camera matrix");
  if (!mm::inverse(in.world_to_clip, inverse) || !finite(in.camera_eye))
    return fail(E::invalid_input, "Noninvertible/nonfinite raster camera");
  uint64_t remaining = limits.max_input_bytes;
  auto charge = [&](size_t n, size_t stride) {
    if (n > remaining / stride)
      return false;
    remaining -= n * stride;
    return true;
  };
  if (!charge(in.materials.size(), sizeof(RasterMaterial)) || !charge(in.draws.size(), sizeof(RasterDraw)) ||
      !charge(in.identities.size(), sizeof(RasterIdentity)) ||
      !charge(in.textures.size(), sizeof(RasterTexture)) || !charge(in.atlases.size(), sizeof(RasterAtlas)))
    return fail(E::resource_limit, "Raster input budget exceeded");
  for (const auto &t : in.textures) {
    const auto &d = t.description;
    if (!d.width || !d.height || d.width > limits.max_texture_dimension ||
        d.height > limits.max_texture_dimension || uint64_t(d.width) * d.height > UINT64_MAX / 4 ||
        uint64_t(d.width) * d.height * 4 != t.rgba8.size())
      return fail(E::invalid_input, "Raster texture dimensions/payload mismatch");
    if (d.pixel_format != "rgba8" ||
        (d.color_space != data::ColorSpace::linear && d.color_space != data::ColorSpace::srgb) ||
        d.wrap_u < data::WrapMode::repeat || d.wrap_u > data::WrapMode::mirror ||
        d.wrap_v < data::WrapMode::repeat || d.wrap_v > data::WrapMode::mirror ||
        d.min_filter < data::FilterMode::nearest || d.min_filter > data::FilterMode::linear ||
        d.mag_filter < data::FilterMode::nearest || d.mag_filter > data::FilterMode::linear)
      return fail(E::unsupported_capability, "Unsupported raster texture format/sampler");
    if (!charge(t.rgba8.size(), 1))
      return fail(E::resource_limit, "Raster texture budget exceeded");
  }
  for (const auto &a : in.atlases) {
    // Mips stop at four texels per view; powers of two keep every view isolated.
    if (a.edge < 32 || a.edge > limits.max_texture_dimension || (a.edge & (a.edge - 1)) ||
        uint64_t(a.edge) * a.edge > UINT64_MAX / 8 || uint64_t(a.edge) * a.edge * 8 != a.rgba8.size())
      return fail(E::invalid_input, "Invalid prepared impostor atlas");
    if (!charge(a.rgba8.size(), 1))
      return fail(E::resource_limit, "Impostor atlas budget exceeded");
  }
  // Reuse the R3 native material validator, including explicit flags policy.
  CompositionInputs table;
  table.materials.clear();
  for (const auto &m : in.materials) {
    table.materials.push_back(m.shading);
    if (!finite(m.normal_scale) || m.normal_scale < 0 || !unit(m.alpha_cutoff) ||
        !unit(m.shading.base_roughness[3]) || !unit(m.shading.metal_opacity_spec_coat[0]) ||
        !unit(m.shading.metal_opacity_spec_coat[1]) ||
        (m.alpha != RasterAlpha::opaque && m.alpha != RasterAlpha::mask))
      return fail(E::invalid_input, "Invalid raster material");
    for (size_t lane = 0; lane < 3; ++lane)
      if (!unit(m.shading.base_roughness[lane]) || m.shading.emission_strength[lane] < 0)
        return fail(E::invalid_input, "Invalid raster color/emission");
    if (m.shading.emission_strength[3] < 0)
      return fail(E::invalid_input, "Invalid emission strength");
    for (size_t i = 0; i < m.textures.size(); ++i) {
      const auto t = m.textures[i];
      if (t < -1 || (t >= 0 && size_t(t) >= in.textures.size()))
        return fail(E::missing_dependency, "Raster material texture is missing");
      if (t >= 0 && (i == 1 || i == 2) &&
          in.textures[size_t(t)].description.color_space != data::ColorSpace::linear)
        return fail(E::invalid_input, "Normal/metallic-roughness textures require linear color space");
    }
  }
  CompositionLimits cl;
  cl.max_materials = limits.max_materials;
  cl.max_input_bytes = limits.max_input_bytes;
  valid = validate_composition(table, cl);
  if (!valid.ok())
    return valid;
  std::set<const RasterGeometry *> checked;
  std::set<uint32_t> tokens;
  for (const auto &draw : in.draws) {
    if (draw.material >= in.materials.size() || !draw.identity || draw.identity == UINT32_MAX)
      return fail(E::invalid_input, "Invalid raster material/identity reference");
    tokens.insert(draw.identity);
    valid = data::validate_transform(draw.transform);
    if (!valid.ok())
      return valid;
    if (draw.impostor) {
      const auto &i = *draw.impostor;
      if (draw.geometry || i.atlas >= in.atlases.size() || !finite(i.center) || !finite(i.half_extent) ||
          i.half_extent <= 0)
        return fail(E::invalid_input, "Invalid impostor draw");
      // The legacy billboard frame assumes orthogonal scale/rotation columns.
      // Shear and reflection require an explicit representation extension.
      const auto &m = draw.transform.m;
      const data::Vec3 x{m[0], m[4], m[8]}, y{m[1], m[5], m[9]}, z{m[2], m[6], m[10]};
      const auto orthogonal = [](data::Vec3 a, data::Vec3 b) {
        return std::abs(double(mm::dot(a, b))) <= 1e-5 * std::sqrt(double(mm::dot(a, a)) * mm::dot(b, b));
      };
      if (!orthogonal(x, y) || !orthogonal(x, z) || !orthogonal(y, z) || mm::dot(mm::cross(x, y), z) <= 0)
        return fail(E::unsupported_capability,
                    "Impostor placement requires rotation and positive orthogonal scales");
      if (std::any_of(in.materials[draw.material].textures.begin(),
                      in.materials[draw.material].textures.end(), [](int32_t t) { return t >= 0; }))
        return fail(E::unsupported_capability,
                    "Impostors consume prepared atlas tint, not mesh texture bindings");
      continue;
    }
    if (!draw.geometry)
      return fail(E::missing_dependency, "Raster geometry is missing");
    const auto &g = *draw.geometry;
    if (!draw.index_count || draw.index_count % 3 || draw.first_index % 3 ||
        uint64_t(draw.first_index) + draw.index_count > g.indices.size())
      return fail(E::invalid_input, "Invalid raster index range");
    if (!checked.insert(&g).second)
      continue;
    if (g.vertices.empty() || g.vertices.size() > UINT32_MAX || g.indices.size() > UINT32_MAX ||
        !charge(g.vertices.size(), sizeof(RasterVertex)) || !charge(g.indices.size(), sizeof(uint32_t)))
      return fail(E::resource_limit, "Raster geometry budget exceeded");
    if (g.winding != data::Winding::counterclockwise && g.winding != data::Winding::clockwise)
      return fail(E::invalid_input, "Invalid raster winding");
    for (const auto &v : g.vertices)
      if (!finite(v.position) || !finite(v.normal) || !finite(v.tangent) || !finite(v.uv) ||
          !finite(v.base_uv) ||
          (mm::dot(data::Vec3{v.tangent.x, v.tangent.y, v.tangent.z},
                   data::Vec3{v.tangent.x, v.tangent.y, v.tangent.z}) > 0 &&
           v.tangent.w != 1 && v.tangent.w != -1))
        return fail(E::invalid_input, "Invalid raster vertex/frame");
    for (auto index : g.indices)
      if (index >= g.vertices.size())
        return fail(E::invalid_input, "Raster index out of range");
  }
  std::set<uint32_t> identity_tokens;
  for (const auto &id : in.identities) {
    if (!charge(id.instance.value.size(), 1) || !charge(id.path.size(), sizeof(data::SemanticId)))
      return fail(E::resource_limit, "Raster identity budget exceeded");
    for (const auto &part : id.path)
      if (!charge(part.value.size(), 1))
        return fail(E::resource_limit, "Raster identity path budget exceeded");
    if (!id.token || id.token == UINT32_MAX || (require_placed_ids && !tokens.count(id.token)) ||
        !identity_tokens.insert(id.token).second || id.instance.value.empty())
      return fail(E::invalid_input, "Invalid raster identity map");
  }
  if (input_bytes)
    *input_bytes = limits.max_input_bytes - remaining;
  return {};
}
data::ValidationResult validate_raster(const RasterInputs &in, RasterLimits limits) {
  return detail::validate_raster_products(in, limits, true);
}
data::ValidationResult raster_inputs(const SceneFrame &scene, uint32_t w, uint32_t h, RasterInputs &out,
                                     RasterLimits limits) {
  try {
    RasterInputs result;
    result.width = w;
    result.height = h;
    result.world_to_clip = mm::multiply(scene.camera.projection, scene.camera.view);
    data::Mat4 camera_world;
    if (!mm::inverse(scene.camera.view, camera_world))
      return fail(E::invalid_input, "Invalid scene camera");
    result.camera_eye = mm::transform_point(camera_world, {0, 0, 0});
    auto initial = validate_raster(result, limits);
    if (!initial.ok())
      return initial;
    if (scene.visible.size() > limits.max_draws)
      return fail(E::resource_limit, "Scene raster instance limit exceeded");
    uint64_t remaining = limits.max_input_bytes;
    auto charge = [&](size_t n, size_t stride) {
      if (n > remaining / stride)
        return false;
      remaining -= n * stride;
      return true;
    };
    if (!charge(1, sizeof(RasterMaterial)))
      return fail(E::resource_limit, "Scene raster material budget exceeded");
    struct AssetMap {
      std::map<std::string, uint32_t> materials;
    };
    std::map<const SceneAsset *, AssetMap> assets;
    std::map<std::tuple<const SceneAsset *, size_t, uint32_t>, std::shared_ptr<const RasterGeometry>>
        geometries;
    for (const auto &draw : scene.visible) {
      if (!draw.asset || !draw.asset->inputs.asset)
        return fail(E::invalid_input, "Scene draw has no immutable asset");
      const auto &a = *draw.asset;
      const auto &doc = *a.inputs.asset;
      if (draw.mesh >= doc.meshes.size())
        return fail(E::invalid_input, "Scene draw mesh is missing");
      if (!assets.count(&a)) {
        const std::vector<data::Capability> caps{{"matter.asset.core", 1}, {"matter.material.pbr", 1}};
        auto valid = services::require_capabilities(a.inputs.required_capabilities, caps);
        // The immutable SceneBackend snapshot already validated its resolved
        // inputs. Recheck portable geometry by reference; never copy the whole
        // AssetDocument just to validate it. Verify the bytes we consume below.
        if (valid.ok())
          valid = data::validate(doc, data::ValidationOptions{{}, caps});
        if (!valid.ok())
          return valid;
        auto &map = assets[&a];
        std::map<std::string, int32_t> textures;
        for (const auto &t : doc.textures) {
          const auto resource =
              std::find_if(a.inputs.resources.resources.begin(), a.inputs.resources.resources.end(),
                           [&](const auto &r) { return r.description.id == t.resource; });
          if (resource == a.inputs.resources.resources.end() || !resource->bytes)
            return fail(E::missing_dependency, "Scene texture bytes missing");
          if (result.textures.size() >= limits.max_textures || result.textures.size() >= INT32_MAX)
            return fail(E::resource_limit, "Scene raster texture limit exceeded");
          if (!charge(1, sizeof(RasterTexture)) || !charge(resource->bytes->size(), 1))
            return fail(E::resource_limit, "Scene raster texture byte budget exceeded");
          const auto expected = std::find_if(doc.resources.begin(), doc.resources.end(),
                                             [&](const auto &r) { return r.id == t.resource; });
          if (expected == doc.resources.end() || resource->bytes->size() != expected->byte_length ||
              data::sha256(std::string_view(reinterpret_cast<const char *>(resource->bytes->data()),
                                            resource->bytes->size()))
                      .hex != expected->content.hex)
            return fail(E::invalid_input, "Scene texture payload identity mismatch");
          textures.emplace(t.id.value, int32_t(result.textures.size()));
          result.textures.push_back({t, *resource->bytes});
        }
        for (const auto &m : doc.materials) {
          if (result.materials.size() >= limits.max_materials || !charge(1, sizeof(RasterMaterial)))
            return fail(E::resource_limit, "Scene raster material limit exceeded");
          RasterMaterial rm;
          auto flat = m;
          flat.base_color.texture.reset();
          flat.metallic_roughness_texture.reset();
          flat.normal_texture.reset();
          flat.emissive_texture.reset();
          valid = shading_material(flat, rm.shading);
          if (!valid.ok())
            return valid;
          rm.normal_scale = m.normal_scale;
          const std::optional<data::SemanticId> ids[] = {m.base_color.texture, m.metallic_roughness_texture,
                                                         m.normal_texture, m.emissive_texture};
          for (size_t i = 0; i < 4; ++i)
            if (ids[i])
              rm.textures[i] = textures.at(ids[i]->value);
          map.materials.emplace(m.id.value, uint32_t(result.materials.size()));
          result.materials.push_back(rm);
        }
      }
      const auto &mesh = doc.meshes[draw.mesh];
      if (result.identities.size() >= UINT32_MAX - 1)
        return fail(E::resource_limit, "Raster identity limit exceeded");
      const uint32_t token = uint32_t(result.identities.size() + 1);
      if (!charge(1, sizeof(RasterIdentity)) || !charge(draw.instance.value.size(), 1) ||
          !charge(draw.path.size(), sizeof(data::SemanticId)))
        return fail(E::resource_limit, "Scene raster identity budget exceeded");
      for (const auto &part : draw.path)
        if (!charge(part.value.size(), 1))
          return fail(E::resource_limit, "Scene raster path budget exceeded");
      result.identities.push_back({token, draw.instance, draw.path});
      for (const auto &sub : mesh.submeshes) {
        const auto mi = std::find_if(doc.materials.begin(), doc.materials.end(),
                                     [&](const auto &m) { return m.id == sub.material; });
        if (mi == doc.materials.end())
          return fail(E::missing_dependency, "Scene submesh material missing");
        const auto uv = mi->base_color.texture ? mi->base_color.uv_set : 0u;
        const auto key = std::make_tuple(&a, draw.mesh, uv);
        auto &geometry = geometries[key];
        if (!geometry) {
          if (!charge(mesh.positions.size(), sizeof(RasterVertex)) ||
              !charge(mesh.indices.size(), sizeof(uint32_t)))
            return fail(E::resource_limit, "Scene raster packed geometry budget exceeded");
          auto g = std::make_shared<RasterGeometry>();
          g->indices = mesh.indices;
          g->winding = mesh.winding;
          g->vertices.resize(mesh.positions.size());
          for (size_t i = 0; i < g->vertices.size(); ++i) {
            auto &v = g->vertices[i];
            v.position = mesh.positions[i];
            if (!mesh.normals.empty())
              v.normal = mesh.normals[i];
            if (!mesh.tangents.empty())
              v.tangent = mesh.tangents[i];
            if (!mesh.uv_sets.empty())
              v.uv = mesh.uv_sets[0][i];
            if (uv < mesh.uv_sets.size())
              v.base_uv = mesh.uv_sets[uv][i];
          }
          geometry = std::move(g);
        }
        if (result.draws.size() >= limits.max_draws || !charge(1, sizeof(RasterDraw)))
          return fail(E::resource_limit, "Scene raster draw limit exceeded");
        result.draws.push_back({geometry,
                                sub.first_index,
                                sub.index_count,
                                assets.at(&a).materials.at(sub.material.value),
                                token,
                                draw.world_transform,
                                {}});
      }
    }
    auto valid = validate_raster(result, limits);
    if (!valid.ok())
      return valid;
    out = std::move(result);
    return {};
  } catch (const std::exception &e) {
    return {{{data::Severity::error, E::backend_failure, e.what(), {}, {}, {}}}};
  }
}
void use_gbuffer(GbufferOutput g, CompositionInputs &out) {
  out.albedo = std::move(g.albedo);
  out.normal = std::move(g.normal);
  out.orm = std::move(g.orm);
  out.depth = std::move(g.depth);
  out.identity = std::move(g.identity);
  out.materials = std::move(g.materials);
  out.emission = std::move(g.emission);
}
} // namespace matter::graphics
