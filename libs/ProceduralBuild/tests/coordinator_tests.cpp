#include <matter/build/coordinator.h>
#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace d = matter::contracts;
namespace s = matter::services;
namespace b = matter::build;
using E = d::ErrorCode;
using V = d::Value;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #x); } while (false)
template <class T> void good(const s::JobResult<T> &result) {
  if (!result.ok()) {
    for (const auto &diag : result.diagnostics()) std::cerr << d::code_name(diag.code) << ": " << diag.message << '\n';
    CHECK(result.ok());
  }
}
template <class T> void error(const s::JobResult<T> &result, E code) {
  CHECK(!result.ok());
  auto matched = std::any_of(result.diagnostics().begin(), result.diagnostics().end(),
      [&](const auto &diag) { return diag.code == code; });
  if (!matched) {
    std::cerr << "Expected " << d::code_name(code) << '\n';
    for (const auto &diag : result.diagnostics()) std::cerr << d::code_name(diag.code) << ": " << diag.message << '\n';
  }
  CHECK(matched);
}
std::vector<d::Capability> capabilities() { return d::ValidationOptions{}.supported_capabilities; }
d::SourceIdentity source(const std::string &id) { return {{id}, "fixture://" + id, "v1", {}, {}}; }
s::BuildRequest request(const std::string &id) {
  s::BuildRequest result;
  result.source.source = source(id);
  result.authoring.parameters = V::Object{{"width", 1}};
  return result;
}
s::SourceSnapshot snapshot(const std::string &id) {
  return {{id}, d::sha256("resolver-snapshot"), {{source(id), "generate " + id, {}}}, {}};
}
d::AssetDocument asset(const d::RecipeDocument &recipe) {
  d::AssetDocument a;
  a.id = {"asset." + recipe.source.id.value}; a.provenance = recipe.source;
  a.required_capabilities = {{"matter.asset.core", 1}, {"matter.material.pbr", 1}};
  d::MaterialDescription material; material.id = {"material"}; a.materials = {material};
  a.bounds = {{0, 0, 0}, {1, 1, 0}};
  d::MeshPrototype mesh; mesh.id = {"triangle"};
  mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  mesh.indices = {0, 1, 2}; mesh.bounds = a.bounds;
  mesh.submeshes = {{0, 3, {"material"}}};
  a.meshes.push_back(mesh);
  d::Instance left; left.id = {"left"}; left.prototype = {"triangle"};
  d::Instance right = left; right.id = {"right"};
  a.instances = {left, right};
  return a;
}
s::JobEnvelope stamp(s::JobEnvelope job, bool stale) { if (stale) ++job.generation_epoch; return job; }
struct Resolver : s::ResolverService {
  std::map<std::string, s::SourceSnapshot> sources;
  unsigned calls = 0; bool stale = false;
  std::function<void()> hook;
  std::vector<d::Capability> capabilities() const override { return ::capabilities(); }
  s::ResolveResult resolve(const s::JobEnvelope &job, const s::ResolveRequest &r,
                           const s::ResourceBudget &, const s::JobControl &) override {
    ++calls; if (hook) hook();
    auto found = sources.find(r.source.id.value);
    if (found == sources.end()) return s::ResolveResult::failure(job, E::missing_dependency, "fake missing module");
    return s::ResolveResult::success(stamp(job, stale), found->second);
  }
};
struct Host : s::AuthoringHost {
  unsigned calls = 0; bool stale = false, bad_recipe = false, unknown = false;
  std::vector<s::SourceSnapshot> seen;
  std::function<void()> hook;
  std::vector<d::Capability> capabilities() const override { return ::capabilities(); }
  s::RecipeResult author(const s::JobEnvelope &job, const s::SourceSnapshot &ss,
                        const s::AuthoringRequest &r, const s::ResourceBudget &, const s::JobControl &) override {
    ++calls; seen.push_back(ss); if (hook) hook();
    d::RecipeDocument recipe;
    auto entry = std::find_if(ss.modules.begin(), ss.modules.end(), [&](const auto &m) { return m.identity.id == ss.entry; });
    CHECK(entry != ss.modules.end());
    recipe.source = entry->identity; recipe.seed = r.seed; recipe.parameters = r.parameters;
    recipe.required_capabilities = {{"matter.recipe.core", 1}};
    if (unknown) recipe.required_capabilities.push_back({"unknown.required", 1});
    d::RecipeNode box; box.id = {"box"}; box.operation = "primitive.box";
    box.parameters = V::Object{{"size", V::Array{1, 1, 1}}};
    recipe.nodes = {box}; recipe.outputs = {{bad_recipe ? "missing" : "box"}};
    for (const auto &child : ss.resources.assets) recipe.asset_dependencies.push_back(child.description);
    return s::RecipeResult::success(stamp(job, stale), std::move(recipe));
  }
};
struct Geometry : s::GeometryService {
  std::vector<std::string> order;
  unsigned processed = 0; bool stale = false, stale_process = false, bad_asset = false, throws = false;
  std::function<void(const s::JobControl &)> hook;
  std::vector<d::Capability> capabilities() const override { return ::capabilities(); }
  s::AssetResult evaluate(const s::JobEnvelope &job, const d::RecipeDocument &recipe,
                          const s::ResolvedInputs &, const s::EvaluationOptions &, const s::JobControl &control) override {
    order.push_back(recipe.source.id.value);
    if (hook) hook(control);
    if (throws) throw std::runtime_error("fake backend exception");
    auto output = asset(recipe);
    if (bad_asset) output.meshes.front().indices = {0, 1, 30};
    return s::AssetResult::success(stamp(job, stale), s::AssetProduct{output});
  }
  s::QueryResult query(const s::JobEnvelope &job, const s::AssetProduct &, const s::QueryBatch &,
                       const s::EvaluationOptions &, const s::JobControl &) override {
    return s::QueryResult::failure(job, E::unsupported_capability, "fake query unavailable");
  }
  s::AssetResult process(const s::JobEnvelope &job, const s::AssetProduct &a, const s::ProcessingRequest &,
                        const s::EvaluationOptions &, const s::JobControl &) override {
    ++processed;
    return s::AssetResult::success(stamp(job, stale_process), a);
  }
};
struct Cache : s::CacheService {
  unsigned reads = 0, writes = 0; bool stale = false;
  std::map<std::string, std::shared_ptr<const s::AssetProduct>> entries;
  std::shared_ptr<const s::AssetProduct> forced;
  std::vector<d::Capability> capabilities() const override { return {}; }
  s::CacheResult lookup(const s::JobEnvelope &job, const s::CacheKey &key, const s::JobControl &) override {
    ++reads; auto found = entries.find(key.name_space + key.content.hex);
    return s::CacheResult::success(stamp(job, stale), {forced ? forced : found == entries.end() ? nullptr : found->second});
  }
  s::StatusResult store(const s::JobEnvelope &job, const s::CacheKey &key,
                        std::shared_ptr<const s::AssetProduct> value, const s::JobControl &) override {
    ++writes; entries[key.name_space + key.content.hex] = std::move(value);
    return s::StatusResult::success(job, {});
  }
};
struct Fixture {
  Resolver resolver; Host host; Geometry geometry; Cache cache;
  b::CoordinatorOptions options;
  s::JobEnvelope job{"request", "revision", 1};
  std::unique_ptr<b::Coordinator> coordinator;
  Fixture() { options.implementation = d::sha256("fake services v1"); reset(); }
  void reset() {
    coordinator = std::make_unique<b::Coordinator>(resolver, host, geometry, options, &cache);
    CHECK(coordinator->activate(job).ok());
  }
  b::NodeRequest node(const std::string &id, const std::string &module = "") {
    auto name = module.empty() ? id : module;
    resolver.sources[name] = snapshot(name);
    return {{id}, request(name), {}};
  }
  b::GraphRequest one() { return {{node("root")}, {{"root"}}, {}}; }
  b::GraphResult run(const b::GraphRequest &g, const s::JobControl &control = {}) {
    return coordinator->build_graph(job, g, control);
  }
};

void graph_tests() {
  Fixture f;
  auto shared = f.node("shared"); auto left = f.node("left"); auto right = f.node("right"); auto root = f.node("root");
  left.children = {{{"shared"}, {"shared-child"}}}; right.children = left.children;
  root.children = {{{"left"}, {"left-child"}}, {{"right"}, {"right-child"}}};
  b::GraphRequest graph{{root, right, shared, left}, {{"root"}}, {}};
  auto result = f.run(graph); good(result);
  CHECK((f.geometry.order == std::vector<std::string>{"shared", "left", "right", "root"}));
  CHECK(result.value()->generated == 4); CHECK(result.value()->nodes.size() == 4);
  CHECK(f.host.seen.back().resources.assets.size() == 2);
  CHECK(result.value()->roots[0].asset->document.instances[0].id.value == "left");
  CHECK(result.value()->roots[0].asset->document.instances[1].id.value == "right");
  auto prior = f.coordinator->published();
  graph.nodes[0].children[0].node = {"absent"};
  error(f.run(graph), E::missing_dependency); CHECK(f.coordinator->published() == prior);
  graph.nodes[0].children[0].node = {"root"};
  auto cycle = f.run(graph); error(cycle, E::cycle); CHECK(!cycle.diagnostics()[0].dependency_path.empty());
  Fixture d;
  b::GraphRequest repeated{{d.node("first", "prototype"), d.node("alias", "prototype")},
                           {{"first"}, {"alias"}, {"first"}}, {}};
  auto reuse = d.run(repeated); good(reuse);
  CHECK(d.geometry.order.size() == 1); CHECK(d.host.calls == 1); CHECK(reuse.value()->deduplicated == 1);
  CHECK(reuse.value()->roots.size() == 3);
  CHECK(reuse.value()->roots[0].asset == reuse.value()->roots[1].asset);
  CHECK(reuse.value()->roots[1].id.value == "alias");
  repeated.nodes[1].build.authoring.parameters = V::Object{{"width", 2}};
  auto variants = d.run(repeated); good(variants); CHECK(variants.value()->generated == 2);
  Fixture single; single.node("one");
  good(single.coordinator->build(single.job, request("one"), {}));
  CHECK(single.coordinator->published()->roots.size() == 1);
  std::cout << "BUILD-GRAPH, BUILD-SHARED-CHILD, BUILD-MISSING, BUILD-CYCLE, BUILD-PROTOTYPE passed\n";
}
void source_tests() {
  Fixture f; auto graph = f.one();
  auto &ss = f.resolver.sources["root"];
  ss.modules = {{source("root"), "entry", {{"left"}, {"right"}}},
                {source("left"), "left", {{"utility"}}}, {source("right"), "right", {{"utility"}}},
                {source("utility"), "utility only, no generator export", {}}};
  good(f.run(graph)); CHECK(f.host.calls == 1);
  CHECK(f.host.seen.back().modules.front().identity.id.value == "utility");
  CHECK(f.host.seen.back().modules.back().identity.id.value == "root");
  ss.modules[3].dependencies = {{"absent"}}; error(f.run(graph), E::missing_dependency);
  ss.modules[3].dependencies = {{"root"}}; error(f.run(graph), E::cycle);
  ss.modules[3].dependencies = {{"matter:authoring"}};
  error(f.run(graph), E::missing_dependency);
  f.options.host_modules = {{"matter:authoring"}}; f.reset(); good(f.run(graph));
  ss.modules[3].dependencies.clear();
  ss.modules[3].identity.content = d::sha256("incorrect"); error(f.run(graph), E::invalid_input);
  ss.modules[3].identity.content.reset();
  ss.modules[0].identity.revision = "different"; error(f.run(graph), E::invalid_input);
  ss.modules[0].identity.revision = "v1";
  ss.modules[0].dependencies.push_back({"left"}); error(f.run(graph), E::invalid_input);
  ss.modules[0].dependencies.pop_back();
  ss.modules.push_back({source("unused"), "unused", {{"unused"}}}); error(f.run(graph), E::cycle);
  Fixture missing; auto g = missing.one(); missing.resolver.sources.clear(); error(missing.run(g), E::missing_dependency);
  std::cout << "BUILD-SOURCE-DAG, BUILD-SOURCE-MISSING, BUILD-SOURCE-CYCLE, BUILD-FROZEN-INPUT passed\n";
}
void key_tests() {
  Fixture f; auto graph = f.one();
  auto &ss = f.resolver.sources["root"];
  ss.modules[0].dependencies = {{"utility"}};
  ss.modules.push_back({source("utility"), "first", {}});
  auto key = [&] { auto r = f.run(graph); good(r); return r.value()->roots[0].key.content.hex; };
  const auto initial = key();
  std::reverse(ss.modules.begin(), ss.modules.end()); CHECK(key() == initial);
  ss.modules[0].source_utf8 = "second"; CHECK(key() != initial);
  ss.modules[0].source_utf8 = "first"; CHECK(key() == initial);
  auto changed = [&](auto edit) { auto old = graph; edit(graph.nodes[0].build); CHECK(key() != initial); graph = old; };
  changed([](auto &r) { r.authoring.parameters = V::Object{{"width", 2}}; });
  changed([](auto &r) { r.authoring.seed = UINT64_MAX; });
  changed([](auto &r) { r.evaluation.algorithms = {{"matter.recipe.core", 1}}; });
  changed([](auto &r) { r.processing = s::ProcessingRequest{{}, V::Object{{"quality", 3}}}; });
  changed([](auto &r) { r.evaluation.validation.limits.max_vertices -= 1; });
  changed([](auto &r) { r.evaluation.budget.vertices -= 1; });
  changed([](auto &r) { r.source.source.legacy_hashes = {{"matter.source.v1", UINT64_MAX}}; });
  auto payload = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{42});
  ss.resources.resources.push_back({{{"payload"}, d::sha256("*"), d::ScalarType::u8, 1, 1, 1}, payload});
  const auto payload_key = key(); CHECK(payload_key != initial);
  ss.resources.resources[0].bytes = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{43});
  error(f.run(graph), E::invalid_input); // Stale declared digest never hits cache.
  ss.resources.resources[0].description.content = d::sha256("+"); CHECK(key() != payload_key);
  ss.resources.resources.clear();
  f.options.implementation = d::sha256("service v2"); f.reset(); CHECK(key() != initial);
  s::CacheKey legacy_source, legacy_resolved;
  CHECK(b::legacy_cache_key({"matter.source.v1", UINT64_MAX}, legacy_source).ok());
  CHECK(b::legacy_cache_key({"matter.resolved.v1", UINT64_MAX}, legacy_resolved).ok());
  CHECK(legacy_source.content.hex != legacy_resolved.content.hex);
  CHECK(legacy_source.name_space != "matter.portable.asset.v1");
  auto saved = legacy_source.content.hex;
  CHECK(!b::legacy_cache_key({"", 1}, legacy_source).ok()); CHECK(legacy_source.content.hex == saved);
  graph.nodes[0].build.authoring.parameters = V::Object{{"width", std::numeric_limits<double>::infinity()}};
  error(f.run(graph), E::invalid_input);
  Fixture transitive; auto child = transitive.node("child"), root = transitive.node("root");
  root.children = {{{"child"}, {"part"}}};
  b::GraphRequest g{{root, child}, {{"root"}}, {}};
  auto before = transitive.run(g); good(before);
  g.nodes[1].build.authoring.seed = 7; auto after = transitive.run(g); good(after);
  CHECK(before.value()->roots[0].key.content.hex != after.value()->roots[0].key.content.hex);
  std::cout << "BUILD-CANONICAL-KEY, BUILD-TRANSITIVE-KEY, BUILD-LEGACY-NAMESPACE passed\n";
}
void cache_tests() {
  Fixture f; auto graph = f.one(); auto first = f.run(graph); good(first);
  const auto &entry = first.value()->roots.front();
  CHECK(f.cache.writes == 0);
  good(f.cache.store(f.job, entry.key, entry.asset, {}));
  auto hit = f.run(graph); good(hit); CHECK(hit.value()->cache_hits == 1);
  CHECK(f.host.calls == 1); CHECK(f.geometry.order.size() == 1);
  CHECK(hit.value()->roots[0].asset == entry.asset);
  auto malformed = *entry.asset; malformed.document.meshes[0].indices = {0, 1, 44};
  f.cache.forced = std::make_shared<const s::AssetProduct>(malformed);
  error(f.run(graph), E::invalid_input);
  f.cache.forced = entry.asset;
  graph.nodes[0].build.evaluation.budget.vertices = 2;
  error(f.run(graph), E::resource_limit);
  graph.nodes[0].build.evaluation.budget.vertices = 100;
  graph.nodes[0].build.evaluation.algorithms = {{"unknown.algorithm", 1}};
  error(f.run(graph), E::unsupported_capability);
  graph.nodes[0].build.evaluation.algorithms.clear();
  graph.nodes[0].build.processing = s::ProcessingRequest{{{"unknown.process", 1}}, {}};
  error(f.run(graph), E::unsupported_capability);
  CHECK(f.cache.writes == 1);
  std::cout << "BUILD-CACHE-HIT, BUILD-CACHE-VALIDATION, BUILD-CAPABILITY passed\n";
}
void control_tests() {
  for (unsigned stage = 0; stage < 5; ++stage) {
    Fixture f; auto g = f.one();
    f.resolver.stale = stage == 0; f.cache.stale = stage == 1;
    f.host.stale = stage == 2; f.geometry.stale = stage == 3; f.geometry.stale_process = stage == 4;
    g.nodes[0].build.processing = s::ProcessingRequest{};
    error(f.run(g), E::stale_result); CHECK(!f.coordinator->published()); CHECK(f.cache.writes == 0);
  }
  for (unsigned field = 0; field < 3; ++field) {
    Fixture f; auto g = f.one(); auto stale = f.job;
    if (field == 0) stale.request_id += "changed";
    if (field == 1) stale.source_revision += "changed";
    if (field == 2) ++stale.generation_epoch;
    error(f.coordinator->build_graph(stale, g), E::stale_result); CHECK(f.resolver.calls == 0);
  }
  Fixture f; auto g = f.one(); good(f.run(g)); auto prior = f.coordinator->published();
  s::CancellationSource cancelled; cancelled.cancel();
  error(f.run(g, {cancelled.token(), {}}), E::cancelled); CHECK(f.coordinator->published() == prior);
  CHECK(f.coordinator->activate(f.job).ok());
  s::CancellationSource late;
  error(f.run(g, {late.token(), [&](const s::Progress &p) { if (p.stage == "complete") late.cancel(); }}), E::cancelled);
  CHECK(f.coordinator->published() == prior);
  CHECK(f.coordinator->activate(f.job).ok());
  error(f.run(g, {{}, [](const s::Progress &) { throw std::runtime_error("callback"); }}), E::backend_failure);
  f.geometry.throws = true; error(f.run(g), E::backend_failure); f.geometry.throws = false;
  f.host.bad_recipe = true; error(f.run(g), E::missing_dependency); f.host.bad_recipe = false;
  f.host.unknown = true; error(f.run(g), E::unsupported_capability); f.host.unknown = false;
  f.geometry.bad_asset = true; error(f.run(g), E::invalid_input); f.geometry.bad_asset = false;

  // Deterministic overlap: invalidate/re-activate on the control thread while a
  // fake service deliberately ignores cancellation and returns a late success.
  std::mutex mutex; std::condition_variable cv; bool entered = false, release = false, saw_cancel = false;
  f.geometry.hook = [&](const s::JobControl &c) {
    std::unique_lock<std::mutex> lock(mutex); entered = true; cv.notify_one();
    cv.wait(lock, [&] { return release; }); saw_cancel = c.cancellation->load();
  };
  std::optional<b::GraphResult> result;
  std::thread worker([&] { result = f.run(g); });
  { std::unique_lock<std::mutex> lock(mutex); cv.wait(lock, [&] { return entered; }); }
  f.coordinator->invalidate(); CHECK(f.coordinator->activate(f.job).ok()); // Same envelope, distinct activation.
  { std::lock_guard<std::mutex> lock(mutex); release = true; } cv.notify_one(); worker.join();
  CHECK(saw_cancel); error(*result, E::stale_result); CHECK(f.coordinator->published() == prior);
  CHECK(f.cache.writes == 0);
  std::cout << "BUILD-STALE-STAGES, BUILD-ENVELOPE, BUILD-CANCEL, BUILD-LATE, BUILD-EXCEPTIONS passed\n";
}
void budget_tests() {
  Fixture f; auto g = f.one();
  g.budget.operations = 1; error(f.run(g), E::resource_limit);
  g.budget = {}; g.budget.vertices = 2; error(f.run(g), E::resource_limit);
  g.budget = {}; g.budget.progress_events = 1;
  error(f.run(g, {{}, [](const auto &) {}}), E::resource_limit);
  g.budget = {}; f.options.max_stages = 2; f.reset();
  error(f.run(g), E::resource_limit); CHECK(f.host.calls == 1); // Only earlier budget fixture authored.
  f.options.max_stages = 100; f.options.max_source_bytes = 2; f.reset(); error(f.run(g), E::resource_limit);
  Fixture depth;
  auto leaf = depth.node("leaf"), middle = depth.node("middle"), root = depth.node("root");
  middle.children = {{{"leaf"}, {"leaf"}}}; root.children = {{{"middle"}, {"middle"}}};
  b::GraphRequest graph{{leaf, middle, root}, {{"leaf"}, {"root"}}, {}};
  graph.budget.dependency_depth = 2;
  error(depth.run(graph), E::resource_limit); CHECK(depth.geometry.order.empty());
  graph.budget.dependency_depth = 3; good(depth.run(graph));
  std::cout << "BUILD-OPERATIONS, BUILD-STAGES, BUILD-OUTPUT-BUDGET, BUILD-PROGRESS, BUILD-DAG-DEPTH passed\n";
}
int main(int argc, char **argv) {
  try {
    CHECK(argc == 2); std::string fixture = argv[1];
    if (fixture == "graph") graph_tests();
    else if (fixture == "source_graph") source_tests();
    else if (fixture == "cache_keys") key_tests();
    else if (fixture == "cache") cache_tests();
    else if (fixture == "controls") control_tests();
    else if (fixture == "budgets") budget_tests();
    else throw std::runtime_error("unknown fixture");
    return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
