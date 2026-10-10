#include "internal.h"
#include <type_traits>

namespace matter::contracts {
namespace {
using O = Value::Object;
using A = Value::Array;
struct DecodeError : std::runtime_error {
  explicit DecodeError(std::string s) : std::runtime_error(std::move(s)) {}
};
[[noreturn]] void bad(const std::string &path, const char *message) {
  throw DecodeError(path + ": " + message);
}
template <class T> struct Fields;
#define OBJECT(TYPE, BODY)                                                     \
  template <> struct Fields<TYPE> {                                            \
    template <class Archive, class X> static void visit(Archive &a, X &x) {    \
      BODY                                                                     \
    }                                                                          \
  };
#define FIELD(NAME) a.field(#NAME, x.NAME);
OBJECT(SchemaVersion, FIELD(major) FIELD(minor))
OBJECT(ContentDigest, a.literal("algorithm", "sha256"); FIELD(hex))
OBJECT(LegacyHash, a.field("namespace", x.name_space); FIELD(value))
OBJECT(SourceLocation, FIELD(uri) FIELD(line) FIELD(column))
OBJECT(SourceIdentity,
       FIELD(id) FIELD(uri) FIELD(revision) FIELD(content) FIELD(legacy_hashes))
OBJECT(Capability, FIELD(name) FIELD(version))
OBJECT(ExtensionEnvelope, FIELD(capability) FIELD(required) FIELD(payload))
OBJECT(ResourceDescription,
       FIELD(id) FIELD(content) FIELD(scalar_type) FIELD(components)
           FIELD(element_count) FIELD(byte_length))
OBJECT(AssetDependency, FIELD(id) FIELD(content) FIELD(schema_version))
OBJECT(MaterialInput, FIELD(factor) FIELD(texture) FIELD(uv_set))
OBJECT(MaterialDescription,
       FIELD(id) FIELD(base_color) FIELD(metallic) FIELD(roughness)
           FIELD(normal_scale) FIELD(emissive) FIELD(metallic_roughness_texture)
               FIELD(normal_texture) FIELD(emissive_texture))
OBJECT(TextureDescription,
       FIELD(id) FIELD(resource) FIELD(width) FIELD(height) FIELD(pixel_format)
           FIELD(color_space) FIELD(wrap_u) FIELD(wrap_v) FIELD(min_filter)
               FIELD(mag_filter))
OBJECT(NodeInput, FIELD(port) FIELD(kind) FIELD(reference) FIELD(type))
OBJECT(RecipeNode,
       FIELD(id) FIELD(operation) FIELD(operation_version) FIELD(output_type)
           FIELD(inputs) FIELD(parameters) FIELD(transform) FIELD(materials))
OBJECT(RecipeDocument, a.literal("document_type", "recipe");
       a.literal("canonical_profile", canonical_profile);
       FIELD(schema_version) FIELD(required_capabilities) FIELD(source)
           FIELD(parameters) FIELD(seed) FIELD(rng_algorithm) FIELD(resources)
               FIELD(asset_dependencies) FIELD(materials) FIELD(textures)
                   FIELD(nodes) FIELD(outputs) FIELD(source_map)
                       FIELD(extensions) FIELD(metadata))
OBJECT(Bounds, FIELD(min) FIELD(max))
OBJECT(Submesh, FIELD(first_index) FIELD(index_count) FIELD(material))
OBJECT(MeshPrototype,
       FIELD(id) FIELD(positions) FIELD(normals) FIELD(tangents) FIELD(uv_sets)
           FIELD(indices) FIELD(submeshes) FIELD(bounds) FIELD(winding))
OBJECT(Instance, FIELD(id) FIELD(prototype) FIELD(target) FIELD(parent)
                     FIELD(local_transform))
OBJECT(AssemblyPrototype, FIELD(id) FIELD(instances))
OBJECT(Attachment, FIELD(id) FIELD(instance) FIELD(local_transform))
OBJECT(LodRepresentation, FIELD(prototype) FIELD(max_error_metres))
OBJECT(CollisionDescription,
       FIELD(id) FIELD(instance) FIELD(shape) FIELD(prototype) FIELD(size))
OBJECT(RigJoint, FIELD(id) FIELD(parent) FIELD(inverse_bind))
OBJECT(Rig, FIELD(id) FIELD(joints))
OBJECT(SkinWeights, FIELD(prototype) FIELD(rig) FIELD(joints) FIELD(weights))
OBJECT(AnimationKey,
       FIELD(time_seconds) FIELD(translation) FIELD(rotation) FIELD(scale))
OBJECT(AnimationTrack, FIELD(joint) FIELD(keys))
OBJECT(AnimationClip,
       FIELD(id) FIELD(rig) FIELD(duration_seconds) FIELD(tracks))
OBJECT(TerrainBoundary, FIELD(neighbor) FIELD(policy)
                            FIELD(sample_spacing_metres) FIELD(seam_resource))
OBJECT(TerrainProduct, FIELD(region) FIELD(region_key) FIELD(origin_metres)
                           FIELD(prototypes) FIELD(boundaries))
OBJECT(AssetDocument, a.literal("document_type", "asset");
       a.literal("canonical_profile", canonical_profile); FIELD(schema_version)
           FIELD(id) FIELD(provenance) FIELD(required_capabilities)
               FIELD(bounds) FIELD(resources) FIELD(materials) FIELD(textures)
                   FIELD(meshes) FIELD(assemblies) FIELD(instances)
                       FIELD(attachments) FIELD(lods) FIELD(collisions)
                           FIELD(rigs) FIELD(skins) FIELD(animations)
                               FIELD(terrain) FIELD(extensions) FIELD(metadata))
#undef FIELD
#undef OBJECT

template <class T> const std::vector<std::pair<T, const char *>> &enum_values();
#define ENUM(TYPE, ...)                                                        \
  template <>                                                                  \
  const std::vector<std::pair<TYPE, const char *>> &enum_values<TYPE>() {      \
    static const std::vector<std::pair<TYPE, const char *>> v{__VA_ARGS__};    \
    return v;                                                                  \
  }
ENUM(ScalarType, {ScalarType::u8, "u8"}, {ScalarType::u32, "u32"},
     {ScalarType::f32, "f32"})
ENUM(ColorSpace, {ColorSpace::linear, "linear"}, {ColorSpace::srgb, "srgb"})
ENUM(WrapMode, {WrapMode::repeat, "repeat"}, {WrapMode::clamp, "clamp"},
     {WrapMode::mirror, "mirror"})
ENUM(FilterMode, {FilterMode::nearest, "nearest"},
     {FilterMode::linear, "linear"})
ENUM(ProductType, {ProductType::mesh, "mesh"},
     {ProductType::profile, "profile"}, {ProductType::assembly, "assembly"})
ENUM(InputKind, {InputKind::node, "node"}, {InputKind::resource, "resource"},
     {InputKind::asset, "asset"})
ENUM(Winding, {Winding::counterclockwise, "ccw"}, {Winding::clockwise, "cw"})
ENUM(InstanceTarget, {InstanceTarget::mesh, "mesh"},
     {InstanceTarget::assembly, "assembly"})
#undef ENUM

template <class T> struct is_vector : std::false_type {};
template <class T, class Alloc>
struct is_vector<std::vector<T, Alloc>> : std::true_type {};
template <class T> struct is_array : std::false_type {};
template <class T, size_t N>
struct is_array<std::array<T, N>> : std::true_type {};
template <class T> struct is_optional : std::false_type {};
template <class T> struct is_optional<std::optional<T>> : std::true_type {};
template <class T> struct is_map : std::false_type {};
template <class V, class Compare, class Alloc>
struct is_map<std::map<std::string, V, Compare, Alloc>> : std::true_type {};

template <class T> Value write(const T &);
template <class T> void read(const Value &, T &, const std::string &);
struct WriteArchive {
  O object;
  template <class T> void field(const char *name, const T &x) {
    object.emplace(name, write(x));
  }
  void literal(const char *name, const char *text) {
    object.emplace(name, text);
  }
};
struct ReadArchive {
  const O &object;
  std::string path;
  std::set<std::string> consumed;
  const Value &get(const char *name) {
    auto it = object.find(name);
    if (it == object.end())
      bad(path + "." + name, "missing field");
    consumed.insert(name);
    return it->second;
  }
  template <class T> void field(const char *name, T &x) {
    read(get(name), x, path + "." + name);
  }
  void literal(const char *name, const char *text) {
    std::string value;
    field(name, value);
    if (value != text)
      bad(path + "." + name, "unsupported encoding or document type");
  }
  void finish() {
    if (consumed.size() != object.size())
      bad(path, "unknown field (use declared extensions)");
  }
};

template <class T> Value write(const T &x) {
  if constexpr (std::is_same_v<T, Value>)
    return x;
  else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, bool>)
    return Value(x);
  else if constexpr (std::is_same_v<T, SemanticId>)
    return x.value;
  else if constexpr (std::is_same_v<T, uint64_t>)
    return detail::hex64(x);
  else if constexpr (std::is_arithmetic_v<T>)
    return static_cast<double>(x);
  else if constexpr (std::is_enum_v<T>) {
    for (const auto &e : enum_values<T>())
      if (e.first == x)
        return e.second;
    throw DecodeError("invalid in-memory enum");
  } else if constexpr (is_optional<T>::value) {
    return x ? write(*x) : Value(nullptr);
  } else if constexpr (is_vector<T>::value || is_array<T>::value) {
    A a;
    a.reserve(x.size());
    for (const auto &e : x)
      a.push_back(write(e));
    return a;
  } else if constexpr (is_map<T>::value) {
    O o;
    for (const auto &e : x)
      o.emplace(e.first, write(e.second));
    return o;
  } else if constexpr (std::is_same_v<T, Vec2>)
    return A{x.x, x.y};
  else if constexpr (std::is_same_v<T, Vec3>)
    return A{x.x, x.y, x.z};
  else if constexpr (std::is_same_v<T, Vec4> || std::is_same_v<T, Quat>)
    return A{x.x, x.y, x.z, x.w};
  else if constexpr (std::is_same_v<T, Mat4>) {
    A a;
    for (float n : x.m)
      a.emplace_back(n);
    return a;
  } else {
    WriteArchive a;
    Fields<T>::visit(a, x);
    return a.object;
  }
}
template <class T> void read(const Value &v, T &x, const std::string &path) {
  if constexpr (std::is_same_v<T, Value>)
    x = v;
  else if constexpr (std::is_same_v<T, std::string>) {
    auto p = std::get_if<std::string>(&v.data);
    if (!p)
      bad(path, "expected string");
    x = *p;
  } else if constexpr (std::is_same_v<T, bool>) {
    auto p = std::get_if<bool>(&v.data);
    if (!p)
      bad(path, "expected boolean");
    x = *p;
  } else if constexpr (std::is_same_v<T, SemanticId>)
    read(v, x.value, path);
  else if constexpr (std::is_same_v<T, uint64_t>) {
    std::string text;
    read(v, text, path);
    if (text.size() != 16 ||
        text.find_first_not_of("0123456789abcdef") != std::string::npos)
      bad(path, "expected 16 lower-case hex digits");
    x = 0;
    for (char c : text)
      x = (x << 4) | static_cast<uint64_t>(c <= '9' ? c - '0' : c - 'a' + 10);
  } else if constexpr (std::is_arithmetic_v<T>) {
    auto p = std::get_if<double>(&v.data);
    if (!p || !std::isfinite(*p))
      bad(path, "expected finite number");
    if (*p < static_cast<double>(std::numeric_limits<T>::lowest()) ||
        *p > static_cast<double>(std::numeric_limits<T>::max()))
      bad(path, "number out of range");
    if constexpr (std::is_integral_v<T>)
      if (std::floor(*p) != *p)
        bad(path, "expected integer");
    x = static_cast<T>(*p);
  } else if constexpr (std::is_enum_v<T>) {
    std::string text;
    read(v, text, path);
    for (const auto &e : enum_values<T>())
      if (text == e.second) {
        x = e.first;
        return;
      }
    bad(path, "unknown enum value");
  } else if constexpr (is_optional<T>::value) {
    if (std::holds_alternative<std::nullptr_t>(v.data))
      x.reset();
    else {
      typename T::value_type candidate{};
      read(v, candidate, path);
      x = std::move(candidate);
    }
  } else if constexpr (is_vector<T>::value || is_array<T>::value) {
    auto p = std::get_if<A>(&v.data);
    if (!p)
      bad(path, "expected array");
    if constexpr (is_vector<T>::value)
      x.resize(p->size());
    else if (p->size() != x.size())
      bad(path, "invalid fixed array length");
    for (size_t i = 0; i < p->size(); ++i)
      read((*p)[i], x[i], path + "[" + std::to_string(i) + "]");
  } else if constexpr (is_map<T>::value) {
    auto p = std::get_if<O>(&v.data);
    if (!p)
      bad(path, "expected object");
    for (const auto &e : *p) {
      typename T::mapped_type item{};
      read(e.second, item, path + "." + e.first);
      x.emplace(e.first, std::move(item));
    }
  } else if constexpr (std::is_same_v<T, Vec2>) {
    std::array<float, 2> n{};
    read(v, n, path);
    x = {n[0], n[1]};
  } else if constexpr (std::is_same_v<T, Vec3>) {
    std::array<float, 3> n{};
    read(v, n, path);
    x = {n[0], n[1], n[2]};
  } else if constexpr (std::is_same_v<T, Vec4> || std::is_same_v<T, Quat>) {
    std::array<float, 4> n{};
    read(v, n, path);
    x = {n[0], n[1], n[2], n[3]};
  } else if constexpr (std::is_same_v<T, Mat4>) {
    std::array<float, 16> n{};
    read(v, n, path);
    for (size_t i = 0; i < 16; ++i)
      x.m[i] = n[i];
  } else {
    auto p = std::get_if<O>(&v.data);
    if (!p)
      bad(path, "expected object");
    ReadArchive a{*p, path, {}};
    Fields<T>::visit(a, x);
    a.finish();
  }
}
template <class Document>
ValidationResult decode(std::string_view bytes, Document &out,
                        const ValidationOptions &options) {
  Value value;
  auto r = parse_json(bytes, value, options.limits);
  if (!r.ok())
    return r;
  try {
    Document candidate;
    read(value, candidate, "$");
    r = validate(candidate, options);
    if (r.ok())
      out = std::move(candidate);
    return r;
  } catch (const DecodeError &e) {
    return detail::failure(ErrorCode::invalid_input, e.what());
  } catch (const std::bad_alloc &) {
    return detail::failure(ErrorCode::resource_limit,
                           "decode allocation failed");
  }
}
template <class Document>
ValidationResult encode_document(const Document &doc, std::string &out,
                                 const ValidationOptions &options) {
  auto r = validate(doc, options);
  if (!r.ok())
    return r;
  try {
    return canonical_json(write(doc), out, options.limits);
  } catch (const DecodeError &e) {
    return detail::failure(ErrorCode::invalid_input, e.what());
  } catch (const std::bad_alloc &) {
    return detail::failure(ErrorCode::resource_limit,
                           "encode allocation failed");
  }
}
} // namespace
Value to_value(const RecipeDocument &x) { return write(x); }
Value to_value(const AssetDocument &x) { return write(x); }
ValidationResult decode_recipe(std::string_view s, RecipeDocument &x,
                               const ValidationOptions &o) {
  return decode(s, x, o);
}
ValidationResult decode_asset(std::string_view s, AssetDocument &x,
                              const ValidationOptions &o) {
  return decode(s, x, o);
}
ValidationResult encode(const RecipeDocument &x, std::string &s,
                        const ValidationOptions &o) {
  return encode_document(x, s, o);
}
ValidationResult encode(const AssetDocument &x, std::string &s,
                        const ValidationOptions &o) {
  return encode_document(x, s, o);
}
} // namespace matter::contracts
