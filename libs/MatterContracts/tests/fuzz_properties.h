#pragma once

// Shared oracles for the bounded CTest corpus and the opt-in fuzz driver.
// All documents and byte strings are generated in memory. No assert(): these
// checks must also run in Release builds and libFuzzer campaigns.
#include "fixtures.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace properties {
namespace mc = matter::contracts;
inline uint64_t current_seed = 0;
inline const char *current_area = "startup";
inline void require(bool ok, const char *message) {
  if (!ok) {
    std::cerr << "FAIL area=" << current_area << " seed=" << current_seed
              << ": " << message << '\n';
    std::abort();
  }
}
inline bool has_code(const mc::ValidationResult &r, mc::ErrorCode code) {
  return std::any_of(r.diagnostics.begin(), r.diagnostics.end(),
                     [code](const auto &d) { return d.code == code; });
}
inline std::string canonical(const mc::Value &v) {
  std::string text;
  require(mc::canonical_json(v, text).ok(), "canonicalization succeeds");
  return text;
}
template <class D> std::string wire(const D &d, const mc::ValidationOptions &o = {}) {
  std::string text;
  require(mc::encode(d, text, o).ok(), "valid document encodes");
  return text;
}
template <class D> mc::ContentDigest digest(const D &d, const mc::ValidationOptions &o = {}) {
  mc::ContentDigest result;
  require(mc::content_digest(d, result, o).ok(), "valid document hashes");
  return result;
}
inline mc::ValidationResult decode(std::string_view text, mc::RecipeDocument &d,
                                  const mc::ValidationOptions &o) {
  return mc::decode_recipe(text, d, o);
}
inline mc::ValidationResult decode(std::string_view text, mc::AssetDocument &d,
                                  const mc::ValidationOptions &o) {
  return mc::decode_asset(text, d, o);
}
inline mc::ValidationOptions bounded_options() {
  auto o = fixtures::optional_options();
  o.limits.max_document_bytes = 32768;
  o.limits.max_value_nodes = 4096;
  o.limits.max_string_bytes = 2048;
  o.limits.max_depth = 32;
  o.limits.max_nodes = 32;
  o.limits.max_instances = 64;
  o.limits.max_vertices = 256;
  o.limits.max_indices = 768;
  o.limits.max_diagnostics = 8;
  return o;
}
inline mc::Value generated_value(mc::Rng &rng, unsigned depth = 0) {
  const auto choice = rng.next_u64() % (depth < 3 ? 6 : 4);
  switch (choice) {
  case 0: return nullptr;
  case 1: return (rng.next_u64() & 1) != 0;
  case 2: return double(int(rng.next_u64() % 4096) - 2048) / 16;
  case 3: {
    std::string text = "utf8:\xc3\xa9\xe6\xb0\xb4\xf0\x9f\x8c\x8d";
    for (unsigned i = 0; i < 8; ++i)
      text.push_back(static_cast<char>(rng.next_u64() % 128));
    return text;
  }
  case 4: {
    mc::Value::Array a;
    const auto count = rng.next_u64() % 4;
    for (uint64_t i = 0; i < count; ++i) a.push_back(generated_value(rng, depth + 1));
    return a;
  }
  default: {
    mc::Value::Object object;
    for (const char *key : {"z", "\xc3\xa9", "a", "\xe6\xb0\xb4"})
      object.emplace(key, generated_value(rng, depth + 1));
    return object;
  }
  }
}
inline mc::RecipeDocument recipe(uint64_t seed) {
  mc::Rng rng(seed);
  auto d = (seed & 1) ? fixtures::recipe() : fixtures::extrusion();
  d.seed = rng.next_u64();
  d.parameters = mc::Value::Object{{"generated", generated_value(rng)}};
  d.metadata = generated_value(rng);
  d.extensions = {{{"vendor.note", 1}, false, generated_value(rng)}};
  d.source.legacy_hashes = {{"matter.source.v1", rng.next_u64()}};
  d.asset_dependencies = {{{"dependency"}, mc::sha256(std::to_string(seed)), {}}};
  d.nodes.clear();
  d.source_map.clear();
  const auto count = 2 + rng.next_u64() % 6;
  for (uint64_t i = 0; i < count; ++i) {
    mc::RecipeNode n;
    n.id = {"node." + std::to_string(i)};
    if (i < 2) {
      n.operation = "primitive.box";
      n.parameters = mc::Value::Object{{"size", mc::Value::Array{
          double(1 + rng.next_u64() % 128) / 8, 1, 2}}};
    } else {
      n.operation = "solid.csg";
      n.parameters = mc::Value::Object{{"operator", "subtract"}};
      n.inputs = {{"operand", mc::InputKind::node,
                   {"node." + std::to_string(i - 1)}, mc::ProductType::mesh},
                  {"operand", mc::InputKind::node, {"node.0"}, mc::ProductType::mesh}};
      n.transform = mm::scale(float(1 + rng.next_u64() % 8));
    }
    d.nodes.push_back(n);
  }
  d.outputs = {{"node." + std::to_string(count - 1)}};
  d.source_map[d.outputs[0].value] = {"memory://generated", 1, 1};
  // Authored order deliberately includes forward references.
  std::rotate(d.nodes.begin(), d.nodes.begin() + rng.next_u64() % count, d.nodes.end());
  return d;
}
inline mc::AssetDocument asset(uint64_t seed) {
  mc::Rng rng(seed);
  auto d = (seed & 1) ? fixtures::optional_asset() : fixtures::asset();
  d.metadata = generated_value(rng);
  d.extensions = {{{"vendor.note", 1}, false, generated_value(rng)}};
  d.provenance.legacy_hashes = {{"matter.resolved.v1", rng.next_u64()}};
  auto &mesh = d.meshes[0];
  const float scale = float(1 + rng.next_u64() % 32) / 4;
  mesh.positions[1].x = scale;
  mesh.positions[2].y = scale;
  mesh.bounds.max = {scale, scale, 0};
  for (auto &n : d.instances) {
    const float sx = float(1 + rng.next_u64() % 16) / 4;
    n.local_transform = mm::multiply(
        mm::translation({float(rng.next_u64() % 8), 2, -1}),
        mm::scale({(rng.next_u64() & 1) ? -sx : sx, 2, 3}));
  }
  // Typed resource and sampler metadata; payload bytes aren't needed here.
  mc::ResourceDescription resource;
  resource.id = {"texels"};
  resource.content = mc::sha256(std::to_string(seed));
  resource.components = 4;
  resource.element_count = 1 + rng.next_u64() % 8;
  resource.byte_length = 4 * resource.element_count;
  d.resources = {resource};
  mc::TextureDescription texture;
  texture.id = {"albedo"};
  texture.resource = resource.id;
  texture.width = static_cast<uint32_t>(resource.element_count);
  texture.color_space = mc::ColorSpace::srgb;
  d.textures = {texture};
  d.materials[0].base_color.texture = texture.id;
  return d;
}
template <class D> void roundtrip(const D &doc, const mc::ValidationOptions &o) {
  const auto text = wire(doc, o);
  const auto hash = digest(doc, o).hex;
  D out;
  require(decode(text, out, o).ok(), "encoded document decodes");
  require(wire(out, o) == text, "canonical document round trip is exact");
  require(digest(out, o).hex == hash, "round trip preserves identity");
  auto metadata = doc;
  metadata.metadata = mc::Value::Object{{"different", true}};
  metadata.extensions = {{{"vendor.note", 1}, false, "changed optional extension"}};
  require(digest(metadata, o).hex == hash, "nonsemantic edits preserve identity");
}
template <class D> void reject_document(const D &doc, mc::ErrorCode code,
                                       const mc::ValidationOptions &o) {
  auto result = mc::validate(doc, o);
  require(!result.ok() && has_code(result, code), "mutation rejected with expected code");
  require(!result.diagnostics.empty() && result.diagnostics.size() <= std::max(1u, o.limits.max_diagnostics),
          "diagnostics remain bounded and report failure");
  std::string text = "unchanged";
  require(!mc::encode(doc, text, o).ok() && text == "unchanged", "failed encode is atomic");
  mc::ContentDigest hash{"unchanged"};
  require(!mc::content_digest(doc, hash, o).ok() && hash.hex == "unchanged", "failed digest is atomic");
}
template <class D> void decode_bytes(std::string_view bytes, const D &sentinel,
                                    const mc::ValidationOptions &o) {
  D out = sentinel;
  const auto before = wire(out, o);
  const auto result = decode(bytes, out, o);
  if (!result.ok()) {
    require(wire(out, o) == before, "failed decode leaves complete output unchanged");
  } else {
    require(mc::validate(out, o).ok(), "successful decode validates");
    const auto encoded = wire(out, o);
    D again;
    require(decode(encoded, again, o).ok() && wire(again, o) == encoded,
            "accepted mutated input stabilizes after canonicalization");
  }
}
template <class D> void wire_rejections(const D &d, const mc::ValidationOptions &o) {
  const auto before = wire(d, o);
  auto exact = o;
  exact.limits.max_document_bytes = before.size();
  require(wire(d, exact) == before, "inclusive document byte limit passes");
  --exact.limits.max_document_bytes;
  reject_document(d, mc::ErrorCode::resource_limit, exact);
  D out = d;
  require(!decode(before, out, exact).ok() && wire(out, o) == before,
          "document decode budget failure is atomic");
  auto value = mc::to_value(d);
  auto &object = std::get<mc::Value::Object>(value.data);
  object["unknown_field"] = true;
  require(!decode(canonical(value), out, o).ok() && wire(out, o) == before,
          "unknown structural fields fail atomically");
  object.erase("unknown_field");
  object.erase("schema_version");
  require(!decode(canonical(value), out, o).ok() && wire(out, o) == before,
          "missing structural fields fail atomically");
}
inline void mutate_value(mc::Value &v, mc::Rng &rng, unsigned depth = 0) {
  if (depth < 8) {
    if (auto a = std::get_if<mc::Value::Array>(&v.data); a && !a->empty()) {
      if (rng.next_u64() & 1) { a->pop_back(); return; }
      mutate_value((*a)[rng.next_u64() % a->size()], rng, depth + 1);
      return;
    }
    if (auto o = std::get_if<mc::Value::Object>(&v.data); o && !o->empty()) {
      auto it = o->begin();
      std::advance(it, static_cast<long>(rng.next_u64() % o->size()));
      if (rng.next_u64() % 4 == 0) { o->erase(it); return; }
      mutate_value(it->second, rng, depth + 1);
      return;
    }
  }
  v = generated_value(rng);
}
inline void json_bytes(std::string_view bytes) {
  const auto limits = bounded_options().limits;
  mc::Value out = mc::Value::Object{{"sentinel", true}};
  const auto before = canonical(out);
  const auto result = mc::parse_json(bytes, out, limits);
  if (!result.ok()) {
    require(canonical(out) == before, "failed parse is atomic");
    return;
  }
  std::string text = "unchanged";
  const auto encoded = mc::canonical_json(out, text, limits);
  if (!encoded.ok()) {
    require(text == "unchanged", "canonical expansion budget failure is atomic");
    return;
  }
  mc::Value again;
  require(mc::parse_json(text, again, limits).ok(), "canonical JSON reparses");
  require(canonical(again) == text, "canonical JSON is idempotent");
}
inline void recipe_case(uint64_t seed) {
  current_area = "recipe"; current_seed = seed;
  const auto o = bounded_options();
  const auto d = recipe(seed);
  const auto before = wire(d, o);
  roundtrip(d, o);
  wire_rejections(d, o);
  roundtrip(fixtures::extrusion(), o);
  auto copy = d;
  copy.nodes.back().parameters = mc::Value::Object{{"changed", true}};
  copy.parameters = nullptr;
  copy.source_map.clear();
  copy.extensions[0].payload = nullptr;
  copy.nodes.clear();
  require(wire(d, o) == before, "recipe copy owns nested data independently");
  auto mapped = d;
  mapped.source_map.clear();
  require(digest(mapped, o).hex == digest(d, o).hex, "source map is nonsemantic");
  auto changed = d;
  changed.seed ^= 1;
  require(digest(changed, o).hex != digest(d, o).hex, "seed is semantic");
  changed = d;
  changed.asset_dependencies[0].content = mc::sha256("new dependency");
  require(digest(changed, o).hex != digest(d, o).hex, "dependency content is semantic");
  changed = d;
  for (auto &node : changed.nodes)
    if (node.operation == "solid.csg") {
      std::reverse(node.inputs.begin(), node.inputs.end());
      break;
    }
  if (d.nodes.size() > 2)
    require(digest(changed, o).hex != digest(d, o).hex, "ordered CSG operands affect identity");
  using E = mc::ErrorCode;
  for (unsigned mutation = 0; mutation < 7; ++mutation) {
    changed = d;
    E code = E::invalid_input;
    switch (mutation) {
    case 0: changed.nodes[0].id = changed.nodes[1].id; break;
    case 1: changed.outputs[0] = {"missing"}; code = E::missing_dependency; break;
    case 2: changed.schema_version.major = 2; code = E::unsupported_version; break;
    case 3: changed.required_capabilities.push_back({"vendor.unsupported", 1}); code = E::unsupported_capability; break;
    case 4: changed.parameters = mc::Value::Object{{"nan", std::numeric_limits<double>::quiet_NaN()}}; break;
    case 5: changed.nodes[0].transform.m[15] = 0; break;
    case 6: changed.nodes[0].operation_version = 0; code = E::unsupported_version; break;
    }
    reject_document(changed, code, o);
  }
  auto small = o;
  small.limits.max_nodes = static_cast<uint32_t>(d.nodes.size() - 1);
  reject_document(d, E::resource_limit, small);
  mc::Rng rng(seed);
  auto value = mc::to_value(d);
  mutate_value(value, rng);
  decode_bytes(canonical(value), d, o);
  // A generated explicit self-reference must be detected, even with forward order.
  changed = d;
  auto &n = changed.nodes[0];
  n.operation = "placement.affine";
  n.parameters = mc::Value::Object{};
  n.inputs = {{"mesh", mc::InputKind::node, n.id, mc::ProductType::mesh}};
  n.transform = {};
  reject_document(changed, E::cycle, o);
}
inline void asset_case(uint64_t seed) {
  current_area = "asset"; current_seed = seed;
  const auto o = bounded_options();
  const auto d = asset(seed);
  const auto before = wire(d, o);
  roundtrip(d, o);
  wire_rejections(d, o);
  auto copy = d;
  copy.meshes[0].positions[0].x = 99;
  copy.meshes[0].uv_sets[0][0].x = 99;
  copy.assemblies[0].instances.clear();
  copy.extensions[0].payload = nullptr;
  copy.meshes.clear();
  require(wire(d, o) == before, "asset copy owns buffers and nested instances independently");
  auto changed = d;
  changed.instances[0].local_transform.m[3] += 1;
  require(digest(changed, o).hex != digest(d, o).hex, "placement affects identity");
  using E = mc::ErrorCode;
  for (unsigned mutation = 0; mutation < 12; ++mutation) {
    changed = d;
    E code = E::invalid_input;
    switch (mutation) {
    case 0: changed.meshes[0].indices[0] = UINT32_MAX; break;
    case 1: changed.meshes[0].normals.pop_back(); break;
    case 2: changed.meshes[0].positions[0].x = std::numeric_limits<float>::infinity(); break;
    case 3: changed.meshes[0].submeshes[0].index_count = UINT32_MAX; break;
    case 4: changed.instances[0].prototype = {"missing"}; code = E::missing_dependency; break;
    case 5: changed.instances[0].parent = changed.instances[0].id; code = E::cycle; break;
    case 6: changed.resources[0].element_count = UINT64_MAX; break;
    case 7: changed.meshes[0].uv_sets[0].clear(); break;
    case 8: changed.assemblies[0].instances[0].target = mc::InstanceTarget::assembly;
            changed.assemblies[0].instances[0].prototype = changed.assemblies[0].id; code = E::cycle; break;
    case 9: changed.schema_version.minor = 1; code = E::unsupported_version; break;
    case 10: changed.resources[0].byte_length ^= 1; break;
    case 11: changed.textures[0].width = UINT32_MAX; break;
    }
    reject_document(changed, code, o);
  }
  auto small = o;
  small.limits.max_vertices = 2;
  reject_document(d, E::resource_limit, small);
  if (seed & 1) {
    for (unsigned mutation = 0; mutation < 6; ++mutation) {
      changed = d;
      switch (mutation) {
      case 0: changed.rigs[0].joints[0].parent = INT32_MAX; break;
      case 1: changed.skins[0].joints[0][0] = UINT32_MAX; break;
      case 2: changed.skins[0].joints.clear(); break;
      case 3: changed.skins[0].weights[0].x = std::numeric_limits<float>::quiet_NaN(); break;
      case 4: changed.animations[0].tracks[0].keys[1].time_seconds = 0; break;
      case 5: changed.terrain->origin_metres[0] = std::numeric_limits<double>::infinity(); break;
      }
      reject_document(changed, E::invalid_input, o);
    }
  }
  mc::Rng rng(seed);
  auto value = mc::to_value(d);
  mutate_value(value, rng);
  decode_bytes(canonical(value), d, o);
}
} // namespace properties
