#include "internal.h"
#include <algorithm>
#include <functional>

namespace matter::contracts {
namespace {
using Index = std::map<std::string, size_t>;
using Graph = std::map<std::string, std::vector<std::string>>;
class Check {
public:
  const ValidationOptions &options;
  ValidationResult result;
  explicit Check(const ValidationOptions &o) : options(o) {}
  void add(ErrorCode code, const std::string &text,
           const std::string &id = {}) {
    // Zero is not permission to drop every failure.
    if (result.diagnostics.size() <
        std::max(1u, options.limits.max_diagnostics))
      result.diagnostics.push_back(detail::error(code, text, id));
  }
  void need(bool ok, const std::string &text, const std::string &id = {}) {
    if (!ok)
      add(ErrorCode::invalid_input, text, id);
  }
  void id(const SemanticId &x, const std::string &where) {
    need(detail::valid_id(x), where + ": invalid semantic ID", x.value);
  }
  void text(const std::string &s, const std::string &where) {
    need(detail::valid_utf8(s), where + ": invalid UTF-8");
    if (s.size() > options.limits.max_string_bytes)
      add(ErrorCode::resource_limit, where + ": string budget exceeded");
  }
  bool supported(const Capability &c) const {
    for (const auto &s : options.supported_capabilities)
      if (c.name == s.name && c.version == s.version)
        return true;
    return false;
  }
  bool declared(const std::vector<Capability> &caps, const std::string &name,
                uint32_t version = 1) const {
    for (const auto &c : caps)
      if (c.name == name && c.version == version)
        return true;
    return false;
  }
  void caps(const std::vector<Capability> &cs) {
    std::set<std::string> seen;
    for (const auto &c : cs) {
      id({c.name}, "capability");
      need(c.version > 0, "zero capability version");
      need(seen.insert(c.name).second, "duplicate capability " + c.name);
      if (!supported(c))
        add(ErrorCode::unsupported_capability,
            "unsupported required capability " + c.name + "@" +
                std::to_string(c.version));
    }
  }
  void version(SchemaVersion s) {
    if (s.major != 1 || s.minor != 0)
      add(ErrorCode::unsupported_version, "supported document schema is 1.0");
  }
  void source(const SourceIdentity &s) {
    id(s.id, "source");
    text(s.uri, "source URI");
    text(s.revision, "source revision");
    if (s.content)
      need(detail::valid_digest(*s.content), "invalid source SHA-256 digest");
    std::set<std::string> seen;
    for (const auto &h : s.legacy_hashes) {
      need(h.name_space == "matter.source.v1" ||
               h.name_space == "matter.resolved.v1",
           "unknown legacy hash namespace");
      need(seen.insert(h.name_space).second, "duplicate legacy hash namespace");
    }
  }
  template <class T>
  Index index(const std::vector<T> &list, const std::string &kind) {
    Index idx;
    for (size_t i = 0; i < list.size(); ++i) {
      id(list[i].id, kind);
      need(idx.emplace(list[i].id.value, i).second, "duplicate " + kind + " ID",
           list[i].id.value);
    }
    return idx;
  }
  void ref(const Index &index, const SemanticId &id, const std::string &where) {
    this->id(id, where);
    if (index.find(id.value) == index.end())
      add(ErrorCode::missing_dependency,
          where + ": missing reference " + id.value, id.value);
  }
  void transform(const Mat4 &m, const std::string &id, bool csg = false) {
    auto r = validate_transform(m, csg);
    for (auto &d : r.diagnostics) {
      add(d.code, d.message, id);
    }
  }
  void bounds(const Bounds &b, const std::string &id) {
    need(detail::finite(b.min) && detail::finite(b.max) && b.min.x <= b.max.x &&
             b.min.y <= b.max.y && b.min.z <= b.max.z,
         "invalid bounds", id);
  }
  void accumulate(uint64_t &sum, uint64_t amount, uint64_t max,
                  const std::string &what) {
    if (sum > max || amount > max - sum) {
      add(ErrorCode::resource_limit, what + " budget exceeded");
      sum = max;
    } else
      sum += amount;
  }
  void graph(const Graph &g) {
    std::map<std::string, uint8_t> color;
    std::vector<std::string> path;
    std::function<void(const std::string &, uint32_t)> visit =
        [&](const std::string &key, uint32_t depth) {
          if (depth > options.limits.max_depth) {
            add(ErrorCode::resource_limit, "dependency depth budget exceeded",
                key);
            return;
          }
          if (color[key] == 2)
            return;
          if (color[key] == 1) {
            add(ErrorCode::cycle, "cyclic reference", key);
            if (!result.diagnostics.empty() &&
                result.diagnostics.back().code == ErrorCode::cycle) {
              result.diagnostics.back().dependency_path = path;
              result.diagnostics.back().dependency_path.push_back(key);
            }
            return;
          }
          color[key] = 1;
          path.push_back(key);
          auto i = g.find(key);
          if (i != g.end())
            for (const auto &dep : i->second)
              if (g.count(dep))
                visit(dep, depth + 1);
          path.pop_back();
          color[key] = 2;
        };
    for (const auto &e : g)
      visit(e.first, 0);
    // Cached DFS visits alone cannot measure every route in a DAG. Compute
    // longest dependency paths after cycle rejection, without exponential
    // walks.
    if (!result.ok())
      return;
    std::map<std::string, uint32_t> height;
    std::function<uint32_t(const std::string &)> longest =
        [&](const std::string &key) {
          auto found = height.find(key);
          if (found != height.end())
            return found->second;
          uint32_t n = 0;
          for (const auto &dep : g.at(key))
            if (g.count(dep))
              n = std::max(n, longest(dep) + 1);
          height[key] = n;
          return n;
        };
    for (const auto &e : g)
      if (longest(e.first) > options.limits.max_depth)
        add(ErrorCode::resource_limit, "dependency depth budget exceeded",
            e.first);
  }
  Index resources(const std::vector<ResourceDescription> &rs) {
    auto idx = index(rs, "resource");
    uint64_t bytes = 0;
    for (const auto &r : rs) {
      need(detail::valid_digest(r.content), "invalid resource SHA-256 digest",
           r.id.value);
      need(r.components >= 1 && r.components <= 4,
           "resource components must be 1..4", r.id.value);
      uint64_t size = 0;
      switch (r.scalar_type) {
      case ScalarType::u8:
        size = 1;
        break;
      case ScalarType::u32:
      case ScalarType::f32:
        size = 4;
        break;
      default:
        need(false, "invalid scalar type");
      }
      const uint64_t stride = size * r.components;
      need(stride > 0 &&
               r.element_count <= UINT64_MAX / std::max(UINT64_C(1), stride),
           "overflowing resource length", r.id.value);
      if (stride && r.element_count <= UINT64_MAX / stride)
        need(r.byte_length == r.element_count * stride,
             "resource byte length does not match typed elements", r.id.value);
      accumulate(bytes, r.byte_length, options.limits.max_resource_bytes,
                 "resource bytes");
    }
    return idx;
  }
  Index materials(const std::vector<MaterialDescription> &ms,
                  const std::vector<TextureDescription> &ts,
                  const Index &resources,
                  const std::vector<ResourceDescription> &descriptions) {
    auto textures = index(ts, "texture");
    for (const auto &t : ts) {
      ref(resources, t.resource, "texture resource");
      need(t.width > 0 && t.height > 0 && t.pixel_format == "rgba8",
           "v1 texture requires positive dimensions and rgba8 format",
           t.id.value);
      auto resource = resources.find(t.resource.value);
      if (resource != resources.end()) {
        const auto &buffer = descriptions[resource->second];
        need(buffer.scalar_type == ScalarType::u8 && buffer.components == 4 &&
                 buffer.element_count == uint64_t(t.width) * t.height,
             "texture dimensions must match u8x4 resource", t.id.value);
      }
      need(t.color_space == ColorSpace::linear ||
               t.color_space == ColorSpace::srgb,
           "invalid color space");
      need(t.wrap_u == WrapMode::repeat || t.wrap_u == WrapMode::clamp ||
               t.wrap_u == WrapMode::mirror,
           "invalid wrap mode");
      need(t.wrap_v == WrapMode::repeat || t.wrap_v == WrapMode::clamp ||
               t.wrap_v == WrapMode::mirror,
           "invalid wrap mode");
      need(t.min_filter == FilterMode::nearest ||
               t.min_filter == FilterMode::linear,
           "invalid filter mode");
      need(t.mag_filter == FilterMode::nearest ||
               t.mag_filter == FilterMode::linear,
           "invalid filter mode");
    }
    auto idx = index(ms, "material");
    for (const auto &m : ms) {
      auto unit = [](float n) { return std::isfinite(n) && n >= 0 && n <= 1; };
      const auto &c = m.base_color.factor;
      need(unit(c.x) && unit(c.y) && unit(c.z) && c.w == 1,
           "opaque base color must be RGB in [0,1], alpha 1", m.id.value);
      need(unit(m.metallic) && unit(m.roughness),
           "metallic/roughness must be in [0,1]", m.id.value);
      need(std::isfinite(m.normal_scale) && m.normal_scale >= 0,
           "invalid normal scale", m.id.value);
      need(detail::finite(m.emissive) && m.emissive.x >= 0 &&
               m.emissive.y >= 0 && m.emissive.z >= 0,
           "invalid emissive", m.id.value);
      auto texture = [&](const std::optional<SemanticId> &t, bool linear) {
        if (t) {
          ref(textures, *t, "material texture");
          auto it = textures.find(t->value);
          if (linear && it != textures.end())
            need(ts[it->second].color_space == ColorSpace::linear,
                 "data texture requires linear color space", m.id.value);
        }
      };
      texture(m.base_color.texture, false);
      texture(m.metallic_roughness_texture, true);
      texture(m.normal_texture, true);
      texture(m.emissive_texture, false);
    }
    return idx;
  }
  void extensions(const std::vector<ExtensionEnvelope> &es,
                  const std::vector<Capability> &caps) {
    std::set<std::string> seen;
    for (const auto &e : es) {
      id({e.capability.name}, "extension");
      need(e.capability.version > 0, "zero extension version");
      need(seen.insert(e.capability.name).second,
           "duplicate extension envelope");
      if (e.required) {
        need(declared(caps, e.capability.name, e.capability.version),
             "semantic extension must declare required capability");
        if (!supported(e.capability))
          add(ErrorCode::unsupported_capability,
              "unsupported semantic extension " + e.capability.name);
      }
      value(e.payload, "extension payload");
    }
  }
  void value(const Value &v, const std::string &where) {
    auto r = validate_value(v, options.limits);
    for (const auto &d : r.diagnostics)
      add(d.code, where + ": " + d.message);
  }
  template <class Document> void final_value(const Document &doc) {
    if (!result.ok())
      return;
    try {
      value(to_value(doc), "document");
    } catch (const std::bad_alloc &) {
      add(ErrorCode::resource_limit, "validation allocation failed");
    } catch (const std::exception &e) {
      add(ErrorCode::invalid_input, e.what());
    }
  }
};
double determinant(const Mat4 &m) {
  return double(m.m[0]) * (double(m.m[5]) * m.m[10] - double(m.m[6]) * m.m[9]) -
         double(m.m[1]) * (double(m.m[4]) * m.m[10] - double(m.m[6]) * m.m[8]) +
         double(m.m[2]) * (double(m.m[4]) * m.m[9] - double(m.m[5]) * m.m[8]);
}
const Value *member(const Value &v, const char *key) {
  auto p = std::get_if<Value::Object>(&v.data);
  if (!p)
    return nullptr;
  auto i = p->find(key);
  return i == p->end() ? nullptr : &i->second;
}
bool number(const Value *v, double &n) {
  if (!v)
    return false;
  auto p = std::get_if<double>(&v->data);
  if (!p)
    return false;
  n = *p;
  return std::isfinite(n);
}
bool positive(const Value *v) {
  double n = 0;
  return number(v, n) && n > 0;
}
bool vector_positive(const Value *v, size_t size) {
  if (!v)
    return false;
  auto p = std::get_if<Value::Array>(&v->data);
  if (!p || p->size() != size)
    return false;
  for (const auto &e : *p)
    if (!positive(&e))
      return false;
  return true;
}
void operation_parameters(Check &c, const RecipeNode &n) {
  const auto id = n.id.value;
  c.need(std::holds_alternative<Value::Object>(n.parameters.data),
         "node parameters must be an object", id);
  std::set<std::string> allowed;
  if (n.operation == "primitive.box")
    allowed = {"size"};
  else if (n.operation == "primitive.sphere")
    allowed = {"radius", "segments"};
  else if (n.operation == "profile.polygon")
    allowed = {"points"};
  else if (n.operation == "profile.extrude")
    allowed = {"depth"};
  else if (n.operation == "solid.csg")
    allowed = {"operator"};
  else if (n.operation != "mesh.input" && n.operation != "placement.affine" &&
           n.operation != "assembly.reference")
    return;
  if (auto parameters = std::get_if<Value::Object>(&n.parameters.data))
    for (const auto &entry : *parameters)
      c.need(allowed.count(entry.first) != 0,
             "unknown core operation parameter", id);
  if (n.operation == "primitive.box")
    c.need(vector_positive(member(n.parameters, "size"), 3),
           "primitive.box needs positive size[3]", id);
  else if (n.operation == "primitive.sphere") {
    c.need(positive(member(n.parameters, "radius")),
           "primitive.sphere needs positive radius", id);
    double segments = 0;
    c.need(number(member(n.parameters, "segments"), segments) &&
               segments >= 3 && segments <= 65536 &&
               std::floor(segments) == segments,
           "sphere segments must be an integer in [3,65536]", id);
  } else if (n.operation == "profile.polygon") {
    const auto p = member(n.parameters, "points");
    auto a = p ? std::get_if<Value::Array>(&p->data) : nullptr;
    c.need(a && a->size() >= 3, "polygon needs at least three 2D points", id);
    if (a)
      for (const auto &e : *a) {
        auto xy = std::get_if<Value::Array>(&e.data);
        double x = 0, y = 0;
        c.need(xy && xy->size() == 2 && number(&(*xy)[0], x) &&
                   number(&(*xy)[1], y),
               "polygon point must be finite[2]", id);
      }
  } else if (n.operation == "profile.extrude")
    c.need(positive(member(n.parameters, "depth")),
           "extrude needs positive depth", id);
  else if (n.operation == "solid.csg") {
    const auto p = member(n.parameters, "operator");
    auto s = p ? std::get_if<std::string>(&p->data) : nullptr;
    c.need(s && (*s == "union" || *s == "subtract" || *s == "intersect"),
           "CSG operator must be union/subtract/intersect", id);
  }
}
void instances(Check &c, const std::vector<Instance> &xs, const Index &meshes,
               const Index &assemblies, Graph *assembly_graph = nullptr,
               const std::string &owner = {}) {
  auto idx = c.index(xs, "instance");
  Graph parents;
  for (const auto &x : xs) {
    c.transform(x.local_transform, x.id.value);
    parents[x.id.value];
    if (x.parent) {
      c.ref(idx, *x.parent, "instance parent");
      parents[x.id.value].push_back(x.parent->value);
      c.need(*x.parent != x.id, "self parent", x.id.value);
    }
    if (x.target == InstanceTarget::mesh)
      c.ref(meshes, x.prototype, "instance mesh");
    else if (x.target == InstanceTarget::assembly) {
      c.ref(assemblies, x.prototype, "instance assembly");
      if (assembly_graph)
        (*assembly_graph)[owner].push_back(x.prototype.value);
    } else
      c.need(false, "invalid instance target");
  }
  c.graph(parents);
}
} // namespace
ValidationResult validate_transform(const Mat4 &m, bool csg) {
  for (float f : m.m)
    if (!std::isfinite(f))
      return detail::failure(ErrorCode::invalid_input, "nonfinite transform");
  if (m.m[12] != 0 || m.m[13] != 0 || m.m[14] != 0 || m.m[15] != 1)
    return detail::failure(ErrorCode::invalid_input,
                           "transform must be affine (last row 0,0,0,1)");
  double length[3]{};
  double dot[3]{};
  for (int i = 0; i < 3; ++i)
    for (int row = 0; row < 3; ++row)
      length[i] += double(m.m[row * 4 + i]) * m.m[row * 4 + i];
  const double product = std::sqrt(length[0] * length[1] * length[2]),
               det = determinant(m);
  if (product == 0 || std::abs(det) <= product * 1e-12)
    return detail::failure(ErrorCode::invalid_input,
                           "singular or degenerate transform");
  if (csg) {
    for (int row = 0; row < 3; ++row) {
      dot[0] += double(m.m[row * 4]) * m.m[row * 4 + 1];
      dot[1] += double(m.m[row * 4]) * m.m[row * 4 + 2];
      dot[2] += double(m.m[row * 4 + 1]) * m.m[row * 4 + 2];
    }
    const double tolerance = *std::max_element(length, length + 3) * 1e-5;
    if (det < 0 || std::abs(length[0] - length[1]) > tolerance ||
        std::abs(length[0] - length[2]) > tolerance ||
        std::abs(dot[0]) > tolerance || std::abs(dot[1]) > tolerance ||
        std::abs(dot[2]) > tolerance)
      return detail::failure(ErrorCode::unsupported_capability,
                             "CSG v1 accepts rotation, translation and "
                             "positive uniform scale only");
  }
  return {};
}
bool reverses_winding(const Mat4 &m) { return determinant(m) < 0; }
std::optional<Vec3> transform_normal(const Mat4 &m, Vec3 n) {
  if (!validate_transform(m).ok() || !detail::finite(n))
    return std::nullopt;
  // Cofactor matrix divided by determinant = inverse transpose.
  double a = m.m[0], b = m.m[1], c = m.m[2], d = m.m[4], e = m.m[5], f = m.m[6],
         g = m.m[8], h = m.m[9], i = m.m[10];
  const double det = determinant(m);
  double x =
      ((e * i - f * h) * n.x + (f * g - d * i) * n.y + (d * h - e * g) * n.z) /
      det;
  double y =
      ((c * h - b * i) * n.x + (a * i - c * g) * n.y + (b * g - a * h) * n.z) /
      det;
  double z =
      ((b * f - c * e) * n.x + (c * d - a * f) * n.y + (a * e - b * d) * n.z) /
      det;
  double len = std::sqrt(x * x + y * y + z * z);
  if (len == 0 || !std::isfinite(len))
    return std::nullopt;
  return Vec3{static_cast<float>(x / len), static_cast<float>(y / len),
              static_cast<float>(z / len)};
}
ValidationResult validate(const RecipeDocument &doc,
                          const ValidationOptions &options) {
  Check c(options);
  c.version(doc.schema_version);
  c.caps(doc.required_capabilities);
  c.source(doc.source);
  c.need(c.declared(doc.required_capabilities, "matter.recipe.core"),
         "recipe requires matter.recipe.core@1");
  if (!doc.materials.empty() || !doc.textures.empty())
    c.need(c.declared(doc.required_capabilities, "matter.material.pbr"),
           "materials require matter.material.pbr@1");
  c.need(doc.rng_algorithm == "matter.splitmix64.v1", "unknown RNG algorithm");
  if (doc.nodes.size() > options.limits.max_nodes) {
    c.add(ErrorCode::resource_limit, "recipe node budget exceeded");
    return c.result;
  }
  auto resources = c.resources(doc.resources);
  auto materials =
      c.materials(doc.materials, doc.textures, resources, doc.resources);
  auto nodes = c.index(doc.nodes, "node");
  Index assets;
  for (size_t i = 0; i < doc.asset_dependencies.size(); ++i) {
    const auto &dependency = doc.asset_dependencies[i];
    c.id(dependency.id, "asset dependency");
    c.need(detail::valid_digest(dependency.content),
           "invalid dependency SHA-256 digest");
    c.version(dependency.schema_version);
    c.need(assets.emplace(dependency.id.value, i).second,
           "duplicate asset dependency");
  }
  Graph graph;
  for (const auto &n : doc.nodes) {
    const auto &id = n.id.value;
    c.text(n.operation, "operation");
    c.transform(n.transform, id);
    c.value(n.parameters, "node parameters");
    operation_parameters(c, n);
    graph[id];
    const bool builtin =
        n.operation == "primitive.box" || n.operation == "primitive.sphere" ||
        n.operation == "profile.polygon" || n.operation == "profile.extrude" ||
        n.operation == "solid.csg" || n.operation == "mesh.input" ||
        n.operation == "placement.affine" ||
        n.operation == "assembly.reference";
    if (builtin && n.operation_version != 1)
      c.add(ErrorCode::unsupported_version, "unsupported operation version",
            id);
    if (!builtin) {
      Capability cap{"matter.operation." + n.operation, n.operation_version};
      if (!c.declared(doc.required_capabilities, cap.name, cap.version) ||
          !c.supported(cap))
        c.add(ErrorCode::unsupported_capability,
              "unsupported operation " + n.operation, id);
    }
    c.need(n.operation_version > 0, "zero operation version", id);
    if (builtin) {
      const auto type = n.operation == "profile.polygon" ? ProductType::profile
                        : n.operation == "assembly.reference"
                            ? ProductType::assembly
                        : n.operation == "placement.affine" ? n.output_type
                                                            : ProductType::mesh;
      c.need(n.output_type == type, "operation output type mismatch", id);
      if (n.operation == "primitive.box" || n.operation == "primitive.sphere" ||
          n.operation == "profile.polygon")
        c.need(n.inputs.empty(), "primitive/profile takes no inputs", id);
      else if (n.operation == "solid.csg")
        c.need(n.inputs.size() >= 2,
               "ordered CSG needs at least two mesh inputs", id);
      else if (n.operation == "mesh.input")
        c.need(n.inputs.size() >= 2,
               "mesh.input needs positions and indices buffers", id);
      else
        c.need(n.inputs.size() == 1, "operation requires exactly one input",
               id);
    }
    for (const auto &input : n.inputs) {
      c.id({input.port}, "input port");
      switch (input.kind) {
      case InputKind::node: {
        c.ref(nodes, input.reference, "node input");
        graph[id].push_back(input.reference.value);
        auto i = nodes.find(input.reference.value);
        if (i != nodes.end())
          c.need(doc.nodes[i->second].output_type == input.type,
                 "typed node input mismatch", id);
        break;
      }
      case InputKind::resource:
        c.ref(resources, input.reference, "node resource");
        break;
      case InputKind::asset:
        c.ref(assets, input.reference, "node asset");
        break;
      default:
        c.need(false, "invalid input kind", id);
      }
      if (builtin) {
        if (n.operation == "mesh.input")
          c.need(input.kind == InputKind::resource &&
                     input.type == ProductType::mesh,
                 "mesh.input requires typed resource", id);
        else if (n.operation == "assembly.reference")
          c.need(input.kind == InputKind::asset &&
                     input.type == ProductType::assembly,
                 "assembly.reference requires assembly dependency", id);
        else
          c.need(input.kind == InputKind::node, "operation requires node input",
                 id);
        if (n.operation == "profile.extrude")
          c.need(input.type == ProductType::profile, "extrude requires profile",
                 id);
        if (n.operation == "solid.csg")
          c.need(input.type == ProductType::mesh, "CSG requires mesh", id);
        if (n.operation == "placement.affine")
          c.need(input.type == n.output_type &&
                     input.type != ProductType::profile,
                 "placement output/input types must match", id);
      }
    }
    for (const auto &m : n.materials)
      c.ref(materials, m, "node material");
    if (n.operation == "mesh.input") {
      std::map<std::string, const ResourceDescription *> buffers;
      for (const auto &input : n.inputs) {
        c.need(input.port == "positions" || input.port == "indices" ||
                   input.port == "normals" || input.port == "tangents" ||
                   input.port == "uv0",
               "unknown mesh buffer port", id);
        auto resource = resources.find(input.reference.value);
        if (resource != resources.end())
          c.need(buffers.emplace(input.port, &doc.resources[resource->second])
                     .second,
                 "duplicate mesh buffer port", id);
      }
      c.need(buffers.count("positions") && buffers.count("indices"),
             "mesh.input needs positions and indices buffers", id);
      uint64_t vertex_count = buffers.count("positions")
                                  ? buffers.at("positions")->element_count
                                  : 0;
      for (const auto &buffer : buffers) {
        const auto &description = *buffer.second;
        const bool index = buffer.first == "indices";
        const uint32_t components = index                        ? 1
                                    : buffer.first == "tangents" ? 4
                                    : buffer.first == "uv0"      ? 2
                                                                 : 3;
        c.need(description.scalar_type ==
                       (index ? ScalarType::u32 : ScalarType::f32) &&
                   description.components == components,
               "mesh buffer scalar/components mismatch", id);
        if (index) {
          c.need(description.element_count > 0 &&
                     description.element_count % 3 == 0,
                 "index buffer must contain complete triangles", id);
          if (description.element_count > options.limits.max_indices)
            c.add(ErrorCode::resource_limit, "mesh input index budget exceeded",
                  id);
        } else
          c.need(description.element_count == vertex_count && vertex_count > 0,
                 "mesh buffer vertex length mismatch", id);
      }
      if (vertex_count > options.limits.max_vertices)
        c.add(ErrorCode::resource_limit, "mesh input vertex budget exceeded",
              id);
      c.need(n.materials.size() == 1,
             "mesh.input v1 needs one whole-mesh material", id);
    }
    if (n.output_type != ProductType::mesh &&
        n.output_type != ProductType::profile &&
        n.output_type != ProductType::assembly)
      c.need(false, "invalid product type", id);
    if (n.operation != "placement.affine" && n.operation != "solid.csg") {
      Mat4 identity{};
      c.need(std::equal(n.transform.m, n.transform.m + 16, identity.m),
             "only placement.affine and solid.csg carry nonidentity transforms",
             id);
    }
  }
  c.graph(graph);
  // SDF CSG operands must obey the supported transform class throughout their
  // dependency subgraph. Visit each once per CSG node, with an explicit bound.
  if (c.result.ok())
    for (const auto &n : doc.nodes)
      if (n.operation == "solid.csg") {
        std::set<std::string> visited;
        std::vector<std::string> pending{n.id.value};
        while (!pending.empty()) {
          auto id = pending.back();
          pending.pop_back();
          if (!visited.insert(id).second)
            continue;
          const auto &child = doc.nodes[nodes.at(id)];
          c.transform(child.transform, n.id.value, true);
          for (const auto &dep : graph.at(id))
            pending.push_back(dep);
        }
      }
  c.need(!doc.outputs.empty(), "recipe requires outputs");
  std::set<std::string> outputs;
  for (const auto &id : doc.outputs) {
    c.ref(nodes, id, "recipe output");
    c.need(outputs.insert(id.value).second, "duplicate output");
  }
  for (const auto &entry : doc.source_map) {
    c.ref(nodes, {entry.first}, "source map");
    c.text(entry.second.uri, "source map URI");
  }
  c.need(std::holds_alternative<Value::Object>(doc.parameters.data),
         "recipe parameters must be object");
  c.value(doc.parameters, "recipe parameters");
  c.extensions(doc.extensions, doc.required_capabilities);
  c.value(doc.metadata, "metadata");
  c.final_value(doc);
  for (auto &d : c.result.diagnostics) {
    auto it = doc.source_map.find(d.node_id);
    if (it != doc.source_map.end())
      d.source = it->second;
  }
  return c.result;
}
ValidationResult validate(const AssetDocument &doc,
                          const ValidationOptions &options) {
  Check c(options);
  c.version(doc.schema_version);
  c.id(doc.id, "asset");
  c.source(doc.provenance);
  c.caps(doc.required_capabilities);
  c.bounds(doc.bounds, doc.id.value);
  c.need(c.declared(doc.required_capabilities, "matter.asset.core"),
         "asset requires matter.asset.core@1");
  if (!doc.materials.empty() || !doc.textures.empty())
    c.need(c.declared(doc.required_capabilities, "matter.material.pbr"),
           "materials require matter.material.pbr@1");
  if (doc.meshes.size() > options.limits.max_nodes ||
      doc.assemblies.size() > options.limits.max_nodes) {
    c.add(ErrorCode::resource_limit, "prototype budget exceeded");
    return c.result;
  }
  auto resources = c.resources(doc.resources);
  auto materials =
      c.materials(doc.materials, doc.textures, resources, doc.resources);
  auto meshes = c.index(doc.meshes, "mesh prototype");
  auto assemblies = c.index(doc.assemblies, "assembly prototype");
  for (const auto &a : assemblies)
    c.need(!meshes.count(a.first),
           "mesh and assembly prototype IDs must be disjoint", a.first);
  uint64_t vertices = 0, indices = 0, instance_count = 0;
  for (const auto &m : doc.meshes) {
    c.accumulate(vertices, m.positions.size(), options.limits.max_vertices,
                 "vertex");
    c.accumulate(indices, m.indices.size(), options.limits.max_indices,
                 "index");
  }
  c.accumulate(instance_count, doc.instances.size(),
               options.limits.max_instances, "instance");
  for (const auto &a : doc.assemblies)
    c.accumulate(instance_count, a.instances.size(),
                 options.limits.max_instances, "instance");
  if (!c.result.ok())
    return c.result;
  for (const auto &m : doc.meshes) {
    const auto &id = m.id.value;
    c.bounds(m.bounds, id);
    c.need(m.winding == Winding::counterclockwise,
           "v1 mesh winding must be CCW", id);
    c.need(!m.positions.empty(), "mesh positions cannot be empty", id);
    c.need(!m.indices.empty() && m.indices.size() % 3 == 0,
           "mesh indices must be complete triangles", id);
    c.need(m.normals.empty() || m.normals.size() == m.positions.size(),
           "normal length mismatch", id);
    c.need(m.tangents.empty() || m.tangents.size() == m.positions.size(),
           "tangent length mismatch", id);
    for (const auto &p : m.positions) {
      c.need(detail::finite(p), "nonfinite position", id);
      c.need(p.x >= m.bounds.min.x && p.x <= m.bounds.max.x &&
                 p.y >= m.bounds.min.y && p.y <= m.bounds.max.y &&
                 p.z >= m.bounds.min.z && p.z <= m.bounds.max.z,
             "position outside mesh bounds", id);
    }
    for (const auto &n : m.normals)
      c.need(detail::finite(n) && (double(n.x) * n.x + double(n.y) * n.y +
                                   double(n.z) * n.z) > 0,
             "nonfinite/zero normal", id);
    for (const auto &t : m.tangents)
      c.need(detail::finite(t) && (t.w == 1 || t.w == -1) &&
                 (double(t.x) * t.x + double(t.y) * t.y + double(t.z) * t.z) >
                     0,
             "invalid tangent/handedness", id);
    for (const auto &uv : m.uv_sets) {
      c.need(uv.size() == m.positions.size(), "UV length mismatch", id);
      for (const auto &p : uv)
        c.need(detail::finite(p), "nonfinite UV", id);
    }
    for (uint32_t index : m.indices)
      c.need(index < m.positions.size(), "index outside positions", id);
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
      auto a = m.indices[i], b = m.indices[i + 1], d = m.indices[i + 2];
      if (a < m.positions.size() && b < m.positions.size() &&
          d < m.positions.size()) {
        const auto &p = m.positions[a];
        const auto &q = m.positions[b];
        const auto &r = m.positions[d];
        const double ux = double(q.x) - p.x, uy = double(q.y) - p.y,
                     uz = double(q.z) - p.z, vx = double(r.x) - p.x,
                     vy = double(r.y) - p.y, vz = double(r.z) - p.z;
        const double x = uy * vz - uz * vy, y = uz * vx - ux * vz,
                     z = ux * vy - uy * vx;
        c.need(x * x + y * y + z * z > 0, "degenerate triangle", id);
      }
    }
    uint64_t end = 0;
    for (const auto &s : m.submeshes) {
      c.need(s.first_index == end && s.index_count > 0 &&
                 s.index_count % 3 == 0,
             "submeshes must partition triangles in order", id);
      end = uint64_t(s.first_index) + s.index_count;
      c.need(end <= m.indices.size(), "submesh length outside indices", id);
      c.ref(materials, s.material, "submesh material");
      auto material = materials.find(s.material.value);
      if (material != materials.end() &&
          doc.materials[material->second].base_color.texture)
        c.need(doc.materials[material->second].base_color.uv_set <
                   m.uv_sets.size(),
               "material UV set unavailable", id);
      if (material != materials.end()) {
        const auto &mat = doc.materials[material->second];
        if (mat.normal_texture || mat.metallic_roughness_texture ||
            mat.emissive_texture)
          c.need(!m.uv_sets.empty(), "data texture requires UV set 0", id);
      }
    }
    c.need(end == m.indices.size(), "submeshes must cover all indices", id);
  }
  Graph assembly_graph;
  for (const auto &a : doc.assemblies) {
    assembly_graph[a.id.value];
    instances(c, a.instances, meshes, assemblies, &assembly_graph, a.id.value);
  }
  c.graph(assembly_graph);
  instances(c, doc.instances, meshes, assemblies);
  auto root_instances = c.index(doc.instances, "root instance");
  auto product = [&](bool present, const char *capability) {
    if (present) {
      c.need(c.declared(doc.required_capabilities, capability),
             std::string("typed product must declare ") + capability + "@1");
      if (!c.supported({capability, 1}))
        c.add(ErrorCode::unsupported_capability,
              std::string("unsupported typed product ") + capability);
    }
  };
  product(!doc.attachments.empty(), "matter.asset.attachments");
  product(!doc.lods.empty(), "matter.asset.lod");
  product(!doc.collisions.empty(), "matter.asset.collision");
  product(!doc.rigs.empty() || !doc.skins.empty() || !doc.animations.empty(),
          "matter.asset.animation");
  product(doc.terrain.has_value(), "matter.asset.terrain");
  c.index(doc.attachments, "attachment");
  for (const auto &a : doc.attachments) {
    c.ref(root_instances, a.instance, "attachment instance");
    c.transform(a.local_transform, a.id.value);
  }
  std::set<std::string> lod_ids;
  for (const auto &l : doc.lods) {
    c.ref(meshes, l.prototype, "LOD prototype");
    c.need(lod_ids.insert(l.prototype.value).second, "duplicate LOD prototype");
    c.need(std::isfinite(l.max_error_metres) && l.max_error_metres >= 0,
           "invalid LOD error metric");
  }
  c.index(doc.collisions, "collision");
  for (const auto &x : doc.collisions) {
    c.ref(root_instances, x.instance, "collision instance");
    c.need(x.shape == "mesh" || x.shape == "convex_mesh" || x.shape == "box" ||
               x.shape == "capsule",
           "unsupported collision shape", x.id.value);
    if (x.shape == "mesh" || x.shape == "convex_mesh") {
      c.need(x.prototype.has_value(), "mesh collision requires prototype",
             x.id.value);
      if (x.prototype)
        c.ref(meshes, *x.prototype, "collision prototype");
    } else
      c.need(!x.prototype, "analytic collision cannot contain mesh prototype",
             x.id.value);
    c.need(detail::finite(x.size) && x.size.x > 0 && x.size.y > 0 &&
               x.size.z > 0,
           "collision size must be positive", x.id.value);
    if (x.shape == "capsule")
      c.need(x.size.x == x.size.z && x.size.y >= 2 * x.size.x,
             "capsule height must include two radii", x.id.value);
  }
  auto rigs = c.index(doc.rigs, "rig");
  for (const auto &rig : doc.rigs) {
    if (rig.joints.size() > options.limits.max_nodes) {
      c.add(ErrorCode::resource_limit, "joint budget exceeded", rig.id.value);
      continue;
    }
    c.need(!rig.joints.empty(), "rig needs joints", rig.id.value);
    auto joints = c.index(rig.joints, "joint");
    Graph graph;
    for (const auto &j : rig.joints) {
      graph[j.id.value];
      c.transform(j.inverse_bind, j.id.value);
      c.need(j.parent >= -1, "invalid joint parent", j.id.value);
      if (j.parent >= 0) {
        c.need(static_cast<size_t>(j.parent) < rig.joints.size(),
               "joint parent out of range", j.id.value);
        if (static_cast<size_t>(j.parent) < rig.joints.size())
          graph[j.id.value].push_back(
              rig.joints[static_cast<size_t>(j.parent)].id.value);
      }
    }
    c.graph(graph);
  }
  std::set<std::string> skin_ids;
  for (const auto &s : doc.skins) {
    c.ref(meshes, s.prototype, "skin prototype");
    c.ref(rigs, s.rig, "skin rig");
    c.need(skin_ids.insert(s.prototype.value).second,
           "duplicate skin prototype");
    auto mesh = meshes.find(s.prototype.value), rig = rigs.find(s.rig.value);
    c.need(s.joints.size() == s.weights.size(),
           "skin joint/weight length mismatch");
    if (mesh != meshes.end())
      c.need(s.weights.size() == doc.meshes[mesh->second].positions.size(),
             "skin vertex length mismatch");
    for (size_t i = 0; i < s.weights.size(); ++i) {
      auto w = s.weights[i];
      c.need(detail::finite(w) && w.x >= 0 && w.y >= 0 && w.z >= 0 &&
                 w.w >= 0 &&
                 std::abs(double(w.x) + w.y + w.z + w.w - 1) <= 1e-5,
             "invalid skin weights");
      if (i < s.joints.size() && rig != rigs.end())
        for (auto j : s.joints[i])
          c.need(j < doc.rigs[rig->second].joints.size(),
                 "skin joint index out of range");
    }
  }
  c.index(doc.animations, "animation clip");
  for (const auto &clip : doc.animations) {
    c.ref(rigs, clip.rig, "animation rig");
    c.need(std::isfinite(clip.duration_seconds) && clip.duration_seconds >= 0,
           "invalid clip duration", clip.id.value);
    auto rig = rigs.find(clip.rig.value);
    Index joints;
    if (rig != rigs.end())
      joints = c.index(doc.rigs[rig->second].joints, "joint");
    std::set<std::string> tracks;
    for (const auto &t : clip.tracks) {
      c.ref(joints, t.joint, "animation joint");
      c.need(tracks.insert(t.joint.value).second,
             "duplicate animation joint track");
      c.need(!t.keys.empty(), "animation track must have keys");
      double prior = -1;
      for (const auto &k : t.keys) {
        c.need(std::isfinite(k.time_seconds) && k.time_seconds >= 0 &&
                   k.time_seconds <= clip.duration_seconds &&
                   k.time_seconds > prior,
               "animation times must increase within duration");
        prior = k.time_seconds;
        c.need(detail::finite(k.translation) && detail::finite(k.rotation) &&
                   detail::finite(k.scale),
               "nonfinite animation key");
        const double q = double(k.rotation.x) * k.rotation.x +
                         double(k.rotation.y) * k.rotation.y +
                         double(k.rotation.z) * k.rotation.z +
                         double(k.rotation.w) * k.rotation.w;
        c.need(std::abs(q - 1) <= 1e-5,
               "animation rotation must be normalized");
        c.need(k.scale.x != 0 && k.scale.y != 0 && k.scale.z != 0,
               "animation scale cannot be singular");
      }
    }
  }
  if (doc.terrain) {
    const auto &t = *doc.terrain;
    c.id(t.region, "terrain region");
    for (auto n : t.origin_metres)
      c.need(std::isfinite(n), "nonfinite terrain origin");
    for (const auto &id : t.prototypes)
      c.ref(meshes, id, "terrain mesh");
    std::set<std::array<int32_t, 3>> neighbors;
    for (const auto &b : t.boundaries) {
      c.need(neighbors.insert(b.neighbor).second, "duplicate terrain neighbor");
      c.need(b.neighbor != t.region_key, "terrain neighbor cannot be self");
      c.id({b.policy}, "terrain boundary policy");
      c.need(std::isfinite(b.sample_spacing_metres) &&
                 b.sample_spacing_metres > 0,
             "invalid terrain sample spacing");
      if (b.seam_resource)
        c.ref(resources, *b.seam_resource, "terrain seam resource");
    }
  }
  c.extensions(doc.extensions, doc.required_capabilities);
  c.value(doc.metadata, "metadata");
  c.final_value(doc);
  return c.result;
}
} // namespace matter::contracts
