#include "service_fakes.h"
#include "fixtures.h"
#include <matter/contracts/abi.h>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <type_traits>

namespace ms = matter::services;
namespace mc = matter::contracts;
int checks = 0;
void check(bool ok, const char *name) {
  ++checks;
  if (!ok) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
}
void rejects(const mc::ValidationResult &r, mc::ErrorCode code, const char *name) {
  check(!r.ok(), name);
  bool found = false;
  for (const auto &d : r.diagnostics) found |= d.code == code;
  check(found, "stable service diagnostic");
}
template<class T> void fails(const ms::JobResult<T> &r, mc::ErrorCode code, const char *name) {
  check(!r.ok() && !r.value(), name);
  rejects({r.diagnostics()}, code, name);
}
ms::TerrainRequest terrain_request() {
  ms::TerrainRequest t;
  t.job = {"terrain-0", "field-rev-1", 1};
  t.field = {{"field"}, "fixture://field", "field-rev-1", mc::sha256("field"), {}};
  t.region = {"chunk-0"}; t.origin_metres = {1000000000, 0, -1000000000};
  t.sample_domain = {{0, 0, 0}, {2, 2, 2}};
  t.cell_counts = {2, 2, 2};
  t.required_capabilities = {{"matter.terrain.generate", 1}, {"fixture.shared-edge", 1}};
  t.boundaries = {{ms::BoundaryFace::positive_x, {1, 0, 0}, 1, 1, {"fixture.shared-edge", 1}, {}}};
  return t;
}
int main() {
  using E = mc::ErrorCode;
  static_assert(std::is_const_v<ms::AssetOwner::element_type>);
  static_assert(std::is_const_v<ms::BytesOwner::element_type>);
  ms::JobEnvelope job{"request-1", "source-1", 1};
  auto geometry = fake_geometry();
  auto resolver = fake_resolver();
  auto host = fake_host();
  auto cache = fake_cache();
  auto graphics = fake_graphics();
  auto terrain = fake_terrain();
  auto recipe = fixtures::recipe();
  ms::EvaluationOptions options;
  options.algorithms = {{"fixture.geometry", 1}};
  auto result = geometry->evaluate(job, recipe, {}, options, {});
  check(result.ok(), "SVC-EVALUATE public fake producer");
  std::shared_ptr<const ms::AssetProduct> published;
  check(ms::admit(result, job, published).ok() && published == result.value(), "SVC-ADMIT current envelope");
  for (unsigned field = 0; field < 3; ++field) {
    auto current = job;
    if (field == 0) current.request_id += "new";
    if (field == 1) current.source_revision += "new";
    if (field == 2) ++current.generation_epoch;
    auto old = published;
    rejects(ms::admit(result, current, published), E::stale_result, "SVC-STALE each envelope field");
    check(published == old, "stale admission preserves consumer owner");
  }
  auto failure = ms::AssetResult::failure(job, E::backend_failure, "fixture failure");
  auto old = published;
  rejects(ms::admit(failure, job, published), E::backend_failure, "SVC-FAIL no publication");
  check(published == old, "failure preserves consumer owner");
  fails(ms::AssetResult::success({}, fixtures::asset()), E::invalid_input, "invalid result envelope");
  fails(ms::AssetResult::success(job, fixtures::asset(), failure.diagnostics()), E::backend_failure,
        "error diagnostic cannot accompany valid asset");
  std::weak_ptr<const ms::AssetProduct> weak;
  {
    auto owned = geometry->evaluate(job, recipe, {}, options, {});
    weak = owned.value();
    auto retained = owned;
    owned = failure;
    check(!weak.expired(), "SVC-OWNER copied result retains storage");
  }
  check(weak.expired(), "SVC-RELEASE final owner releases storage");
  auto unsupported = recipe;
  unsupported.required_capabilities.push_back({"unknown.required", 1});
  fails(geometry->evaluate(job, unsupported, {}, options, {}), E::unsupported_capability, "SVC-CAPS unknown required");
  auto unknown_version = options;
  unknown_version.algorithms[0].version = 2;
  fails(geometry->evaluate(job, recipe, {}, unknown_version, {}), E::unsupported_capability, "SVC-CAPS exact algorithm version");
  ms::CancellationSource cancellation;
  ms::JobControl control;
  control.cancellation = cancellation.token();
  cancellation.cancel();
  fails(geometry->evaluate(job, recipe, {}, options, control), E::cancelled, "SVC-CANCEL pre-cancelled");
  ms::CancellationSource during;
  auto calling_thread = std::this_thread::get_id();
  control.cancellation = during.token();
  control.progress = [&](const auto &) { check(std::this_thread::get_id() == calling_thread, "SVC-CALLBACK same thread"); during.cancel(); };
  fails(geometry->evaluate(job, recipe, {}, options, control), E::cancelled, "SVC-CANCEL callback before completion");
  control = {};
  control.progress = [](const auto &) { throw 42; };
  fails(geometry->evaluate(job, recipe, {}, options, control), E::backend_failure, "SVC-CALLBACK exception translated");
  for (unsigned limit = 0; limit < 6; ++limit) {
    auto limited = options;
    if (limit == 0) limited.budget.operations = 1;
    if (limit == 1) limited.budget.vertices = 1;
    if (limit == 2) limited.budget.indices = 1;
    if (limit == 3) limited.budget.output_bytes = 1;
    if (limit == 4) limited.budget.progress_events = 0;
    if (limit == 5) limited.budget.execution_microseconds = 1;
    ms::JobControl counted;
    counted.progress = [](const auto &) {};
    fails(geometry->evaluate(job, recipe, {}, limited, counted), E::resource_limit, "SVC-BUDGET bounded evaluation");
  }
  ms::ResourceBudget budget;
  budget.progress_events = 1;
  unsigned callbacks = 0;
  ms::JobControl counted;
  counted.progress = [&](const auto &) { ++callbacks; };
  ms::JobMonitor monitor(budget, counted);
  check(monitor.report({0.1, "first"}).ok(), "SVC-PROGRESS first event");
  rejects(monitor.report({0.2, "second"}), E::resource_limit, "SVC-PROGRESS bound");
  check(callbacks == 1, "progress callback count remains bounded");
  ms::JobMonitor working(budget, {});
  ms::ResourceUsage usage;
  usage.working_bytes = budget.working_bytes + 1;
  rejects(working.checkpoint(usage), E::resource_limit, "SVC-BUDGET working memory");
  ms::JobMonitor depth(budget, {});
  usage = {}; usage.dependency_depth = budget.dependency_depth + 1;
  rejects(depth.checkpoint(usage), E::resource_limit, "SVC-BUDGET dependency depth");
  ms::JobMonitor cumulative(budget, {});
  usage = {}; usage.operations = 2;
  check(cumulative.checkpoint(usage).ok() && cumulative.checkpoint().ok(), "SVC-USAGE checkpoint retains cumulative usage");
  usage.operations = 1;
  rejects(cumulative.checkpoint(usage), E::invalid_input, "SVC-USAGE no counter rollback");
  ms::ResolvedResource resource;
  resource.bytes = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3});
  resource.description = {{"samples"}, mc::sha256(std::string_view("\1\2\3", 3)), mc::ScalarType::u8, 1, 3, 3};
  auto with_resource = recipe;
  with_resource.resources = {resource.description};
  ms::ResolvedInputs inputs;
  inputs.resources = {resource};
  check(ms::validate(with_resource, inputs).ok(), "SVC-RESOLVE verified payload");
  rejects(ms::validate(with_resource, {}), E::missing_dependency, "missing resolved payload");
  inputs.resources[0].description.byte_length = UINT64_MAX;
  rejects(ms::validate(with_resource, inputs), E::invalid_input, "overflowing resolved lengths");
  inputs.resources = {resource};
  inputs.resources[0].bytes = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{4, 5, 6});
  rejects(ms::validate(with_resource, inputs), E::invalid_input, "resolved content corruption");
  ms::QueryBatch batch;
  batch.samples = {{{"triangle"}, ms::QueryKind::closest_surface, {0, 0, 0}, {0, -1, 0}, 1},
                   {{"triangle"}, ms::QueryKind::ray_surface, {1, 0, 0}, {0, -1, 0}, 1}};
  auto queried = geometry->query(job, *result.value(), batch, options, {});
  check(queried.ok() && queried.value()->samples.size() == 2 && queried.value()->samples[1].position.x == 1,
        "SVC-QUERY staged batch preserves order");
  auto query_options = options; query_options.budget.queries = 1;
  fails(geometry->query(job, *result.value(), batch, query_options, {}), E::resource_limit, "SVC-QUERY query budget");
  batch.samples[0].prototype = {"missing"};
  fails(geometry->query(job, *result.value(), batch, options, {}), E::missing_dependency, "SVC-QUERY invalid prototype");
  ms::ProcessingRequest processing;
  processing.required_capabilities = {{"fixture.process", 1}};
  check(geometry->process(job, *result.value(), processing, options, {}).ok(), "SVC-PROCESS explicit stage");
  ms::AssetOwner aliased_document;
  std::weak_ptr<const std::vector<uint8_t>> generated_bytes;
  {
    auto document = fixtures::asset(); document.resources = {resource.description};
    ms::JobMonitor output_monitor(options.budget, {});
    fails(ms::finish_asset(job, document, options, output_monitor, {}), E::missing_dependency,
          "SVC-PAYLOAD missing output storage");
    ms::ResolvedInputs output;
    auto generated_resource = resource;
    generated_resource.bytes = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3});
    generated_bytes = generated_resource.bytes;
    output.resources.push_back(std::move(generated_resource));
    auto complete = ms::finish_asset(job, std::move(document), options, output_monitor, {}, std::move(output));
    check(complete.ok(), "SVC-PAYLOAD complete owned output");
    auto upload = ms::upload_request(complete);
    check(upload.resources.resources.size() == 1 && upload.asset->resources.size() == 1,
          "SVC-PAYLOAD upload carries actual bytes");
    aliased_document = ms::asset_owner(complete);
    check(geometry->process(job, *complete.value(), processing, options, {}).ok(),
          "SVC-PAYLOAD processing retains resources");
  }
  check(!generated_bytes.expired(), "SVC-PAYLOAD document alias retains complete product");
  aliased_document.reset();
  check(generated_bytes.expired(), "SVC-PAYLOAD last consumer releases bytes");
  ms::ResolveRequest source;
  source.source = recipe.source; source.required_capabilities = {{"fixture.source", 1}};
  auto snapshot = resolver->resolve(job, source, {}, {});
  check(snapshot.ok(), "SVC-HOST resolver snapshot");
  ms::AuthoringRequest authoring; authoring.seed = 42;
  auto authored = host->author(job, *snapshot.value(), authoring, {}, {});
  check(authored.ok() && authored.value()->seed == 42, "SVC-HOST injected authoring");
  auto build = fake_build(*resolver, *host, *geometry);
  ms::BuildRequest build_request;
  build_request.source = source; build_request.authoring = authoring;
  build_request.evaluation = options; build_request.processing = processing;
  check(build->build(job, build_request, {}).ok(), "SVC-BUILD injected public services");
  ms::CacheKey key; key.content = mc::sha256("all-inputs");
  check(cache->lookup(job, key, {}).ok() && !cache->lookup(job, key, {}).value()->asset, "SVC-CACHE explicit miss");
  check(cache->store(job, key, result.value(), {}).ok() && cache->lookup(job, key, {}).value()->asset == result.value(), "SVC-CACHE explicit store/hit");
  control = {}; control.cancellation = cancellation.token();
  fails(cache->store(job, key, std::make_shared<const ms::AssetProduct>(fixtures::asset()), control), E::cancelled, "SVC-CACHE cancelled no replacement");
  check(cache->lookup(job, key, {}).value()->asset == result.value(), "cache prior owner preserved");

  std::weak_ptr<const mc::AssetDocument> upload_owner;
  ms::UploadTicket ticket;
  fails(graphics->upload(job, {ms::asset_owner(result), {}, {{"unknown.graphics", 1}}}, {}),
        E::unsupported_capability, "GFX-CAPS explicit unsupported representation");
  {
    auto asset = std::make_shared<const mc::AssetDocument>(fixtures::asset());
    upload_owner = asset;
    auto submitted = graphics->upload(job, {asset, {}, {}}, {});
    check(submitted.ok(), "GFX-SUBMIT hand-built asset"); ticket = *submitted.value();
  }
  check(!upload_owner.expired(), "GFX-OWNER retains asynchronous input");
  auto pending = graphics->poll(job, ticket);
  check(pending.ok() && pending.value()->state == ms::UploadState::pending && !pending.value()->resource.slot, "GFX-PENDING no resource publication");
  auto completed = graphics->poll(job, ticket);
  check(completed.ok() && completed.value()->state == ms::UploadState::ready, "GFX-READY explicit completion");
  auto handle = completed.value()->resource;
  ms::SceneUpdate scene; scene.scene_revision = 1;
  scene.upsert = {{{"instance-a"}, {"triangle"}, handle, {}}, {{"instance-b"}, {"triangle"}, handle, {}}};
  check(graphics->update_scene(job, scene).ok(), "GFX-SCENE shared prototype");
  fails(graphics->update_scene(job, scene), E::stale_result, "GFX-SCENE stale revision rejected");
  auto foreign = handle; ++foreign.owner;
  rejects(graphics->release_resource(foreign), E::invalid_input, "GFX-RELEASE foreign handle");
  check(graphics->release_resource(handle).ok(), "GFX-RELEASE caller ownership");
  check(!upload_owner.expired(), "scene independently retains shared resource");
  scene = {}; scene.scene_revision = 2; scene.remove = {{"instance-a"}};
  check(graphics->update_scene(job, scene).ok() && !upload_owner.expired(), "GFX-SHARING delete one instance");
  scene.scene_revision = 3; scene.remove = {{"instance-b"}};
  check(graphics->update_scene(job, scene).ok() && upload_owner.expired(), "GFX-RELEASE final scene owner");
  rejects(graphics->release_resource(handle), E::invalid_input, "GFX-RELEASE repeated release");
  ms::CancellationSource cancel_upload;
  control = {}; control.cancellation = cancel_upload.token();
  {
    auto asset = std::make_shared<const mc::AssetDocument>(fixtures::asset()); upload_owner = asset;
    auto submitted = graphics->upload(job, {asset, {}, {}}, control); ticket = *submitted.value();
  }
  cancel_upload.cancel();
  fails(graphics->poll(job, ticket), E::cancelled, "GFX-CANCEL failed upload no resource");
  check(upload_owner.expired(), "GFX-CANCEL releases retained input");
  {
    auto asset = std::make_shared<const mc::AssetDocument>(fixtures::asset()); upload_owner = asset;
    ticket = *graphics->upload(job, {asset, {}, {}}, {}).value();
  }
  check(graphics->release_ticket(ticket).ok() && upload_owner.expired(), "GFX-TICKET dispose pending upload");
  rejects(graphics->release_ticket(ticket), E::invalid_input, "GFX-TICKET repeated disposal");

  auto request = terrain_request();
  auto generated = terrain->generate(request, {});
  check(generated.ok(), "TERRAIN-ENVELOPE public fake producer");
  std::shared_ptr<const ms::TerrainProduct> terrain_owner;
  check(ms::admit_terrain(generated, request, terrain->capabilities(), terrain_owner).ok(), "TERRAIN-ADMIT");
  auto adjacent = request;
  adjacent.region_key[0] = 1; adjacent.region = {"chunk-1"}; adjacent.job.request_id = "terrain-1";
  adjacent.origin_metres[0] += 2;
  adjacent.boundaries = {{ms::BoundaryFace::negative_x, {0, 0, 0}, 1, 1, {"fixture.shared-edge", 1}, {}}};
  auto neighbor = terrain->generate(adjacent, {});
  check(neighbor.ok() && neighbor.value()->boundaries[0].neighbor_key == generated.value()->region_key &&
        generated.value()->boundaries[0].neighbor_key == neighbor.value()->region_key, "TERRAIN-ADJACENT neighbor envelopes");
  auto coarser = adjacent;
  coarser.cell_counts = {1, 1, 1}; coarser.detail_level = 1;
  coarser.job.generation_epoch = 2; coarser.boundaries[0].sample_spacing_metres = 2;
  check(terrain->generate(coarser, {}).ok(), "TERRAIN-DETAIL transition envelope");
  rejects(ms::admit_terrain(neighbor, coarser, terrain->capabilities(), terrain_owner), E::stale_result, "TERRAIN-LATE invalidated detail completion");
  auto previous_terrain = terrain_owner;
  auto corrupt_product = *generated.value();
  corrupt_product.boundaries[0].neighbor_key = {99, 0, 0};
  auto corrupt = ms::TerrainResult::success(request.job, corrupt_product);
  rejects(ms::admit_terrain(corrupt, request, terrain->capabilities(), terrain_owner), E::invalid_input, "TERRAIN-BOUNDARY corrupt neighbor rejected");
  check(terrain_owner == previous_terrain, "terrain boundary failure preserves prior owner");
  corrupt_product = *generated.value(); corrupt_product.field_content = mc::sha256("other");
  rejects(ms::validate(corrupt_product, request, terrain->capabilities()), E::stale_result, "TERRAIN-FIELD frozen identity");
  corrupt_product = *generated.value(); ++corrupt_product.detail_level;
  rejects(ms::validate(corrupt_product, request, terrain->capabilities()), E::stale_result, "TERRAIN-DETAIL same envelope wrong product detail");
  corrupt_product = *generated.value(); corrupt_product.geometry.reset();
  rejects(ms::validate(corrupt_product, request, terrain->capabilities()), E::invalid_input, "TERRAIN-OWNER null product rejected");
  corrupt_product = *generated.value(); corrupt_product.boundaries[0].seam_resource = mc::SemanticId{"missing-seam"};
  rejects(ms::validate(corrupt_product, request, terrain->capabilities()), E::missing_dependency, "TERRAIN-SEAM missing payload");
  auto bad_request = request; bad_request.required_capabilities[1].version = 2;
  fails(terrain->generate(bad_request, {}), E::unsupported_capability, "TERRAIN-CAPS policy version");
  bad_request = request; bad_request.region_key[0] = INT32_MAX;
  rejects(ms::validate(bad_request, terrain->capabilities()), E::invalid_input, "TERRAIN-BOUNDARY integer overflow");
  bad_request = request; bad_request.cell_counts = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
  rejects(ms::validate(bad_request, terrain->capabilities()), E::resource_limit, "TERRAIN-BUDGET overflowing sample grid");
  bad_request = request; bad_request.options.budget.output_bytes = 1;
  fails(terrain->generate(bad_request, {}), E::resource_limit, "TERRAIN-BUDGET output bytes");
  fails(terrain->generate(request, control), E::cancelled, "TERRAIN-CANCEL no product publication");
  std::cout << "service contract fixtures: " << checks << " checks passed (fake services only)\n";
}
