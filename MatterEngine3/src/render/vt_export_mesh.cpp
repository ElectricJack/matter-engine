#include "../asset_export.h"
#include "../part_surface.h"
#include "vt_chart_gpu.h"
#include "vt_export.h"
#include "vt_receiver_material.h"
#include <map>
#include <stdexcept>

namespace vt {
namespace {
using V3 = std::array<float, 3>;
V3 v3(const float *p) { return {p[0], p[1], p[2]}; }
V3 add(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 mul(V3 a, float f) { return {a[0] * f, a[1] * f, a[2] * f}; }
V3 sub(V3 a, V3 b) { return add(a, mul(b, -1)); }
float dot(V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(V3 a, V3 b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}
V3 unit(V3 a) {
  const float n = std::sqrt(dot(a, a));
  if (!(n > 1e-15f) || !std::isfinite(n))
    throw std::runtime_error(
        "export contains a degenerate normal or UV tangent");
  return mul(a, 1 / n);
}
V3 frame_t(V3 n) {
  V3 a = std::abs(n[0]) > .999f ? V3{0, 0, 1} : V3{1, 0, 0};
  return unit(sub(a, mul(n, dot(a, n))));
}
uint8_t byte(float f) {
  return uint8_t(std::lround(std::clamp(f, 0.f, 1.f) * 255));
}
float srgb(float f) {
  f = std::clamp(f, 0.f, 1.f);
  return f <= .0031308f ? 12.92f * f : 1.055f * std::pow(f, 1.f / 2.4f) - .055f;
}
struct Transform {
  V3 rows[3], normal_rows[3], translation;
  float determinant;
  explicit Transform(const float *m) {
    for (int i = 0; i < 16; ++i)
      if (!std::isfinite(m[i]))
        throw std::runtime_error("nonfinite asset transform");
    if (m[12] != 0 || m[13] != 0 || m[14] != 0 || m[15] != 1)
      throw std::runtime_error("asset transform must be affine");
    rows[0] = v3(m);
    rows[1] = v3(m + 4);
    rows[2] = v3(m + 8);
    translation = {m[3], m[7], m[11]};
    determinant = dot(rows[0], cross(rows[1], rows[2]));
    const float scale = std::sqrt(
        dot(rows[0], rows[0]) * dot(rows[1], rows[1]) * dot(rows[2], rows[2]));
    if (!(scale > 0) || !std::isfinite(scale) ||
        std::abs(determinant) < 1e-8f * scale)
      throw std::runtime_error("singular asset transform");
    normal_rows[0] = mul(cross(rows[1], rows[2]), 1 / determinant);
    normal_rows[1] = mul(cross(rows[2], rows[0]), 1 / determinant);
    normal_rows[2] = mul(cross(rows[0], rows[1]), 1 / determinant);
  }
  V3 point(V3 p) const {
    return add({dot(rows[0], p), dot(rows[1], p), dot(rows[2], p)},
               translation);
  }
  V3 normal(V3 n) const {
    return {dot(normal_rows[0], n), dot(normal_rows[1], n),
            dot(normal_rows[2], n)};
  }
};
struct Sample {
  float albedo[3]{}, normal[2]{}, orm[3]{}, height = 0;
};
Sample read(const VtExportAtlas &a, size_t i) {
  Sample s;
  for (int k = 0; k < 3; ++k) {
    s.albedo[k] = a.channels[0][i * 4 + k] / 255.f;
    s.orm[k] = a.channels[2][i * 4 + k] / 255.f;
  }
  for (int k = 0; k < 2; ++k)
    s.normal[k] = a.channels[1][i * 4 + k] / 255.f * 2 - 1;
  s.height =
      a.height_decode.min_m + a.heights[i] / 65535.f * a.height_decode.range_m;
  return s;
}
Sample mix(Sample a, Sample b, float t) {
  for (int k = 0; k < 3; ++k) {
    a.albedo[k] += (b.albedo[k] - a.albedo[k]) * t;
    a.orm[k] += (b.orm[k] - a.orm[k]) * t;
  }
  for (int k = 0; k < 2; ++k)
    a.normal[k] += (b.normal[k] - a.normal[k]) * t;
  a.height += (b.height - a.height) * t;
  return a;
}
Sample wrapped(const VtExportAtlas &a, float u, float v) {
  const float x = (u - std::floor(u)) * a.width - .5f,
              y = (v - std::floor(v)) * a.height - .5f;
  const int ix = int(std::floor(x)), iy = int(std::floor(y));
  const auto sample = [&](int dx, int dy) {
    const uint32_t sx = uint32_t((ix + dx + int(a.width)) % int(a.width)),
                   sy = uint32_t((iy + dy + int(a.height)) % int(a.height));
    return read(a, size_t(sy) * a.width + sx);
  };
  return mix(mix(sample(0, 0), sample(1, 0), x - ix),
             mix(sample(0, 1), sample(1, 1), x - ix), y - iy);
}
struct Mapping {
  bool active = false;
  uint32_t module = 0, lo = 0, hi = 0;
  float blend = 0;
  VtReceiverMaterialGpu gpu;
};
} // namespace

bool vt_export_mesh(matter::VulkanDevice &device,
                    std::shared_ptr<VtCompositor> compositor,
                    std::shared_ptr<const VtPartSnapshot> snapshot,
                    const part_surface::Prepared *source,
                    const float transform[16],
                    const std::vector<float> &baked_ao,
                    asset_export::Mesh &out_mesh,
                    asset_export::Material &out_material, std::string &error,
                    const std::vector<std::array<float, 2>> &clearcoat) {
  try {
    if (!snapshot || !snapshot->owns_context_inputs())
      throw std::runtime_error("missing export snapshot");
    const auto &geo = *snapshot->geometry;
    const auto &atlas = geo.atlas;
    const Transform xf(transform);
    std::vector<GpuChart> charts;
    std::vector<GpuTri> triangles;
    std::vector<VtTriangleCorners> corners;
    if (!vt_build_chart_gpu_streams(atlas, snapshot->context, charts, triangles,
                                    nullptr, 0, &corners) ||
        triangles.size() != snapshot->context.triangle_count ||
        triangles.size() > 4000000u / 3)
      throw std::runtime_error(
          "export charts must cover every triangle within the geometry limit");
    if (geo.surface_uvs.size() != geo.positions.size() / 3 * 2)
      throw std::runtime_error("asset has no complete surface UV stream");
    VtExportAtlas raw;
    if (!vt_export_atlas(device, compositor, snapshot, raw, error))
      return false;
    std::vector<Mapping> mappings(charts.size());
    std::map<std::pair<uint32_t, uint32_t>, VtExportAtlas> modules;
    uint64_t module_texels = 0;
    if (source && !source->modules.empty())
      for (uint32_t ci = 0; ci < charts.size(); ++ci) {
        const auto &chart = charts[ci];
        if (!chart.tri_range[1])
          continue;
        const auto &tri = triangles[chart.tri_range[0]];
        const auto p = v3(tri.p0), n = unit(v3(tri.n0));
        for (const auto &authored : source->material_mappings) {
          const auto &f = authored.frame;
          const V3 fn = {f.n.x, f.n.y, f.n.z},
                   origin = {f.origin_m.x, f.origin_m.y, f.origin_m.z};
          if (std::abs(dot(sub(p, origin), fn)) > .00005f ||
              dot(n, fn) < .9999f)
            continue;
          auto &mapping = mappings[ci];
          if (mapping.active || authored.module >= source->modules.size())
            throw std::runtime_error(
                "ambiguous or invalid export material module");
          const auto &module = source->modules[authored.module];
          const auto &domain = module->context.periodic;
          VtReceiverMaterialChart request;
          request.chart = ci;
          request.frame = f;
          request.phase = authored.phase;
          request.datum_m = authored.datum_m;
          request.u_range_m = authored.u_range_m;
          if (!vt_receiver_material_chart(*snapshot, domain, request,
                                          mapping.gpu, error))
            return false;
          mapping.active = true;
          mapping.module = authored.module;
          const float lod = std::clamp(mapping.gpu.uv_u[3], 0.f,
                                       float(vt_periodic_tail_mip(domain)));
          mapping.lo = uint32_t(std::floor(lod));
          mapping.hi = uint32_t(std::ceil(lod));
          mapping.blend = lod - mapping.lo;
          for (uint32_t level : {mapping.lo, mapping.hi}) {
            const auto key = std::make_pair(mapping.module, level);
            if (modules.count(key))
              continue;
            module_texels += uint64_t(std::max(domain.width >> level, 1u)) *
                             std::max(domain.height >> level, 1u);
            if (module_texels > 16u * 1024u * 1024u)
              throw std::runtime_error(
                  "export module scratch exceeds 16 million texels");
            VtExportAtlas pixels;
            if (!vt_export_atlas(device, compositor, module, pixels, error, {},
                                 level))
              return false;
            modules.emplace(key, std::move(pixels));
          }
        }
      }
    asset_export::Mesh mesh;
    mesh.source_hash = snapshot->context.variant_hash;
    mesh.vertices.resize(triangles.size() * 3);
    mesh.indices.resize(triangles.size() * 3);
    std::vector<uint32_t> tri_charts(triangles.size());
    for (uint32_t ci = 0; ci < charts.size(); ++ci)
      for (uint32_t ti = charts[ci].tri_range[0];
           ti < charts[ci].tri_range[0] + charts[ci].tri_range[1]; ++ti) {
        tri_charts[ti] = ci;
        const auto &tri = triangles[ti];
        const float *ps[] = {tri.p0, tri.p1, tri.p2};
        const float *ns[] = {tri.n0, tri.n1, tri.n2};
        for (int k = 0; k < 3; ++k) {
          auto &v = mesh.vertices[ti * 3 + k];
          v.position = xf.point(v3(ps[k]));
          v.normal = unit(xf.normal(unit(v3(ns[k]))));
          const auto corner = corners[ti][k];
          v.uv = {geo.surface_uvs[corner * 2], geo.surface_uvs[corner * 2 + 1]};
          mesh.indices[ti * 3 + k] = ti * 3 + k;
        }
        auto *v = &mesh.vertices[ti * 3];
        const auto e1 = sub(v[1].position, v[0].position),
                   e2 = sub(v[2].position, v[0].position);
        const float u1 = v[1].uv[0] - v[0].uv[0], u2 = v[2].uv[0] - v[0].uv[0],
                    v1 = v[1].uv[1] - v[0].uv[1], v2 = v[2].uv[1] - v[0].uv[1],
                    det = u1 * v2 - u2 * v1;
        if (std::abs(det) < 1e-20f || !std::isfinite(det))
          throw std::runtime_error("degenerate export UV triangle");
        const auto t = mul(sub(mul(e1, v2), mul(e2, v1)), 1 / det),
                   b = mul(sub(mul(e2, u1), mul(e1, u2)), 1 / det);
        for (int k = 0; k < 3; ++k) {
          const auto tv = unit(sub(t, mul(v[k].normal, dot(t, v[k].normal))));
          v[k].tangent = {tv[0], tv[1], tv[2],
                          dot(cross(v[k].normal, tv), b) < 0 ? -1.f : 1.f};
        }
        if (xf.determinant < 0)
          std::swap(mesh.indices[ti * 3 + 1], mesh.indices[ti * 3 + 2]);
      }
    asset_export::Material material;
    material.width = raw.width;
    material.height = raw.height;
    const size_t pixels = raw.heights.size();
    material.albedo.resize(pixels * 3);
    material.normal.resize(pixels * 3);
    material.orm.resize(pixels * 3);
    material.displacement.resize(pixels);
    const bool has_coat = std::any_of(clearcoat.begin(), clearcoat.end(),
                                      [](const auto &c) { return c[0] > 0; });
    if (has_coat)
      material.clearcoat.resize(pixels * 3);
    for (const auto &chart : atlas.charts) {
      if (material.source_density_min == 0)
        material.source_density_min = chart.texels_per_meter;
      material.source_density_min =
          std::min(material.source_density_min, chart.texels_per_meter);
      material.source_density_max =
          std::max(material.source_density_max, chart.texels_per_meter);
    }
    std::vector<float> heights(pixels);
    float minimum = 0, maximum = 0;
    for (size_t i = 0; i < pixels; ++i) {
      auto s = read(raw, i);
      if (has_coat) {
        const auto *aux = raw.channels[3].data() + i * 4;
        const uint32_t a = aux[0], b = aux[3] == 255 ? aux[1] : a;
        const float blend = aux[3] == 255 ? aux[2] / 255.f : 0.f;
        const std::array<float, 2> ca = a < clearcoat.size()
                                            ? clearcoat[a]
                                            : std::array<float, 2>{},
                                   cb = b < clearcoat.size()
                                            ? clearcoat[b]
                                            : std::array<float, 2>{};
        for (int k = 0; k < 2; ++k)
          material.clearcoat[i * 3 + k] = byte(ca[k] + (cb[k] - ca[k]) * blend);
      }
      const auto &point = raw.points[i];
      V3 encoded = {0, 0, 1};
      if (point.triangle != UINT32_MAX) {
        if (point.triangle >= triangles.size())
          throw std::runtime_error("invalid export surface correspondence");
        const uint32_t ti = point.triangle;
        const auto &tri = triangles[ti];
        const float *ns[] = {tri.n0, tri.n1, tri.n2};
        V3 n{}, world_n{}, t{};
        float tint[4]{}, ao = 0, uv[2]{};
        for (int k = 0; k < 3; ++k) {
          const float w = point.barycentric[k];
          if (!std::isfinite(w) || w < -.0001f || w > 1.0001f)
            throw std::runtime_error("invalid export barycentric coordinates");
          n = add(n, mul(v3(ns[k]), w));
          const auto &v = mesh.vertices[ti * 3 + k];
          world_n = add(world_n, mul(v.normal, w));
          t = add(t, mul(v3(v.tangent.data()), w));
          for (int a = 0; a < 2; ++a)
            uv[a] += v.uv[a] * w;
          const auto corner = corners[ti][k];
          if (geo.tint_rgba.size() == geo.positions.size() / 3 * 4)
            for (int a = 0; a < 4; ++a)
              tint[a] += geo.tint_rgba[corner * 4 + a] / 255.f * w;
          ao += (baked_ao.size() == geo.positions.size() / 3 ? baked_ao[corner]
                                                             : 1.f) *
                w;
        }
        n = unit(n);
        world_n = unit(world_n);
        t = unit(sub(t, mul(world_n, dot(t, world_n))));
        const auto &mapping = mappings[tri_charts[ti]];
        if (mapping.active) {
          const auto &g = mapping.gpu;
          const float u = g.uv_u[0] * uv[0] + g.uv_u[1] * uv[1] + g.uv_u[2],
                      v = g.uv_v[0] * uv[0] + g.uv_v[1] * uv[1] + g.uv_v[2];
          float lo, hi;
          std::memcpy(&lo, &g.binding[2], 4);
          std::memcpy(&hi, &g.binding[3], 4);
          if (u >= lo && u <= hi) {
            s = mix(wrapped(modules.at({mapping.module, mapping.lo}), u, v),
                    wrapped(modules.at({mapping.module, mapping.hi}), u, v),
                    mapping.blend);
            const float nx = s.normal[0], ny = s.normal[1];
            s.normal[0] = g.normal_xy[0] * nx + g.normal_xy[1] * ny;
            s.normal[1] = g.normal_xy[2] * nx + g.normal_xy[3] * ny;
            s.height += g.uv_v[3];
          }
        }
        const V3 nt = frame_t(n), nb = cross(n, nt);
        const float z = std::sqrt(std::max(0.f, 1 - s.normal[0] * s.normal[0] -
                                                    s.normal[1] * s.normal[1]));
        const auto world_detail = unit(xf.normal(
            add(add(mul(nt, s.normal[0]), mul(nb, s.normal[1])), mul(n, z))));
        const auto b = mul(cross(world_n, t), mesh.vertices[ti * 3].tangent[3]);
        encoded = unit({dot(world_detail, t), dot(world_detail, b),
                        dot(world_detail, world_n)});
        for (int k = 0; k < 3; ++k)
          s.albedo[k] *= 1 + (tint[k] - 1) * tint[3];
        s.orm[0] *= ao;
        const auto normal_xf = xf.normal(n);
        s.height /= std::sqrt(dot(normal_xf, normal_xf));
      }
      for (int k = 0; k < 3; ++k) {
        material.albedo[i * 3 + k] = byte(srgb(s.albedo[k]));
        material.normal[i * 3 + k] = byte(encoded[k] * .5f + .5f);
        material.orm[i * 3 + k] = byte(s.orm[k]);
      }
      if (!std::isfinite(s.height))
        throw std::runtime_error("nonfinite exported height");
      heights[i] = s.height;
      minimum = std::min(minimum, s.height);
      maximum = std::max(maximum, s.height);
    }
    material.height_min_m = minimum;
    material.height_range_m = maximum - minimum;
    for (size_t i = 0; i < pixels; ++i)
      material.displacement[i] =
          material.height_range_m > 0
              ? uint16_t(std::lround(
                    std::clamp((heights[i] - minimum) / material.height_range_m,
                               0.f, 1.f) *
                    65535))
              : 0;
    out_mesh = std::move(mesh);
    out_material = std::move(material);
    error.clear();
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }
}
} // namespace vt
