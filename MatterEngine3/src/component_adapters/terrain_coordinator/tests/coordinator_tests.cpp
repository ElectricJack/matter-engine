#include <matter/terrain_coordinator/coordinator.h>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>

namespace t = matter::terrain_coordinator;
namespace s = matter::services;
namespace d = matter::contracts;
using E = d::ErrorCode;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + " " #x); } while (false)
static bool valid(const d::ValidationResult &v) {
  if (!v.ok()) for (const auto &e : v.diagnostics) std::cerr << d::code_name(e.code) << ": " << e.message << '\n';
  return v.ok();
}
static bool is_error(const d::ValidationResult &v, E code) {
  for (const auto &e : v.diagnostics) if (e.code == code) return !v.ok();
  return false;
}
static std::vector<d::Capability> caps() { return {{"matter.terrain.generate", 1}, {"fixture.collision", 1}}; }
static t::Options options() {
  t::Options o;
  o.implementation = d::sha256("fixture-generator-v1");
  o.generation_capabilities = caps();
  return o;
}
static s::TerrainRequest request(std::string id = "region.a", std::string revision = "1") {
  s::TerrainRequest r;
  r.job = {"caller.request", revision, 1};
  r.field = {{"field.plane"}, "fixture://plane", revision, d::sha256("plane-" + revision), {}};
  r.region = {id};
  r.sample_domain = {{0, 0, 0}, {8, 8, 8}};
  r.cell_counts = {{4, 4, 4}};
  r.required_capabilities = {{"matter.terrain.generate", 1}};
  return r;
}
static s::TerrainResult product(const s::TerrainRequest &r) {
  d::AssetDocument a;
  a.id = {"terrain.tile"};
  a.provenance = r.field;
  a.required_capabilities = {{"matter.asset.core", 1}, {"matter.material.pbr", 1}};
  a.bounds = {{0, 0, 0}, {1, 1, 0}};
  d::MaterialDescription material;
  material.id = {"ground"};
  a.materials = {material};
  d::MeshPrototype mesh;
  mesh.id = {"triangle"};
  mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  mesh.indices = {0, 1, 2};
  mesh.submeshes = {{0, 3, {"ground"}}};
  mesh.bounds = a.bounds;
  a.meshes = {mesh};
  s::TerrainProduct p;
  p.job = r.job; p.field_content = *r.field.content;
  p.region = r.region; p.region_key = r.region_key; p.origin_metres = r.origin_metres;
  p.cell_counts = r.cell_counts; p.detail_level = r.detail_level; p.boundaries = r.boundaries;
  p.bounds = a.bounds; p.geometry = std::make_shared<const d::AssetDocument>(std::move(a));
  p.products = {{{"fixture.collision", 1}, true, d::Value::Object{{"mesh", "triangle"}}}};
  CHECK(valid(s::validate(p, r, caps())));
  return s::TerrainResult::success(r.job, std::move(p));
}
struct Generator : s::TerrainService {
  unsigned calls = 0;
  std::vector<d::Capability> capabilities() const override { return caps(); }
  s::TerrainResult generate(const s::TerrainRequest &r, const s::JobControl &) override {
    ++calls; return product(r);
  }
};
// Retains terminal tickets like the production GraphicsService implementation.
// All races are driven explicitly by tests, without a device or sleeps.
struct Graphics : s::GraphicsService {
  struct Ticket {
    s::JobEnvelope job;
    s::UploadRequest input;
    s::JobControl control;
    std::optional<s::GraphicsHandle> resource;
  };
  std::map<uint64_t, Ticket> tickets;
  std::map<uint64_t, s::UploadRequest> resources;
  std::vector<s::GraphicsObservation> demands;
  uint64_t next = 0;
  unsigned submissions = 0, polls = 0;
  bool ready = false, stale_submission = false, stale_completion = false;
  bool fail_upload = false, fail_poll = false, throw_upload = false, throw_poll = false;
  bool fail_release = false;
  std::vector<d::Capability> capabilities() const override { return {{"matter.asset.core", 1}, {"matter.material.pbr", 1}}; }
  s::UploadSubmission upload(const s::JobEnvelope &job, s::UploadRequest input, const s::JobControl &c) override {
    ++submissions;
    if (throw_upload) throw std::runtime_error("upload");
    if (fail_upload) return s::UploadSubmission::failure(job, E::backend_failure, "upload");
    auto n = ++next;
    tickets.emplace(n, Ticket{job, std::move(input), c, {}});
    auto reply = job;
    if (stale_submission) ++reply.generation_epoch;
    return s::UploadSubmission::success(reply, {{77, n, 1}});
  }
  s::UploadResult poll(const s::JobEnvelope &job, s::UploadTicket t) override {
    ++polls;
    if (throw_poll) throw std::runtime_error("poll");
    auto i = tickets.find(t.handle.slot);
    CHECK(i != tickets.end()); CHECK(i->second.job == job);
    if (fail_poll) return s::UploadResult::failure(job, E::backend_failure, "poll");
    if (!ready) return s::UploadResult::success(job, {s::UploadState::pending, {}});
    if (!i->second.resource) {
      auto n = ++next;
      resources.emplace(n, i->second.input);
      i->second.resource = s::GraphicsHandle{77, n, 1};
    }
    auto reply = job;
    if (stale_completion) reply.source_revision = "foreign";
    return s::UploadResult::success(reply, {s::UploadState::ready, *i->second.resource});
  }
  s::GraphicsStatus update_scene(const s::JobEnvelope &j, const s::SceneUpdate &) override { return s::GraphicsStatus::success(j, {}); }
  s::GraphicsStatus set_camera(const s::JobEnvelope &j, const s::CameraState &) override { return s::GraphicsStatus::success(j, {}); }
  s::GraphicsStatus set_lights(const s::JobEnvelope &j, const std::vector<s::LightState> &) override { return s::GraphicsStatus::success(j, {}); }
  std::vector<s::GraphicsObservation> observations() const override { return demands; }
  d::ValidationResult release_resource(s::GraphicsHandle h) override {
    if (fail_release) return {{{d::Severity::error, E::backend_failure, "release", {}, {}, {}}}};
    CHECK(h.owner == 77 && h.generation == 1 && resources.erase(h.slot) == 1); return {};
  }
  d::ValidationResult release_ticket(s::UploadTicket t) override {
    if (fail_release) return {{{d::Severity::error, E::backend_failure, "release", {}, {}, {}}}};
    CHECK(t.handle.owner == 77 && tickets.erase(t.handle.slot) == 1); return {};
  }
};
static t::GenerationWork take(t::Coordinator &c) {
  std::optional<t::GenerationWork> w;
  CHECK(c.take(w).ok()); CHECK(w); return *w;
}
static void finish(t::Coordinator &c) {
  auto w = take(c); CHECK(c.complete(w, product(*w.request)).ok());
}
static void epochs() {
  std::cout << "TC-EPOCH-EDIT TC-EPOCH-EVICT TC-EPOCH-RETRY TC-DEMAND-OFF-ON\n";
  t::MemoryCache cache(8, 2ull << 30);
  t::Coordinator c(options(), &cache);
  CHECK(c.define(request()).ok()); CHECK(c.demand({"region.a"}, {true, true, false}).ok());
  auto old = take(c);
  std::weak_ptr<const s::TerrainRequest> old_inputs = old.request;
  CHECK(c.define(request("region.a", "2")).ok());
  CHECK(old.control.cancellation->load());
  auto current = take(c);
  CHECK(current.request->job.generation_epoch > old.request->job.generation_epoch);
  CHECK(c.complete(current, product(*current.request)).ok());
  auto saved = c.cpu({"region.a"});
  CHECK(is_error(c.complete(old, product(*old.request)), E::stale_result));
  old = {}; CHECK(old_inputs.expired());
  CHECK(c.cpu({"region.a"}) == saved && cache.size() == 1);
  CHECK(is_error(c.complete(current, product(*current.request)), E::stale_result));
  CHECK(c.retry({"region.a"}).ok());
  std::optional<t::GenerationWork> hit;
  CHECK(c.take(hit).ok() && !hit); // Retry uses exact cached inputs with a new envelope.
  CHECK(c.cpu({"region.a"})->job.generation_epoch > saved->job.generation_epoch);
  CHECK(c.define(request("region.a", "3")).ok());
  auto evicted = take(c);
  CHECK(c.evict({"region.a"}).ok());
  CHECK(c.define(request("region.a", "3")).ok());
  CHECK(c.demand({"region.a"}, {true, false, false}).ok());
  auto replacement = take(c);
  CHECK(c.complete(replacement, product(*replacement.request)).ok());
  saved = c.cpu({"region.a"});
  CHECK(is_error(c.complete(evicted, product(*evicted.request)), E::stale_result));
  CHECK(c.cpu({"region.a"}) == saved);
  CHECK(c.define(request("region.a", "4")).ok());
  auto off = take(c);
  CHECK(c.demand({"region.a"}, {}).ok());
  CHECK(c.demand({"region.a"}, {true, false, false}).ok());
  auto on = take(c);
  CHECK(c.complete(on, product(*on.request)).ok());
  CHECK(is_error(c.complete(off, product(*off.request)), E::stale_result));
  CHECK(c.statistics().running == 0);
  Graphics graphics;
  t::Coordinator gpu_only(options(), nullptr, &graphics);
  CHECK(gpu_only.define(request()).ok());
  CHECK(gpu_only.demand({"region.a"}, {false, false, true}).ok());
  auto first = take(gpu_only);
  auto first_result = product(*first.request);
  CHECK(gpu_only.complete(first, first_result).ok());
  graphics.ready = true;
  CHECK(gpu_only.pump_graphics().ok()); CHECK(gpu_only.pump_graphics().ok());
  CHECK(!gpu_only.cpu({"region.a"}));
  CHECK(gpu_only.demand({"region.a"}, {true, false, true}).ok());
  auto again = take(gpu_only);
  CHECK(!(again.request->job == first.request->job));
  CHECK(is_error(gpu_only.complete(first, first_result), E::stale_result));
  CHECK(gpu_only.statistics().running == 1 && !gpu_only.cpu({"region.a"}));
  CHECK(gpu_only.complete(again, product(*again.request)).ok());
}
static void lifetimes() {
  std::cout << "TC-LIFETIME-GPU TC-LIFETIME-COLLISION TC-DISPOSE\n";
  Graphics graphics;
  std::weak_ptr<const s::TerrainProduct> released;
  std::weak_ptr<const d::AssetDocument> geometry;
  t::GenerationWork abandoned;
  {
    t::Coordinator c(options(), nullptr, &graphics);
    CHECK(c.define(request()).ok());
    CHECK(c.demand({"region.a"}, {true, true, true}).ok());
    finish(c);
    auto field = c.definition({"region.a"});
    auto collision = c.collision({"region.a"});
    released = collision;
    geometry = collision->geometry;
    CHECK(c.pump_graphics().ok() && graphics.tickets.size() == 1);
    CHECK(c.drop_cpu({"region.a"}).ok() && !c.cpu({"region.a"}));
    CHECK(c.evict_gpu({"region.a"}).ok());
    CHECK(graphics.tickets.empty() && graphics.resources.empty());
    CHECK(c.definition({"region.a"}) == field && c.collision({"region.a"}) == collision);
    CHECK(c.demand({"region.a"}, {false, true, true}).ok());
    CHECK(c.statistics().queued == 0); // Collision owner also serves a new upload.
    graphics.ready = true;
    CHECK(c.pump_graphics().ok()); CHECK(c.pump_graphics().ok());
    CHECK(c.gpu({"region.a"}) && graphics.tickets.empty() && graphics.resources.size() == 1);
    CHECK(c.evict_gpu({"region.a"}).ok() && graphics.resources.empty());
    CHECK(!geometry.expired());
    CHECK(c.evict({"region.a"}).ok()); CHECK(!released.expired());
    collision.reset(); CHECK(released.expired() && geometry.expired());
    CHECK(c.define(request()).ok()); CHECK(c.demand({"region.a"}, {false, true, true}).ok());
    auto collision_work = take(c);
    CHECK(c.evict_gpu({"region.a"}).ok() && !collision_work.control.cancellation->load());
    CHECK(c.complete(collision_work, product(*collision_work.request)).ok());
    CHECK(c.collision({"region.a"}) && !c.cpu({"region.a"}));
    CHECK(c.demand({"region.a"}, {false, false, true}).ok());
    CHECK(c.cpu({"region.a"}) && !c.collision({"region.a"}) && c.statistics().queued == 0);
    CHECK(c.pump_graphics().ok()); CHECK(c.pump_graphics().ok());
    CHECK(c.gpu({"region.a"}) && !c.cpu({"region.a"}));
    CHECK(c.evict({"region.a"}).ok());
    CHECK(c.define(request()).ok()); CHECK(c.demand({"region.a"}, {true, false, true}).ok());
    finish(c); CHECK(c.pump_graphics().ok());
    CHECK(c.define(request("region.b")).ok()); CHECK(c.demand({"region.b"}, {true, false, false}).ok());
    abandoned = take(c);
  }
  CHECK(graphics.tickets.empty() && graphics.resources.empty());
  CHECK(abandoned.control.cancellation->load());
  CHECK(abandoned.request); // Host still owns its inputs after coordinator destruction.
}
struct WrongCache : t::Cache {
  std::shared_ptr<const t::CacheEntry> entry;
  bool stale = false, fail = false;
  t::CacheResult lookup(const s::JobEnvelope &j, const t::CacheKey &) override {
    if (fail) return t::CacheResult::failure(j, E::backend_failure, "cache");
    auto reply = j; if (stale) ++reply.generation_epoch;
    return t::CacheResult::success(reply, {entry});
  }
  t::Status store(const s::JobEnvelope &j, const t::CacheKey &, std::shared_ptr<const t::CacheEntry>) override {
    return t::Status::success(j, {});
  }
};
static void cache_tests() {
  std::cout << "TC-CACHE-IDENTITY TC-CACHE-ADMISSION TC-CACHE-BOUNDS\n";
  auto r = request();
  t::CacheKey base, key;
  CHECK(t::cache_key(r, options().implementation, base).ok());
  auto changed = [&](s::TerrainRequest v) {
    CHECK(valid(t::cache_key(v, options().implementation, key))); CHECK(key.content.hex != base.content.hex);
  };
  auto v = r; v.job.request_id = "another"; v.job.generation_epoch = UINT64_MAX;
  CHECK(t::cache_key(v, options().implementation, key).ok() && key.content.hex == base.content.hex);
  v = r; v.job.source_revision = "2"; changed(v);
  v = r; v.field.content = d::sha256("changed"); changed(v);
  v = r; v.region_key[0] = 1; changed(v);
  v = r; v.origin_metres[0] = 1e12; changed(v);
  v = r; v.sample_domain.max = {16, 16, 16}; changed(v);
  v = r; v.cell_counts = {{2, 2, 2}}; changed(v);
  v = r; v.detail_level = 1; changed(v);
  v = r; ++v.options.budget.operations; changed(v);
  v = r; v.options.algorithms.push_back({"algorithm.other", 1}); changed(v);
  v = r; v.required_capabilities.push_back({"boundary.fixture", 1});
  v.boundaries.push_back({s::BoundaryFace::positive_x, {{1, 0, 0}}, 2, 2, {"boundary.fixture", 1}, {}}); changed(v);
  CHECK(t::cache_key(r, d::sha256("other-implementation"), key).ok() && key.content.hex != base.content.hex);
  v = r;
  v.resolved.resources.push_back({{{"field.bytes"}, d::sha256("A"), d::ScalarType::u8, 1, 1, 1},
      std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{'A'})});
  changed(v);
  auto bytes_key = key;
  v.resolved.resources[0].description.content = d::sha256("B");
  v.resolved.resources[0].bytes = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{'B'});
  CHECK(t::cache_key(v, options().implementation, key).ok() && key.content.hex != bytes_key.content.hex);
  auto dependency = product(r).value()->geometry;
  d::ContentDigest dependency_digest;
  CHECK(d::content_digest(*dependency, dependency_digest).ok());
  v = r; v.dependencies = {{{"child.asset"}, dependency_digest, {1, 0}}};
  v.resolved.assets = {{v.dependencies[0], dependency}};
  changed(v);
  // Same product metadata cannot hide a different sampling domain in a corrupt cache entry.
  WrongCache wrong;
  v = r; v.sample_domain.max = {16, 16, 16};
  wrong.entry = std::make_shared<const t::CacheEntry>(t::CacheEntry{v, product(v).value()});
  t::Coordinator c(options(), &wrong);
  CHECK(c.define(r).ok()); CHECK(c.demand(r.region, {true, false, false}).ok());
  std::optional<t::GenerationWork> work;
  CHECK(is_error(c.take(work), E::stale_result) && !work && !c.cpu(r.region));
  CHECK(c.retry(r.region).ok()); wrong.entry.reset(); wrong.stale = true;
  CHECK(is_error(c.take(work), E::stale_result));
  CHECK(c.retry(r.region).ok()); wrong.stale = false; wrong.fail = true;
  CHECK(is_error(c.take(work), E::backend_failure));
  constexpr uint64_t reserve = 3 * 64ull * 1024 * 1024;
  t::MemoryCache cache(2, reserve);
  auto entry = std::make_shared<const t::CacheEntry>(t::CacheEntry{r, product(r).value()});
  CHECK(cache.store(r.job, base, entry).ok() && cache.reserved_bytes() == reserve);
  std::weak_ptr<const t::CacheEntry> weak = entry;
  auto lease = cache.lookup(r.job, base);
  entry.reset();
  v = request("region.b");
  CHECK(t::cache_key(v, options().implementation, key).ok());
  CHECK(cache.store(v.job, key, std::make_shared<const t::CacheEntry>(t::CacheEntry{v, product(v).value()})).ok());
  CHECK(cache.size() == 1 && !cache.lookup(r.job, base).value()->entry && !weak.expired());
  cache.clear(); CHECK(cache.size() == 0 && cache.reserved_bytes() == 0);
  t::MemoryCache tiny(1, 1);
  CHECK(!tiny.store(r.job, base, lease.value()->entry).ok() && tiny.size() == 0);
}
static void pressure() {
  std::cout << "TC-BACKPRESSURE TC-OBSERVATION-BOUNDS TC-UPLOAD-BOUNDS\n";
  Graphics graphics;
  auto o = options(); o.limits.regions = 3; o.limits.queued = 1; o.limits.running = 1; o.limits.uploads = 1; o.limits.observations = 2;
  t::Coordinator c(o, nullptr, &graphics);
  CHECK(c.define(request()).ok()); CHECK(c.define(request("region.b")).ok()); CHECK(c.define(request("region.c")).ok());
  CHECK(is_error(c.define(request("region.d")), E::resource_limit));
  CHECK(c.demand({"region.a"}, {true, false, true}).ok());
  CHECK(is_error(c.demand({"region.b"}, {true, false, true}), E::resource_limit));
  auto old = take(c);
  CHECK(c.retry({"region.a"}).ok()); CHECK(old.control.cancellation->load());
  for (unsigned n = 0; n < 25; ++n) CHECK(c.retry({"region.a"}).ok());
  std::optional<t::GenerationWork> work;
  CHECK(c.take(work).ok() && !work && c.statistics().running == 1);
  CHECK(is_error(c.complete(old, product(*old.request)), E::stale_result));
  finish(c);
  CHECK(c.demand({"region.b"}, {true, false, true}).ok()); finish(c);
  CHECK(c.pump_graphics().ok() && graphics.tickets.size() == 1);
  graphics.ready = true;
  CHECK(c.pump_graphics().ok() && graphics.tickets.size() == 1);
  CHECK(c.pump_graphics().ok() && graphics.tickets.empty());
  CHECK(c.gpu({"region.a"}) && c.gpu({"region.b"}));
  CHECK(is_error(c.observe({{{"region.a"}, 0, 0, {}}, {{"region.b"}, 0, 0, {}}, {{"region.c"}, 0, 0, {}}}), E::resource_limit));
  CHECK(c.observe({{{"region.c"}, 0, 0, {}}, {{"region.c"}, 0, 0, {}}}).ok() && c.statistics().queued == 1);
  CHECK(c.demand({"region.c"}, {}).ok());
  CHECK(is_error(c.observe({{{"region.c"}, 0, 0, d::Capability{"unknown", 1}}}), E::unsupported_capability));
  CHECK(c.statistics().queued == 0);
}
static void failures() {
  std::cout << "TC-FAILURE-RETRY TC-INVALID-RESULT TC-CAPABILITIES\n";
  t::Coordinator c(options());
  CHECK(c.define(request()).ok()); CHECK(c.demand({"region.a"}, {true, false, false}).ok());
  auto work = take(c);
  auto failed = s::TerrainResult::failure(work.request->job, E::backend_failure, "fixture");
  CHECK(is_error(c.complete(work, failed), E::backend_failure));
  CHECK(c.statistics().running == 0 && !c.cpu({"region.a"}));
  CHECK(c.retry({"region.a"}).ok()); work = take(c);
  auto malformed = product(*work.request);
  auto p = *malformed.value(); ++p.cell_counts[0];
  CHECK(is_error(c.complete(work, s::TerrainResult::success(work.request->job, p)), E::stale_result));
  CHECK(c.retry({"region.a"}).ok()); work = take(c);
  auto foreign = work.request->job; foreign.request_id = "foreign";
  CHECK(is_error(c.complete(work, s::TerrainResult::failure(foreign, E::backend_failure, "foreign")), E::stale_result));
  CHECK(c.statistics().running == 0); // Bad service envelope still returns our receipt's credit.
  CHECK(c.retry({"region.a"}).ok()); finish(c);
  auto before = c.definition({"region.a"}); auto cpu = c.cpu({"region.a"});
  auto bad = request(); bad.required_capabilities.push_back({"unknown", 999});
  CHECK(is_error(c.define(bad), E::unsupported_capability));
  CHECK(c.definition({"region.a"}) == before && c.cpu({"region.a"}) == cpu);
  bad = request(); bad.options.budget.output_bytes = UINT64_MAX;
  CHECK(is_error(c.define(bad), E::resource_limit));
  bad = request(); bad.options.budget.working_bytes = UINT64_MAX;
  CHECK(is_error(c.define(bad), E::resource_limit));
  CHECK(is_error(c.demand({"region.a"}, {true, false, true}), E::unsupported_capability));
  struct Throws : Generator {
    s::TerrainResult generate(const s::TerrainRequest &, const s::JobControl &) override { throw std::runtime_error("generator"); }
  } generator;
  CHECK(c.retry({"region.a"}).ok()); work = take(c);
  CHECK(is_error(c.complete(work, t::execute(work, generator)), E::backend_failure));
  CHECK(c.statistics().running == 0);
}
static void graphics_tests() {
  std::cout << "TC-GRAPHICS-DEMAND TC-GRAPHICS-LATE TC-GRAPHICS-RETRY\n";
  Graphics graphics;
  t::Coordinator c(options(), nullptr, &graphics);
  Generator generator;
  CHECK(c.define(request()).ok());
  graphics.demands = {{{"region.a"}, 8, 0, d::Capability{"matter.terrain.generate", 1}}};
  CHECK(c.observe_graphics().ok() && generator.calls == 0 && c.statistics().queued == 1);
  auto w = take(c); CHECK(generator.calls == 0);
  CHECK(c.complete(w, t::execute(w, generator)).ok() && generator.calls == 1);
  CHECK(c.pump_graphics().ok());
  auto old_ticket = graphics.tickets.begin()->second;
  CHECK(c.define(request("region.a", "2")).ok());
  CHECK(old_ticket.control.cancellation->load() && graphics.tickets.empty());
  finish(c); CHECK(c.pump_graphics().ok());
  graphics.ready = true; graphics.stale_completion = true;
  CHECK(is_error(c.pump_graphics(), E::stale_result));
  CHECK(!c.gpu({"region.a"}) && graphics.resources.empty() && graphics.tickets.empty());
  graphics.stale_completion = false;
  CHECK(c.retry({"region.a"}).ok()); finish(c);
  graphics.stale_submission = true;
  CHECK(is_error(c.pump_graphics(), E::stale_result) && graphics.tickets.empty());
  graphics.stale_submission = false;
  for (unsigned mode = 0; mode < 4; ++mode) {
    CHECK(c.retry({"region.a"}).ok()); finish(c);
    graphics.fail_upload = mode == 0; graphics.throw_upload = mode == 1;
    graphics.fail_poll = mode == 2; graphics.throw_poll = mode == 3;
    auto result = c.pump_graphics();
    if (mode >= 2) { CHECK(result.ok()); result = c.pump_graphics(); }
    CHECK(is_error(result, E::backend_failure));
    CHECK(graphics.tickets.empty() && graphics.resources.empty());
    graphics.fail_upload = graphics.throw_upload = graphics.fail_poll = graphics.throw_poll = false;
  }
  CHECK(c.retry({"region.a"}).ok()); finish(c);
  CHECK(c.pump_graphics().ok()); CHECK(c.pump_graphics().ok());
  CHECK(c.gpu({"region.a"}));
  graphics.fail_release = true;
  CHECK(!c.evict_gpu({"region.a"}).ok() && graphics.resources.size() == 1);
  graphics.fail_release = false;
  CHECK(c.evict_gpu({"region.a"}).ok() && graphics.resources.empty());
}
static void threaded() {
  std::cout << "TC-WORKER-CANCEL TC-WORKER-OWNERSHIP\n";
  struct Blocked : Generator {
    std::promise<void> entered, release;
    s::TerrainResult generate(const s::TerrainRequest &r, const s::JobControl &) override {
      entered.set_value(); release.get_future().wait(); return product(r); // Deliberately ignores cancellation.
    }
  } generator;
  t::Coordinator c(options());
  CHECK(c.define(request()).ok()); CHECK(c.demand({"region.a"}, {true, false, false}).ok());
  auto w = take(c);
  auto running = std::async(std::launch::async, [&] { return t::execute(w, generator); });
  struct Release { std::promise<void> &p; ~Release() { try { p.set_value(); } catch (...) {} } } release{generator.release};
  generator.entered.get_future().wait();
  CHECK(c.define(request("region.a", "2")).ok());
  auto next = take(c); CHECK(c.complete(next, product(*next.request)).ok());
  auto saved = c.cpu({"region.a"});
  generator.release.set_value();
  auto late = running.get();
  CHECK(!late.ok());
  CHECK(is_error({late.diagnostics()}, E::cancelled));
  CHECK(is_error(c.complete(w, late), E::stale_result));
  CHECK(c.cpu({"region.a"}) == saved && c.statistics().running == 0);
}
int main(int argc, char **argv) {
  try {
    CHECK(argc == 2);
    std::string f = argv[1];
    if (f == "epochs") epochs(); else if (f == "lifetimes") lifetimes();
    else if (f == "cache") cache_tests(); else if (f == "pressure") pressure();
    else if (f == "failures") failures(); else if (f == "graphics") graphics_tests();
    else if (f == "thread") threaded(); else CHECK(false);
    std::cout << "PASS " << f << '\n'; return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
