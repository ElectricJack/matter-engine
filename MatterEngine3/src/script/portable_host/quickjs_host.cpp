#include <matter/portable_host/quickjs_host.h>
#include "embedded_modules.h"
#include <quickjs.h>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace matter::portable_host {
namespace {
namespace d = contracts;
namespace s = services;
using E = d::ErrorCode;
struct Failure {};
struct Value {
  JSContext *ctx;
  JSValue value;
  Value(JSContext *c, JSValue v) : ctx(c), value(v) {}
  ~Value() { JS_FreeValue(ctx, value); }
  Value(const Value &) = delete;
  Value &operator=(const Value &) = delete;
  JSValue take() { auto v = value; value = JS_UNDEFINED; return v; }
};
struct CString {
  JSContext *ctx;
  size_t size = 0;
  const char *value;
  CString(JSContext *c, JSValueConst v) : ctx(c), value(JS_ToCStringLen(c, &size, v)) {}
  ~CString() { if (value) JS_FreeCString(ctx, value); }
};
struct Module {
  std::string_view source;
  const s::SourceModule *supplied = nullptr;
};
struct alignas(std::max_align_t) Allocation { size_t bytes; };
bool internal(std::string_view id) { return id.substr(0, 16) == "matter-internal/"; }
bool digest(const d::ContentDigest &v) {
  return v.hex.size() == 64 && std::all_of(v.hex.begin(), v.hex.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
bool module_id(std::string_view id) {
  if (id.empty() || id.size() > 256) return false;
  auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
  if (!alnum(id.front())) return false;
  size_t start = 0;
  for (size_t i = 0; i <= id.size(); ++i) {
    if (i == id.size() || id[i] == '/') {
      auto segment = id.substr(start, i - start);
      if (segment.empty() || segment == "." || segment == "..") return false;
      start = i + 1;
    } else if (!alnum(id[i]) && id[i] != '.' && id[i] != '_' && id[i] != ':' && id[i] != '-') return false;
  }
  return true;
}
std::string hex64(uint64_t n) {
  std::string out(16, '0');
  for (size_t i = 16; i-- > 0;) { out[i] = "0123456789abcdef"[n & 15]; n >>= 4; }
  return out;
}

struct Invocation {
  const QuickJsOptions &options;
  const s::SourceSnapshot &snapshot;
  s::ResourceBudget budget;
  s::JobMonitor monitor;
  s::ResourceUsage usage;
  d::ValidationOptions validation;
  std::map<std::string, Module, std::less<>> modules;
  std::vector<d::Diagnostic> errors;
  struct Rejection { JSValue promise, reason; };
  std::map<void *, Rejection> rejections;
  JSRuntime *runtime = nullptr;
  JSContext *ctx = nullptr;
  bool callback_failed = false;
  bool allocation_failed = false;
  size_t heap_limit = 0, heap_used = 0;

  Invocation(const QuickJsOptions &o, const s::SourceSnapshot &ss,
             const s::ResourceBudget &b, const s::JobControl &control)
      : options(o), snapshot(ss), budget(b), monitor(b, control), validation(o.validation) {
    auto &l = validation.limits;
    const d::ValidationLimits ceiling;
    l.max_document_bytes = std::min({l.max_document_bytes, b.output_bytes, b.working_bytes});
    l.max_resource_bytes = std::min(l.max_resource_bytes, b.output_bytes);
    l.max_nodes = static_cast<uint32_t>(std::min<uint64_t>({l.max_nodes, b.operations, ceiling.max_nodes}));
    l.max_value_nodes = std::min({l.max_value_nodes, b.operations, ceiling.max_value_nodes});
    l.max_vertices = std::min(l.max_vertices, b.vertices);
    l.max_indices = std::min(l.max_indices, b.indices);
    l.max_depth = std::min({l.max_depth, b.dependency_depth, ceiling.max_depth});
    l.max_string_bytes = std::min(l.max_string_bytes, ceiling.max_string_bytes);
    l.max_vertices = std::min(l.max_vertices, ceiling.max_vertices);
    l.max_indices = std::min(l.max_indices, ceiling.max_indices);
    l.max_resource_bytes = std::min(l.max_resource_bytes, ceiling.max_resource_bytes);
  }
  ~Invocation() {
    if (ctx) {
      JS_SetHostPromiseRejectionTracker(runtime, nullptr, nullptr);
      for (auto &r : rejections) { JS_FreeValue(ctx, r.second.promise); JS_FreeValue(ctx, r.second.reason); }
      JS_FreeContext(ctx);
    }
    if (runtime) JS_FreeRuntime(runtime);
  }
  void require(d::ValidationResult result) {
    if (!result.ok()) { errors = std::move(result.diagnostics); throw Failure{}; }
  }
  [[noreturn]] void fail(E code, std::string message, std::string id = {},
                         std::vector<std::string> path = {}) {
    if (errors.empty()) {
      auto found = modules.find(id.empty() ? snapshot.entry.value : id);
      std::optional<d::SourceLocation> location;
      if (found != modules.end() && found->second.supplied)
        location = d::SourceLocation{found->second.supplied->identity.uri, 0, 0};
      errors.push_back({d::Severity::error, code, std::move(message), {}, std::move(location), std::move(path)});
    }
    throw Failure{};
  }
  void checkpoint() {
    if (allocation_failed) fail(E::resource_limit, "QuickJS working memory budget exhausted");
    if (callback_failed) fail(E::backend_failure, "host callback failed");
    if (!errors.empty()) throw Failure{};
    if (usage.operations == budget.operations) fail(E::resource_limit, "host work budget exhausted");
    ++usage.operations;
    require(monitor.checkpoint(usage));
  }
  // A budgeted allocator makes OOM observable even when the VM cannot allocate
  // an Error object. No C++ exception may cross these C callback boundaries.
  static void *allocate(void *opaque, size_t size) noexcept {
    auto &self = *static_cast<Invocation *>(opaque);
    if (size > SIZE_MAX - sizeof(Allocation) || size + sizeof(Allocation) > self.heap_limit - self.heap_used) {
      self.allocation_failed = true; return nullptr;
    }
    auto *block = static_cast<Allocation *>(std::malloc(sizeof(Allocation) + size));
    if (!block) { self.allocation_failed = true; return nullptr; }
    block->bytes = size; self.heap_used += sizeof(Allocation) + size;
    return block + 1;
  }
  static void release(void *opaque, void *ptr) noexcept {
    if (!ptr) return;
    auto &self = *static_cast<Invocation *>(opaque);
    auto *block = static_cast<Allocation *>(ptr) - 1;
    self.heap_used -= sizeof(Allocation) + block->bytes;
    std::free(block);
  }
  static void *resize(void *opaque, void *ptr, size_t size) noexcept {
    if (!ptr) return allocate(opaque, size);
    if (!size) { release(opaque, ptr); return nullptr; }
    auto &self = *static_cast<Invocation *>(opaque);
    auto *block = static_cast<Allocation *>(ptr) - 1;
    const auto old_size = block->bytes;
    if (size > SIZE_MAX - sizeof(Allocation) || size > self.heap_limit - (self.heap_used - old_size)) {
      self.allocation_failed = true; return nullptr;
    }
    block = static_cast<Allocation *>(std::realloc(block, sizeof(Allocation) + size));
    if (!block) { self.allocation_failed = true; return nullptr; }
    block->bytes = size; self.heap_used = self.heap_used - old_size + size;
    return block + 1;
  }
  static void *zero_allocate(void *opaque, size_t count, size_t size) noexcept {
    if (size && count > SIZE_MAX / size) {
      static_cast<Invocation *>(opaque)->allocation_failed = true; return nullptr;
    }
    auto *ptr = allocate(opaque, count * size);
    if (ptr) std::memset(ptr, 0, count * size);
    return ptr;
  }
  static size_t allocation_size(const void *ptr) noexcept {
    return ptr ? (static_cast<const Allocation *>(ptr) - 1)->bytes : 0;
  }
  static int interrupt(JSRuntime *, void *opaque) noexcept {
    auto &self = *static_cast<Invocation *>(opaque);
    try { self.checkpoint(); return 0; }
    catch (const Failure &) { return 1; }
    catch (...) { self.callback_failed = true; return 1; }
  }
  void admit_sources() {
    checkpoint();
    if (!options.max_modules || !options.max_source_bytes || options.stack_bytes < 65536 ||
        snapshot.modules.size() > options.max_modules)
      fail(E::resource_limit, "module count or host limits invalid/exceeded");
    if (!digest(snapshot.content)) fail(E::invalid_input, "snapshot needs a SHA-256 identity");
    uint64_t bytes = 0;
    for (const auto &m : embedded::modules) {
      modules.emplace(m.id, Module{std::string_view(reinterpret_cast<const char *>(m.source), m.size), nullptr});
      bytes += m.size;
    }
    if (bytes > options.max_source_bytes) fail(E::resource_limit, "embedded SDK exceeds source byte budget");
    for (const auto &m : snapshot.modules) {
      checkpoint();
      const auto &id = m.identity.id.value;
      if (!module_id(id) || internal(id) || id == "matter:authoring" || !modules.emplace(id, Module{m.source_utf8, &m}).second)
        fail(E::invalid_input, "invalid, reserved or duplicate module ID", id);
      if (m.source_utf8.size() > options.max_source_bytes || bytes > options.max_source_bytes - m.source_utf8.size())
        fail(E::resource_limit, "source byte budget exceeded", id);
      bytes += m.source_utf8.size();
      auto text_limits = validation.limits;
      text_limits.max_string_bytes = static_cast<uint32_t>(std::min<uint64_t>(options.max_source_bytes, UINT32_MAX));
      require(d::validate_value(m.source_utf8, text_limits));
      require(d::validate_value(d::Value::Array{m.identity.uri, m.identity.revision}, validation.limits));
      if (m.source_utf8.find('\0') != std::string::npos || m.identity.uri.empty() || m.identity.revision.empty() ||
          m.identity.uri.find('\0') != std::string::npos)
        fail(E::invalid_input, "source text/identity must be nonempty, identified UTF-8 without NUL", id);
      if (m.identity.content && (!digest(*m.identity.content) || d::sha256(m.source_utf8).hex != m.identity.content->hex))
        fail(E::invalid_input, "module source digest mismatch", id);
      std::set<std::string> legacy_names;
      for (const auto &hash : m.identity.legacy_hashes)
        if ((hash.name_space != "matter.source.v1" && hash.name_space != "matter.resolved.v1") ||
            !legacy_names.insert(hash.name_space).second)
          fail(E::invalid_input, "invalid legacy source hash namespace", id);
      if (m.dependencies.size() > options.max_modules) fail(E::resource_limit, "module dependency count exceeded", id);
    }
    auto entry = modules.find(snapshot.entry.value);
    if (entry == modules.end() || !entry->second.supplied)
      fail(E::missing_dependency, "entry module is absent from supplied snapshot");
    // Validate the complete discovery snapshot, including currently unused edges.
    std::map<std::string, uint32_t> depths;
    std::set<std::string> active;
    std::vector<std::string> path;
    std::function<uint32_t(const std::string &)> visit = [&](const std::string &id) -> uint32_t {
      checkpoint();
      if (path.size() >= validation.limits.max_depth) fail(E::resource_limit, "module dependency depth exceeded", id, path);
      if (active.count(id)) { auto cycle = path; cycle.push_back(id); fail(E::cycle, "cyclic source dependency snapshot", id, cycle); }
      auto cached = depths.find(id);
      if (cached != depths.end()) return cached->second;
      active.insert(id); path.push_back(id);
      uint32_t depth = 1;
      std::set<std::string> seen;
      for (const auto &dep : modules.at(id).supplied->dependencies) {
        if (!seen.insert(dep.value).second) fail(E::invalid_input, "duplicate source dependency", id, path);
        if (dep.value == "matter:authoring") continue;
        auto found = modules.find(dep.value);
        if (found == modules.end() || !found->second.supplied)
          fail(E::missing_dependency, "declared source dependency is absent: " + dep.value, id, {id, dep.value});
        depth = std::max(depth, 1 + visit(dep.value));
      }
      if (depth > validation.limits.max_depth) fail(E::resource_limit, "module dependency depth exceeded", id, path);
      path.pop_back(); active.erase(id); depths[id] = depth;
      return depth;
    };
    for (const auto &m : snapshot.modules) visit(m.identity.id.value);
    usage.working_bytes = bytes;
    checkpoint();
  }
  std::string resolve(std::string base, std::string name) {
    checkpoint();
    if (name == "matter:authoring") return "matter-internal/index.mjs";
    std::string id = name;
    if (name.rfind("./", 0) == 0 || name.rfind("../", 0) == 0) {
      auto slash = base.rfind('/');
      std::string full = (slash == std::string::npos ? "" : base.substr(0, slash + 1)) + name;
      std::vector<std::string> parts;
      size_t start = 0;
      while (start <= full.size()) {
        auto end = full.find('/', start);
        auto part = full.substr(start, end == std::string::npos ? end : end - start);
        if (part == "..") {
          if (parts.empty()) fail(E::missing_dependency, "import escapes module namespace", base, {base, name});
          parts.pop_back();
        } else if (part != "." && !part.empty()) parts.push_back(part);
        if (end == std::string::npos) break;
        start = end + 1;
      }
      id.clear();
      for (const auto &p : parts) { if (!id.empty()) id += '/'; id += p; }
    }
    auto target = modules.find(id), owner = modules.find(base);
    if (target == modules.end() || owner == modules.end())
      fail(E::missing_dependency, "unresolved snapshot import: " + name, base, {base, id});
    if (owner->second.supplied) {
      const auto &deps = owner->second.supplied->dependencies;
      if (!target->second.supplied || std::none_of(deps.begin(), deps.end(), [&](const auto &d) { return d.value == id; }))
        fail(E::missing_dependency, "import was not declared in frozen snapshot: " + name, base, {base, id});
    }
    return id;
  }
  static char *normalize(JSContext *c, const char *base, const char *name, void *opaque) noexcept {
    auto &self = *static_cast<Invocation *>(opaque);
    try {
      auto id = self.resolve(base, name);
      auto *out = static_cast<char *>(js_malloc(c, id.size() + 1));
      if (!out) { JS_ThrowOutOfMemory(c); return nullptr; }
      std::memcpy(out, id.c_str(), id.size() + 1);
      return out;
    } catch (const Failure &) { JS_ThrowReferenceError(c, "snapshot import rejected"); }
      catch (...) { self.callback_failed = true; JS_ThrowOutOfMemory(c); }
    return nullptr;
  }
  static JSModuleDef *load(JSContext *c, const char *name, void *opaque) noexcept {
    auto &self = *static_cast<Invocation *>(opaque);
    try {
      self.checkpoint();
      auto it = self.modules.find(name);
      if (it == self.modules.end()) self.fail(E::missing_dependency, "missing module", name);
      auto source = it->second.source;
      Value compiled(c, JS_Eval(c, source.data(), source.size(), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY));
      if (JS_IsException(compiled.value)) return nullptr;
      return static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(compiled.value));
    } catch (const Failure &) { JS_ThrowReferenceError(c, "snapshot module rejected"); }
      catch (...) { self.callback_failed = true; JS_ThrowOutOfMemory(c); }
    return nullptr;
  }
  static JSValue utf8(JSContext *c, JSValueConst, int argc, JSValueConst *argv) noexcept {
    if (argc != 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(c, "UTF-8 string required");
    CString text(c, argv[0]);
    if (!text.value) return JS_EXCEPTION;
    return JS_NewUint8ArrayCopy(c, reinterpret_cast<const uint8_t *>(text.value), text.size);
  }
  static JSValue sha(JSContext *c, JSValueConst, int argc, JSValueConst *argv) noexcept {
    auto &self = *static_cast<Invocation *>(JS_GetContextOpaque(c));
    try {
      self.checkpoint();
      if (argc != 1) return JS_ThrowTypeError(c, "SHA-256 bytes required");
      size_t size = 0;
      const auto *bytes = JS_GetUint8Array(c, &size, argv[0]);
      if (!bytes) return JS_EXCEPTION;
      if (size > self.validation.limits.max_document_bytes) self.fail(E::resource_limit, "digest byte budget exceeded");
      auto hash = d::sha256(std::string_view(reinterpret_cast<const char *>(bytes), size));
      uint8_t output[32];
      auto nibble = [](char x) { return x <= '9' ? x - '0' : x - 'a' + 10; };
      for (size_t i = 0; i < 32; ++i) output[i] = static_cast<uint8_t>(nibble(hash.hex[i * 2]) * 16 + nibble(hash.hex[i * 2 + 1]));
      self.checkpoint();
      return JS_NewUint8ArrayCopy(c, output, 32);
    } catch (const Failure &) { return JS_ThrowInternalError(c, "digest interrupted"); }
      catch (...) { self.callback_failed = true; return JS_ThrowOutOfMemory(c); }
  }
  static void rejection(JSContext *c, JSValueConst promise, JSValueConst reason, bool handled, void *opaque) noexcept {
    auto &self = *static_cast<Invocation *>(opaque);
    try {
      auto key = JS_VALUE_GET_PTR(promise);
      auto found = self.rejections.find(key);
      if (found != self.rejections.end()) {
        JS_FreeValue(c, found->second.promise); JS_FreeValue(c, found->second.reason); self.rejections.erase(found);
      }
      if (!handled) {
        auto inserted = self.rejections.emplace(key, Rejection{JS_UNDEFINED, JS_UNDEFINED});
        inserted.first->second = {JS_DupValue(c, promise), JS_DupValue(c, reason)};
      }
    } catch (...) { self.callback_failed = true; }
  }
  std::string text_property(JSValueConst object, const char *key) {
    Value field(ctx, JS_GetPropertyStr(ctx, object, key));
    if (!JS_IsString(field.value)) return {};
    CString text(ctx, field.value);
    return text.value ? std::string(text.value, std::min<size_t>(text.size, validation.limits.max_string_bytes)) : std::string{};
  }
  uint32_t integer_property(JSValueConst object, const char *key) {
    Value field(ctx, JS_GetPropertyStr(ctx, object, key));
    double n = 0;
    if (!JS_IsNumber(field.value) || JS_ToFloat64(ctx, &n, field.value) < 0 || !(n >= 0 && n <= UINT32_MAX)) return 0;
    return static_cast<uint32_t>(n);
  }
  [[noreturn]] void exception(JSValueConst error) {
    if (!errors.empty()) throw Failure{};
    checkpoint();
    Value keep_alive(ctx, JS_DupValue(ctx, error));
    d::Diagnostic diagnostic;
    diagnostic.code = E::backend_failure;
    diagnostic.message = text_property(error, "message");
    if (diagnostic.message.empty() && JS_IsString(error)) {
      CString message(ctx, error);
      if (message.value) diagnostic.message.assign(message.value, std::min<size_t>(message.size, validation.limits.max_string_bytes));
    }
    if (diagnostic.message.empty()) diagnostic.message = "JavaScript authoring failed";
    if (diagnostic.message.find("out of memory") != std::string::npos || diagnostic.message.find("stack overflow") != std::string::npos ||
        diagnostic.message.find("Maximum call stack size exceeded") != std::string::npos ||
        diagnostic.message.find("budget") != std::string::npos)
      diagnostic.code = E::resource_limit;
    auto stack = text_property(error, "stack");
    Value detail(ctx, JS_GetPropertyStr(ctx, error, "diagnostic"));
    if (JS_IsObject(detail.value)) {
      auto code = text_property(detail.value, "code");
      for (E known : {E::invalid_input, E::unsupported_capability, E::unsupported_version, E::missing_dependency,
                     E::cycle, E::resource_limit, E::cancelled, E::stale_result, E::backend_failure})
        if (code == d::code_name(known)) diagnostic.code = known;
      auto message = text_property(detail.value, "message");
      if (!message.empty()) diagnostic.message = std::move(message);
      diagnostic.node_id = text_property(detail.value, "node_id");
      Value location(ctx, JS_GetPropertyStr(ctx, detail.value, "source"));
      if (JS_IsObject(location.value)) {
        auto uri = text_property(location.value, "uri");
        if (!uri.empty()) diagnostic.source = d::SourceLocation{uri, integer_property(location.value, "line"), integer_property(location.value, "column")};
      }
      Value path(ctx, JS_GetPropertyStr(ctx, detail.value, "dependency_path"));
      if (JS_IsArray(path.value)) {
        int64_t size = 0;
        if (JS_GetLength(ctx, path.value, &size) == 0)
          for (uint32_t i = 0; i < std::min<int64_t>(size, budget.dependency_depth); ++i) {
            Value item(ctx, JS_GetPropertyUint32(ctx, path.value, i));
            if (JS_IsString(item.value)) {
              CString text(ctx, item.value);
              if (text.value) diagnostic.dependency_path.emplace_back(text.value, std::min<size_t>(text.size, validation.limits.max_string_bytes));
            }
          }
      }
    }
    if (!diagnostic.source) {
      size_t best = std::string::npos;
      for (const auto &m : snapshot.modules) {
        auto pos = stack.find(m.identity.id.value + ':');
        if (pos == std::string::npos || pos >= best) continue;
        best = pos;
        size_t at = pos + m.identity.id.value.size() + 1;
        auto number = [&]() {
          uint64_t n = 0;
          while (at < stack.size() && stack[at] >= '0' && stack[at] <= '9') {
            n = std::min<uint64_t>(UINT32_MAX, n * 10 + static_cast<unsigned>(stack[at++] - '0'));
          }
          return static_cast<uint32_t>(n);
        };
        auto line = number();
        uint32_t column = 0;
        if (at < stack.size() && stack[at] == ':') { ++at; column = number(); }
        diagnostic.source = d::SourceLocation{m.identity.uri, line, column};
      }
    }
    if (!diagnostic.source) diagnostic.source = d::SourceLocation{modules.at(snapshot.entry.value).supplied->identity.uri, 0, 0};
    checkpoint();
    errors.push_back(std::move(diagnostic));
    throw Failure{};
  }
  void checked(JSValueConst value) {
    if (JS_IsException(value)) { Value error(ctx, JS_GetException(ctx)); exception(error.value); }
    checkpoint();
  }
  JSValue settle(JSValueConst value) {
    checked(value);
    while (JS_IsJobPending(runtime)) {
      checkpoint();
      JSContext *job_context = nullptr;
      if (JS_ExecutePendingJob(runtime, &job_context) < 0) {
        Value error(ctx, JS_GetException(job_context ? job_context : ctx)); exception(error.value);
      }
    }
    checkpoint();
    if (JS_IsPromise(value)) {
      auto status = JS_PromiseState(ctx, value);
      if (status == JS_PROMISE_PENDING) fail(E::missing_dependency, "unsettled authoring promise: no external event source is available");
      Value result(ctx, JS_PromiseResult(ctx, value));
      if (status == JS_PROMISE_REJECTED) exception(result.value);
      if (!rejections.empty()) exception(rejections.begin()->second.reason);
      return result.take();
    }
    if (!rejections.empty()) exception(rejections.begin()->second.reason);
    return JS_DupValue(ctx, value);
  }
  JSValue evaluate_module(const std::string &id) {
    const auto &source = modules.at(id).source;
    Value compiled(ctx, JS_Eval(ctx, source.data(), source.size(), id.c_str(), JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY));
    checked(compiled.value);
    auto *module = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(compiled.value));
    Value result(ctx, JS_EvalFunction(ctx, compiled.take()));
    Value resolved(ctx, settle(result.value));
    Value ns(ctx, JS_GetModuleNamespace(ctx, module));
    checked(ns.value);
    return ns.take();
  }
  JSValue parse(const d::Value &value) {
    std::string json;
    require(d::canonical_json(value, json, validation.limits));
    Value result(ctx, JS_ParseJSON(ctx, json.c_str(), json.size(), "matter-input"));
    checked(result.value);
    return result.take();
  }
  d::RecipeDocument run(const s::AuthoringRequest &request) {
    admit_sources();
    require(d::validate_value(request.parameters, validation.limits));
    if (!std::holds_alternative<d::Value::Object>(request.parameters.data)) fail(E::invalid_input, "authoring parameters must be an object");
    heap_limit = static_cast<size_t>(std::min<uint64_t>(budget.working_bytes - usage.working_bytes, SIZE_MAX));
    const JSMallocFunctions allocator{zero_allocate, allocate, release, resize, allocation_size};
    runtime = JS_NewRuntime2(&allocator, this);
    if (!runtime) fail(E::resource_limit, "cannot allocate QuickJS runtime");
    JS_SetMaxStackSize(runtime, options.stack_bytes);
    JS_SetCanBlock(runtime, false);
    JS_SetInterruptHandler(runtime, interrupt, this);
    JS_SetModuleLoaderFunc(runtime, normalize, load, this);
    JS_SetHostPromiseRejectionTracker(runtime, rejection, this);
    ctx = JS_NewContext(runtime);
    if (!ctx) fail(E::resource_limit, "cannot allocate QuickJS context within working budget");
    JS_SetContextOpaque(ctx, this);
    Value global(ctx, JS_GetGlobalObject(ctx));
    Value utf8_fn(ctx, JS_NewCFunction(ctx, utf8, "encode", 1));
    checked(utf8_fn.value);
    if (JS_SetPropertyStr(ctx, global.value, "__matterUtf8", utf8_fn.take()) < 0)
      fail(E::resource_limit, "cannot install UTF-8 adapter");
    auto setup = modules.at("matter-internal/setup.js").source;
    Value setup_result(ctx, JS_Eval(ctx, setup.data(), setup.size(), "matter-internal/setup.js", JS_EVAL_TYPE_GLOBAL));
    checked(setup_result.value);
    Value bridge(ctx, evaluate_module("matter-internal/bridge.mjs"));
    Value prepare(ctx, JS_GetPropertyStr(ctx, bridge.value, "prepare"));
    Value serialize(ctx, JS_GetPropertyStr(ctx, bridge.value, "serialize"));
    checked(prepare.value); checked(serialize.value);
    // Capture trusted bridge functions before user module initialization.
    const auto &identity = modules.at(snapshot.entry.value).supplied->identity;
    d::RecipeDocument provenance; provenance.source = identity;
    auto recipe_value = d::to_value(provenance);
    auto source = std::get<d::Value::Object>(recipe_value.data).at("source");
    const auto &l = validation.limits;
    d::Value limits = d::Value::Object{{"maxDepth", l.max_depth}, {"maxValues", static_cast<double>(l.max_value_nodes)},
      {"maxNodes", l.max_nodes}, {"maxBytes", static_cast<double>(l.max_document_bytes)}, {"maxStringBytes", l.max_string_bytes},
      {"maxResourceBytes", static_cast<double>(l.max_resource_bytes)}, {"maxVertices", static_cast<double>(l.max_vertices)},
      {"maxIndices", static_cast<double>(l.max_indices)}};
    d::Value::Array caps;
    for (const auto &c : validation.supported_capabilities) caps.emplace_back(d::Value::Object{{"name", c.name}, {"version", c.version}});
    Value input(ctx, parse(d::Value::Object{{"source", source}, {"seed", hex64(request.seed)}, {"parameters", request.parameters},
      {"limits", limits}, {"supportedCapabilities", std::move(caps)}}));
    Value js_limits(ctx, parse(limits));
    Value digest_fn(ctx, JS_NewCFunction(ctx, sha, "sha256Bytes", 1));
    checked(digest_fn.value);
    JSValueConst prepare_args[] = {input.value, digest_fn.value};
    Value context(ctx, JS_Call(ctx, prepare.value, JS_UNDEFINED, 2, prepare_args));
    checked(context.value);
    require(monitor.report({0.25, "authoring"}));
    Value entry(ctx, evaluate_module(snapshot.entry.value));
    Value generate(ctx, JS_GetPropertyStr(ctx, entry.value, "default"));
    checked(generate.value);
    if (!JS_IsFunction(ctx, generate.value)) fail(E::invalid_input, "entry must default-export an authoring function");
    Value invoked(ctx, JS_Call(ctx, generate.value, JS_UNDEFINED, 1, &context.value));
    Value result(ctx, settle(invoked.value));
    JSValueConst serialize_args[] = {result.value, js_limits.value};
    Value encoded(ctx, JS_Call(ctx, serialize.value, JS_UNDEFINED, 2, serialize_args));
    Value settled_encoding(ctx, settle(encoded.value));
    CString json(ctx, encoded.value);
    if (!json.value) { Value error(ctx, JS_GetException(ctx)); exception(error.value); }
    usage.output_bytes = json.size;
    checkpoint();
    d::RecipeDocument recipe;
    require(d::decode_recipe(std::string_view(json.value, json.size), recipe, validation));
    require(monitor.report({1.0, "validated recipe"}));
    checkpoint();
    return recipe;
  }
};

class Host final : public s::AuthoringHost {
  QuickJsOptions options_;
public:
  explicit Host(QuickJsOptions o) : options_(std::move(o)) {}
  std::vector<d::Capability> capabilities() const override { return options_.validation.supported_capabilities; }
  s::RecipeResult author(const s::JobEnvelope &envelope, const s::SourceSnapshot &snapshot,
                         const s::AuthoringRequest &request, const s::ResourceBudget &budget,
                         const s::JobControl &control) override {
    try {
      for (auto valid : {s::validate(envelope), s::validate(budget), s::require_capabilities(request.required_capabilities, capabilities())})
        if (!valid.ok()) return s::RecipeResult::failure(envelope, std::move(valid.diagnostics));
      Invocation invocation(options_, snapshot, budget, control);
      try { return s::RecipeResult::success(envelope, invocation.run(request)); }
      catch (const Failure &) { return s::RecipeResult::failure(envelope, std::move(invocation.errors)); }
    } catch (const std::bad_alloc &) {
      return s::RecipeResult::failure(envelope, E::resource_limit, "host allocation failed");
    } catch (const std::exception &e) {
      return s::RecipeResult::failure(envelope, E::backend_failure, e.what());
    } catch (...) {
      return s::RecipeResult::failure(envelope, E::backend_failure, "host callback failed");
    }
  }
};
} // namespace
std::unique_ptr<s::AuthoringHost> make_quickjs_host(QuickJsOptions options) {
  return std::make_unique<Host>(std::move(options));
}
d::ContentDigest quickjs_sdk_content() {
  std::string sources = "matter.host.quickjs.v1\n";
  for (const auto &m : embedded::modules) {
    sources += m.id; sources += '\0'; sources += std::to_string(m.size); sources += '\0';
    sources.append(reinterpret_cast<const char *>(m.source), m.size);
  }
  return d::sha256(sources);
}
} // namespace matter::portable_host
