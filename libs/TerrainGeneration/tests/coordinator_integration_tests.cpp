// Coordinator integration: the portable CPU terrain service behind the
// contracts-only terrain coordinator adapter. No renderer, GPU, script host or
// Matter world is created. One subcommand per recorded fixture:
//   adjacent           terrain.example.coordinator_adjacent.v1
//   late_completion    terrain.example.coordinator_late_completion.v1
//   resolution_change  terrain.example.coordinator_resolution_change.v1
//   faults             terrain.example.coordinator_faults.v1
#include "examples/terrain_example_tiles.h"
#include "../../../MatterEngine3/tests/check.h"
#include <matter/terrain_coordinator/coordinator.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace d = matter::contracts;
namespace s = matter::services;
namespace c = matter::terrain_coordinator;
namespace ex = matter::terrain::example;
using E = d::ErrorCode;
using Owner = c::ProductOwner;

namespace {
const d::SemanticId west{"terrain.example.west"}, east{"terrain.example.east"},
    north{"terrain.example.north"};

// Expected refusals name their code; anything else is printed for the record.
bool rejects(const d::ValidationResult &result, E code) {
  const bool found = std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto &e) {
    return e.severity == d::Severity::error && e.code == code;
  });
  if (!found) {
    std::printf("  expected %s, got %s\n", d::code_name(code), result.ok() ? "success" : "");
    for (const auto &e : result.diagnostics) std::printf("    %s: %s\n", d::code_name(e.code), e.message.c_str());
  }
  return found;
}
bool rejects(const s::TerrainResult &result, E code) {
  return rejects(d::ValidationResult{result.diagnostics()}, code);
}

// Resolves each frozen request to the source text it names, as a host would
// before handing work to a worker. Unknown field content is a missing input.
class SourceRouter final : public s::TerrainService {
public:
  void add(const matter::terrain::EncodedField &field) {
    services_.emplace_back(d::sha256(field.text).hex,
                           std::make_unique<matter::terrain::CpuTerrain>(field, matter::terrain::ChunkSettings{ex::tile_size_metres}));
  }
  std::vector<d::Capability> capabilities() const override { return matter::terrain::mesh_capabilities(); }
  s::TerrainResult generate(const s::TerrainRequest &request, const s::JobControl &control) override {
    ++runs;
    for (auto &entry : services_)
      if (request.field.content && entry.first == request.field.content->hex)
        return entry.second->generate(request, control);
    return s::TerrainResult::failure(request.job, E::missing_dependency, "no source for frozen field identity");
  }
  std::atomic<int> runs{0};
private:
  std::vector<std::pair<std::string, std::unique_ptr<s::TerrainService>>> services_;
};

// A backend that throws a fixed number of times before delegating.
class Throwing final : public s::TerrainService {
public:
  Throwing(s::TerrainService &inner, int throws) : inner_(inner), throws_(throws) {}
  std::vector<d::Capability> capabilities() const override { return inner_.capabilities(); }
  s::TerrainResult generate(const s::TerrainRequest &r, const s::JobControl &control) override {
    if (throws_ > 0) {
      --throws_;
      throw std::runtime_error("injected terrain backend fault");
    }
    return inner_.generate(r, control);
  }
private:
  s::TerrainService &inner_;
  int throws_;
};

// Later steps dereference the taken work, so a missing dispatch ends the fixture.
bool dispatched(bool taken, const char *what) {
  CHECK(taken, what);
  return taken;
}

c::Options coordinator_options() {
  c::Options o;
  o.implementation = d::sha256("matter.terrain.example.implementation/cpu-contour-profile-v1");
  o.generation_capabilities = matter::terrain::mesh_capabilities();
  return o;
}

// Every taken item reaches complete(); returns how many generator runs it took.
int settle(c::Coordinator &coordinator, s::TerrainService &service) {
  int runs = 0;
  for (std::optional<c::GenerationWork> work;;) {
    CHECK(coordinator.take(work).ok(), "take on the application lane succeeds");
    if (!work) return runs;
    ++runs;
    const auto completed = coordinator.complete(*work, c::execute(*work, service));
    CHECK(completed.ok(), "a current completion is admitted and published");
  }
}

d::ValidationResult define(c::Coordinator &coordinator, const char *tile, const matter::terrain::EncodedField &f,
                           const std::string &revision = "source.1", float spacing = 0,
                           float north_spacing = 0) {
  auto request = ex::request(ex::tile(tile), f, revision, spacing);
  // World policy states the neighbour's current detail in every envelope.
  for (auto &b : request.boundaries)
    if (north_spacing > 0 && b.neighbor_key == ex::tile("north").key) b.neighbor_spacing_metres = north_spacing;
  return coordinator.define(std::move(request));
}

std::string seam_densities(const Owner &product, const std::array<int32_t, 3> &neighbor) {
  if (!product) return {};
  auto boundary = std::find_if(product->boundaries.begin(), product->boundaries.end(),
                               [&](const auto &b) { return b.neighbor_key == neighbor && b.seam_resource; });
  if (boundary == product->boundaries.end()) return {};
  for (const auto &resource : product->resources.resources) {
    if (resource.description.id != *boundary->seam_resource) continue;
    d::Value record;
    if (!d::parse_json(std::string(resource.bytes->begin(), resource.bytes->end()), record).ok()) return {};
    const auto *fields = std::get_if<d::Value::Object>(&record.data);
    auto densities = fields ? fields->find("densities") : d::Value::Object::const_iterator{};
    std::string text;
    if (!fields || densities == fields->end() || !d::canonical_json(densities->second, text).ok()) return {};
    return text;
  }
  return {};
}

// Mesh edges whose two endpoints lie on the world plane, as ordered pairs of
// world points; the integer origins keep the float-to-double sums exact.
std::set<std::array<double, 6>> seam_edges(const Owner &product, int axis, double plane) {
  std::set<std::array<double, 6>> found;
  if (!product || product->geometry->meshes.empty()) return found;
  const auto &mesh = product->geometry->meshes.front();
  auto world = [&](uint32_t index) {
    const auto &p = mesh.positions[index];
    return std::array<double, 3>{{product->origin_metres[0] + p.x, product->origin_metres[1] + p.y,
                                  product->origin_metres[2] + p.z}};
  };
  for (size_t tri = 0; tri + 2 < mesh.indices.size(); tri += 3)
    for (size_t k = 0; k < 3; ++k) {
      auto a = world(mesh.indices[tri + k]), b = world(mesh.indices[tri + (k + 1) % 3]);
      if (a[size_t(axis)] != plane || b[size_t(axis)] != plane) continue;
      if (b < a) std::swap(a, b);
      found.insert({{a[0], a[1], a[2], b[0], b[1], b[2]}});
    }
  return found;
}

void same_seam(const Owner &a, const Owner &b, int axis, double plane, const std::string &what) {
  const auto da = seam_densities(a, b ? b->region_key : std::array<int32_t, 3>{});
  const auto db = seam_densities(b, a ? a->region_key : std::array<int32_t, 3>{});
  CHECK(!da.empty() && da == db, (what + ": published seam densities agree").c_str());
  const auto ea = seam_edges(a, axis, plane), eb = seam_edges(b, axis, plane);
  CHECK(!ea.empty() && ea == eb, (what + ": published meshes share every seam edge").c_str());
  std::printf("%s: %zu shared seam edges\n", what.c_str(), ea.size());
}

void adjacent() {
  std::puts("fixture terrain.example.coordinator_adjacent.v1");
  const auto field = ex::field();
  SourceRouter router;
  router.add(field);
  c::Coordinator coordinator(coordinator_options());
  for (const auto *tile : {"west", "east", "north"}) {
    CHECK(define(coordinator, tile, field).ok(), "example tile definition admitted");
    CHECK(coordinator.demand({std::string("terrain.example.") + tile}, {true, true, false}).ok(),
          "CPU and collision demand admitted");
  }
  CHECK(coordinator.statistics().regions == 3 && coordinator.statistics().queued == 3, "three regions queued");
  // Dispatch all three to worker threads, then complete out of order on the lane.
  std::vector<c::GenerationWork> taken;
  for (std::optional<c::GenerationWork> work; coordinator.take(work).ok() && work;) taken.push_back(*work);
  CHECK(taken.size() == 3 && coordinator.statistics().running == 3, "three generations run concurrently");
  std::vector<std::future<s::TerrainResult>> running;
  for (const auto &work : taken)
    running.push_back(std::async(std::launch::async, [&router, work] { return c::execute(work, router); }));
  for (size_t i = taken.size(); i-- > 0;)
    CHECK(coordinator.complete(taken[i], running[i].get()).ok(), "worker result admitted on the lane");
  CHECK(coordinator.statistics().running == 0, "every running credit returned");
  const Owner w = coordinator.cpu(west), e = coordinator.cpu(east), n = coordinator.cpu(north);
  CHECK(w && e && n, "every adjacent region has a CPU product");
  CHECK(coordinator.collision(east) == e, "collision lease shares the immutable product");
  for (const auto &p : {w, e, n})
    CHECK(p && p->field_content.hex == d::sha256(field.text).hex && p->job.source_revision == "source.1",
          "product carries the frozen source identity");
  same_seam(w, e, 0, -16, "west|east equal detail");
  same_seam(e, n, 2, 0, "east|north 2:1");
}

void late_completion() {
  std::puts("fixture terrain.example.coordinator_late_completion.v1");
  const auto original = ex::field(), edited = ex::field(1);
  SourceRouter router;
  router.add(original);
  router.add(edited);
  c::Coordinator coordinator(coordinator_options());
  CHECK(define(coordinator, "east", original).ok(), "east defined");
  CHECK(coordinator.demand(east, {true, true, false}).ok(), "east demanded");

  // 1. A source edit lands while the worker is generating the old revision.
  std::optional<c::GenerationWork> work;
  if (!dispatched(coordinator.take(work).ok() && work, "original revision dispatched")) return;
  auto old_run = std::async(std::launch::async, [&router, w = *work] { return c::execute(w, router); });
  CHECK(define(coordinator, "east", edited, "source.2").ok(), "edit replaces the definition mid-run");
  CHECK(work->control.cancellation->load(), "the superseded run is cancelled");
  CHECK(rejects(coordinator.complete(*work, old_run.get()), E::stale_result), "late completion is stale");
  CHECK(!coordinator.cpu(east) && !coordinator.collision(east), "nothing from the old revision is published");
  CHECK(coordinator.statistics().running == 0, "the stale receipt returned its credit");
  CHECK(settle(coordinator, router) == 1, "the edit regenerates once");
  const Owner edited_product = coordinator.cpu(east);
  CHECK(edited_product && edited_product->field_content.hex == d::sha256(edited.text).hex &&
            edited_product->job.source_revision == "source.2",
        "the edited revision is published");

  // 2. An edit after publication: held owners stay valid, the region empties.
  CHECK(define(coordinator, "east", original, "source.3").ok(), "edit back to the original source");
  CHECK(!coordinator.cpu(east) && !coordinator.collision(east), "edit withdraws the published product");
  CHECK(edited_product->geometry && !edited_product->geometry->meshes.empty() &&
            edited_product->job.source_revision == "source.2",
        "a consumer's owner of the superseded product remains intact");
  CHECK(settle(coordinator, router) == 1 && coordinator.cpu(east), "the reverted source republishes");

  // 3. A completion arrives after the region was evicted.
  CHECK(define(coordinator, "east", edited, "source.4").ok(), "edit queues new work");
  if (!dispatched(coordinator.take(work).ok() && work, "work dispatched before eviction")) return;
  CHECK(coordinator.evict(east).ok(), "region evicted while running");
  const auto evicted = c::execute(*work, router);
  CHECK(rejects(coordinator.complete(*work, evicted), E::stale_result), "completion after eviction is stale");
  CHECK(rejects(coordinator.complete(*work, evicted), E::stale_result), "a duplicate receipt is refused");
  CHECK(coordinator.statistics().regions == 0 && coordinator.statistics().running == 0,
        "eviction leaves no region or credit behind");

  // 4. Demand dropped and restored before a non-cooperative worker returns.
  CHECK(define(coordinator, "east", original).ok(), "east redefined");
  CHECK(coordinator.demand(east, {true, false, false}).ok(), "CPU demand");
  if (!dispatched(coordinator.take(work).ok() && work, "work dispatched")) return;
  CHECK(coordinator.demand(east, {}).ok() && coordinator.demand(east, {true, false, false}).ok(),
        "demand off/on cycle");
  const int before = router.runs;
  CHECK(rejects(coordinator.complete(*work, c::execute(*work, router)), E::stale_result),
        "the cycled-out run is stale");
  CHECK(router.runs == before, "the cancelled receipt never reached the generator");
  CHECK(settle(coordinator, router) == 1 && coordinator.cpu(east),
        "exactly one fresh run publishes after the cycle");
}

void resolution_change() {
  std::puts("fixture terrain.example.coordinator_resolution_change.v1");
  const auto field = ex::field();
  SourceRouter router;
  router.add(field);
  c::MemoryCache cache(16, 16ull * 1024 * 1024 * 1024);
  c::Coordinator coordinator(coordinator_options(), &cache);
  for (const auto *tile : {"east", "north"}) {
    CHECK(define(coordinator, tile, field).ok(), "static detail definition");
    CHECK(coordinator.demand({std::string("terrain.example.") + tile}, {true, true, false}).ok(), "demand");
  }
  CHECK(settle(coordinator, router) == 2, "static detail generates both tiles");
  const Owner east_static = coordinator.cpu(east), north_static = coordinator.cpu(north);
  CHECK(north_static && north_static->detail_level == 1 && north_static->cell_counts[0] == 10,
        "north starts at 4 m (detail 1, 10 cells)");
  same_seam(east_static, north_static, 2, 0, "static east|north 2:1");

  // World policy refines north to 2 m: both sides get new definitions.
  CHECK(define(coordinator, "north", field, "source.1", 2).ok() &&
            define(coordinator, "east", field, "source.1", 0, 2).ok(),
        "refined definitions admitted");
  CHECK(settle(coordinator, router) == 2, "refinement regenerates both tiles");
  const Owner east_fine = coordinator.cpu(east), north_fine = coordinator.cpu(north);
  CHECK(north_fine && north_fine->detail_level == 0 && north_fine->cell_counts[0] == 14,
        "north refined to 2 m (detail 0, 14 cells)");
  CHECK(north_static->detail_level == 1 && east_static->boundaries.size() == 2,
        "owners of the coarse products are unchanged");
  same_seam(east_fine, north_fine, 2, 0, "refined east|north equal detail");

  // Coarsening back is a cache hit: no generator runs, identical geometry.
  CHECK(define(coordinator, "north", field).ok() && define(coordinator, "east", field).ok(),
        "coarse definitions restored");
  const int runs = router.runs;
  CHECK(settle(coordinator, router) == 0 && router.runs == runs, "restored detail is served from the cache");
  const Owner north_again = coordinator.cpu(north);
  std::string a, b;
  CHECK(north_again && d::encode(*north_again->geometry, a).ok() && d::encode(*north_static->geometry, b).ok() &&
            a == b,
        "cached product reproduces the coarse geometry exactly");
  CHECK(north_again && north_again->job == coordinator.definition(north)->job,
        "cached product is rebound to the current receipt");

  // A 4:1 transition is outside contour profile v1 and must fail explicitly.
  const auto defined = coordinator.define(ex::request(ex::tile("north"), field, "source.1", 8));
  std::printf("4:1 transition refused at %s\n", defined.ok() ? "completion" : "definition");
  if (!defined.ok()) {
    CHECK(rejects(defined, E::unsupported_capability), "4:1 definition refused as unsupported");
    CHECK(coordinator.cpu(north) == north_again, "the current product stays published");
  } else {
    std::optional<c::GenerationWork> work;
    if (!dispatched(coordinator.take(work).ok() && work, "4:1 request dispatched")) return;
    CHECK(work && rejects(coordinator.complete(*work, c::execute(*work, router)), E::unsupported_capability),
          "4:1 generation refused as unsupported");
    CHECK(!coordinator.cpu(north), "nothing is published for the refused transition");
  }
}

void faults() {
  std::puts("fixture terrain.example.coordinator_faults.v1");
  const auto field = ex::field();
  SourceRouter router;
  router.add(field);
  c::Coordinator coordinator(coordinator_options());

  // Unknown required capability on the definition.
  auto future = ex::request(ex::tile("west"), field);
  future.required_capabilities.push_back({"matter.terrain.example.future", 1});
  CHECK(rejects(coordinator.define(future), E::unsupported_capability), "unknown capability refused at define");
  CHECK(coordinator.statistics().regions == 0 && !coordinator.definition(west), "refused definition is not kept");
  // Unknown required capability inside an already-generated product.
  CHECK(define(coordinator, "west", field).ok() && coordinator.demand(west, {true, false, false}).ok(), "west");
  std::optional<c::GenerationWork> work;
  if (!dispatched(coordinator.take(work).ok() && work, "west dispatched")) return;
  auto result = c::execute(*work, router);
  if (result.ok()) {
    auto product = *result.value();
    product.products.push_back({{"matter.terrain.example.future", 1}, true, d::Value::Object{}});
    const auto job = result.envelope();
    result = s::TerrainResult::success(job, std::move(product));
  }
  CHECK(rejects(coordinator.complete(*work, result), E::unsupported_capability),
        "product requiring an unknown capability is not admitted");
  CHECK(!coordinator.cpu(west) && coordinator.statistics().running == 0, "refused product leaves no owner or credit");
  // Rendering is not part of this build: graphics demand fails explicitly.
  CHECK(rejects(coordinator.demand(west, {true, false, true}), E::unsupported_capability),
        "graphics demand without a graphics service is refused");

  // Backend exception, then a retry with the same frozen definition.
  Throwing throwing(router, 1);
  if (!dispatched(coordinator.retry(west).ok() && coordinator.take(work).ok() && work, "retry dispatches")) return;
  result = c::execute(*work, throwing);
  CHECK(rejects(result, E::backend_failure), "a throwing backend becomes backend_failure");
  CHECK(rejects(coordinator.complete(*work, result), E::backend_failure), "the failure is reported on completion");
  CHECK(coordinator.take(work).ok() && !work, "a failed region does not spin");
  CHECK(coordinator.retry(west).ok() && settle(coordinator, throwing) == 1 && coordinator.cpu(west),
        "retry after the fault publishes");

  // Tampered payload bytes are caught by content identity.
  CHECK(define(coordinator, "east", field).ok() && coordinator.demand(east, {false, true, false}).ok(), "east");
  if (!dispatched(coordinator.take(work).ok() && work, "east dispatched")) return;
  result = c::execute(*work, router);
  if (result.ok()) {
    auto product = *result.value();
    auto seam = std::find_if(product.resources.resources.begin(), product.resources.resources.end(),
                             [](const auto &r) { return r.description.id.value.rfind("terrain.face.", 0) == 0; });
    if (seam != product.resources.resources.end()) {
      auto bytes = *seam->bytes;
      bytes[0] ^= 0x20;
      seam->bytes = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    }
    const auto job = result.envelope();
    result = s::TerrainResult::success(job, std::move(product));
  }
  CHECK(rejects(coordinator.complete(*work, result), E::invalid_input), "tampered seam payload refused");
  CHECK(!coordinator.collision(east), "tampered product never reaches the collision lease");

  // A result for a different region is a foreign envelope.
  if (!dispatched(coordinator.retry(east).ok() && coordinator.take(work).ok() && work, "east retried")) return;
  const auto west_result = c::execute(c::GenerationWork{coordinator.definition(west), {}}, router);
  CHECK(rejects(coordinator.complete(*work, west_result), E::stale_result), "foreign result envelope refused");

  // Cancelled before it ran: the worker observes it and the lane drops it.
  if (!dispatched(coordinator.retry(east).ok() && coordinator.take(work).ok() && work, "east dispatched again")) return;
  CHECK(coordinator.demand(east, {}).ok(), "demand withdrawn before the worker starts");
  result = c::execute(*work, router);
  CHECK(rejects(result, E::cancelled), "cancelled work does not generate");
  CHECK(rejects(coordinator.complete(*work, result), E::stale_result), "cancelled receipt is stale");

  // A budget too small for the requested samples is refused before any work.
  auto starved = ex::request(ex::tile("north"), field);
  starved.options.budget.operations = 1000;
  CHECK(rejects(coordinator.define(starved), E::resource_limit), "operation budget refused at define");
  CHECK(!coordinator.definition(north), "refused budget leaves no region");
  // An execution deadline that expires mid-run is a resource limit, not a
  // partial product.
  auto hurried = ex::request(ex::tile("north"), field);
  hurried.options.budget.execution_microseconds = 1;
  CHECK(coordinator.define(hurried).ok() && coordinator.demand(north, {true, false, false}).ok(), "north");
  if (!dispatched(coordinator.take(work).ok() && work, "north dispatched")) return;
  CHECK(rejects(coordinator.complete(*work, c::execute(*work, router)), E::resource_limit),
        "deadline exhaustion is reported");
  CHECK(!coordinator.cpu(north) && coordinator.statistics().running == 0, "no partial product is published");
}
} // namespace

int main(int argc, char **argv) {
  const std::string fixture = argc > 1 ? argv[1] : "";
  if (fixture == "adjacent") adjacent();
  else if (fixture == "late_completion") late_completion();
  else if (fixture == "resolution_change") resolution_change();
  else if (fixture == "faults") faults();
  else {
    std::fprintf(stderr, "usage: %s adjacent|late_completion|resolution_change|faults\n", argv[0]);
    return 2;
  }
  return check_summary();
}
