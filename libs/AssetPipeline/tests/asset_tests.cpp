#include <matter/asset/archive.h>
#include <matter/asset/lod.h>
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
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace matter::asset;
namespace {
using V = data::Value;
using O = V::Object;
using A = V::Array;
using E = data::ErrorCode;
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
std::string canonical(const data::AssetDocument &d,
                      const data::ValidationOptions &o = {}) {
  std::string s;
  check(data::encode(d, s, o).ok(), "document encode");
  return s;
}
void equal(const services::AssetProduct &a, const services::AssetProduct &b,
           const data::ValidationOptions &o = {}) {
  check(canonical(a.document, o) == canonical(b.document, o),
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
// Prepared representations. Kernel-free: these fixtures run in every build.
const std::vector<data::Diagnostic> &diags(const data::ValidationResult &r) {
  return r.diagnostics;
}
template <class R> const std::vector<data::Diagnostic> &diags(const R &r) {
  return r.diagnostics();
}
template <class R> void passes(const R &r, const std::string &what) {
  if (!r.ok())
    for (const auto &d : diags(r))
      std::cerr << data::code_name(d.code) << ": " << d.message << "\n";
  check(r.ok(), what);
}
template <class R>
void fails(const R &r, E code, const std::string &what,
           const char *node = nullptr) {
  const auto &d = diags(r);
  const bool matched = !r.ok() && !d.empty() && d[0].code == code &&
                       (!node || d[0].node_id == node);
  if (!matched) {
    std::cerr << what << ": expected " << data::code_name(code)
              << (node ? std::string(" at ") + node : std::string()) << "\n";
    if (r.ok())
      std::cerr << "  got success\n";
    for (const auto &x : d)
      std::cerr << "  got " << data::code_name(x.code) << " at '" << x.node_id
                << "': " << x.message << "\n";
  }
  check(matched, what);
}
services::ProcessingRequest lod_request(A prototypes, A errors) {
  services::ProcessingRequest r;
  r.required_capabilities = {{lod_simplify, 1}};
  r.settings = O{{"prototypes", std::move(prototypes)},
                 {"max_error_metres", std::move(errors)}};
  return r;
}
// Options of a host that consumes prepared LOD products.
services::EvaluationOptions extended() {
  services::EvaluationOptions o;
  for (const auto &c : prepared_capabilities())
    o.validation.supported_capabilities.push_back(c);
  return o;
}
data::ContentDigest digest_of(const data::Value &settings) {
  LodSettings s;
  passes(parse_lod_settings(settings, s), "parse LOD settings");
  data::ContentDigest d;
  passes(settings_digest(s, d), "LOD settings digest");
  return d;
}
PreparedRequirements requiring(const char *source,
                               std::optional<data::ContentDigest> settings) {
  PreparedRequirements r;
  r.lod_sources.push_back({source});
  r.lod_settings = std::move(settings);
  return r;
}
// Hand-written output in the shape prepare_lods publishes, so the prepared
// loader is exercised with no generation kernel linked.
services::AssetProduct baked(LodChainRecord *out = nullptr) {
  auto p = fixture();
  auto &d = p.document;
  d.id = {"prepared.baked"};
  d.metadata = O{{"fixture_id", "AP-PREPARED-LOAD"}};
  data::MeshPrototype level = d.meshes[0];
  level.id = {"shared.triangles.lod1"};
  level.positions.resize(3);
  level.normals.resize(3);
  level.tangents.resize(3);
  for (auto &uv : level.uv_sets)
    uv.resize(3);
  level.indices = {0, 1, 2};
  level.submeshes = {{0, 3, {"red"}}};
  level.bounds = {{0, 0, 0}, {1, 1, 0}};
  d.meshes.push_back(level);
  d.lods = {{d.meshes[0].id, 0.0f}, {level.id, 0.5f}};
  d.required_capabilities.push_back({lod_product, 1});
  LodChainRecord record;
  record.settings = digest_of(
      O{{"prototypes", A{"shared.triangles"}}, {"max_error_metres", A{0.25}}});
  LodLevelRecord l;
  l.prototype = level.id;
  l.requested_error_metres = 0.25f;
  l.measured_lower_metres = 0.1;
  l.measured_upper_metres = 0.4;
  l.triangles = 1;
  l.queries = 10;
  record.chains.push_back({d.meshes[0].id, 2, {l}});
  d.extensions.push_back({{lod_chain_record, 1}, false, to_value(record)});
  passes(services::validate(p, extended().validation), "baked fixture validation");
  if (out)
    *out = record;
  return p;
}
void prepared() {
  LodChainRecord record;
  const auto p = baked(&record);
  const auto options = extended();
  auto bytes = encode_prepared(job, p, options);
  passes(bytes, "encode baked LOD product");
  // A host that does not understand the typed LOD product says so.
  fails(decode_prepared(job, *bytes.value()), E::unsupported_capability,
        "plain decoder rejects the LOD product");
  const auto needs = requiring("shared.triangles", record.settings);
  auto loaded = load_prepared(job, *bytes.value(), needs);
  passes(loaded, "load baked product");
  equal(p, *loaded.value(), options.validation);
  LodChainRecord read;
  passes(read_lod_chain(loaded.value()->document, read), "read chain record");
  check(read.settings.hex == record.settings.hex && read.chains.size() == 1 &&
            read.chains[0].source.value == "shared.triangles" &&
            read.chains[0].source_triangles == 2 &&
            read.chains[0].levels.size() == 1 &&
            read.chains[0].levels[0].measured_upper_metres == 0.4 &&
            read.chains[0].levels[0].queries == 10,
        "chain record roundtrip");
  passes(verify_prepared(p, needs), "verify baked product");
  passes(verify_prepared(p, {}), "no requirements");
  // Missing representations are reported, never generated.
  fails(load_prepared(job, *bytes.value(), requiring("other.mesh", {})),
        E::missing_dependency, "unbaked source");
  const auto other = digest_of(
      O{{"prototypes", A{"shared.triangles"}}, {"max_error_metres", A{0.5}}});
  fails(load_prepared(job, *bytes.value(), requiring("shared.triangles", other)),
        E::missing_dependency, "different generation settings");
  const auto plain = fixture();
  fails(verify_prepared(plain, needs), E::missing_dependency, "no chain record");
  passes(verify_prepared(plain, {}), "unprepared asset without requirements");
  // Inconsistent or newer records fail explicitly.
  auto q = p;
  q.document.lods[1].max_error_metres = 0.3f;
  fails(verify_prepared(q, needs), E::invalid_input,
        "representation bound below measured error");
  q = p;
  q.document.extensions[0].capability.version = 2;
  fails(verify_prepared(q, needs), E::unsupported_version, "newer chain record");
  auto changed = record;
  changed.algorithm = {"vendor.lod.simplify", 1};
  q = p;
  q.document.extensions[0].payload = to_value(changed);
  fails(verify_prepared(q, needs), E::unsupported_capability, "unknown algorithm");
  changed = record;
  changed.algorithm.version = 2;
  q.document.extensions[0].payload = to_value(changed);
  fails(verify_prepared(q, needs), E::unsupported_version, "newer algorithm");
  changed = record;
  changed.chains[0].levels[0].triangles = 2;
  q.document.extensions[0].payload = to_value(changed);
  fails(verify_prepared(q, needs), E::invalid_input, "triangle count mismatch");
  PreparedRequirements bad;
  bad.lod_settings = data::ContentDigest{"not-a-digest"};
  fails(verify_prepared(p, bad), E::invalid_input, "invalid settings digest");
  // Any other required capability still fails in the prepared loader.
  q = p;
  q.document.required_capabilities.push_back({"vendor.unknown", 1});
  auto vendor = options;
  vendor.validation.supported_capabilities.push_back({"vendor.unknown", 1});
  auto unknown = encode_prepared(job, q, vendor);
  passes(unknown, "encode with a vendor capability");
  fails(load_prepared(job, *unknown.value(), needs), E::unsupported_capability,
        "unknown required capability on load");
  services::CancellationSource cancelled;
  cancelled.cancel();
  services::JobControl control;
  control.cancellation = cancelled.token();
  fails(load_prepared(job, *bytes.value(), needs, {}, control), E::cancelled,
        "cancelled load");
  std::cout << "AP-PREPARED-LOAD AP-PREPARED-MISSING AP-PREPARED-RECORD\n";
}
void cache_settings() {
  const auto source = fixture();
  const data::Capability algorithm{lod_simplify, 1};
  auto key = [&](const services::AssetProduct &p,
                 const services::ProcessingRequest &r) {
    services::CacheKey k;
    passes(processing_cache_key(p, r, algorithm, k), "processing cache key");
    return k.content.hex;
  };
  auto tolerance = [](services::ProcessingRequest r, double metres) {
    std::get<O>(r.settings.data)["verification_tolerance_metres"] = metres;
    return r;
  };
  const auto base =
      lod_request(A{"shared.triangles", "other.mesh"}, A{0.01, 0.05});
  const auto respelled = tolerance(
      lod_request(A{"other.mesh", "shared.triangles"}, A{0.01, 0.05}), 0.001);
  services::CacheKey k0;
  passes(processing_cache_key(source, base, algorithm, k0), "base key");
  check(k0.name_space == services::CacheKey{}.name_space,
        "processing keys use the portable cache namespace");
  check(key(source, respelled) == k0.content.hex,
        "equivalent settings spellings share a key");
  check(key(source, lod_request(A{"shared.triangles", "other.mesh"},
                                A{0.01, 0.06})) != k0.content.hex,
        "error targets change the key");
  check(key(source, tolerance(base, 0.002)) != k0.content.hex,
        "verification tolerance changes the key");
  auto moved = source;
  moved.document.meshes[0].positions[5].y = 0.5f;
  check(key(moved, base) != k0.content.hex, "source content changes the key");
  services::CacheKey untouched;
  fails(processing_cache_key(source, base, {lod_simplify, 2}, untouched),
        E::unsupported_capability, "newer algorithm version");
  auto undeclared = base;
  undeclared.required_capabilities.clear();
  fails(processing_cache_key(source, undeclared, algorithm, untouched),
        E::invalid_input, "undeclared algorithm");
  auto vendor = base;
  vendor.required_capabilities.push_back({"vendor.unknown", 1});
  fails(processing_cache_key(source, vendor, algorithm, untouched),
        E::unsupported_capability, "unknown request capability");
  fails(processing_cache_key(source,
                             lod_request(A{"shared.triangles"}, A{0.05, 0.01}),
                             algorithm, untouched),
        E::invalid_input, "invalid settings");
  check(untouched.content.hex.empty(), "failed key leaves output untouched");

  LodSettings parsed;
  passes(parse_lod_settings(respelled.settings, parsed), "parse settings");
  check(parsed.prototypes.size() == 2 &&
            parsed.prototypes[0].value == "other.mesh" &&
            parsed.prototypes[1].value == "shared.triangles" &&
            parsed.max_error_metres == std::vector<float>{0.01f, 0.05f} &&
            parsed.verification_tolerance_metres == 0.001,
        "normalized settings");
  LodSettings defaulted;
  passes(parse_lod_settings(base.settings, defaulted), "parse defaulted settings");
  const auto spelled = to_value(defaulted);
  check(std::get<O>(spelled.data).count("verification_tolerance_metres") == 1,
        "normalized settings spell defaults");
  data::ContentDigest d1, d2;
  passes(settings_digest(parsed, d1), "settings digest");
  passes(settings_digest(defaulted, d2), "defaulted settings digest");
  check(d1.hex == d2.hex && d1.hex.size() == 64, "equivalent settings digests");
  const LodSettings before = parsed;
  auto reject = [&](const data::Value &v, const char *what) {
    LodSettings out = before;
    fails(parse_lod_settings(v, out), E::invalid_input, what);
    check(out.prototypes == before.prototypes &&
              out.max_error_metres == before.max_error_metres,
          "failed parse leaves settings untouched");
  };
  auto settings = [](A prototypes, A errors) {
    return O{{"prototypes", std::move(prototypes)},
             {"max_error_metres", std::move(errors)}};
  };
  O extra = settings(A{"a"}, A{0.1});
  extra["seed"] = 1;
  reject(extra, "unknown setting");
  reject(settings(A{"a"}, A{0.1, 0.1}), "non-increasing error targets");
  A many;
  for (int i = 1; i <= 17; ++i)
    many.push_back(0.01 * i);
  reject(settings(A{"a"}, many), "17 levels");
  reject(settings(A{"a", "a"}, A{0.1}), "duplicate prototype");
  reject(settings(A{"a"}, A{0.0}), "zero error target");
  reject(settings(A{"a"}, A{-0.1}), "negative error target");
  reject(settings(A{"a"}, A{}), "no error targets");
  reject(settings(A{}, A{0.1}), "no prototypes");
  reject(settings(A{"bad id!"}, A{0.1}), "invalid prototype id");
  O zero = settings(A{"a"}, A{0.1});
  zero["verification_tolerance_metres"] = 0.0;
  reject(zero, "zero verification tolerance");
  reject(V(A{}), "settings must be an object");
  many.pop_back();
  passes(parse_lod_settings(settings(A{"a"}, many), parsed), "16 levels");
#if MATTER_ASSET_STORAGE
  // FileCache hosts the processing key; it must be told the LOD product is a
  // type it may persist, like any other typed product.
  Temp temp;
  const auto product = std::make_shared<const services::AssetProduct>(baked());
  services::CacheKey stored;
  passes(processing_cache_key(source,
                              lod_request(A{"shared.triangles"}, A{0.25}),
                              algorithm, stored),
         "key for the baked entry");
  FileCache plain(temp.path);
  fails(plain.store(job, stored, product, {}), E::unsupported_capability,
        "cache without prepared capabilities refuses the LOD product");
  FileCache cache(temp.path, extended());
  passes(cache.store(job, stored, product, {}), "store prepared entry");
  auto hit = FileCache(temp.path, extended()).lookup(job, stored, {});
  passes(hit, "prepared cache hit");
  check(hit.value()->asset != nullptr, "prepared entry persisted");
  equal(*product, *hit.value()->asset, extended().validation);
  fails(plain.lookup(job, stored, {}), E::unsupported_capability,
        "plain cache cannot decode the LOD product");
#endif
  std::cout << "AP-CACHE-SETTINGS AP-LOD-SETTINGS\n";
}
#if MATTER_ASSET_LOD
// Unit UV sphere: welded, closed, normals only, two submeshes split at the
// equator, placed by the same assembly and roots as the archive fixture.
services::AssetProduct sphere() {
  auto p = fixture();
  auto &d = p.document;
  d.id = {"prepared.sphere"};
  d.metadata = O{{"fixture_id", "AP-LOD-IDENTITY"}};
  data::MeshPrototype m;
  m.id = {"sphere"};
  constexpr int rings = 16, segments = 32;
  const double pi = 3.14159265358979323846;
  m.positions.push_back({0, 1, 0});
  for (int r = 1; r < rings; ++r) {
    const double phi = pi * r / rings;
    for (int s = 0; s < segments; ++s) {
      const double theta = 2 * pi * s / segments;
      m.positions.push_back({float(std::sin(phi) * std::cos(theta)),
                             float(std::cos(phi)),
                             float(std::sin(phi) * std::sin(theta))});
    }
  }
  m.positions.push_back({0, -1, 0});
  const uint32_t south = uint32_t(m.positions.size() - 1);
  auto at = [&](int r, int s) {
    return uint32_t(1 + (r - 1) * segments + s % segments);
  };
  std::vector<std::array<uint32_t, 3>> tris;
  for (int s = 0; s < segments; ++s) {
    tris.push_back({0, at(1, s), at(1, s + 1)});
    for (int r = 1; r + 1 < rings; ++r) {
      tris.push_back({at(r, s), at(r + 1, s), at(r, s + 1)});
      tris.push_back({at(r, s + 1), at(r + 1, s), at(r + 1, s + 1)});
    }
    tris.push_back({south, at(rings - 1, s + 1), at(rings - 1, s)});
  }
  std::vector<uint32_t> upper, lower;
  for (auto &t : tris) {
    const auto a = m.positions[t[0]], b = m.positions[t[1]],
               c = m.positions[t[2]];
    const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz,
                 nz = ux * vy - uy * vx;
    // Counterclockwise seen from outside.
    if (nx * (a.x + b.x + c.x) + ny * (a.y + b.y + c.y) +
            nz * (a.z + b.z + c.z) < 0)
      std::swap(t[1], t[2]);
    auto &half = a.y + b.y + c.y > 0 ? upper : lower;
    half.insert(half.end(), t.begin(), t.end());
  }
  m.indices = upper;
  m.indices.insert(m.indices.end(), lower.begin(), lower.end());
  m.submeshes = {{0, uint32_t(upper.size()), {"red"}},
                 {uint32_t(upper.size()), uint32_t(lower.size()), {"blue"}}};
  m.normals = m.positions;
  m.bounds = {m.positions[0], m.positions[0]};
  for (const auto &v : m.positions) {
    m.bounds.min = {std::min(m.bounds.min.x, v.x), std::min(m.bounds.min.y, v.y),
                    std::min(m.bounds.min.z, v.z)};
    m.bounds.max = {std::max(m.bounds.max.x, v.x), std::max(m.bounds.max.y, v.y),
                    std::max(m.bounds.max.z, v.z)};
  }
  d.meshes = {m};
  for (auto &child : d.assemblies[0].instances)
    child.prototype = m.id;
  d.bounds = m.bounds;
  passes(services::validate(p), "sphere fixture validation");
  return p;
}
bool contains(const data::Bounds &outer, const data::Bounds &inner) {
  return outer.min.x <= inner.min.x && outer.min.y <= inner.min.y &&
         outer.min.z <= inner.min.z && outer.max.x >= inner.max.x &&
         outer.max.y >= inner.max.y && outer.max.z >= inner.max.z;
}
void lod() {
  const auto source = sphere();
  const auto &src = source.document;
  const auto request = lod_request(A{"sphere"}, A{0.02, 0.1});
  auto result = prepare_lods(job, source, request);
  passes(result, "prepare LODs");
  const auto &out = result.value()->document;
  // Identity: the source mesh, assembly children, roots, materials and
  // provenance are untouched; preparation only adds levels and records.
  data::AssetDocument stripped = out;
  stripped.meshes.resize(src.meshes.size());
  stripped.lods.clear();
  stripped.extensions.clear();
  stripped.required_capabilities = src.required_capabilities;
  stripped.bounds = src.bounds;
  check(canonical(stripped) == canonical(src),
        "source prototypes, assembly children and placements unchanged");
  for (const auto &child : out.assemblies[0].instances)
    check(child.prototype.value == "sphere",
          "children keep referencing the source prototype");
  check(out.meshes.size() == 3 && out.meshes[1].id.value == "sphere.lod1" &&
            out.meshes[2].id.value == "sphere.lod2",
        "derived level ids");
  check(out.required_capabilities.size() == src.required_capabilities.size() + 1 &&
            out.required_capabilities.back().name == lod_product,
        "typed LOD product declared");
  check(contains(out.bounds, src.bounds), "asset bounds only grow");

  LodSettings settings;
  passes(parse_lod_settings(request.settings, settings), "parse request");
  data::ContentDigest expected;
  passes(settings_digest(settings, expected), "request settings digest");
  LodChainRecord record;
  passes(read_lod_chain(out, record), "read chain record");
  check(record.settings.hex == expected.hex,
        "chain records the normalized settings digest");
  check(record.chains.size() == 1 && record.chains[0].source.value == "sphere" &&
            record.chains[0].levels.size() == 2,
        "one chain with two levels");
  const uint64_t triangles = src.meshes[0].indices.size() / 3;
  check(record.chains[0].source_triangles == triangles, "source triangles");
  check(out.lods.size() == 3 && out.lods[0].prototype.value == "sphere" &&
            out.lods[0].max_error_metres == 0,
        "source is the zero-error representation");
  uint64_t previous = triangles;
  for (size_t i = 0; i < 2; ++i) {
    const auto &level = record.chains[0].levels[i];
    const auto &mesh = out.meshes[1 + i];
    check(level.prototype == mesh.id && out.lods[1 + i].prototype == mesh.id,
          "level records name their meshes");
    check(level.requested_error_metres == settings.max_error_metres[i],
          "requested error recorded");
    check(level.queries > 0 && level.measured_lower_metres >= 0 &&
              level.measured_lower_metres <= level.measured_upper_metres &&
              std::isfinite(level.measured_upper_metres),
          "measured error interval");
    check(double(out.lods[1 + i].max_error_metres) >= level.measured_upper_metres,
          "representation bound covers the measured error");
    // A target, not a guarantee in general; on this smooth closed fixture the
    // measured surface distance stays within it.
    check(level.measured_upper_metres > 0 &&
              level.measured_upper_metres <= level.requested_error_metres,
          "measured error within the requested target");
    check(level.triangles <= previous, "levels never gain triangles");
    check(mesh.normals.size() == mesh.positions.size() && mesh.tangents.empty() &&
              mesh.uv_sets.empty(),
          "levels carry normals only");
    for (const auto &n : mesh.normals)
      check(std::abs(std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z) - 1) < 1e-3f,
            "unit level normals");
    std::set<std::string> materials;
    for (const auto &s : mesh.submeshes)
      materials.insert(s.material.value);
    check(materials == std::set<std::string>{"blue", "red"},
          "levels keep both source materials");
    std::cout << mesh.id.value << ": requested " << level.requested_error_metres
              << " m, measured [" << level.measured_lower_metres << ", "
              << level.measured_upper_metres << "] m, bound "
              << out.lods[1 + i].max_error_metres << " m, " << level.triangles
              << "/" << triangles << " triangles, " << level.queries
              << " queries" << (level.depth_limited ? ", depth limited" : "")
              << "\n";
    previous = level.triangles;
  }
  check(record.chains[0].levels[0].triangles < triangles, "first level simplifies");
  check(record.chains[0].levels[1].measured_upper_metres > 0,
        "coarse level reports nonzero measured error");

  const auto options = extended();
  auto again = prepare_lods(job, source, request);
  passes(again, "repeat LOD preparation");
  equal(*result.value(), *again.value(), options.validation);
  auto bytes = encode_prepared(job, *result.value(), options);
  passes(bytes, "encode prepared LOD asset");
  auto loaded = load_prepared(job, *bytes.value(), requiring("sphere", expected));
  passes(loaded, "prepared LOD asset loads without generation");
  equal(*result.value(), *loaded.value(), options.validation);
  std::cout << "AP-LOD-IDENTITY AP-LOD-ERROR AP-LOD-DETERMINISM\n";
}
void lod_failures() {
  const auto source = sphere();
  const auto request = lod_request(A{"sphere"}, A{0.02, 0.1});
  // Cancellation and progress.
  services::CancellationSource cancelled;
  cancelled.cancel();
  services::JobControl control;
  control.cancellation = cancelled.token();
  fails(prepare_lods(job, source, request, {}, control), E::cancelled,
        "pre-cancelled");
  std::vector<services::Progress> seen;
  auto record = [&](const services::Progress &p) { seen.push_back(p); };
  control = {};
  control.progress = record;
  passes(prepare_lods(job, source, request, {}, control), "progress run");
  check(seen.size() == 4 && seen.front().stage == "validate" &&
            seen[1].stage == "lod" && seen[2].stage == "lod" &&
            seen.back().stage == "complete" && seen.back().fraction == 1,
        "progress stages");
  for (size_t i = 1; i < seen.size(); ++i)
    check(seen[i].fraction >= seen[i - 1].fraction, "monotonic progress");
  services::CancellationSource during;
  control.cancellation = during.token();
  control.progress = [&](const services::Progress &p) {
    if (p.stage == "lod")
      during.cancel();
  };
  fails(prepare_lods(job, source, request, {}, control), E::cancelled,
        "cancelled during generation");
  control = {};
  control.progress = [](const services::Progress &) {
    throw std::runtime_error("host progress failure");
  };
  fails(prepare_lods(job, source, request, {}, control), E::backend_failure,
        "throwing progress callback");
  services::EvaluationOptions options;
  options.budget.progress_events = 2;
  seen.clear();
  control = {};
  control.progress = record;
  passes(prepare_lods(job, source, request, options, control),
         "two progress events");
  check(seen.size() == 2 && seen[0].stage == "validate" &&
            seen[1].stage == "complete",
        "progress thinned to the event budget");
  options.budget.progress_events = 1;
  fails(prepare_lods(job, source, request, options, control), E::resource_limit,
        "progress event budget");
  // Budgets.
  auto limited = [&](auto set, const char *what) {
    services::EvaluationOptions o;
    set(o.budget);
    fails(prepare_lods(job, source, request, o), E::resource_limit, what);
  };
  limited([](services::ResourceBudget &b) { b.queries = 10; }, "query budget");
  limited([](services::ResourceBudget &b) { b.vertices = 100; }, "vertex budget");
  limited([](services::ResourceBudget &b) { b.working_bytes = 100000; },
          "working memory budget");
  limited([](services::ResourceBudget &b) { b.operations = 100; },
          "operation budget");
  limited([](services::ResourceBudget &b) { b.output_bytes = 1024; },
          "output budget");
  // Capabilities and unsupported input fail explicitly.
  auto undeclared = request;
  undeclared.required_capabilities.clear();
  fails(prepare_lods(job, source, undeclared), E::invalid_input,
        "undeclared algorithm");
  auto newer = request;
  newer.required_capabilities = {{lod_simplify, 2}};
  fails(prepare_lods(job, source, newer), E::unsupported_capability,
        "newer algorithm version");
  auto vendor = request;
  vendor.required_capabilities.push_back({"vendor.unknown", 1});
  fails(prepare_lods(job, source, vendor), E::unsupported_capability,
        "unknown request capability");
  services::EvaluationOptions host;
  host.algorithms = {{"vendor.unknown", 1}};
  fails(prepare_lods(job, source, request, host), E::unsupported_capability,
        "unknown host algorithm");
  auto semantic = source;
  semantic.document.required_capabilities.push_back({"vendor.semantic", 1});
  semantic.document.extensions.push_back({{"vendor.semantic", 1}, true, O{}});
  services::EvaluationOptions supports;
  supports.validation.supported_capabilities.push_back({"vendor.semantic", 1});
  fails(prepare_lods(job, semantic, request, supports), E::unsupported_capability,
        "required document extension", "vendor.semantic");
  auto textured = source;
  textured.document.meshes[0].uv_sets = {std::vector<data::Vec2>(
      textured.document.meshes[0].positions.size(), data::Vec2{0, 0})};
  fails(prepare_lods(job, textured, request), E::unsupported_capability,
        "UV sets are not reprojected", "sphere");
  fails(prepare_lods(job, source, lod_request(A{"missing"}, A{0.02})),
        E::missing_dependency, "missing prototype", "missing");
  auto out = prepare_lods(job, source, request);
  passes(out, "prepare LODs");
  fails(prepare_lods(job, *out.value(), request, extended()), E::invalid_input,
        "already prepared");
  auto collision = source;
  auto taken = collision.document.meshes[0];
  taken.id = {"sphere.lod1"};
  collision.document.meshes.push_back(taken);
  fails(prepare_lods(job, collision, request), E::invalid_input,
        "derived id collision", "sphere.lod1");
  fails(prepare_lods(job, source, lod_request(A{"sphere"}, A{0.1, 0.02})),
        E::invalid_input, "decreasing error targets");
  std::cout << "AP-LOD-CANCEL AP-LOD-PROGRESS AP-LOD-BUDGET AP-LOD-CAPS\n";
}
#else
void lod_disabled() {
  check(lod_capabilities().empty(), "no LOD algorithm advertised");
  fails(prepare_lods(job, fixture(), lod_request(A{"shared.triangles"}, A{0.25})),
        E::unsupported_capability, "LOD generation not built");
  LodChainRecord record;
  const auto p = baked(&record);
  const auto options = extended();
  auto bytes = encode_prepared(job, p, options);
  passes(bytes, "encode baked LOD product");
  auto loaded = load_prepared(job, *bytes.value(),
                              requiring("shared.triangles", record.settings));
  passes(loaded, "prepared asset loads without generation kernels");
  equal(p, *loaded.value(), options.validation);
  std::cout << "AP-LOD-DISABLED AP-PREPARED-NO-KERNEL\n";
}
#endif
#if MATTER_ASSET_GLTF
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
    else if (name == "prepared")
      prepared();
    else if (name == "cache_settings")
      cache_settings();
#if MATTER_ASSET_LOD
    else if (name == "lod")
      lod();
    else if (name == "lod_failures")
      lod_failures();
#else
    else if (name == "lod_disabled")
      lod_disabled();
#endif
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
