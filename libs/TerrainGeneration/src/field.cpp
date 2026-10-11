#include <matter/terrain/field.h>
#include "terrain_field.h"
#include "field_internal.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace matter::terrain {
namespace {
using E = data::ErrorCode;
using Position = std::array<double, 3>;
data::ValidationResult error(E code, std::string message) {
  return {{{data::Severity::error, code, std::move(message), {}, {}, {}}}};
}
Position minimum(const services::TerrainRequest &r) {
  return {{r.sample_domain.min.x, r.sample_domain.min.y, r.sample_domain.min.z}};
}
Position maximum(const services::TerrainRequest &r) {
  return {{r.sample_domain.max.x, r.sample_domain.max.y, r.sample_domain.max.z}};
}
double spacing(const services::TerrainRequest &r) {
  return (double(r.sample_domain.max.x) - r.sample_domain.min.x) / r.cell_counts[0];
}
uint64_t sample_count(const services::TerrainRequest &r) {
  // Called only after contract validation bounded the product by operations.
  uint64_t n = 1;
  for (auto cells : r.cell_counts) n *= uint64_t(cells) + 1;
  return n;
}

// The legacy parser accepts numeric prefixes/trailing tokens and unbounded
// octave parameters. Tighten the portable input boundary without changing the
// source algorithm or compatibility entrypoints.
data::ValidationResult validate_text(const std::string &text) {
  static const std::map<std::string, size_t> arities = {
      {"const", 2}, {"input", 2}, {"noise2", 6}, {"ridge2", 6},
      {"noise3", 6}, {"ridge3", 6}, {"warp2", 5}, {"add", 3},
      {"sub", 3}, {"mul", 3}, {"min", 3}, {"max", 3}, {"clamp", 4},
      {"blend", 4}, {"smoothstep", 4}, {"abs", 2}, {"oneminus", 2},
      {"pow", 3}, {"height", 2}, {"density", 2}, {"moisture", 2},
      {"relief", 2}, {"seaLevel", 2}, {"biome", 3}};
  std::istringstream lines(text);
  std::string line;
  std::set<std::string> directives;
  while (std::getline(lines, line)) {
    std::istringstream words(line);
    std::vector<std::string> tokens;
    std::string token;
    while (words >> token) tokens.push_back(token);
    if (tokens.empty()) continue;
    auto found = arities.find(tokens[0]);
    if (found == arities.end()) return error(E::unsupported_capability, "unsupported field operation: " + tokens[0]);
    if (tokens.size() != found->second) return error(E::invalid_input, "field operation has incorrect arity: " + tokens[0]);
    if (tokens[0] == "height" || tokens[0] == "density" || tokens[0] == "moisture" ||
        tokens[0] == "relief" || tokens[0] == "seaLevel" || tokens[0] == "biome") {
      if (!directives.insert(tokens[0]).second) return error(E::invalid_input, "duplicate field directive");
    }
    if (tokens[0] == "input") continue; // Names validated by FieldProgram.
    for (size_t i = 1; i < tokens.size(); ++i) {
      const auto &s = tokens[i];
      if (s[0] == 'r') {
        if (s.size() < 2 || !std::all_of(s.begin() + 1, s.end(), [](char c) { return c >= '0' && c <= '9'; }))
          return error(E::invalid_input, "invalid field register token");
        continue;
      }
      bool seed = ((tokens[0] == "warp2" && i == 2) ||
                   ((tokens[0] == "noise2" || tokens[0] == "ridge2" ||
                     tokens[0] == "noise3" || tokens[0] == "ridge3") && i == 1));
      size_t consumed = 0;
      try {
        if (seed) {
          if (s[0] == '-' || s[0] == '+') return error(E::invalid_input, "field seed must be uint32");
          auto n = std::stoull(s, &consumed, 10);
          if (n > UINT32_MAX) return error(E::invalid_input, "field seed exceeds uint32");
        } else {
          const float n = std::stof(s, &consumed);
          if (!std::isfinite(n)) return error(E::invalid_input, "nonfinite field literal");
          if ((tokens[0] == "noise2" || tokens[0] == "ridge2" ||
               tokens[0] == "noise3" || tokens[0] == "ridge3") && i == 3) {
            if (n < 1 || n > 16 || std::floor(n) != n || s.find_first_of(".eE") != std::string::npos)
              return error(E::invalid_input, "field octave count must be an integer in [1,16]");
          }
        }
      } catch (const std::exception &) { return error(E::invalid_input, "invalid field numeric literal"); }
      if (consumed != s.size()) return error(E::invalid_input, "trailing bytes in field numeric literal");
    }
  }
  return {};
}

data::ValidationResult validate_program(const terrain_field::FieldProgram &p,
    const services::TerrainRequest &r, uint64_t &cost) {
  Position lo, hi;
  auto valid = local_to_world(r.origin_metres, minimum(r), lo);
  if (!valid.ok()) return valid;
  valid = local_to_world(r.origin_metres, maximum(r), hi);
  if (!valid.ok()) return valid;
  const double step = spacing(r);
  for (unsigned axis = 0; axis < 3; ++axis) {
    // Prevent loss of local extent in double addition or float evaluation.
    const double extent = maximum(r)[axis] - minimum(r)[axis];
    if (hi[axis] - lo[axis] != extent)
      return error(E::unsupported_capability, "world origin loses region extent precision");
    const double mag = std::max(std::abs(lo[axis]), std::abs(hi[axis]));
    if (mag > double(std::numeric_limits<float>::max()))
      return error(E::unsupported_capability, "field world coordinates exceed float32");
    const float at = static_cast<float>(mag);
    const double ulp = double(std::nextafter(at, std::numeric_limits<float>::infinity())) - at;
    if (!std::isfinite(ulp) || ulp > step / 8.0)
      return error(E::unsupported_capability, "field world-f32 precision exceeds one sixteenth of sample spacing");
  }
  double warp_reach = 0;
  for (const auto &o : p.ops) if (o.kind == terrain_field::Op::Warp2) warp_reach += std::abs(double(o.f1));
  Position reach;
  for (unsigned axis = 0; axis < 3; ++axis)
    reach[axis] = std::max(std::abs(lo[axis]), std::abs(hi[axis])) + (axis == 1 ? 0 : warp_reach);
  for (double distance : reach)
    if (distance >= double(std::numeric_limits<float>::max()) / 2)
      return error(E::unsupported_capability, "field warped coordinates exceed safe float32 domain");
  std::vector<uint64_t> prefix_cost(1, 0);
  std::vector<uint32_t> prefix_depth(1, 0);
  for (const auto &o : p.ops) {
    using Op = terrain_field::Op;
    uint64_t work = 1;
    uint32_t depth = prefix_depth.back();
    const bool noise = o.kind == Op::Noise2 || o.kind == Op::Ridge2 || o.kind == Op::Noise3 || o.kind == Op::Ridge3;
    if (noise || o.kind == Op::Warp2) {
      if (o.f0 < 0 || (noise && (o.oct < 1 || o.oct > 16 || o.f1 < 0 || o.f1 > 1 || o.f2 <= 0)))
        return error(E::invalid_input, "invalid field frequency, gain or lacunarity");
      float frequency = o.f0;
      const int octaves = noise ? o.oct : 1;
      for (int octave = 0; octave < octaves; ++octave) {
        for (unsigned axis = 0; axis < 3; ++axis) {
          if (axis == 1 && o.kind != Op::Noise3 && o.kind != Op::Ridge3) continue;
          // Margin protects floor-to-int and neighboring-corner addition, and
          // float rounding/warped-coordinate addition. No UB reaches noise.
          if (!std::isfinite(frequency) || reach[axis] * double(frequency) >= 536870912.0)
            return error(E::unsupported_capability, "field noise lattice exceeds safe int32 domain");
        }
        if (octave + 1 < octaves) frequency *= o.f2;
      }
      work = static_cast<uint64_t>(octaves);
    }
    if (o.kind == Op::Warp2) {
      work = prefix_cost[static_cast<size_t>(o.a) + 1] + 1;
      depth = std::max(depth, prefix_depth[static_cast<size_t>(o.a) + 1] + 1);
    }
    if ((o.kind == Op::Clamp && o.f0 > o.f1) || (o.kind == Op::Smoothstep && o.f0 >= o.f1))
      return error(E::invalid_input, "invalid field clamp/smoothstep interval");
    if (work > r.options.budget.operations - prefix_cost.back() || depth > r.options.budget.dependency_depth)
      return error(E::resource_limit, "field expression cost or warp depth exceeds budget");
    prefix_cost.push_back(prefix_cost.back() + work);
    prefix_depth.push_back(depth);
  }
  if (prefix_cost.back() > r.options.budget.operations / 4)
    return error(E::resource_limit, "field sample cost exceeds operation budget");
  cost = prefix_cost.back() * 4; // Height, density, moisture, relief; conservative full tapes.
  return {};
}

data::ValidationResult boundary_policies(const services::TerrainRequest &r) {
  const double step = spacing(r);
  auto lo = minimum(r), hi = maximum(r);
  for (unsigned axis = 1; axis < 3; ++axis)
    if ((hi[axis] - lo[axis]) / r.cell_counts[axis] != step)
      return error(E::invalid_input, "field lattice requires exactly uniform spacing");
  for (const auto &b : r.boundaries) {
    if (b.seam_resource) return error(E::unsupported_capability, "field sampling cannot generate a mesh seam resource");
    if (b.sample_spacing_metres != static_cast<float>(step))
      return error(E::invalid_input, "boundary spacing must equal encoded lattice spacing");
    if (b.policy.name == "matter.terrain.boundary.shared-samples") {
      if (b.neighbor_spacing_metres != b.sample_spacing_metres)
        return error(E::invalid_input, "shared-samples requires matching resolution");
    } else {
      int exponent = 0;
      const double ratio = std::max(double(b.neighbor_spacing_metres), double(b.sample_spacing_metres)) /
                           std::min(double(b.neighbor_spacing_metres), double(b.sample_spacing_metres));
      if (std::frexp(ratio, &exponent) != 0.5)
        return error(E::invalid_input, "aligned-samples requires power-of-two spacing ratio");
    }
  }
  return {};
}
} // namespace

namespace detail {
struct RegionState {
  services::TerrainRequest request;
  terrain_field::FieldRuntime runtime;
  uint64_t cost, retained_bytes;
  RegionState(services::TerrainRequest r, terrain_field::FieldProgram p, uint64_t c, uint64_t bytes)
      : request(std::move(r)), runtime(std::move(p)), cost(c), retained_bytes(bytes) {}
};
} // namespace detail
struct FieldAccess {
  static const detail::RegionState &state(const FrozenRegion &r) { return *r.state_; }
};
detail::MeshingField detail::meshing_field(const FrozenRegion &r) {
  const auto &s = FieldAccess::state(r);
  return {s.runtime, s.cost, s.retained_bytes};
}
FrozenRegion::FrozenRegion(std::shared_ptr<const detail::RegionState> state) : state_(std::move(state)) {}
const services::TerrainRequest &FrozenRegion::request() const { return state_->request; }
bool FrozenRegion::is_heightfield() const { return state_->runtime.is_heightfield(); }
uint64_t FrozenRegion::legacy_text_hash() const { return state_->runtime.hash(); }
std::vector<data::Capability> field_capabilities() {
  return {{"matter.terrain.generate", 1}, {"matter.terrain.field.text", 1},
          {"matter.terrain.field.world-f32", 1}, {"matter.terrain.field.sample", 1},
          {"matter.terrain.boundary.shared-samples", 1}, {"matter.terrain.boundary.aligned-samples", 1}};
}
data::ValidationResult local_to_world(const Position &origin, const Position &local, Position &world) {
  Position converted;
  for (unsigned i = 0; i < 3; ++i) {
    converted[i] = origin[i] + local[i];
    if (!std::isfinite(origin[i]) || !std::isfinite(local[i]) || !std::isfinite(converted[i]))
      return error(E::invalid_input, "nonfinite or overflowing coordinate conversion");
  }
  world = converted;
  return {};
}
data::ValidationResult world_to_local(const Position &origin, const Position &world, Position &local) {
  Position negated;
  for (unsigned i = 0; i < 3; ++i) negated[i] = -origin[i];
  return local_to_world(negated, world, local);
}

FreezeResult freeze_region(const services::TerrainRequest &request, const EncodedField &field,
                           const services::JobControl &control) {
  try {
    services::JobMonitor monitor(request.options.budget, control);
    auto valid = monitor.checkpoint();
    if (!valid.ok()) return FreezeResult::failure(request.job, valid.diagnostics);
    if (field.target != FieldTarget::cpu)
      return FreezeResult::failure(request.job, E::unsupported_capability, "unsupported field evaluation target");
    valid = services::require_capabilities({field.encoding}, {{"matter.terrain.field.text", 1}});
    if (!valid.ok()) return FreezeResult::failure(request.job, valid.diagnostics);
    const auto &limits = request.options.validation.limits;
    if (field.text.size() > limits.max_document_bytes || field.text.size() > limits.max_string_bytes ||
        request.dependencies.size() > limits.max_nodes || request.required_capabilities.size() > limits.max_nodes ||
        request.options.algorithms.size() > limits.max_nodes || request.field.legacy_hashes.size() > limits.max_nodes ||
        request.resolved.resources.size() > limits.max_nodes || request.resolved.assets.size() > limits.max_nodes ||
        request.options.validation.supported_capabilities.size() > limits.max_nodes)
      return FreezeResult::failure(request.job, E::resource_limit, "field input size exceeds budget");
    uint64_t retained = sizeof(detail::RegionState) + field.text.size() +
                        terrain_field::kMaxSurfaceOps * sizeof(terrain_field::Op);
    auto account = [&](uint64_t bytes) {
      if (retained > request.options.budget.working_bytes || bytes > request.options.budget.working_bytes - retained)
        return false;
      retained += bytes;
      return true;
    };
    auto string_bytes = [&](const std::string &value) {
      return value.size() <= limits.max_string_bytes && account(value.size());
    };
    bool bounded = string_bytes(request.job.request_id) && string_bytes(request.job.source_revision) &&
                   string_bytes(request.field.id.value) && string_bytes(request.field.uri) &&
                   string_bytes(request.field.revision) && string_bytes(request.region.value);
    for (const auto &h : request.field.legacy_hashes) bounded = bounded && string_bytes(h.name_space) && account(sizeof(h));
    for (const auto &cap : request.required_capabilities) bounded = bounded && string_bytes(cap.name) && account(sizeof(cap));
    for (const auto &cap : request.options.algorithms) bounded = bounded && string_bytes(cap.name) && account(sizeof(cap));
    for (const auto &cap : request.options.validation.supported_capabilities)
      bounded = bounded && string_bytes(cap.name) && account(sizeof(cap));
    for (const auto &dep : request.dependencies)
      bounded = bounded && string_bytes(dep.id.value) && string_bytes(dep.content.hex) && account(sizeof(dep));
    for (const auto &b : request.boundaries)
      bounded = bounded && string_bytes(b.policy.name) && account(sizeof(b)) &&
                (!b.seam_resource || string_bytes(b.seam_resource->value));
    for (const auto &r : request.resolved.resources)
      bounded = bounded && string_bytes(r.description.id.value) && string_bytes(r.description.content.hex) &&
                account(sizeof(r)) && account(r.bytes ? r.bytes->size() : 0);
    for (const auto &a : request.resolved.assets)
      bounded = bounded && string_bytes(a.description.id.value) && string_bytes(a.description.content.hex) && account(sizeof(a));
    if (!bounded || !account(0))
      return FreezeResult::failure(request.job, E::resource_limit, "frozen input metadata/payload exceeds working budget");
    services::ResourceUsage usage;
    usage.working_bytes = retained;
    valid = monitor.checkpoint(usage);
    if (!valid.ok()) return FreezeResult::failure(request.job, valid.diagnostics);
    valid = services::validate(request, field_capabilities());
    if (!valid.ok()) return FreezeResult::failure(request.job, valid.diagnostics);
    if (data::sha256(field.text).hex != request.field.content->hex)
      return FreezeResult::failure(request.job, E::invalid_input, "field text does not match frozen content identity");
    valid = boundary_policies(request);
    if (!valid.ok()) return FreezeResult::failure(request.job, valid.diagnostics);
    valid = validate_text(field.text);
    if (!valid.ok()) return FreezeResult::failure(request.job, valid.diagnostics);
    terrain_field::FieldProgram program;
    std::string parse_error;
    if (!terrain_field::FieldProgram::parse(field.text, program, parse_error))
      return FreezeResult::failure(request.job, E::invalid_input, std::move(parse_error));
    uint64_t cost = 0;
    valid = validate_program(program, request, cost);
    if (!valid.ok()) return FreezeResult::failure(request.job, valid.diagnostics);
    for (const auto &a : request.resolved.assets) {
      std::string encoded;
      valid = data::encode(*a.asset, encoded, request.options.validation);
      if (!valid.ok()) return FreezeResult::failure(request.job, valid.diagnostics);
      if (!account(encoded.size())) return FreezeResult::failure(request.job, E::resource_limit, "frozen asset snapshot exceeds working budget");
    }
    usage.working_bytes = retained;
    usage.output_bytes = retained;
    usage.operations = program.ops.size();
    valid = monitor.checkpoint(usage);
    if (!valid.ok()) return FreezeResult::failure(request.job, valid.diagnostics);
    auto frozen = request;
    for (auto &r : frozen.resolved.resources) r.bytes = std::make_shared<const std::vector<uint8_t>>(*r.bytes);
    for (auto &a : frozen.resolved.assets) a.asset = std::make_shared<const data::AssetDocument>(*a.asset);
    auto state = std::make_shared<const detail::RegionState>(std::move(frozen), std::move(program), cost, retained);
    valid = monitor.checkpoint(usage);
    if (!valid.ok()) return FreezeResult::failure(request.job, valid.diagnostics);
    return FreezeResult::success(request.job, FrozenRegion(std::move(state)));
  } catch (const std::bad_alloc &) {
    return FreezeResult::failure(request.job, E::resource_limit, "field allocation failed");
  } catch (...) {
    return FreezeResult::failure(request.job, E::backend_failure, "field preparation failed");
  }
}

data::ValidationResult lattice_position(const FrozenRegion &input, const std::array<uint32_t, 3> &index, Position &local) {
  const auto &r = input.request();
  const auto lo = minimum(r), hi = maximum(r);
  Position p;
  for (unsigned axis = 0; axis < 3; ++axis) {
    if (index[axis] > r.cell_counts[axis]) return error(E::invalid_input, "lattice index outside closed region");
    p[axis] = index[axis] == r.cell_counts[axis] ? hi[axis] :
              lo[axis] + (hi[axis] - lo[axis]) * index[axis] / r.cell_counts[axis];
  }
  local = p;
  return {};
}
namespace {
data::ValidationResult evaluate(const detail::RegionState &s, const Position &local, FieldSample &out) {
  const auto lo = minimum(s.request), hi = maximum(s.request);
  for (unsigned axis = 0; axis < 3; ++axis)
    if (!std::isfinite(local[axis]) || local[axis] < lo[axis] || local[axis] > hi[axis])
      return error(E::invalid_input, "field sample outside declared domain; include halo in the request");
  Position world;
  auto valid = local_to_world(s.request.origin_metres, local, world);
  if (!valid.ok()) return valid;
  const float x = static_cast<float>(world[0]), y = static_cast<float>(world[1]), z = static_cast<float>(world[2]);
  FieldSample sample{s.runtime.height_at(x, z), s.runtime.density_at(x, y, z),
                     s.runtime.moisture_at(x, z), s.runtime.relief_at(x, z)};
  if (!std::isfinite(sample.height_metres) || !std::isfinite(sample.density) ||
      !std::isfinite(sample.moisture) || !std::isfinite(sample.relief))
    return error(E::invalid_input, "field evaluation produced nonfinite output");
  out = sample;
  return {};
}
} // namespace
PointResult sample_local(const FrozenRegion &input, const Position &local, const services::JobControl &control) {
  const auto &s = FieldAccess::state(input);
  try {
    services::JobMonitor monitor(s.request.options.budget, control);
    services::ResourceUsage usage;
    usage.operations = s.cost; usage.queries = 1;
    usage.output_bytes = sizeof(FieldSample); usage.working_bytes = s.retained_bytes + sizeof(FieldSample);
    auto valid = monitor.checkpoint(usage);
    if (!valid.ok()) return PointResult::failure(s.request.job, valid.diagnostics);
    FieldSample sample;
    valid = evaluate(s, local, sample);
    if (valid.ok()) valid = monitor.checkpoint(usage);
    if (!valid.ok()) return PointResult::failure(s.request.job, valid.diagnostics);
    return PointResult::success(s.request.job, sample);
  } catch (...) { return PointResult::failure(s.request.job, E::backend_failure, "field sampling failed"); }
}
SamplesResult sample_region(const FrozenRegion &input, const services::JobControl &control) {
  const auto &s = FieldAccess::state(input);
  try {
    services::JobMonitor monitor(s.request.options.budget, control);
    const uint64_t count = sample_count(s.request);
    const auto &budget = s.request.options.budget;
    if (count > budget.operations / s.cost || count > budget.queries ||
        count > budget.output_bytes / sizeof(FieldSample) || count > SIZE_MAX / sizeof(FieldSample) ||
        s.retained_bytes > budget.working_bytes || count > (budget.working_bytes - s.retained_bytes) / sizeof(FieldSample))
      return SamplesResult::failure(s.request.job, E::resource_limit, "field lattice exceeds evaluation/output/working budget");
    services::ResourceUsage usage;
    usage.output_bytes = count * sizeof(FieldSample); usage.working_bytes = s.retained_bytes + usage.output_bytes;
    auto valid = monitor.checkpoint(usage);
    if (!valid.ok()) return SamplesResult::failure(s.request.job, valid.diagnostics);
    RegionSamples product{input, {}};
    product.samples.reserve(static_cast<size_t>(count));
    const auto &cells = s.request.cell_counts;
    for (uint64_t z = 0; z <= cells[2]; ++z)
      for (uint64_t y = 0; y <= cells[1]; ++y)
        for (uint64_t x = 0; x <= cells[0]; ++x) {
          usage.operations += s.cost; ++usage.queries;
          valid = monitor.checkpoint(usage);
          if (!valid.ok()) return SamplesResult::failure(s.request.job, valid.diagnostics);
          Position p;
          valid = lattice_position(input, {{static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z)}}, p);
          if (!valid.ok()) return SamplesResult::failure(s.request.job, valid.diagnostics);
          FieldSample sample;
          valid = evaluate(s, p, sample);
          if (!valid.ok()) return SamplesResult::failure(s.request.job, valid.diagnostics);
          product.samples.push_back(sample);
        }
    if (budget.progress_events) valid = monitor.report({1, "field.sample.complete"});
    if (valid.ok()) valid = monitor.checkpoint(usage);
    if (!valid.ok()) return SamplesResult::failure(s.request.job, valid.diagnostics);
    return SamplesResult::success(s.request.job, std::move(product));
  } catch (const std::bad_alloc &) {
    return SamplesResult::failure(s.request.job, E::resource_limit, "field allocation failed");
  } catch (...) { return SamplesResult::failure(s.request.job, E::backend_failure, "field sampling failed"); }
}
} // namespace matter::terrain
