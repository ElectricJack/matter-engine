#include <matter/asset/archive.h>
#if MATTER_ASSET_STORAGE
#include <matter/asset/storage.h>
#endif
#if MATTER_ASSET_GLTF
#include <matter/asset/gltf.h>
#endif
#if MATTER_ASSET_LEGACY
#include <matter/asset/legacy.h>
#endif
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace matter::asset;
namespace {
services::JobEnvelope job{"asset-fixture", "source-v1", 17};
void check(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class R> void success(const R &r, const char *message) {
  if (!r.ok()) {
    for (const auto &d : r.diagnostics())
      std::cerr << data::code_name(d.code) << ": " << d.message << "\n";
  }
  check(r.ok(), message);
}
template <class R> void error(const R &r, data::ErrorCode code) {
  check(!r.ok() && !r.diagnostics().empty() && r.diagnostics()[0].code == code,
        "expected explicit failure code");
}
services::AssetProduct fixture(bool texture = false) {
  data::AssetDocument d;
  d.id = {"prepared.prop"};
  d.provenance.id = {"source.prop"};
  d.provenance.uri = "fixture:AP-ROUNDTRIP";
  d.provenance.revision = "1";
  d.provenance.content = data::sha256("source");
  d.required_capabilities = {{"matter.asset.core", 1},
                             {"matter.material.pbr", 1}};
  data::MaterialDescription red;
  red.id = {"red"};
  red.base_color.factor = {0.6f, 0.2f, 0.1f, 1};
  red.metallic = 0.3f;
  red.roughness = 0.7f;
  red.emissive = {0.1f, 0.2f, 0.3f};
  red.normal_scale = 0.6f;
  data::MaterialDescription blue;
  blue.id = {"blue"};
  blue.base_color.factor = {0.1f, 0.2f, 0.6f, 1};
  blue.metallic = 0.8f;
  blue.roughness = 0.25f;
  d.materials = {red, blue};
  data::MeshPrototype m;
  m.id = {"shared.triangles"};
  m.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0},
                 {1, 0, 0}, {2, 0, 0}, {1, 1, 0}};
  m.indices = {0, 1, 2, 3, 4, 5};
  m.bounds = {{0, 0, 0}, {2, 1, 0}};
  m.normals.assign(6, {0, 0, 1});
  m.tangents.assign(6, {1, 0, 0, 1});
  m.uv_sets = {{{0, 0}, {1, 0}, {0, 1}, {0, 0}, {1, 0}, {0, 1}},
               {{0.1f, 0.1f},
                {0.9f, 0.1f},
                {0.1f, 0.9f},
                {0.1f, 0.1f},
                {0.9f, 0.1f},
                {0.1f, 0.9f}}};
  m.submeshes = {{0, 3, red.id}, {3, 3, blue.id}};
  d.meshes = {m};
  d.bounds = m.bounds;
  data::AssemblyPrototype a;
  a.id = {"shared.assembly"};
  data::Instance first;
  first.id = {"child.one"};
  first.prototype = m.id;
  first.local_transform = mm::translation({1, 2, 3});
  data::Instance second;
  second.id = {"child.two"};
  second.prototype = m.id;
  second.parent = first.id;
  second.local_transform =
      mm::multiply(mm::rotation_y(0.25f), mm::scale({-2, 1, 3}));
  a.instances = {first, second};
  d.assemblies = {a};
  data::Instance left;
  left.id = {"left"};
  left.prototype = a.id;
  left.target = data::InstanceTarget::assembly;
  data::Instance right = left;
  right.id = {"right"};
  right.parent = left.id;
  right.local_transform = mm::translation({10, 0, 0});
  d.instances = {left, right};
  d.metadata = data::Value::Object{{"fixture_id", "AP-ROUNDTRIP"},
                                   {"nonsemantic", "retained"}};
  services::ResolvedInputs payload;
  if (texture) {
    Bytes pixels{255, 0, 0,   255, 0,   255, 0,   255,
                 0,   0, 255, 255, 128, 128, 255, 255};
    data::ResourceDescription r{
        {"pixels"},
        data::sha256(std::string_view(
            reinterpret_cast<const char *>(pixels.data()), pixels.size())),
        data::ScalarType::u8,
        4,
        4,
        pixels.size()};
    d.resources = {r};
    payload.resources.push_back({r, std::make_shared<const Bytes>(pixels)});
    data::TextureDescription t;
    t.id = {"color"};
    t.resource = r.id;
    t.width = t.height = 2;
    t.color_space = data::ColorSpace::srgb;
    t.wrap_u = data::WrapMode::mirror;
    t.wrap_v = data::WrapMode::clamp;
    t.min_filter = data::FilterMode::nearest;
    data::TextureDescription normal = t;
    normal.id = {"normal"};
    normal.color_space = data::ColorSpace::linear;
    d.textures = {t, normal};
    d.materials[0].base_color.texture = t.id;
    d.materials[0].base_color.uv_set = 1;
    d.materials[0].normal_texture = normal.id;
    d.materials[0].metallic_roughness_texture = normal.id;
    d.materials[0].emissive_texture = t.id;
  }
  services::AssetProduct product{d, payload};
  check(services::validate(product).ok(), "fixture validation");
  return product;
}
std::string canonical(const data::AssetDocument &d) {
  std::string s;
  check(data::encode(d, s).ok(), "document encode");
  return s;
}
void equal(const services::AssetProduct &a, const services::AssetProduct &b) {
  check(canonical(a.document) == canonical(b.document),
        "exact document roundtrip");
  check(a.resources.resources.size() == b.resources.resources.size(),
        "resource count roundtrip");
  for (size_t i = 0; i < a.resources.resources.size(); ++i)
    check(*a.resources.resources[i].bytes == *b.resources.resources[i].bytes,
          "exact resource bytes");
}
void archive() {
  auto p = fixture(true);
  auto e = encode_prepared(job, p);
  success(e, "encode prepared");
  auto d = decode_prepared(job, *e.value());
  success(d, "decode prepared");
  equal(p, *d.value());
  auto bytes = *e.value();
  bytes.back() ^= 1;
  error(decode_prepared(job, bytes), data::ErrorCode::invalid_input);
  bytes = *e.value();
  bytes[4] = 2;
  error(decode_prepared(job, bytes), data::ErrorCode::unsupported_version);
  bytes = *e.value();
  bytes.resize(83);
  error(decode_prepared(job, bytes), data::ErrorCode::invalid_input);
  p.resources.resources[0].bytes = std::make_shared<const Bytes>(Bytes{0});
  error(encode_prepared(job, p), data::ErrorCode::invalid_input);
  services::CancellationSource cancelled;
  cancelled.cancel();
  services::JobControl c;
  c.cancellation = cancelled.token();
  error(encode_prepared(job, fixture(), {}, c), data::ErrorCode::cancelled);
  error(decode_prepared(job, *e.value(), {}, c), data::ErrorCode::cancelled);
  services::EvaluationOptions tiny;
  tiny.budget.output_bytes = 16;
  error(encode_prepared(job, fixture(), tiny), data::ErrorCode::resource_limit);
  tiny = {};
  tiny.budget.vertices = 5;
  error(encode_prepared(job, fixture(), tiny), data::ErrorCode::resource_limit);
  tiny = {};
  tiny.budget.working_bytes = 16;
  error(decode_prepared(job, *e.value(), tiny),
        data::ErrorCode::resource_limit);
  p = fixture();
  p.document.required_capabilities.push_back({"unknown", 9});
  error(encode_prepared(job, p), data::ErrorCode::unsupported_capability);
  std::cout << "AP-ARCHIVE AP-HASH AP-VERSION AP-CANCEL AP-BUDGET AP-CAPS\n";
}
#if MATTER_ASSET_STORAGE
struct Temp {
  std::filesystem::path path;
  Temp() {
    path = std::filesystem::temp_directory_path() /
           ("matter-asset-test-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(path), "temporary directory");
  }
  ~Temp() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};
Bytes read(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(f), {});
}
void write(const std::filesystem::path &p, const Bytes &b) {
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size()));
  check(bool(f), "test write");
}
void storage() {
  Temp temp;
  FileCache cache(temp.path);
  services::CacheKey key;
  key.content = data::sha256("recipe1");
  services::CacheKey other;
  other.content = data::sha256("recipe2");
  auto miss = cache.lookup(job, key, {});
  success(miss, "cache miss");
  check(!miss.value()->asset, "null successful miss");
  auto p = fixture(true);
  success(cache.store(job, key,
                      std::make_shared<const services::AssetProduct>(p), {}),
          "cache store");
  FileCache reopened(temp.path);
  auto loaded = reopened.lookup(job, key, {});
  success(loaded, "cache persisted reopen");
  equal(p, *loaded.value()->asset);
  auto path = temp.path / (key.content.hex + ".mapa");
  auto before = read(path);
  auto failure = services::AssetResult::failure(
      job, data::ErrorCode::backend_failure, "failed job");
  error(cache.store_result(job, other, failure),
        data::ErrorCode::backend_failure);
  check(!std::filesystem::exists(temp.path / (other.content.hex + ".mapa")),
        "failed result publishes nothing");
  auto old = job;
  old.generation_epoch--;
  auto stale = services::AssetResult::success(old, p);
  error(cache.store_result(job, key, stale), data::ErrorCode::stale_result);
  check(read(path) == before, "stale preserves existing entry");
  services::CancellationSource source;
  services::JobControl control;
  control.cancellation = source.token();
  control.progress = [&](const auto &) { source.cancel(); };
  error(cache.store(job, key,
                    std::make_shared<const services::AssetProduct>(fixture()),
                    control),
        data::ErrorCode::cancelled);
  check(read(path) == before, "cancel at commit preserves entry");
  control = {};
  control.progress = [](const auto &) {
    throw std::runtime_error("publication hook failed");
  };
  error(cache.store(job, other,
                    std::make_shared<const services::AssetProduct>(p), control),
        data::ErrorCode::backend_failure);
  check(!std::filesystem::exists(temp.path / (other.content.hex + ".mapa")),
        "callback failure publishes nothing");
  for (const auto &entry : std::filesystem::directory_iterator(temp.path))
    check(entry.path().extension() == ".mapa", "stages cleaned after failure");
  auto badkey = key;
  badkey.name_space = "matter.legacy.fnv1a64";
  error(cache.lookup(job, badkey, {}), data::ErrorCode::unsupported_version);
  badkey = key;
  badkey.content.hex = "../invalid";
  error(cache.lookup(job, badkey, {}), data::ErrorCode::invalid_input);
  write(temp.path / (other.content.hex + ".mapa"), before);
  error(cache.lookup(job, other, {}), data::ErrorCode::invalid_input);
  auto damaged = before;
  damaged.back() ^= 1;
  write(path, damaged);
  error(cache.lookup(job, key, {}), data::ErrorCode::invalid_input);
  auto output = temp.path / "export.glb";
  success(write_new_file(job, output, Bytes{1, 2, 3}), "new explicit export");
  error(write_new_file(job, output, Bytes{4}),
        data::ErrorCode::backend_failure);
  check(read(output) == Bytes({1, 2, 3}), "existing export preserved");
  std::cout << "AP-CACHE-REOPEN AP-NO-PARTIAL AP-STALE AP-ATOMIC AP-EXPORT\n";
}
#endif
#if MATTER_ASSET_GLTF
using V = data::Value;
using O = V::Object;
using A = V::Array;
V parse(const std::string &s) {
  V v;
  check(data::parse_json(s, v).ok(), "parse fixture glTF");
  return v;
}
std::string json(const V &v) {
  std::string s;
  check(data::canonical_json(v, s).ok(), "encode fixture glTF");
  return s;
}
O &obj(V &v) { return std::get<O>(v.data); }
A &arr(V &v) { return std::get<A>(v.data); }
void gltf(const char *output) {
  auto p = fixture(true);
  auto e = export_gltf(job, p);
  success(e, "export glTF");
  auto d = import_gltf(job, *e.value());
  success(d, "import glTF");
  equal(p, *d.value());
  auto b = export_glb(job, p);
  success(b, "export GLB");
  auto back = import_glb(job, *b.value());
  success(back, "import GLB");
  equal(p, *back.value());
  auto v = parse(e.value()->json);
  check(arr(obj(v).at("meshes")).size() == 1, "shared mesh emitted once");
  check(arr(obj(v).at("nodes")).size() == 6,
        "repeated assembly expanded without geometry duplication");
  auto changed = *e.value();
  auto root = parse(changed.json);
  obj(arr(obj(root).at("materials"))[0])["emissiveFactor"] = A{1, 0, 0};
  changed.json = json(root);
  error(import_gltf(job, changed), data::ErrorCode::invalid_input);
  auto newer = *e.value();
  root = parse(newer.json);
  obj(obj(obj(root).at("extras")).at("matterPrepared"))["version"] = 2;
  newer.json = json(root);
  error(import_gltf(job, newer), data::ErrorCode::unsupported_version);
  if (output) {
    std::ofstream f(output, std::ios::binary);
    f.write(reinterpret_cast<const char *>(b.value()->data()),
            std::streamsize(b.value()->size()));
    check(bool(f), "fixture output");
  }
  std::cout << "AP-GLTF-ROUNDTRIP AP-GLB-ROUNDTRIP AP-SHARED AP-AFFINE "
               "AP-SCENE-MISMATCH\n";
}
GltfDocument ordinary() {
  auto e = export_gltf(job, fixture(true));
  success(e, "ordinary fixture export");
  auto g = *e.value();
  auto root = parse(g.json);
  obj(root).erase("extras");
  g.json = json(root);
  return g;
}
void external() {
  // Independently authored core glTF: padded positions and uint16 indices.
  // This exercises accessor decoding without using this adapter's exporter.
  Bytes binary(54, 0);
  binary[18] = binary[38] = 128;
  binary[19] = binary[39] = 63;
  binary[12] = binary[28] = binary[44] = 127;
  binary[48] = 2;
  binary[52] = 1;
  GltfDocument independent{R"({
    "asset":{"version":"2.0"},
    "buffers":[{"byteLength":54,"uri":"triangle.bin"}],
    "bufferViews":[{"buffer":0,"byteLength":48,"byteStride":16},
                   {"buffer":0,"byteOffset":48,"byteLength":6}],
    "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                 {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],
    "meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],
    "nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0
  })",
                           {binary},
                           {}};
  auto decoded = import_gltf(job, independent);
  success(decoded, "independently authored strided glTF");
  const auto &mesh = decoded.value()->document.meshes[0];
  check(mesh.positions[1].x == 1 && mesh.positions[2].y == 1 &&
            mesh.indices == std::vector<uint32_t>{2, 0, 1},
        "strided positions and uint16 indices");
  check(decoded.value()->document.materials[0].metallic == 1,
        "glTF default material semantics");
  auto independent_root = parse(independent.json);
  obj(arr(obj(arr(obj(independent_root).at("meshes"))[0]).at("primitives"))[0])
      .erase("indices");
  independent.json = json(independent_root);
  decoded = import_gltf(job, independent);
  success(decoded, "nonindexed glTF");
  check(decoded.value()->document.meshes[0].indices ==
            std::vector<uint32_t>{0, 1, 2},
        "nonindexed topology");
  auto g = ordinary();
  auto d = import_gltf(job, g);
  success(d, "ordinary glTF import");
  const auto &a = d.value()->document;
  check(a.meshes.size() == 1 && a.instances.size() == 6,
        "generic mesh sharing and hierarchy");
  check(a.meshes[0].submeshes.size() == 2 && a.meshes[0].indices.size() == 6,
        "generic topology and materials");
  check(a.materials[0].metallic == 0.3f && a.materials[0].roughness == 0.7f &&
            a.materials[0].normal_scale == 0.6f,
        "generic PBR factors");
  check(a.materials[0].base_color.uv_set == 1 &&
            a.materials[0].emissive_texture && a.materials[0].normal_texture &&
            a.materials[0].metallic_roughness_texture,
        "generic PBR bindings");
  check(a.textures.size() == 2 && a.resources.size() == 2,
        "texture references retained");
  for (const auto &t : a.textures)
    check(t.wrap_u == data::WrapMode::mirror &&
              t.wrap_v == data::WrapMode::clamp &&
              t.min_filter == data::FilterMode::nearest,
          "sampler semantics retained");
  for (const auto &r : d.value()->resources.resources)
    check(*r.bytes == *fixture(true).resources.resources[0].bytes,
          "PNG pixels preserved");
  auto root = parse(g.json);
  auto &nodes = arr(obj(root).at("nodes"));
  auto &n = obj(nodes[0]);
  n.erase("matrix");
  n["translation"] = A{7, 8, 9};
  n["scale"] = A{-2, 3, 4};
  g.json = json(root);
  d = import_gltf(job, g);
  success(d, "TRS import");
  check(d.value()->document.instances[0].local_transform.m[3] == 7 &&
            d.value()->document.instances[0].local_transform.m[0] == -2,
        "TRS convention");
  auto e = export_gltf(job, *d.value());
  success(e, "generic imported asset reexport");
  success(import_gltf(job, *e.value()), "generic reexport roundtrip");
  std::cout << "AP-EXTERNAL AP-STRIDE AP-U16 AP-NONINDEXED AP-TRS AP-PNG "
               "AP-SAMPLER AP-PBR\n";
}
void malformed() {
  auto g = ordinary();
  auto root = parse(g.json);
  obj(obj(root).at("asset"))["version"] = "1.0";
  g.json = json(root);
  error(import_gltf(job, g), data::ErrorCode::unsupported_version);
  g = ordinary();
  root = parse(g.json);
  obj(root)["extensionsRequired"] = A{"KHR_draco_mesh_compression"};
  g.json = json(root);
  error(import_gltf(job, g), data::ErrorCode::unsupported_capability);
  g = ordinary();
  root = parse(g.json);
  obj(arr(obj(root).at("accessors"))[0])["count"] = 1000000000;
  g.json = json(root);
  error(import_gltf(job, g), data::ErrorCode::resource_limit);
  g = ordinary();
  root = parse(g.json);
  obj(arr(obj(root).at("bufferViews"))[0])["byteLength"] = 1000000000;
  g.json = json(root);
  error(import_gltf(job, g), data::ErrorCode::invalid_input);
  g = ordinary();
  root = parse(g.json);
  obj(arr(obj(root).at("nodes"))[2])["children"] = A{0};
  g.json = json(root);
  check(!import_gltf(job, g).ok(), "parent cycle rejected");
  g = ordinary();
  root = parse(g.json);
  obj(arr(obj(root).at("accessors"))[0])["sparse"] = O{};
  g.json = json(root);
  error(import_gltf(job, g), data::ErrorCode::unsupported_capability);
  auto glb = export_glb(job, fixture());
  success(glb, "malformed GLB fixture");
  auto bytes = *glb.value();
  bytes[4] = 3;
  error(import_glb(job, bytes), data::ErrorCode::unsupported_version);
  bytes = *glb.value();
  bytes.resize(bytes.size() - 1);
  error(import_glb(job, bytes), data::ErrorCode::invalid_input);
  services::CancellationSource cancelled;
  cancelled.cancel();
  services::JobControl c;
  c.cancellation = cancelled.token();
  error(import_gltf(job, ordinary(), {}, c), data::ErrorCode::cancelled);
  error(export_gltf(job, fixture(), {}, c), data::ErrorCode::cancelled);
  services::EvaluationOptions limit;
  limit.budget.output_bytes = 100;
  error(export_glb(job, fixture(), limit), data::ErrorCode::resource_limit);
  std::cout
      << "AP-GLTF-MALFORMED AP-GLB-MALFORMED AP-GLTF-CANCEL AP-GLTF-BUDGET\n";
}
void materials() {
  auto g = ordinary();
  auto root = parse(g.json);
  obj(arr(obj(root).at("materials"))[0])["alphaMode"] = "BLEND";
  g.json = json(root);
  error(import_gltf(job, g), data::ErrorCode::unsupported_capability);
  g = ordinary();
  root = parse(g.json);
  obj(arr(obj(root).at("materials"))[0])["doubleSided"] = true;
  g.json = json(root);
  error(import_gltf(job, g), data::ErrorCode::unsupported_capability);
  g = ordinary();
  root = parse(g.json);
  obj(arr(obj(root).at("materials"))[0])["occlusionTexture"] = O{{"index", 0}};
  g.json = json(root);
  error(import_gltf(job, g), data::ErrorCode::unsupported_capability);
  g = ordinary();
  root = parse(g.json);
  obj(arr(obj(root).at("samplers"))[0])["minFilter"] = 9987;
  g.json = json(root);
  error(import_gltf(job, g), data::ErrorCode::unsupported_capability);
  auto p = fixture(true);
  p.document.textures[0].color_space = data::ColorSpace::linear;
  error(export_gltf(job, p), data::ErrorCode::unsupported_capability);
  p = fixture();
  p.document.materials[0].emissive = {2, 0, 0};
  error(export_gltf(job, p), data::ErrorCode::unsupported_capability);
  p = fixture();
  p.document.meshes[0].uv_sets.resize(9, p.document.meshes[0].uv_sets[0]);
  error(export_gltf(job, p), data::ErrorCode::unsupported_capability);
  std::cout << "AP-UNSUPPORTED-MATERIAL AP-COLORSPACE AP-NO-VISUAL-LOSS\n";
}
#endif
#if MATTER_ASSET_LEGACY
void legacy() {
  auto p = fixture();
  p.document.provenance.legacy_hashes = {
      {"matter.source.v1", UINT64_C(0xfedcba9876543210)}};
  LegacyArtifactRequest request;
  request.source_hash = p.document.provenance.legacy_hashes[0];
  request.artifact_identity = "existing:matter.artifact.v2";
  LegacyReader reader = [&](const auto &j, const auto &, const auto &,
                            const auto &) {
    return services::AssetResult::success(j, p);
  };
  auto imported = import_legacy(job, request, reader);
  success(imported, "legacy adapter");
  equal(p, *imported.value());
  request.source_hash.value++;
  error(import_legacy(job, request, reader), data::ErrorCode::invalid_input);
  request.source_hash.value--;
  request.artifact_version = 99;
  error(import_legacy(job, request, reader),
        data::ErrorCode::unsupported_version);
  request.artifact_version = 2;
  error(import_legacy(job, request, {}),
        data::ErrorCode::unsupported_capability);
  auto hash = p.document.provenance.legacy_hashes[0].value;
  check(hash == UINT64_C(0xfedcba9876543210),
        "64-bit legacy identity remains exact");
  std::cout << "AP-LEGACY-NAMESPACE AP-LEGACY-VERSION AP-LEGACY-HASH\n";
}
#endif
} // namespace
int main(int argc, char **argv) {
  try {
    check(argc >= 2, "fixture name required");
    std::string name = argv[1];
    if (name == "archive")
      archive();
#if MATTER_ASSET_STORAGE
    else if (name == "storage")
      storage();
#endif
#if MATTER_ASSET_GLTF
    else if (name == "gltf")
      gltf(argc > 2 ? argv[2] : nullptr);
    else if (name == "external")
      external();
    else if (name == "malformed")
      malformed();
    else if (name == "materials")
      materials();
#endif
#if MATTER_ASSET_LEGACY
    else if (name == "legacy")
      legacy();
#endif
    else
      throw std::runtime_error("unknown fixture");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
