#include "images.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <matter/asset/gltf.h>
#include <set>

namespace matter::asset {
namespace {
using namespace detail;
using V = data::Value;
using A = V::Array;
using O = V::Object;
V number(size_t n) { return V(static_cast<double>(n)); }
const O &object(const V &v) {
  auto p = std::get_if<O>(&v.data);
  need(p != nullptr, "glTF object required");
  return *p;
}
const A &array(const V &v) {
  auto p = std::get_if<A>(&v.data);
  need(p != nullptr, "glTF array required");
  return *p;
}
const V *find(const V &v, const char *key) {
  const auto &o = object(v);
  auto it = o.find(key);
  return it == o.end() ? nullptr : &it->second;
}
const V &get(const V &v, const char *key) {
  auto p = find(v, key);
  need(p != nullptr, std::string("missing glTF property: ") + key);
  return *p;
}
const A &list(const V &v, const char *key) {
  static const A empty;
  auto p = find(v, key);
  return p ? array(*p) : empty;
}
double real(const V &v) {
  auto p = std::get_if<double>(&v.data);
  need(p && std::isfinite(*p), "finite glTF number required");
  return *p;
}
float f32(const V &v) {
  double n = real(v);
  need(std::abs(n) <= std::numeric_limits<float>::max(), "glTF float overflow");
  return static_cast<float>(n);
}
size_t integer(const V &v) {
  double n = real(v);
  need(n >= 0 && n <= UINT32_MAX && std::floor(n) == n,
       "bounded glTF unsigned integer required");
  return static_cast<size_t>(n);
}
size_t index(const V &v, const char *key, size_t fallback = 0) {
  auto p = find(v, key);
  return p ? integer(*p) : fallback;
}
std::string string(const V &v) {
  auto p = std::get_if<std::string>(&v.data);
  need(p != nullptr, "glTF string required");
  return *p;
}
bool boolean(const V &v) {
  auto p = std::get_if<bool>(&v.data);
  need(p != nullptr, "glTF bool required");
  return *p;
}
const V &at(const A &a, size_t i) {
  need(i < a.size(), "glTF reference outside array");
  return a[i];
}
std::string id(const char *kind, size_t i) {
  return std::string("gltf.") + kind + "." + std::to_string(i);
}
void extensions(const V &v) {
  if (auto p = std::get_if<O>(&v.data)) {
    auto it = p->find("extensions");
    if (it != p->end())
      need(object(it->second).empty(), "glTF extension is unsupported",
           E::unsupported_capability);
    for (const auto &item : *p)
      if (item.first != "extras")
        extensions(item.second);
  } else if (auto values = std::get_if<A>(&v.data))
    for (const auto &item : *values)
      extensions(item);
}
data::Bounds bounds(const std::vector<data::Vec3> &positions) {
  need(!positions.empty(), "glTF mesh has no positions");
  data::Bounds b{positions[0], positions[0]};
  for (auto p : positions) {
    b.min.x = std::min(b.min.x, p.x);
    b.min.y = std::min(b.min.y, p.y);
    b.min.z = std::min(b.min.z, p.z);
    b.max.x = std::max(b.max.x, p.x);
    b.max.y = std::max(b.max.y, p.y);
    b.max.z = std::max(b.max.z, p.z);
  }
  return b;
}
bool unit(data::Vec3 n) {
  return std::abs(double(n.x) * n.x + double(n.y) * n.y + double(n.z) * n.z -
                  1) < 1e-4;
}
void supported(const services::AssetProduct &p) {
  const auto &d = p.document;
  need(d.attachments.empty() && d.lods.empty() && d.collisions.empty() &&
           d.rigs.empty() && d.skins.empty() && d.animations.empty() &&
           !d.terrain && d.extensions.empty(),
       "glTF profile supports static opaque meshes; optional products need "
       "another adapter",
       E::unsupported_capability);
  checked(services::require_capabilities(
      d.required_capabilities,
      {{"matter.asset.core", 1}, {"matter.material.pbr", 1}}));
  for (const auto &m : d.meshes) {
    need(m.uv_sets.size() <= 8, "glTF profile supports at most eight UV sets",
         E::unsupported_capability);
    for (auto n : m.normals)
      need(unit(n), "glTF normals must be unit length");
    for (auto t : m.tangents)
      need(unit({t.x, t.y, t.z}), "glTF tangent direction must be unit length");
  }
  auto texture = [&](const std::optional<data::SemanticId> &ref,
                     data::ColorSpace color) {
    if (!ref)
      return;
    auto it = std::find_if(d.textures.begin(), d.textures.end(),
                           [&](const auto &t) { return t.id == *ref; });
    need(it != d.textures.end() && it->color_space == color,
         "glTF texture color space disagrees with PBR channel",
         E::unsupported_capability);
  };
  for (const auto &m : d.materials) {
    texture(m.base_color.texture, data::ColorSpace::srgb);
    texture(m.emissive_texture, data::ColorSpace::srgb);
    texture(m.normal_texture, data::ColorSpace::linear);
    texture(m.metallic_roughness_texture, data::ColorSpace::linear);
    need(m.emissive.x <= 1 && m.emissive.y <= 1 && m.emissive.z <= 1,
         "glTF core emissive factor exceeds 1", E::unsupported_capability);
  }
}
struct Writer {
  Bytes binary;
  A views, accessors;
  const services::EvaluationOptions &options;
  services::JobMonitor &monitor;
  size_t bytes(const Bytes &b) {
    checked(monitor.checkpoint());
    need(binary.size() <= options.budget.output_bytes &&
             b.size() <= options.budget.output_bytes - binary.size() &&
             binary.size() + b.size() + 3 <= options.budget.working_bytes,
         "glTF buffer exceeds budget", E::resource_limit);
    while (binary.size() % 4)
      binary.push_back(0);
    size_t offset = binary.size();
    binary.insert(binary.end(), b.begin(), b.end());
    views.emplace_back(O{{"buffer", 0},
                         {"byteOffset", number(offset)},
                         {"byteLength", number(b.size())}});
    return views.size() - 1;
  }
  size_t accessor(const Bytes &b, size_t count, const char *type,
                  uint32_t component = 5126,
                  const data::Bounds *bound = nullptr) {
    auto v = bytes(b);
    O a{{"bufferView", number(v)},
        {"componentType", component},
        {"count", number(count)},
        {"type", type}};
    if (bound) {
      a["min"] = A{bound->min.x, bound->min.y, bound->min.z};
      a["max"] = A{bound->max.x, bound->max.y, bound->max.z};
    }
    accessors.emplace_back(std::move(a));
    return accessors.size() - 1;
  }
};
V make_gltf(const services::JobEnvelope &job, const services::AssetProduct &p,
            Writer &w, const services::EvaluationOptions &o,
            const services::JobControl &control) {
  const auto &d = p.document;
  std::map<std::string, size_t> textures, materials, meshes, assemblies;
  A images, samplers, tex, mat, mesh, nodes, roots;
  for (const auto &t : d.textures) {
    auto r = std::find_if(
        p.resources.resources.begin(), p.resources.resources.end(),
        [&](const auto &r) { return r.description.id == t.resource; });
    need(r != p.resources.resources.end() && bool(r->bytes),
         "glTF texture payload missing", E::missing_dependency);
    auto encoded = png(*r->bytes, t.width, t.height);
    images.emplace_back(O{{"name", t.id.value},
                          {"bufferView", number(w.bytes(encoded))},
                          {"mimeType", "image/png"}});
    auto wrap = [](data::WrapMode m) {
      return m == data::WrapMode::repeat   ? 10497
             : m == data::WrapMode::mirror ? 33648
                                           : 33071;
    };
    auto filter = [](data::FilterMode m) {
      return m == data::FilterMode::linear ? 9729 : 9728;
    };
    samplers.emplace_back(O{{"wrapS", wrap(t.wrap_u)},
                            {"wrapT", wrap(t.wrap_v)},
                            {"minFilter", filter(t.min_filter)},
                            {"magFilter", filter(t.mag_filter)}});
    textures[t.id.value] = tex.size();
    tex.emplace_back(O{{"name", t.id.value},
                       {"source", number(images.size() - 1)},
                       {"sampler", number(samplers.size() - 1)}});
  }
  auto ref = [&](const data::SemanticId &t, uint32_t uv = 0) {
    return V(O{{"index", number(textures.at(t.value))}, {"texCoord", uv}});
  };
  for (const auto &m : d.materials) {
    O pbr{{"baseColorFactor", A{m.base_color.factor.x, m.base_color.factor.y,
                                m.base_color.factor.z, m.base_color.factor.w}},
          {"metallicFactor", m.metallic},
          {"roughnessFactor", m.roughness}};
    if (m.base_color.texture)
      pbr["baseColorTexture"] = ref(*m.base_color.texture, m.base_color.uv_set);
    if (m.metallic_roughness_texture)
      pbr["metallicRoughnessTexture"] = ref(*m.metallic_roughness_texture);
    O value{{"name", m.id.value},
            {"pbrMetallicRoughness", std::move(pbr)},
            {"emissiveFactor", A{m.emissive.x, m.emissive.y, m.emissive.z}}};
    if (m.normal_texture) {
      auto n = ref(*m.normal_texture);
      std::get<O>(n.data)["scale"] = m.normal_scale;
      value["normalTexture"] = std::move(n);
    }
    if (m.emissive_texture)
      value["emissiveTexture"] = ref(*m.emissive_texture);
    materials[m.id.value] = mat.size();
    mat.emplace_back(std::move(value));
  }
  for (const auto &m : d.meshes) {
    checked(w.monitor.checkpoint());
    Bytes values;
    for (auto v : m.positions) {
      scalar(values, v.x);
      scalar(values, v.y);
      scalar(values, v.z);
    }
    O attributes{{"POSITION", number(w.accessor(values, m.positions.size(),
                                                "VEC3", 5126, &m.bounds))}};
    if (!m.normals.empty()) {
      values.clear();
      for (auto v : m.normals) {
        scalar(values, v.x);
        scalar(values, v.y);
        scalar(values, v.z);
      }
      attributes["NORMAL"] =
          number(w.accessor(values, m.normals.size(), "VEC3"));
    }
    if (!m.tangents.empty()) {
      values.clear();
      for (auto v : m.tangents) {
        scalar(values, v.x);
        scalar(values, v.y);
        scalar(values, v.z);
        scalar(values, v.w);
      }
      attributes["TANGENT"] =
          number(w.accessor(values, m.tangents.size(), "VEC4"));
    }
    for (size_t i = 0; i < m.uv_sets.size(); ++i) {
      values.clear();
      for (auto v : m.uv_sets[i]) {
        scalar(values, v.x);
        scalar(values, v.y);
      }
      attributes["TEXCOORD_" + std::to_string(i)] =
          number(w.accessor(values, m.uv_sets[i].size(), "VEC2"));
    }
    A primitives;
    for (const auto &s : m.submeshes) {
      values.clear();
      for (size_t i = s.first_index; i < size_t(s.first_index) + s.index_count;
           ++i)
        u32(values, m.indices[i]);
      primitives.emplace_back(
          O{{"attributes", attributes},
            {"indices",
             number(w.accessor(values, s.index_count, "SCALAR", 5125))},
            {"material", number(materials.at(s.material.value))},
            {"mode", 4}});
    }
    meshes[m.id.value] = mesh.size();
    mesh.emplace_back(
        O{{"name", m.id.value}, {"primitives", std::move(primitives)}});
  }
  for (size_t i = 0; i < d.assemblies.size(); ++i)
    assemblies[d.assemblies[i].id.value] = i;
  std::function<void(const std::vector<data::Instance> &, std::optional<size_t>,
                     uint32_t)>
      emit;
  emit = [&](const std::vector<data::Instance> &instances,
             std::optional<size_t> parent, uint32_t depth) {
    need(depth <= o.validation.limits.max_depth,
         "glTF assembly expansion depth exceeds budget", E::resource_limit);
    std::map<std::string, size_t> local;
    for (const auto &i : instances) {
      need(nodes.size() < o.validation.limits.max_instances,
           "glTF expanded instance count exceeds budget", E::resource_limit);
      A transform;
      for (size_t column = 0; column < 4; ++column)
        for (size_t row = 0; row < 4; ++row)
          transform.emplace_back(i.local_transform.m[row * 4 + column]);
      O node{{"name", i.id.value}, {"matrix", std::move(transform)}};
      if (i.target == data::InstanceTarget::mesh)
        node["mesh"] = number(meshes.at(i.prototype.value));
      local[i.id.value] = nodes.size();
      nodes.emplace_back(std::move(node));
    }
    for (const auto &i : instances) {
      checked(w.monitor.checkpoint());
      auto n = local.at(i.id.value);
      auto ancestor =
          i.parent ? std::optional<size_t>(local.at(i.parent->value)) : parent;
      if (ancestor) {
        auto &obj = std::get<O>(nodes[*ancestor].data);
        if (!obj.count("children"))
          obj["children"] = A{};
        std::get<A>(obj.at("children").data).push_back(number(n));
      } else
        roots.push_back(number(n));
      if (i.target == data::InstanceTarget::assembly)
        emit(d.assemblies[assemblies.at(i.prototype.value)].instances, n,
             depth + 1);
    }
  };
  if (d.instances.empty()) {
    std::vector<data::Instance> implicit;
    for (const auto &m : d.meshes)
      implicit.push_back(
          {{m.id.value}, m.id, data::InstanceTarget::mesh, {}, {}});
    emit(implicit, {}, 0);
  } else
    emit(d.instances, {}, 0);
  // glTF has shared mesh prototypes but no shared assembly prototype object.
  // A checksummed prepared product in extras retains that identity and all IDs.
  // Import verifies the entire ordinary glTF representation against this
  // product.
  auto archive = encode_prepared(job, p, o, control);
  need(archive.ok(), archive.ok() ? "" : archive.diagnostics().front().message,
       archive.ok() ? E::invalid_input : archive.diagnostics().front().code);
  auto archive_view = w.bytes(*archive.value());
  O root{
      {"asset",
       O{{"version", "2.0"}, {"generator", "Matter AssetPipeline v1"}}},
      {"buffers",
       A{O{{"byteLength", number(w.binary.size())}, {"uri", "asset.bin"}}}},
      {"bufferViews", std::move(w.views)},
      {"accessors", std::move(w.accessors)},
      {"meshes", std::move(mesh)},
      {"materials", std::move(mat)},
      {"nodes", std::move(nodes)},
      {"scenes", A{O{{"nodes", std::move(roots)}}}},
      {"scene", 0},
      {"extras", O{{"matterPrepared",
                    O{{"version", 1}, {"bufferView", number(archive_view)}}}}}};
  if (!tex.empty()) {
    root["textures"] = std::move(tex);
    root["images"] = std::move(images);
    root["samplers"] = std::move(samplers);
  }
  return root;
}
struct Reader {
  const V &root;
  const GltfDocument &input;
  const services::EvaluationOptions &options;
  struct View {
    const Bytes *bytes;
    size_t offset, length, stride;
  };
  View buffer_view(size_t i) const {
    const auto &v = at(list(root, "bufferViews"), i);
    auto buffer = index(v, "buffer");
    need(buffer < input.buffers.size(), "glTF buffer is unresolved",
         E::missing_dependency);
    const auto &b = input.buffers[buffer];
    auto offset = index(v, "byteOffset");
    auto length = integer(get(v, "byteLength"));
    auto declared =
        integer(get(at(list(root, "buffers"), buffer), "byteLength"));
    need(offset <= declared && length <= declared - offset &&
             declared <= b.size(),
         "glTF bufferView exceeds buffer");
    return {&b, offset, length, index(v, "byteStride")};
  }
  Bytes bytes(size_t i) const {
    auto v = buffer_view(i);
    need(v.stride == 0, "image/prepared bufferView cannot have byteStride");
    return Bytes(v.bytes->begin() + static_cast<ptrdiff_t>(v.offset),
                 v.bytes->begin() +
                     static_cast<ptrdiff_t>(v.offset + v.length));
  }
  std::vector<double> accessor(size_t i, const char *type,
                               bool indices = false) const {
    const auto &a = at(list(root, "accessors"), i);
    need(!find(a, "sparse"), "sparse glTF accessor is unsupported",
         E::unsupported_capability);
    need(!find(a, "normalized") || !boolean(get(a, "normalized")),
         "normalized glTF attributes are unsupported",
         E::unsupported_capability);
    need(string(get(a, "type")) == type, "glTF accessor type mismatch");
    auto component = integer(get(a, "componentType"));
    need(indices ? (component == 5121 || component == 5123 || component == 5125)
                 : component == 5126,
         "glTF accessor component type is unsupported",
         E::unsupported_capability);
    size_t scalar_bytes = component == 5121 ? 1 : component == 5123 ? 2 : 4;
    size_t components = std::string(type) == "SCALAR" ? 1
                        : std::string(type) == "VEC2" ? 2
                        : std::string(type) == "VEC3" ? 3
                                                      : 4;
    size_t count = integer(get(a, "count"));
    need(count > 0, "empty glTF accessor");
    need(count <= (indices ? options.validation.limits.max_indices
                           : options.validation.limits.max_vertices),
         "glTF accessor exceeds budget", E::resource_limit);
    auto v = buffer_view(integer(get(a, "bufferView")));
    auto offset = index(a, "byteOffset");
    size_t packed = scalar_bytes * components;
    size_t stride = v.stride ? v.stride : packed;
    need(!indices || !v.stride, "glTF index accessor cannot be interleaved");
    need(stride >= packed &&
             (!v.stride || (stride >= 4 && stride <= 252 && stride % 4 == 0)) &&
             offset % scalar_bytes == 0 &&
             (v.offset + offset) % scalar_bytes == 0 && offset <= v.length &&
             packed <= v.length - offset &&
             count - 1 <= (v.length - offset - packed) / stride,
         "glTF accessor range/alignment is invalid");
    need(count * components * sizeof(double) <= options.budget.working_bytes,
         "glTF accessor working memory exceeds budget", E::resource_limit);
    std::vector<double> result;
    result.reserve(count * components);
    for (size_t n = 0; n < count; ++n)
      for (size_t c = 0; c < components; ++c) {
        size_t p = v.offset + offset + n * stride + c * scalar_bytes;
        double value;
        if (component == 5126) {
          value = scalar(*v.bytes, p);
          need(std::isfinite(value), "nonfinite glTF attribute");
        } else if (scalar_bytes == 1)
          value = (*v.bytes)[p];
        else if (scalar_bytes == 2)
          value = uint32_t((*v.bytes)[p]) | (uint32_t((*v.bytes)[p + 1]) << 8);
        else
          value = read32(*v.bytes, p);
        result.push_back(value);
      }
    return result;
  }
};
data::Mat4 transform(const V &node) {
  data::Mat4 matrix;
  if (auto m = find(node, "matrix")) {
    need(!find(node, "translation") && !find(node, "rotation") &&
             !find(node, "scale"),
         "glTF node has both matrix and TRS");
    const auto &values = array(*m);
    need(values.size() == 16, "glTF matrix length mismatch");
    for (size_t row = 0; row < 4; ++row)
      for (size_t col = 0; col < 4; ++col)
        matrix.m[row * 4 + col] = f32(values[col * 4 + row]);
  } else {
    data::Vec3 t{}, s{1, 1, 1};
    data::Quat q{};
    auto vec3 = [](const V &v) {
      const auto &a = array(v);
      need(a.size() == 3, "glTF TRS length mismatch");
      return data::Vec3{f32(a[0]), f32(a[1]), f32(a[2])};
    };
    if (auto p = find(node, "translation"))
      t = vec3(*p);
    if (auto p = find(node, "scale"))
      s = vec3(*p);
    if (auto p = find(node, "rotation")) {
      const auto &a = array(*p);
      need(a.size() == 4, "glTF quaternion length mismatch");
      q = {f32(a[0]), f32(a[1]), f32(a[2]), f32(a[3])};
      need(std::abs(double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z +
                    double(q.w) * q.w - 1) < 1e-4,
           "glTF quaternion is not unit length");
    }
    matrix = mm::from_trs(t, q, s);
  }
  checked(data::validate_transform(matrix));
  return matrix;
}
services::AssetResult external(const services::JobEnvelope &job, const V &root,
                               const GltfDocument &input,
                               const services::EvaluationOptions &o,
                               services::JobMonitor &monitor) {
  Reader reader{root, input, o};
  data::AssetDocument doc;
  doc.id = {"gltf.asset"};
  doc.provenance.id = {"gltf.source"};
  doc.provenance.content = data::sha256(input.json);
  doc.provenance.uri = "resolved:gltf";
  doc.required_capabilities = {{"matter.asset.core", 1},
                               {"matter.material.pbr", 1}};
  services::ResolvedInputs resources;
  std::map<size_t,
           std::pair<data::ResourceDescription, std::pair<uint32_t, uint32_t>>>
      image_resources;
  std::map<std::pair<size_t, data::ColorSpace>, data::SemanticId> texture_ids;
  uint64_t payload_bytes = 0;
  auto texture = [&](const V &info, data::ColorSpace color,
                     bool uv0) -> data::SemanticId {
    if (uv0)
      need(index(info, "texCoord") == 0,
           "non-base-color glTF channel requires UV set 0",
           E::unsupported_capability);
    size_t i = integer(get(info, "index"));
    auto key = std::make_pair(i, color);
    auto existing = texture_ids.find(key);
    if (existing != texture_ids.end())
      return existing->second;
    const auto &t = at(list(root, "textures"), i);
    size_t image = integer(get(t, "source"));
    auto ir = image_resources.find(image);
    if (ir == image_resources.end()) {
      const auto &definition = at(list(root, "images"), image);
      Bytes encoded;
      if (auto uri = find(definition, "uri")) {
        auto it = input.images.find(string(*uri));
        need(it != input.images.end(), "glTF image URI is unresolved",
             E::missing_dependency);
        encoded = it->second;
        need(!find(definition, "bufferView"),
             "glTF image has both URI and bufferView");
      } else
        encoded = reader.bytes(integer(get(definition, "bufferView")));
      if (auto mime = find(definition, "mimeType"))
        need(string(*mime) == "image/png" || string(*mime) == "image/jpeg",
             "glTF image MIME type unsupported", E::unsupported_capability);
      uint32_t w = 0, h = 0;
      auto bytes = image_rgba(encoded, w, h,
                              std::min(o.validation.limits.max_resource_bytes,
                                       o.budget.output_bytes - payload_bytes));
      payload_bytes += bytes.size();
      data::ResourceDescription r{
          {id("image", image)}, data::sha256(view(bytes)),
          data::ScalarType::u8, 4,
          uint64_t(w) * h,      bytes.size()};
      doc.resources.push_back(r);
      resources.resources.push_back(
          {r, std::make_shared<const Bytes>(std::move(bytes))});
      ir = image_resources
               .emplace(image, std::make_pair(r, std::make_pair(w, h)))
               .first;
    }
    data::TextureDescription td;
    td.id = {id("texture", i) +
             (color == data::ColorSpace::srgb ? ".srgb" : ".linear")};
    td.resource = ir->second.first.id;
    td.width = ir->second.second.first;
    td.height = ir->second.second.second;
    td.color_space = color;
    if (auto s = find(t, "sampler")) {
      const auto &sampler = at(list(root, "samplers"), integer(*s));
      auto wrap = [](size_t n) {
        need(n == 10497 || n == 33071 || n == 33648, "invalid glTF wrap mode");
        return n == 10497   ? data::WrapMode::repeat
               : n == 33648 ? data::WrapMode::mirror
                            : data::WrapMode::clamp;
      };
      auto filter = [](size_t n) {
        need(n == 9728 || n == 9729,
             "mipmapped glTF sampler requires another adapter",
             E::unsupported_capability);
        return n == 9728 ? data::FilterMode::nearest : data::FilterMode::linear;
      };
      td.wrap_u = wrap(index(sampler, "wrapS", 10497));
      td.wrap_v = wrap(index(sampler, "wrapT", 10497));
      td.min_filter = filter(index(sampler, "minFilter", 9729));
      td.mag_filter = filter(index(sampler, "magFilter", 9729));
    }
    texture_ids[key] = td.id;
    doc.textures.push_back(td);
    return td.id;
  };
  for (const auto &material : list(root, "materials")) {
    checked(monitor.checkpoint());
    data::MaterialDescription m;
    m.id = {id("material", doc.materials.size())};
    m.metallic = 1;
    if (auto alpha = find(material, "alphaMode"))
      need(string(*alpha) == "OPAQUE",
           "glTF transparency requires another adapter",
           E::unsupported_capability);
    if (auto ds = find(material, "doubleSided"))
      need(!boolean(*ds), "double-sided glTF material requires another adapter",
           E::unsupported_capability);
    need(!find(material, "occlusionTexture"),
         "glTF occlusion texture requires another adapter",
         E::unsupported_capability);
    if (auto pbr = find(material, "pbrMetallicRoughness")) {
      if (auto v = find(*pbr, "baseColorFactor")) {
        const auto &a = array(*v);
        need(a.size() == 4, "baseColorFactor length mismatch");
        m.base_color.factor = {f32(a[0]), f32(a[1]), f32(a[2]), f32(a[3])};
      }
      if (auto v = find(*pbr, "metallicFactor"))
        m.metallic = f32(*v);
      if (auto v = find(*pbr, "roughnessFactor"))
        m.roughness = f32(*v);
      if (auto v = find(*pbr, "baseColorTexture")) {
        m.base_color.texture = texture(*v, data::ColorSpace::srgb, false);
        m.base_color.uv_set = static_cast<uint32_t>(index(*v, "texCoord"));
      }
      if (auto v = find(*pbr, "metallicRoughnessTexture"))
        m.metallic_roughness_texture =
            texture(*v, data::ColorSpace::linear, true);
    }
    if (auto v = find(material, "normalTexture")) {
      m.normal_texture = texture(*v, data::ColorSpace::linear, true);
      if (auto s = find(*v, "scale"))
        m.normal_scale = f32(*s);
    }
    if (auto v = find(material, "emissiveTexture"))
      m.emissive_texture = texture(*v, data::ColorSpace::srgb, true);
    if (auto v = find(material, "emissiveFactor")) {
      const auto &a = array(*v);
      need(a.size() == 3, "emissiveFactor length mismatch");
      m.emissive = {f32(a[0]), f32(a[1]), f32(a[2])};
      need(m.emissive.x <= 1 && m.emissive.y <= 1 && m.emissive.z <= 1,
           "glTF emissive factor exceeds 1");
    }
    doc.materials.push_back(m);
  }
  std::optional<data::SemanticId> default_material;
  uint64_t total_vertices = 0, total_indices = 0;
  for (const auto &mesh : list(root, "meshes")) {
    data::MeshPrototype m;
    m.id = {id("mesh", doc.meshes.size())};
    need(!find(mesh, "weights"), "glTF morph weights unsupported",
         E::unsupported_capability);
    const auto &primitives = array(get(mesh, "primitives"));
    need(!primitives.empty(), "empty glTF primitives");
    std::set<std::string> expected_attributes;
    for (const auto &primitive : primitives) {
      checked(monitor.checkpoint());
      need(index(primitive, "mode", 4) == 4,
           "only glTF triangle primitives are supported",
           E::unsupported_capability);
      need(!find(primitive, "targets"), "glTF morph targets unsupported",
           E::unsupported_capability);
      const auto &attributes = object(get(primitive, "attributes"));
      std::set<std::string> present;
      for (const auto &a : attributes) {
        need(a.first == "POSITION" || a.first == "NORMAL" ||
                 a.first == "TANGENT" || a.first.rfind("TEXCOORD_", 0) == 0,
             "glTF vertex attribute unsupported: " + a.first,
             E::unsupported_capability);
        present.insert(a.first);
      }
      if (expected_attributes.empty())
        expected_attributes = present;
      else
        need(present == expected_attributes,
             "glTF primitives have inconsistent attribute sets",
             E::unsupported_capability);
      auto position = attributes.find("POSITION");
      need(position != attributes.end(), "glTF POSITION missing");
      auto positions = reader.accessor(integer(position->second), "VEC3");
      size_t count = positions.size() / 3;
      size_t base = m.positions.size();
      need(count <= o.budget.vertices - total_vertices &&
               count <= o.validation.limits.max_vertices - total_vertices,
           "glTF vertex total exceeds budget", E::resource_limit);
      total_vertices += count;
      for (size_t i = 0; i < positions.size(); i += 3)
        m.positions.push_back({float(positions[i]), float(positions[i + 1]),
                               float(positions[i + 2])});
      auto attribute = [&](const char *name, const char *type,
                           size_t components) {
        auto it = attributes.find(name);
        if (it == attributes.end())
          return std::vector<double>{};
        auto v = reader.accessor(integer(it->second), type);
        need(v.size() / components == count, "glTF attribute count mismatch");
        return v;
      };
      auto normals = attribute("NORMAL", "VEC3", 3);
      for (size_t i = 0; i < normals.size(); i += 3) {
        data::Vec3 n{float(normals[i]), float(normals[i + 1]),
                     float(normals[i + 2])};
        need(unit(n), "glTF normal is not unit length");
        m.normals.push_back(n);
      }
      auto tangents = attribute("TANGENT", "VEC4", 4);
      for (size_t i = 0; i < tangents.size(); i += 4) {
        data::Vec4 t{float(tangents[i]), float(tangents[i + 1]),
                     float(tangents[i + 2]), float(tangents[i + 3])};
        need(unit({t.x, t.y, t.z}), "glTF tangent is not unit length");
        m.tangents.push_back(t);
      }
      size_t uv_count = 0;
      while (attributes.count("TEXCOORD_" + std::to_string(uv_count)))
        ++uv_count;
      need(uv_count <= 8, "glTF UV set count exceeds profile",
           E::resource_limit);
      for (const auto &a : attributes)
        if (a.first.rfind("TEXCOORD_", 0) == 0) {
          bool known = false;
          for (size_t i = 0; i < uv_count; ++i)
            known |= a.first == "TEXCOORD_" + std::to_string(i);
          need(known, "glTF UV sets must be contiguous",
               E::unsupported_capability);
        }
      m.uv_sets.resize(uv_count);
      for (size_t i = 0; i < uv_count; ++i) {
        auto name = "TEXCOORD_" + std::to_string(i);
        auto uv = attribute(name.c_str(), "VEC2", 2);
        for (size_t n = 0; n < uv.size(); n += 2)
          m.uv_sets[i].push_back({float(uv[n]), float(uv[n + 1])});
      }
      std::vector<double> indices;
      if (auto v = find(primitive, "indices"))
        indices = reader.accessor(integer(*v), "SCALAR", true);
      else {
        indices.reserve(count);
        for (size_t i = 0; i < count; ++i)
          indices.push_back(double(i));
      }
      need(indices.size() <= o.budget.indices - total_indices &&
               indices.size() <=
                   o.validation.limits.max_indices - total_indices,
           "glTF index total exceeds budget", E::resource_limit);
      total_indices += indices.size();
      need(indices.size() % 3 == 0, "glTF triangle index count mismatch");
      size_t first = m.indices.size();
      for (double i : indices) {
        need(i < count, "glTF index outside positions");
        need(base + size_t(i) <= UINT32_MAX, "glTF index overflow",
             E::resource_limit);
        m.indices.push_back(static_cast<uint32_t>(base + size_t(i)));
      }
      data::SemanticId material;
      if (auto v = find(primitive, "material")) {
        auto i = integer(*v);
        need(i < list(root, "materials").size(),
             "glTF material reference invalid");
        material = doc.materials[i].id;
      } else {
        if (!default_material) {
          data::MaterialDescription dm;
          dm.id = {"gltf.default.material"};
          dm.metallic = 1;
          doc.materials.push_back(dm);
          default_material = dm.id;
        }
        material = *default_material;
      }
      m.submeshes.push_back({static_cast<uint32_t>(first),
                             static_cast<uint32_t>(indices.size()), material});
    }
    m.bounds = bounds(m.positions);
    doc.meshes.push_back(std::move(m));
  }
  const auto &nodes = list(root, "nodes");
  need(nodes.size() <= o.validation.limits.max_instances,
       "glTF node count exceeds budget", E::resource_limit);
  std::vector<std::optional<size_t>> parents(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i)
    for (const auto &child : list(nodes[i], "children")) {
      auto j = integer(child);
      need(j < nodes.size() && !parents[j] && j != i,
           "invalid or shared glTF child node");
      parents[j] = i;
    }
  for (size_t i = 0; i < nodes.size(); ++i) {
    size_t p = i, depth = 0;
    while (parents[p]) {
      p = *parents[p];
      need(++depth <= o.validation.limits.max_depth,
           "glTF node cycle or depth overflow", E::cycle);
    }
  }
  const auto &scenes = list(root, "scenes");
  need(scenes.size() <= 1,
       "multiple glTF scenes require explicit scene selection",
       E::unsupported_capability);
  if (!scenes.empty()) {
    need(index(root, "scene") == 0, "glTF default scene reference invalid");
    std::set<size_t> scene_roots;
    for (const auto &n : list(scenes[0], "nodes")) {
      auto i = integer(n);
      need(i < nodes.size() && !parents[i] && scene_roots.insert(i).second,
           "invalid glTF scene root");
    }
    for (size_t i = 0; i < nodes.size(); ++i)
      need(parents[i] || scene_roots.count(i),
           "unreferenced glTF nodes require explicit selection",
           E::unsupported_capability);
  }
  for (size_t i = 0; i < nodes.size(); ++i) {
    const auto &n = nodes[i];
    need(!find(n, "skin") && !find(n, "camera") && !find(n, "weights"),
         "glTF node product unsupported", E::unsupported_capability);
    data::Instance instance;
    instance.id = {id("node", i)};
    instance.local_transform = transform(n);
    if (parents[i])
      instance.parent = data::SemanticId{id("node", *parents[i])};
    if (auto mesh = find(n, "mesh")) {
      auto j = integer(*mesh);
      need(j < doc.meshes.size(), "invalid glTF mesh reference");
      instance.prototype = doc.meshes[j].id;
    } else {
      data::AssemblyPrototype group;
      group.id = {id("group", i)};
      doc.assemblies.push_back(group);
      instance.prototype = group.id;
      instance.target = data::InstanceTarget::assembly;
    }
    doc.instances.push_back(instance);
  }
  // Asset bounds describe local prototype data in v1, independent of placement.
  if (!doc.meshes.empty()) {
    doc.bounds = doc.meshes[0].bounds;
    for (const auto &m : doc.meshes) {
      doc.bounds.min.x = std::min(doc.bounds.min.x, m.bounds.min.x);
      doc.bounds.min.y = std::min(doc.bounds.min.y, m.bounds.min.y);
      doc.bounds.min.z = std::min(doc.bounds.min.z, m.bounds.min.z);
      doc.bounds.max.x = std::max(doc.bounds.max.x, m.bounds.max.x);
      doc.bounds.max.y = std::max(doc.bounds.max.y, m.bounds.max.y);
      doc.bounds.max.z = std::max(doc.bounds.max.z, m.bounds.max.z);
    }
  }
  if (auto extras = find(root, "extras"))
    doc.metadata = O{{"gltf_extras", *extras}};
  services::ResourceUsage usage;
  usage.working_bytes = payload_bytes;
  return services::finish_asset(job, std::move(doc), o, monitor, usage,
                                std::move(resources));
}
} // namespace
std::vector<data::Capability> gltf_capabilities() {
  return {{"matter.asset.core", 1},
          {"matter.material.pbr", 1},
          {"matter.import.gltf.static", 1},
          {"matter.export.gltf.static", 1}};
}
GltfResult export_gltf(const services::JobEnvelope &job,
                       const services::AssetProduct &p,
                       const services::EvaluationOptions &o,
                       const services::JobControl &c) {
  return boundary<GltfResult>(job, [&] {
    services::JobMonitor monitor(o.budget, c);
    checked(monitor.checkpoint());
    checked(services::validate(p, o.validation));
    supported(p);
    Writer writer{{}, {}, {}, o, monitor};
    auto root = make_gltf(job, p, writer, o, c);
    std::string json;
    checked(data::canonical_json(root, json, o.validation.limits));
    services::ResourceUsage usage;
    usage.output_bytes = json.size() + writer.binary.size();
    usage.working_bytes = usage.output_bytes;
    checked(monitor.checkpoint(usage));
    return GltfResult::success(
        job, {std::move(json), {std::move(writer.binary)}, {}});
  });
}
BytesResult export_glb(const services::JobEnvelope &job,
                       const services::AssetProduct &p,
                       const services::EvaluationOptions &o,
                       const services::JobControl &c) {
  return boundary<BytesResult>(job, [&] {
    services::JobMonitor monitor(o.budget, c);
    checked(monitor.checkpoint());
    auto result = export_gltf(job, p, o, c);
    if (!result.ok())
      return BytesResult::failure(job, result.diagnostics());
    V root;
    checked(data::parse_json(result.value()->json, root, o.validation.limits));
    auto &buffers = std::get<A>(std::get<O>(root.data).at("buffers").data);
    std::get<O>(buffers[0].data).erase("uri");
    std::string json;
    checked(data::canonical_json(root, json, o.validation.limits));
    while (json.size() % 4)
      json.push_back(' ');
    auto bin = result.value()->buffers[0];
    while (bin.size() % 4)
      bin.push_back(0);
    Bytes out;
    u32(out, 0x46546c67);
    u32(out, 2);
    size32(out, 28 + json.size() + bin.size());
    size32(out, json.size());
    u32(out, 0x4e4f534a);
    out.insert(out.end(), json.begin(), json.end());
    size32(out, bin.size());
    u32(out, 0x004e4942);
    out.insert(out.end(), bin.begin(), bin.end());
    services::ResourceUsage usage;
    usage.output_bytes = out.size();
    usage.working_bytes = out.size();
    checked(monitor.checkpoint(usage));
    return BytesResult::success(job, std::move(out));
  });
}
services::AssetResult import_gltf(const services::JobEnvelope &job,
                                  const GltfDocument &input,
                                  const services::EvaluationOptions &o,
                                  const services::JobControl &c) {
  return boundary<services::AssetResult>(job, [&] {
    services::JobMonitor monitor(o.budget, c);
    checked(monitor.checkpoint());
    uint64_t total = input.json.size();
    need(total <= o.budget.working_bytes, "glTF input exceeds budget",
         E::resource_limit);
    for (const auto &b : input.buffers) {
      need(b.size() <= o.budget.working_bytes - total,
           "glTF buffers exceed budget", E::resource_limit);
      total += b.size();
    }
    for (const auto &b : input.images) {
      need(b.second.size() <= o.budget.working_bytes - total,
           "glTF images exceed budget", E::resource_limit);
      total += b.second.size();
    }
    V root;
    checked(data::parse_json(input.json, root, o.validation.limits));
    need(string(get(get(root, "asset"), "version")) == "2.0",
         "unsupported glTF version", E::unsupported_version);
    if (auto version = find(get(root, "asset"), "minVersion"))
      need(string(*version) == "2.0", "unsupported glTF minVersion",
           E::unsupported_version);
    need(list(root, "extensionsRequired").empty() &&
             list(root, "extensionsUsed").empty(),
         "glTF extension is unsupported", E::unsupported_capability);
    extensions(root);
    need(list(root, "animations").empty() && list(root, "skins").empty() &&
             list(root, "cameras").empty(),
         "glTF optional product unsupported", E::unsupported_capability);
    const auto &buffers = list(root, "buffers");
    need(buffers.size() == input.buffers.size(),
         "glTF resolved buffer count mismatch", E::missing_dependency);
    for (size_t i = 0; i < buffers.size(); ++i)
      need(integer(get(buffers[i], "byteLength")) == input.buffers[i].size(),
           "glTF resolved buffer length mismatch");
    if (auto extras = find(root, "extras"))
      if (std::holds_alternative<O>(extras->data))
        if (auto prepared = find(*extras, "matterPrepared")) {
          need(integer(get(*prepared, "version")) == 1,
               "unsupported Matter glTF preservation version",
               E::unsupported_version);
          Reader reader{root, input, o};
          auto archive = reader.bytes(integer(get(*prepared, "bufferView")));
          auto decoded = decode_prepared(job, archive, o, c);
          if (!decoded.ok())
            return decoded;
          auto expected = export_gltf(job, *decoded.value(), o, c);
          if (!expected.ok())
            return services::AssetResult::failure(job, expected.diagnostics());
          // GLB omits the URI; JSON glTF may choose another URI for the same
          // supplied bytes.
          auto &root_buffers =
              std::get<A>(std::get<O>(root.data).at("buffers").data);
          need(root_buffers.size() == 1,
               "preserved Matter glTF buffer count mismatch");
          std::get<O>(root_buffers[0].data)["uri"] = "asset.bin";
          std::string normalized;
          checked(data::canonical_json(root, normalized, o.validation.limits));
          need(normalized == expected.value()->json &&
                   input.buffers == expected.value()->buffers,
               "glTF scene differs from preserved Matter asset");
          checked(monitor.checkpoint());
          return decoded;
        }
    return external(job, root, input, o, monitor);
  });
}
services::AssetResult import_glb(const services::JobEnvelope &job,
                                 const Bytes &b,
                                 const services::EvaluationOptions &o,
                                 const services::JobControl &c) {
  return boundary<services::AssetResult>(job, [&] {
    services::JobMonitor monitor(o.budget, c);
    checked(monitor.checkpoint());
    need(b.size() <= o.budget.working_bytes, "GLB input exceeds budget",
         E::resource_limit);
    need(b.size() >= 20 && read32(b, 0) == 0x46546c67, "invalid GLB magic");
    need(read32(b, 4) == 2, "unsupported GLB version", E::unsupported_version);
    need(read32(b, 8) == b.size(), "GLB file length mismatch");
    auto json_length = read32(b, 12);
    need(json_length % 4 == 0 && read32(b, 16) == 0x4e4f534a &&
             json_length <= b.size() - 20,
         "invalid GLB JSON chunk");
    GltfDocument input;
    input.json = std::string(view(b).substr(20, json_length));
    size_t at = 20 + json_length;
    if (at < b.size()) {
      auto n = read32(b, at);
      need(n % 4 == 0 && read32(b, at + 4) == 0x004e4942 &&
               n <= b.size() - at - 8,
           "invalid GLB BIN chunk");
      at += 8;
      input.buffers.emplace_back(b.begin() + static_cast<ptrdiff_t>(at),
                                 b.begin() + static_cast<ptrdiff_t>(at + n));
      at += n;
    }
    need(at == b.size(), "unsupported extra GLB chunks",
         E::unsupported_capability);
    V root;
    checked(data::parse_json(input.json, root, o.validation.limits));
    const auto &buffers = list(root, "buffers");
    if (!input.buffers.empty()) {
      need(buffers.size() == 1 && !find(buffers[0], "uri"),
           "GLB must resolve its sole embedded buffer");
      size_t n = integer(get(buffers[0], "byteLength"));
      need(n <= input.buffers[0].size() && input.buffers[0].size() - n <= 3,
           "GLB buffer padding invalid");
      for (size_t i = n; i < input.buffers[0].size(); ++i)
        need(input.buffers[0][i] == 0, "GLB padding is nonzero");
      input.buffers[0].resize(n);
    }
    return import_gltf(job, input, o, c);
  });
}
} // namespace matter::asset
