#include <matter/build/coordinator.h>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace matter::build {
namespace d = contracts;
namespace s = services;
namespace {
using E = d::ErrorCode;
using V = d::Value;
struct Failure { std::vector<d::Diagnostic> diagnostics; };
[[noreturn]] void fail(E code, std::string message, std::vector<std::string> path = {}) {
  throw Failure{{{d::Severity::error, code, std::move(message), {}, {}, std::move(path)}}};
}
void require(d::ValidationResult result) {
  if (!result.ok()) throw Failure{std::move(result.diagnostics)};
}
bool digest(const d::ContentDigest &v) {
  return v.hex.size() == 64 && std::all_of(v.hex.begin(), v.hex.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
std::string hex64(uint64_t value) {
  std::string result(16, '0');
  for (size_t i = 16; i != 0; value >>= 4) result[--i] = "0123456789abcdef"[value & 15];
  return result;
}
void name(const std::string &value) {
  if (value.empty() || value.find('\0') != std::string::npos)
    fail(E::invalid_input, "nonempty identity without NUL required");
  require(d::validate_value(value));
}
V caps(std::vector<d::Capability> values) {
  require(s::require_capabilities(values, values));
  std::sort(values.begin(), values.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
  V::Array result;
  for (const auto &c : values) result.emplace_back(V::Object{{"name", c.name}, {"version", c.version}});
  return result;
}
V identity(const d::SourceIdentity &source) {
  name(source.id.value); name(source.uri); name(source.revision);
  if (source.content && !digest(*source.content)) fail(E::invalid_input, "invalid source digest");
  V::Object legacy;
  for (const auto &h : source.legacy_hashes) {
    s::CacheKey ignored;
    require(legacy_cache_key(h, ignored));
    if (!legacy.emplace(h.name_space, hex64(h.value)).second)
      fail(E::invalid_input, "duplicate legacy hash namespace");
  }
  return V::Object{{"id", source.id.value}, {"uri", source.uri}, {"revision", source.revision},
                   {"content", source.content ? V(source.content->hex) : V()}, {"legacy", legacy}};
}
V budget_value(const s::ResourceBudget &b) {
  require(s::validate(b));
  return V::Array{hex64(b.operations), hex64(b.vertices), hex64(b.indices), hex64(b.output_bytes),
                  hex64(b.working_bytes), hex64(b.execution_microseconds), hex64(b.queries),
                  b.dependency_depth, b.progress_events};
}
V validation_value(const d::ValidationOptions &options) {
  const auto &l = options.limits;
  return V::Array{caps(options.supported_capabilities), hex64(l.max_document_bytes),
                  hex64(l.max_resource_bytes), hex64(l.max_vertices), hex64(l.max_indices),
                  hex64(l.max_value_nodes), l.max_nodes, l.max_instances, l.max_depth,
                  l.max_string_bytes, l.max_diagnostics};
}
d::ContentDigest hash(const V &value, const d::ValidationLimits &limits) {
  std::string encoded;
  require(d::canonical_json(value, encoded, limits));
  return d::sha256(encoded);
}

// Iterative three-colour DFS: independent of the native call stack, catches
// back-edges, memoizes shared children and checks the longest DAG path even
// when a child was visited through a shorter earlier root.
std::vector<std::string> order(const std::map<std::string, std::vector<std::string>> &edges,
                              const std::vector<std::string> &roots, uint32_t max_depth,
                              const std::function<void()> &checkpoint) {
  std::map<std::string, unsigned> colour;
  std::map<std::string, uint64_t> depths;
  std::vector<std::string> result;
  struct Frame { std::string id; size_t next = 0; uint64_t depth = 1; };
  std::vector<Frame> stack;
  for (const auto &root : roots) {
    if (colour[root] == 2) continue;
    stack.push_back({root});
    while (!stack.empty()) {
      checkpoint();
      auto &f = stack.back();
      auto found = edges.find(f.id);
      if (found == edges.end()) {
        std::vector<std::string> path;
        for (const auto &frame : stack) path.push_back(frame.id);
        fail(E::missing_dependency, "missing dependency " + f.id, std::move(path));
      }
      colour[f.id] = 1;
      if (stack.size() > max_depth || f.depth > max_depth)
        fail(E::resource_limit, "dependency depth exceeded", {f.id});
      if (f.next == found->second.size()) {
        auto id = f.id;
        auto depth = f.depth;
        colour[id] = 2; depths[id] = depth; result.push_back(id); stack.pop_back();
        if (!stack.empty()) stack.back().depth = std::max(stack.back().depth, depth + 1);
        continue;
      }
      const auto &child = found->second[f.next++];
      if (colour[child] == 1) {
        std::vector<std::string> path;
        for (const auto &frame : stack) path.push_back(frame.id);
        path.push_back(child);
        fail(E::cycle, "cyclic dependency", std::move(path));
      }
      if (colour[child] == 2) f.depth = std::max(f.depth, depths[child] + 1);
      else stack.push_back({child});
    }
  }
  return result;
}

struct Run {
  const CoordinatorOptions &options;
  s::JobEnvelope job;
  s::JobControl control;
  s::JobMonitor monitor;
  s::ResourceUsage usage;
  std::function<void()> current;
  GraphProduct product;
  Run(const CoordinatorOptions &o, s::JobEnvelope e, const s::ResourceBudget &budget,
      s::JobControl c, std::function<void()> check)
      : options(o), job(std::move(e)), control(std::move(c)), monitor(budget, control), current(std::move(check)) {}
  void check() { current(); require(monitor.checkpoint(usage)); }
  void step() {
    check();
    if (usage.operations == UINT64_MAX) fail(E::resource_limit, "operation count overflow");
    ++usage.operations; check();
  }
  void stage(const char *label) {
    step();
    if (product.stages >= options.max_stages) fail(E::resource_limit, "stage count exceeded");
    ++product.stages;
    // Progress has one coordinator owner; injected service callbacks are muted.
    require(monitor.report({0, label})); check();
  }
  s::JobControl service_control() const { return {control.cancellation, {}}; }
  template <class T> std::shared_ptr<const T> accept(const s::JobResult<T> &result) {
    check();
    std::shared_ptr<const T> value;
    require(s::admit(result, job, value));
    return value;
  }
  void add(uint64_t &total, uint64_t amount) {
    if (amount > UINT64_MAX - total) fail(E::resource_limit, "resource count overflow");
    total += amount;
  }
  void validate_asset(const s::AssetProduct &asset, const s::EvaluationOptions &evaluation) {
    require(s::validate(asset, evaluation.validation));
    s::JobMonitor node_monitor(evaluation.budget, service_control());
    // Enforce the same validation/output budgets on cache hits as generated assets.
    accept(s::finish_asset(job, asset.document, evaluation, node_monitor, {}, asset.resources));
  }
  void account(const s::AssetProduct &asset, const s::EvaluationOptions &evaluation) {
    validate_asset(asset, evaluation);
    std::string encoded;
    require(d::encode(asset.document, encoded, evaluation.validation));
    add(usage.output_bytes, encoded.size());
    for (const auto &r : asset.resources.resources) add(usage.output_bytes, r.description.byte_length);
    for (const auto &mesh : asset.document.meshes) {
      add(usage.vertices, mesh.positions.size()); add(usage.indices, mesh.indices.size());
    }
    check();
  }
};

// Recompute content from every supplied module and resolved input; never trust
// an opaque resolver snapshot hash to cover transitive changes by itself.
V freeze(s::SourceSnapshot &snapshot, const s::BuildRequest &request, Run &run) {
  const auto &options = run.options;
  if (!digest(snapshot.content)) fail(E::invalid_input, "snapshot requires a SHA-256 identity");
  if (snapshot.modules.size() > options.max_source_modules)
    fail(E::resource_limit, "source module count exceeded");
  std::set<std::string> external;
  for (const auto &id : options.host_modules) {
    name(id.value);
    if (!external.insert(id.value).second) fail(E::invalid_input, "duplicate host module");
  }
  std::map<std::string, s::SourceModule> modules;
  std::map<std::string, std::vector<std::string>> edges;
  V::Object sources;
  uint64_t bytes = 0;
  for (const auto &m : snapshot.modules) {
    run.step();
    const auto &id = m.identity.id.value;
    auto source_identity = identity(m.identity);
    if (external.count(id) || !modules.emplace(id, m).second)
      fail(E::invalid_input, "duplicate or reserved source module", {id});
    if (m.source_utf8.size() > options.max_source_bytes - bytes)
      fail(E::resource_limit, "source byte budget exceeded");
    bytes += m.source_utf8.size();
    auto limits = request.evaluation.validation.limits;
    limits.max_string_bytes = static_cast<uint32_t>(std::min<uint64_t>(options.max_source_bytes, UINT32_MAX));
    require(d::validate_value(m.source_utf8, limits));
    if (m.source_utf8.find('\0') != std::string::npos) fail(E::invalid_input, "NUL in source text");
    auto actual = d::sha256(m.source_utf8);
    if (m.identity.content && m.identity.content->hex != actual.hex)
      fail(E::invalid_input, "source digest mismatch", {id});
    if (m.dependencies.size() > options.max_source_modules)
      fail(E::resource_limit, "source edge count exceeded");
    std::set<std::string> deps;
    auto &children = edges[id];
    for (const auto &dep : m.dependencies) {
      run.step(); name(dep.value);
      if (!deps.insert(dep.value).second) fail(E::invalid_input, "duplicate source dependency", {id, dep.value});
      if (!external.count(dep.value)) children.push_back(dep.value);
    }
    std::sort(children.begin(), children.end());
    V::Array names;
    for (const auto &dep : deps) names.emplace_back(dep);
    sources.emplace(id, V::Object{{"identity", source_identity}, {"bytes", actual.hex}, {"dependencies", names}});
  }
  if (snapshot.entry != request.source.source.id || !modules.count(snapshot.entry.value))
    fail(E::missing_dependency, "snapshot entry does not match requested source");
  const auto &entry = modules.at(snapshot.entry.value);
  if (entry.identity.revision != request.source.source.revision ||
      entry.identity.uri != request.source.source.uri ||
      (request.source.source.content && d::sha256(entry.source_utf8).hex != request.source.source.content->hex))
    fail(E::invalid_input, "resolved entry does not match frozen source request");
  auto entry_identity = entry.identity;
  std::vector<std::string> roots;
  for (const auto &m : modules) roots.push_back(m.first);
  auto sorted = order(edges, roots, std::min(request.evaluation.budget.dependency_depth,
      request.evaluation.validation.limits.max_depth), [&] { run.step(); });
  snapshot.modules.clear();
  for (const auto &id : sorted) {
    auto &module = modules.at(id);
    std::sort(module.dependencies.begin(), module.dependencies.end(),
              [](const auto &a, const auto &b) { return a.value < b.value; });
    snapshot.modules.push_back(std::move(module));
  }
  // Resolved inputs are sets. Canonicalize the service-facing collections too,
  // so array insertion order cannot change generation under an identical key.
  auto by_id = [](const auto &a, const auto &b) { return a.description.id.value < b.description.id.value; };
  std::sort(snapshot.resources.resources.begin(), snapshot.resources.resources.end(), by_id);
  std::sort(snapshot.resources.assets.begin(), snapshot.resources.assets.end(), by_id);
  run.usage.working_bytes = std::max(run.usage.working_bytes, bytes); run.check();

  // Reuse the public resolved-input validator, including asset/payload hashes.
  d::AssetDocument probe; probe.id = {"build.inputs"}; probe.provenance = std::move(entry_identity);
  probe.required_capabilities = {{"matter.asset.core", 1}};
  require(s::validate(s::AssetProduct{probe, snapshot.resources}, request.evaluation.validation));
  V::Object resources, assets;
  for (const auto &r : snapshot.resources.resources) {
    run.step(); const auto &v = r.description;
    resources.emplace(v.id.value, V::Array{v.content.hex, static_cast<uint32_t>(v.scalar_type),
        v.components, hex64(v.element_count), hex64(v.byte_length)});
  }
  for (const auto &a : snapshot.resources.assets) {
    run.step(); const auto &v = a.description;
    assets.emplace(v.id.value, V::Array{v.content.hex, v.schema_version.major, v.schema_version.minor});
  }
  return V::Object{{"entry", snapshot.entry.value}, {"resolver_identity", snapshot.content.hex},
                   {"modules", sources}, {"resources", resources}, {"assets", assets}};
}

s::CacheKey cache_key(const s::BuildRequest &request, const V &snapshot, const V &children, Run &run) {
  V processing;
  if (request.processing) processing = V::Object{{"capabilities", caps(request.processing->required_capabilities)},
                                                {"settings", request.processing->settings}};
  auto value = V::Object{{"contract", V::Array{1, 0, d::canonical_profile}},
      {"coordinator", "matter.build.v1"}, {"implementation", run.options.implementation.hex},
      {"source_request", identity(request.source.source)}, {"snapshot", snapshot}, {"children", children},
      {"source_capabilities", caps(request.source.required_capabilities)},
      {"authoring_capabilities", caps(request.authoring.required_capabilities)},
      {"parameters", request.authoring.parameters}, {"seed", hex64(request.authoring.seed)},
      {"rng", "matter.splitmix64.v1"}, {"algorithms", caps(request.evaluation.algorithms)},
      {"processing", processing}, {"budget", budget_value(request.evaluation.budget)},
      {"validation", validation_value(request.evaluation.validation)}};
  return {"matter.portable.asset.v1", hash(value, request.evaluation.validation.limits)};
}
} // namespace

struct Coordinator::Activation {
  s::JobEnvelope job;
  s::CancellationSource cancellation;
};
Coordinator::Coordinator(s::ResolverService &r, s::AuthoringHost &h, s::GeometryService &g,
                         CoordinatorOptions options, s::CacheService *cache)
    : resolver_(r), host_(h), geometry_(g), options_(std::move(options)), cache_(cache) {}
d::ValidationResult Coordinator::activate(const s::JobEnvelope &job) {
  auto valid = s::validate(job);
  if (!valid.ok()) return valid;
  auto next = std::make_shared<Activation>(); next->job = job;
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_) active_->cancellation.cancel();
  active_ = std::move(next);
  return {};
}
void Coordinator::invalidate() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_) active_->cancellation.cancel();
  active_.reset();
}
std::shared_ptr<const GraphProduct> Coordinator::published() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return published_;
}
std::vector<d::Capability> Coordinator::capabilities() const { return {{"matter.build.graph", 1}}; }
s::AssetResult Coordinator::build(const s::JobEnvelope &job, const s::BuildRequest &request,
                                 const s::JobControl &control) {
  GraphRequest graph{{{{"root"}, request, {}}}, {{"root"}}, request.evaluation.budget};
  auto result = build_graph(job, graph, control);
  if (!result.ok()) return s::AssetResult::failure(job, result.diagnostics());
  return s::AssetResult::success(job, *result.value()->roots.front().asset, result.diagnostics());
}
GraphResult Coordinator::build_graph(const s::JobEnvelope &job, const GraphRequest &request,
                                     const s::JobControl &control) {
  try {
    require(s::validate(job)); require(s::validate(request.budget));
    if (!digest(options_.implementation) || !options_.max_nodes || !options_.max_source_modules ||
        !options_.max_source_bytes || !options_.max_stages)
      fail(E::invalid_input, "coordinator needs implementation digest and finite nonzero limits");
    std::shared_ptr<Activation> activation;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      activation = active_;
    }
    auto current = [&] {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!activation || active_ != activation || !(activation->job == job))
        fail(E::stale_result, "build is not the current activation");
      if (control.cancellation && control.cancellation->load(std::memory_order_relaxed)) {
        activation->cancellation.cancel();
        fail(E::cancelled, "build cancelled");
      }
    };
    current();
    // Invalidation is visible inside cooperative services. Caller cancellation
    // is checked between stages and at admission; see README for synchronous limits.
    s::JobControl owned_control{activation->cancellation.token(), control.progress};
    Run run(options_, job, request.budget, owned_control, current);
    run.check();
    if (request.nodes.empty() || request.roots.empty()) fail(E::invalid_input, "build graph needs nodes and roots");
    if (request.nodes.size() > options_.max_nodes || request.roots.size() > options_.max_nodes)
      fail(E::resource_limit, "build node/root count exceeded");
    std::map<std::string, const NodeRequest *> nodes;
    std::map<std::string, std::vector<std::string>> edges;
    std::vector<std::string> all, roots;
    for (const auto &node : request.nodes) {
      run.step(); name(node.id.value);
      if (!nodes.emplace(node.id.value, &node).second) fail(E::invalid_input, "duplicate build node");
      if (node.children.size() > options_.max_nodes) fail(E::resource_limit, "build edge count exceeded");
      auto &children = edges[node.id.value];
      std::set<std::string> bindings;
      for (const auto &child : node.children) {
        run.step(); name(child.node.value); name(child.asset.value);
        if (!bindings.insert(child.asset.value).second) fail(E::invalid_input, "duplicate child binding");
        children.push_back(child.node.value);
      }
      all.push_back(node.id.value);
    }
    // Reject malformed unused nodes too; none can become an implicit later root.
    order(edges, all, request.budget.dependency_depth, [&] { run.step(); });
    for (const auto &root : request.roots) { name(root.value); roots.push_back(root.value); }
    auto sorted = order(edges, roots, request.budget.dependency_depth, [&] { run.step(); });
    std::map<std::string, NodeProduct> completed;
    std::map<std::string, std::shared_ptr<const s::AssetProduct>> prototypes;
    for (const auto &id : sorted) {
      const auto &node = *nodes.at(id);
      const auto &b = node.build;
      require(s::validate(b.evaluation.budget));
      require(d::validate_value(b.authoring.parameters, b.evaluation.validation.limits));
      if (!std::holds_alternative<V::Object>(b.authoring.parameters.data))
        fail(E::invalid_input, "authoring parameters must be an object");
      identity(b.source.source);
      require(s::require_capabilities(b.source.required_capabilities, resolver_.capabilities()));
      require(s::require_capabilities(b.authoring.required_capabilities, host_.capabilities()));
      require(s::require_capabilities(b.evaluation.algorithms, geometry_.capabilities()));
      if (b.processing) {
        require(d::validate_value(b.processing->settings, b.evaluation.validation.limits));
        require(s::require_capabilities(b.processing->required_capabilities, geometry_.capabilities()));
      }
      run.stage("resolve");
      auto resolved = run.accept(resolver_.resolve(job, b.source, b.evaluation.budget, run.service_control()));
      auto snapshot = *resolved;
      V::Object child_keys;
      for (const auto &binding : node.children) {
        run.step();
        const auto &child = completed.at(binding.node.value);
        d::ContentDigest content;
        require(d::content_digest(child.asset->document, content, b.evaluation.validation));
        snapshot.resources.assets.push_back({{binding.asset, content, child.asset->document.schema_version},
            s::AssetOwner(child.asset, &child.asset->document)});
        // Payloads accompany the document; retain immutable ownership, merge
        // identical IDs only, and fail explicitly on an ambiguous namespace.
        for (const auto &resource : child.asset->resources.resources) {
          auto found = std::find_if(snapshot.resources.resources.begin(), snapshot.resources.resources.end(),
              [&](const auto &r) { return r.description.id == resource.description.id; });
          if (found == snapshot.resources.resources.end()) snapshot.resources.resources.push_back(resource);
          else {
            const auto &a = found->description; const auto &c = resource.description;
            if (a.content.hex != c.content.hex || a.byte_length != c.byte_length || a.scalar_type != c.scalar_type ||
                a.element_count != c.element_count || a.components != c.components)
              fail(E::invalid_input, "conflicting child resource ID", {id, binding.node.value});
          }
        }
        child_keys.emplace(binding.asset.value, V::Array{child.key.name_space, child.key.content.hex});
      }
      auto frozen = freeze(snapshot, b, run);
      auto key = cache_key(b, frozen, child_keys, run);
      run.check();
      std::shared_ptr<const s::AssetProduct> asset;
      auto prior = prototypes.find(key.content.hex);
      if (prior != prototypes.end()) { asset = prior->second; ++run.product.deduplicated; }
      else {
        if (cache_) {
          run.stage("cache.lookup");
          asset = run.accept(cache_->lookup(job, key, run.service_control()))->asset;
          if (asset) ++run.product.cache_hits;
        }
        if (!asset) {
          run.stage("author");
          auto recipe = run.accept(host_.author(job, snapshot, b.authoring, b.evaluation.budget, run.service_control()));
          require(s::validate(*recipe, snapshot.resources, b.evaluation.validation));
          require(s::require_capabilities(recipe->required_capabilities, geometry_.capabilities()));
          run.stage("evaluate");
          asset = run.accept(geometry_.evaluate(job, *recipe, snapshot.resources, b.evaluation, run.service_control()));
          if (b.processing) {
            run.validate_asset(*asset, b.evaluation);
            run.stage("process");
            asset = run.accept(geometry_.process(job, *asset, *b.processing, b.evaluation, run.service_control()));
          }
          ++run.product.generated;
        }
        run.account(*asset, b.evaluation);
        prototypes.emplace(key.content.hex, asset);
      }
      NodeProduct output{node.id, key, asset};
      completed.emplace(id, output); run.product.nodes.push_back(std::move(output));
    }
    for (const auto &root : roots) run.product.roots.push_back(completed.at(root));
    require(run.monitor.report({1, "complete"})); run.check();
    auto result = GraphResult::success(job, std::move(run.product));
    // This is the publication linearization point. No callback/service call is
    // made under this lock. Failures leave the previous immutable owner intact.
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (active_ != activation || !(activation->job == job)) fail(E::stale_result, "superseded before publication");
      if ((control.cancellation && control.cancellation->load(std::memory_order_relaxed)) ||
          activation->cancellation.token()->load(std::memory_order_relaxed))
        fail(E::cancelled, "cancelled before publication");
      published_ = result.value();
    }
    return result;
  } catch (const Failure &e) { return GraphResult::failure(job, e.diagnostics); }
    catch (const std::exception &e) { return GraphResult::failure(job, E::backend_failure, e.what()); }
    catch (...) { return GraphResult::failure(job, E::backend_failure, "build service threw"); }
}
d::ValidationResult legacy_cache_key(const d::LegacyHash &legacy, s::CacheKey &key) {
  if (legacy.name_space != "matter.source.v1" && legacy.name_space != "matter.resolved.v1")
    return {{{d::Severity::error, E::invalid_input, "unknown legacy Matter hash namespace", {}, {}, {}}}};
  // CacheKey::content is still a valid SHA-256 value. The legacy namespace and
  // exact uint64 are domain-separated before hashing, never zero-extended.
  key = {"matter.legacy." + legacy.name_space, d::sha256(legacy.name_space + ":" + hex64(legacy.value))};
  return {};
}
} // namespace matter::build
