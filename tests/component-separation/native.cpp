#include <matter/contracts/contracts.h>
#include <matter/contracts/jobs.h>
#include "part_graph.h"
#include "dsl_rng.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace mc = matter::contracts;
namespace ms = matter::services;
using V = mc::Value;
std::filesystem::path root;
unsigned checks = 0;
void check(bool ok, const std::string &id) {
  if (!ok) throw std::runtime_error(id);
  ++checks;
}
const V &at(const V &v, const std::string &key) { return std::get<V::Object>(v.data).at(key); }
const V::Array &array(const V &v) { return std::get<V::Array>(v.data); }
const std::string &str(const V &v) { return std::get<std::string>(v.data); }
double num(const V &v) { return std::get<double>(v.data); }
std::string read(const std::string &path) {
  std::ifstream f(root / path, std::ios::binary);
  if (!f) throw std::runtime_error("missing input: " + path);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
V load(const std::string &path) {
  V v; check(mc::parse_json(read(path), v).ok(), "parse " + path); return v;
}
void rejected(const mc::ValidationResult &r, const std::string &code, const std::string &id) {
  bool found = false;
  for (const auto &d : r.diagnostics)
    found |= d.severity == mc::Severity::error && code == mc::code_name(d.code);
  check(!r.ok() && found, id + " expected " + code);
}
std::string hex(uint64_t n) {
  std::ostringstream s; s << std::hex << std::setfill('0') << std::setw(16) << n; return s.str();
}
void documents() {
  const auto samples = load("fixtures/documents.json");
  for (const auto &sample : array(samples)) {
    const auto path = str(at(sample, "input"));
    std::string wire; mc::ContentDigest digest;
    if (str(at(sample, "kind")) == "recipe") {
      mc::RecipeDocument d;
      check(mc::decode_recipe(read(path), d).ok(), "decode " + path);
      check(mc::encode(d, wire).ok() && mc::content_digest(d, digest).ok(), "encode " + path);
    } else {
      mc::AssetDocument d;
      check(mc::decode_asset(read(path), d).ok(), "decode " + path);
      check(mc::encode(d, wire).ok() && mc::content_digest(d, digest).ok(), "encode " + path);
    }
    check(wire == read(str(at(sample, "expected_wire"))), "canonical " + path);
    check(digest.hex == str(at(sample, "content_sha256")), "identity " + path);
  }
}
void errors() {
  const auto cases = load("fixtures/validation-cases.json");
  for (const auto &group : {"cases", "budgets"}) {
    for (const auto &c : array(at(cases, group))) {
      const auto id = str(at(c, "id")), kind = str(at(c, "kind"));
      mc::ValidationOptions o;
      if (std::string(group) == "budgets") {
        const auto limit = str(at(c, "limit")); const auto n = uint64_t(num(at(c, "value")));
        if (limit == "max_nodes") o.limits.max_nodes = uint32_t(n);
        else if (limit == "max_vertices") o.limits.max_vertices = n;
        else if (limit == "max_indices") o.limits.max_indices = n;
        else if (limit == "max_value_nodes") o.limits.max_value_nodes = n;
        else throw std::runtime_error("unknown limit " + limit);
      }
      const auto input = read(str(at(c, "input")));
      mc::ValidationResult result;
      if (kind == "recipe") {
        mc::RecipeDocument d; d.source.id.value = "previous-owner";
        result = mc::decode_recipe(input, d, o);
        check(d.source.id.value == "previous-owner", id + " unpublished");
      } else if (kind == "asset") {
        mc::AssetDocument d; d.id.value = "previous-owner";
        result = mc::decode_asset(input, d, o);
        check(d.id.value == "previous-owner", id + " unpublished");
      } else {
        V v("previous-owner"); result = mc::parse_json(input, v, o.limits);
        check(str(v) == "previous-owner", id + " unpublished");
      }
      rejected(result, str(at(c, "expected_code")), id);
    }
  }
}
void params_rng() {
  const auto params = load("fixtures/params.json");
  for (const auto &c : array(at(params, "cases"))) {
    part_graph::Params p;
    const auto &values = at(c, "params");
    for (const auto &kv : std::get<V::Object>(values.data)) {
      if (const auto v = std::get_if<double>(&kv.second.data)) p[kv.first] = part_graph::ParamValue::number(*v);
      else if (const auto v = std::get_if<bool>(&kv.second.data)) p[kv.first] = part_graph::ParamValue::boolean_(*v);
      else p[kv.first] = part_graph::ParamValue::string_(str(kv.second));
    }
    const auto id = "PARAM-" + str(at(c, "id")); std::string canonical;
    check(part_graph::serialize_params(p) == str(at(c, "legacy_memo")), id + " memo");
    check(part_graph::params_to_json(p) == str(at(c, "legacy_json")), id + " legacy JSON");
    check(mc::canonical_json(values, canonical).ok() && canonical == str(at(c, "portable_canonical")), id + " portable JSON");
  }
  const auto vectors = load("fixtures/rng-splitmix64.json");
  for (const auto &v : array(at(vectors, "vectors"))) {
    const auto seed = std::stoull(str(at(v, "seed")), nullptr, 16);
    dsl::Rng legacy(seed), legacy_units(seed); mc::Rng portable(seed), portable_units(seed);
    const auto &words = array(at(v, "words")), &units = array(at(v, "units"));
    for (size_t i = 0; i < words.size(); ++i) {
      check(hex(legacy.next_u64()) == str(words[i]) && hex(portable.next_u64()) == str(words[i]), "RNG words");
      check(legacy_units.next_unit() == num(units[i]) && portable_units.next_unit() == num(units[i]), "RNG units");
    }
  }
}
void jobs() {
  const auto cases = load("fixtures/job-cases.json"), e = at(cases, "envelope"), u = at(cases, "usage");
  ms::JobEnvelope envelope{str(at(e, "request_id")), str(at(e, "source_revision")), uint64_t(num(at(e, "generation_epoch")))};
  ms::ResourceUsage usage;
  usage.operations = uint64_t(num(at(u, "operations"))); usage.vertices = uint64_t(num(at(u, "vertices")));
  usage.indices = uint64_t(num(at(u, "indices"))); usage.output_bytes = uint64_t(num(at(u, "output_bytes")));
  usage.working_bytes = uint64_t(num(at(u, "working_bytes"))); usage.queries = uint64_t(num(at(u, "queries")));
  usage.dependency_depth = uint32_t(num(at(u, "dependency_depth")));
  auto fail_publish = [&](const mc::ValidationResult &r, const V &c) {
    const auto id = str(at(c, "id")); rejected(r, str(at(c, "expected_code")), id);
    auto previous = std::make_shared<const int>(17); auto destination = previous;
    auto result = ms::JobResult<int>::failure(envelope, r.diagnostics);
    check(!result.value() && !ms::admit(result, envelope, destination).ok() && destination == previous,
          id + " preserves owner");
  };
  for (const auto &c : array(at(cases, "budgets"))) {
    ms::ResourceBudget b; const auto field = str(at(c, "field")); const auto ceiling = uint64_t(num(at(c, "ceiling")));
    if (field == "operations") b.operations = ceiling;
    else if (field == "vertices") b.vertices = ceiling;
    else if (field == "indices") b.indices = ceiling;
    else if (field == "output_bytes") b.output_bytes = ceiling;
    else if (field == "working_bytes") b.working_bytes = ceiling;
    else if (field == "queries") b.queries = ceiling;
    else if (field == "dependency_depth") b.dependency_depth = uint32_t(ceiling);
    else throw std::runtime_error("unknown budget " + field);
    ms::JobMonitor monitor(b, {}); fail_publish(monitor.checkpoint(usage), c);
  }
  for (const auto &c : array(at(cases, "cancellations"))) {
    ms::CancellationSource cancel; ms::JobControl control; control.cancellation = cancel.token();
    const bool pre = str(at(c, "when")) == "before_checkpoint";
    if (pre) cancel.cancel(); else control.progress = [&](const ms::Progress &) { cancel.cancel(); };
    ms::JobMonitor monitor({}, control);
    if (!pre) monitor.report({0.25, "synthetic checkpoint"});
    fail_publish(monitor.checkpoint(usage), c);
  }
  for (const auto &c : array(at(cases, "late_results"))) {
    auto expected = envelope; const auto field = str(at(c, "field"));
    if (field == "request_id") expected.request_id = str(at(c, "replacement"));
    else if (field == "source_revision") expected.source_revision = str(at(c, "replacement"));
    else if (field == "generation_epoch") expected.generation_epoch = uint64_t(num(at(c, "replacement")));
    else throw std::runtime_error("unknown envelope field " + field);
    auto previous = std::make_shared<const int>(17); auto destination = previous;
    auto result = ms::JobResult<int>::success(envelope, 42);
    rejected(ms::admit(result, expected, destination), str(at(c, "expected_code")), str(at(c, "id")));
    check(destination == previous, str(at(c, "id")) + " preserves owner");
  }
}
void affine() {
  mc::AssetDocument d; check(mc::decode_asset(read("fixtures/affine.asset.json"), d).ok(), "AFFINE decode");
  const auto m = d.instances[0].local_transform;
  const auto oracle = at(load("fixtures/geometry-oracles.json"), "affine");
  auto p = mm::transform_point(m, {1, 2, 3}); const auto &expected = array(at(oracle, "point_out"));
  check(p.x == num(expected[0]) && p.y == num(expected[1]) && p.z == num(expected[2]), "AFFINE point");
  const auto n = mc::transform_normal(m, {1, 1, 0}); const auto &normal = array(at(oracle, "normal_out"));
  const auto policy = load("comparison-policy.json");
  const auto tolerance = num(at(at(at(at(policy, "profiles"), "analytic-mesh-v1"), "normal_components"), "absolute"));
  check(n && std::abs(n->x - num(normal[0])) <= tolerance && std::abs(n->y - num(normal[1])) <= tolerance &&
        std::abs(n->z - num(normal[2])) <= tolerance && mc::reverses_winding(m), "AFFINE normal/winding");
}
int main(int argc, char **argv) {
  try {
    if (argc != 2) throw std::runtime_error("usage: corpus_native CORPUS_DIRECTORY");
    root = argv[1]; documents(); errors(); params_rng(); jobs(); affine();
    std::cout << "PASS " << checks << " corpus checks: contract codec/errors, actual legacy params/RNG, affine math, cooperative jobs.\n"
              << "Geometry producers, terrain mesh generation, renderer and performance require separate adapters/captures.\n";
    return 0;
  } catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
