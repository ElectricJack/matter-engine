#pragma once

// Owned portable data. MathLib is the only dependency outside the standard
// library. Units: metres/seconds/radians; RH Y-up; CCW triangles; parent *
// local transforms.
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <matter_math.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace matter::contracts {
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559 &&
                  std::numeric_limits<float>::digits == 24,
              "v1 geometry requires IEEE float32");
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<double>::digits == 53,
              "v1 values require IEEE binary64");
using Vec2 = mm::Vec2;
using Vec3 = mm::Vec3;
using Vec4 = mm::Vec4;
using Quat = mm::Quat;
using Mat4 = mm::Mat4;

struct SchemaVersion {
  uint32_t major = 1, minor = 0;
};
inline constexpr const char *canonical_profile = "matter-exact-json-v1";
struct SemanticId {
  std::string value;
}; // Authored identity, never a digest.
inline bool operator==(const SemanticId &a, const SemanticId &b) {
  return a.value == b.value;
}
inline bool operator!=(const SemanticId &a, const SemanticId &b) {
  return !(a == b);
}
struct ContentDigest {
  std::string hex;
}; // Exactly 64 lower-case SHA-256 hex digits.
struct LegacyHash {
  std::string name_space;
  uint64_t value = 0;
}; // Adapter-owned namespace.
struct SourceLocation {
  std::string uri;
  uint32_t line = 0, column = 0;
};
struct SourceIdentity {
  SemanticId id;
  std::string uri, revision;
  std::optional<ContentDigest> content;
  std::vector<LegacyHash> legacy_hashes;
};

enum class Severity { info, warning, error };
enum class ErrorCode {
  invalid_input,
  unsupported_capability,
  unsupported_version,
  missing_dependency,
  cycle,
  resource_limit,
  cancelled,
  stale_result,
  backend_failure
};
const char *code_name(ErrorCode code);
struct Diagnostic {
  Severity severity = Severity::error;
  ErrorCode code = ErrorCode::invalid_input;
  std::string message, node_id;
  std::optional<SourceLocation> source;
  std::vector<std::string> dependency_path;
};

// Parameter JSON numbers are finite binary64. Full uint64 seeds/hashes use
// fixed hex strings.
struct Value {
  using Array = std::vector<Value>;
  using Object = std::map<std::string, Value, std::less<>>;
  std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data =
      nullptr;
  Value() = default;
  Value(std::nullptr_t) : data(nullptr) {}
  Value(bool v) : data(v) {}
  Value(double v) : data(v) {}
  Value(float v) : data(static_cast<double>(v)) {}
  Value(int v) : data(static_cast<double>(v)) {}
  Value(uint32_t v) : data(static_cast<double>(v)) {}
  Value(std::string v) : data(std::move(v)) {}
  Value(const char *v) : data(std::string(v)) {}
  Value(Array v) : data(std::move(v)) {}
  Value(Object v) : data(std::move(v)) {}
};
struct Capability {
  std::string name;
  uint32_t version = 1;
};
struct ExtensionEnvelope {
  Capability capability;
  bool required = true; // Unknown optional metadata can be retained; cannot
                        // change semantics.
  Value payload;
};
enum class ScalarType { u8, u32, f32 };
struct ResourceDescription {
  SemanticId id;
  ContentDigest content;
  ScalarType scalar_type = ScalarType::u8;
  uint32_t components = 1;
  uint64_t element_count = 0, byte_length = 0;
};
struct AssetDependency {
  SemanticId id;
  ContentDigest content;
  SchemaVersion schema_version;
};

struct MaterialInput {
  Vec4 factor{1, 1, 1, 1};
  std::optional<SemanticId> texture;
  uint32_t uv_set = 0;
};
struct MaterialDescription {
  SemanticId id;
  MaterialInput base_color;
  float metallic = 0, roughness = 1, normal_scale = 1;
  Vec3 emissive{};
  std::optional<SemanticId> metallic_roughness_texture, normal_texture,
      emissive_texture;
};
enum class ColorSpace { linear, srgb };
enum class WrapMode { repeat, clamp, mirror };
enum class FilterMode { nearest, linear };
struct TextureDescription {
  SemanticId id, resource;
  uint32_t width = 1, height = 1;
  std::string pixel_format = "rgba8";
  ColorSpace color_space = ColorSpace::linear;
  WrapMode wrap_u = WrapMode::repeat, wrap_v = WrapMode::repeat;
  FilterMode min_filter = FilterMode::linear, mag_filter = FilterMode::linear;
};

enum class ProductType { mesh, profile, assembly };
enum class InputKind { node, resource, asset };
struct NodeInput {
  std::string port;
  InputKind kind = InputKind::node;
  SemanticId reference;
  ProductType type = ProductType::mesh;
};
struct RecipeNode {
  SemanticId id;
  std::string operation;
  uint32_t operation_version = 1;
  ProductType output_type = ProductType::mesh;
  std::vector<NodeInput> inputs; // Ordered: solid.csg is a left fold.
  Value parameters{Value::Object{}};
  Mat4 transform{};
  std::vector<SemanticId> materials;
};
struct RecipeDocument {
  SchemaVersion schema_version;
  std::vector<Capability> required_capabilities;
  SourceIdentity source;
  Value parameters{Value::Object{}};
  uint64_t seed = 0;
  std::string rng_algorithm = "matter.splitmix64.v1";
  std::vector<ResourceDescription> resources;
  std::vector<AssetDependency> asset_dependencies;
  std::vector<MaterialDescription> materials;
  std::vector<TextureDescription> textures;
  std::vector<RecipeNode>
      nodes; // Preserved, never sorted or topologically reordered by codec.
  std::vector<SemanticId> outputs;
  std::map<std::string, SourceLocation> source_map;
  std::vector<ExtensionEnvelope> extensions;
  Value metadata{
      Value::Object{}}; // Nonsemantic, excluded from content identity.
};

struct Bounds {
  Vec3 min{}, max{};
};
enum class Winding { counterclockwise, clockwise };
struct Submesh {
  uint32_t first_index = 0, index_count = 0;
  SemanticId material;
};
struct MeshPrototype {
  SemanticId id;
  std::vector<Vec3> positions, normals;
  std::vector<Vec4> tangents;
  std::vector<std::vector<Vec2>> uv_sets;
  std::vector<uint32_t> indices;
  std::vector<Submesh> submeshes;
  Bounds bounds;
  Winding winding = Winding::counterclockwise;
};
enum class InstanceTarget { mesh, assembly };
struct Instance {
  SemanticId id, prototype;
  InstanceTarget target = InstanceTarget::mesh;
  std::optional<SemanticId> parent;
  Mat4 local_transform{};
};
struct AssemblyPrototype {
  SemanticId id;
  std::vector<Instance> instances;
};
struct Attachment {
  SemanticId id, instance;
  Mat4 local_transform{};
};
struct LodRepresentation {
  SemanticId prototype;
  float max_error_metres = 0;
};
struct CollisionDescription {
  SemanticId id, instance;
  std::string shape = "mesh"; // mesh, convex_mesh, box, capsule
  std::optional<SemanticId> prototype;
  Vec3 size{1, 1, 1}; // box extents; capsule uses x=radius, y=height, z=radius.
};
struct RigJoint {
  SemanticId id;
  int32_t parent = -1;
  Mat4 inverse_bind{};
};
struct Rig {
  SemanticId id;
  std::vector<RigJoint> joints;
};
struct SkinWeights {
  SemanticId prototype, rig;
  std::vector<std::array<uint32_t, 4>> joints;
  std::vector<Vec4> weights;
};
struct AnimationKey {
  float time_seconds = 0;
  Vec3 translation{};
  Quat rotation{};
  Vec3 scale{1, 1, 1};
};
struct AnimationTrack {
  SemanticId joint;
  std::vector<AnimationKey> keys;
};
struct AnimationClip {
  SemanticId id, rig;
  float duration_seconds = 0;
  std::vector<AnimationTrack> tracks;
};
struct TerrainBoundary {
  std::array<int32_t, 3> neighbor{};
  std::string
      policy; // Algorithm-specific policy identified by terrain capability.
  float sample_spacing_metres = 1;
  std::optional<SemanticId> seam_resource;
};
struct TerrainProduct {
  SemanticId region;
  std::array<int32_t, 3> region_key{};
  std::array<double, 3>
      origin_metres{}; // Explicit high-precision origin, local float32 mesh.
  std::vector<SemanticId> prototypes;
  std::vector<TerrainBoundary> boundaries;
};
struct AssetDocument {
  SchemaVersion schema_version;
  SemanticId id;
  SourceIdentity provenance;
  std::vector<Capability> required_capabilities;
  Bounds bounds;
  std::vector<ResourceDescription> resources;
  std::vector<MaterialDescription> materials;
  std::vector<TextureDescription> textures;
  std::vector<MeshPrototype> meshes;
  std::vector<AssemblyPrototype> assemblies;
  std::vector<Instance> instances;
  std::vector<Attachment> attachments;
  std::vector<LodRepresentation> lods;
  std::vector<CollisionDescription> collisions;
  std::vector<Rig> rigs;
  std::vector<SkinWeights> skins;
  std::vector<AnimationClip> animations;
  std::optional<TerrainProduct> terrain;
  std::vector<ExtensionEnvelope> extensions;
  Value metadata{Value::Object{}};
};

struct ValidationLimits {
  uint64_t max_document_bytes = 64ull * 1024 * 1024;
  uint64_t max_resource_bytes = 64ull * 1024 * 1024;
  uint64_t max_vertices = 1000000, max_indices = 3000000;
  uint64_t max_value_nodes = 1000000;
  uint32_t max_nodes = 10000, max_instances = 100000, max_depth = 128;
  uint32_t max_string_bytes = 65536, max_diagnostics = 64;
};
struct ValidationOptions {
  ValidationLimits limits;
  std::vector<Capability> supported_capabilities = {{"matter.recipe.core", 1},
                                                    {"matter.asset.core", 1},
                                                    {"matter.material.pbr", 1}};
};
struct ValidationResult {
  std::vector<Diagnostic> diagnostics;
  bool ok() const; // No error diagnostics.
};
ValidationResult validate(const RecipeDocument &,
                          const ValidationOptions & = {});
ValidationResult validate(const AssetDocument &,
                          const ValidationOptions & = {});
ValidationResult validate_value(const Value &, const ValidationLimits & = {});
ValidationResult validate_transform(const Mat4 &,
                                    bool csg_similarity_only = false);
// Inverse transpose for normals and determinant sign for winding/tangent
// handedness.
std::optional<Vec3> transform_normal(const Mat4 &, Vec3);
bool reverses_winding(const Mat4 &);

// Strict JSON parser: duplicate keys, invalid UTF-8, nonfinite numbers and
// budgets fail. Decoders leave output untouched on any failure and validate
// before publishing it.
ValidationResult parse_json(std::string_view, Value &,
                            const ValidationLimits & = {});
ValidationResult canonical_json(const Value &, std::string &,
                                const ValidationLimits & = {});
Value to_value(const RecipeDocument &);
Value to_value(const AssetDocument &);
ValidationResult decode_recipe(std::string_view, RecipeDocument &,
                               const ValidationOptions & = {});
ValidationResult decode_asset(std::string_view, AssetDocument &,
                              const ValidationOptions & = {});
ValidationResult encode(const RecipeDocument &, std::string &,
                        const ValidationOptions & = {});
ValidationResult encode(const AssetDocument &, std::string &,
                        const ValidationOptions & = {});
ContentDigest sha256(std::string_view bytes);
ValidationResult content_digest(const RecipeDocument &, ContentDigest &,
                                const ValidationOptions & = {});
ValidationResult content_digest(const AssetDocument &, ContentDigest &,
                                const ValidationOptions & = {});

// Explicit legacy-compatible stream. No engine include or implicit global
// state.
class Rng {
public:
  explicit Rng(uint64_t seed)
      : state_(seed ? seed : UINT64_C(0x9e3779b97f4a7c15)) {}
  uint64_t next_u64() {
    uint64_t z = (state_ += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
  }
  double next_unit() {
    return static_cast<double>(next_u64() >> 11) / 9007199254740992.0;
  }

private:
  uint64_t state_;
};
} // namespace matter::contracts
