#include <matter/contracts/terrain.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace matter::services {
namespace {
using E = data::ErrorCode;
data::ValidationResult error(E code, std::string message) {
  return {{{data::Severity::error, code, std::move(message), {}, {}, {}}}};
}
void append(data::ValidationResult &to, const data::ValidationResult &from) {
  for (const auto &d : from.diagnostics) {
    if (to.diagnostics.size() == 64) break;
    to.diagnostics.push_back(d);
  }
}
bool digest(const data::ContentDigest &d) {
  return d.hex.size() == 64 && std::all_of(d.hex.begin(), d.hex.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
bool name(const std::string &s) {
  if (s.empty() || s.size() > 256) return false;
  auto alnum = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
  };
  if (!alnum(s.front())) return false;
  return std::all_of(s.begin(), s.end(), [&](char c) {
    return alnum(c) || c == '.' || c == '_' || c == ':' || c == '/' || c == '-';
  });
}
bool capability(const data::Capability &a, const data::Capability &b) {
  return a.name == b.name && a.version == b.version;
}
bool contains(const std::vector<data::Capability> &caps, const data::Capability &c) {
  return std::any_of(caps.begin(), caps.end(), [&](const auto &a) { return capability(a, c); });
}
data::ValidationResult resources(const ResolvedInputs &inputs, const data::ValidationOptions &options) {
  data::ValidationResult r;
  if (inputs.resources.size() > options.limits.max_nodes || inputs.assets.size() > options.limits.max_nodes)
    return error(E::resource_limit, "resolved input count exceeds budget");
  std::set<std::string> seen;
  uint64_t total = 0;
  for (const auto &entry : inputs.resources) {
    const auto &d = entry.description;
    if (!name(d.id.value) || !seen.insert(d.id.value).second || !digest(d.content))
      append(r, error(E::invalid_input, "invalid or duplicate resolved resource identity"));
    uint64_t scalar = d.scalar_type == data::ScalarType::u8 ? 1 : 4;
    bool known = d.scalar_type == data::ScalarType::u8 || d.scalar_type == data::ScalarType::u32 ||
                 d.scalar_type == data::ScalarType::f32;
    if (!known || !d.components || d.element_count > UINT64_MAX / d.components / scalar ||
        d.byte_length != d.element_count * d.components * scalar || !entry.bytes ||
        (entry.bytes && entry.bytes->size() != d.byte_length)) {
      append(r, error(E::invalid_input, "resolved resource length or type mismatch"));
      continue;
    }
    if (d.byte_length > options.limits.max_resource_bytes - total) {
      append(r, error(E::resource_limit, "resolved resource byte budget exceeded"));
      continue;
    }
    total += d.byte_length;
    const auto &bytes = *entry.bytes;
    std::string_view view(bytes.empty() ? "" : reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (data::sha256(view).hex != d.content.hex)
      append(r, error(E::invalid_input, "resolved resource digest mismatch"));
    if (d.scalar_type == data::ScalarType::f32) {
      for (size_t offset = 0; offset < bytes.size(); offset += 4) {
        uint32_t bits = uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
                        (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value)) {
          append(r, error(E::invalid_input, "nonfinite resolved float32 resource"));
          break;
        }
      }
    }
  }
  seen.clear();
  for (const auto &entry : inputs.assets) {
    if (!name(entry.description.id.value) || !seen.insert(entry.description.id.value).second ||
        !digest(entry.description.content) || !entry.asset) {
      append(r, error(E::invalid_input, "invalid resolved asset identity or owner"));
      continue;
    }
    if (entry.description.schema_version.major != entry.asset->schema_version.major ||
        entry.description.schema_version.minor != entry.asset->schema_version.minor)
      append(r, error(E::unsupported_version, "resolved asset version mismatch"));
    data::ContentDigest actual;
    auto valid = data::content_digest(*entry.asset, actual, options);
    append(r, valid);
    if (valid.ok() && actual.hex != entry.description.content.hex)
      append(r, error(E::invalid_input, "resolved asset digest mismatch"));
  }
  return r;
}
data::ValidationResult dependencies(const std::vector<data::AssetDependency> &required,
                                    const ResolvedInputs &inputs) {
  data::ValidationResult r;
  std::set<std::string> seen;
  for (const auto &d : required) {
    if (!name(d.id.value) || !digest(d.content) || !seen.insert(d.id.value).second)
      append(r, error(E::invalid_input, "invalid or duplicate asset dependency"));
    auto found = std::find_if(inputs.assets.begin(), inputs.assets.end(), [&](const auto &a) {
      return a.description.id == d.id;
    });
    if (found == inputs.assets.end()) append(r, error(E::missing_dependency, "missing resolved asset"));
    else if (found->description.content.hex != d.content.hex ||
             found->description.schema_version.major != d.schema_version.major ||
             found->description.schema_version.minor != d.schema_version.minor)
      append(r, error(E::invalid_input, "resolved asset dependency identity mismatch"));
  }
  return r;
}
bool finite_bounds(const data::Bounds &bounds) {
  const float lo[] = {bounds.min.x, bounds.min.y, bounds.min.z};
  const float hi[] = {bounds.max.x, bounds.max.y, bounds.max.z};
  for (unsigned i = 0; i < 3; ++i)
    if (!std::isfinite(lo[i]) || !std::isfinite(hi[i]) || lo[i] > hi[i]) return false;
  return true;
}
bool near(double a, double b) {
  return std::abs(a - b) <= 1e-5 * std::max({1.0, std::abs(a), std::abs(b)});
}
data::ValidationResult boundary(const TerrainBoundaryEnvelope &b, const TerrainRequest &request,
                                const std::vector<data::Capability> &available, double spacing) {
  auto face = static_cast<uint32_t>(b.face);
  if (face >= 6) return error(E::invalid_input, "invalid terrain face");
  auto key = request.region_key;
  int64_t neighbor = int64_t(key[face / 2]) + (face % 2 ? 1 : -1);
  if (neighbor < INT32_MIN || neighbor > INT32_MAX)
    return error(E::invalid_input, "terrain neighbor key overflow");
  key[face / 2] = static_cast<int32_t>(neighbor);
  if (key != b.neighbor_key || !std::isfinite(b.sample_spacing_metres) ||
      !std::isfinite(b.neighbor_spacing_metres) || b.sample_spacing_metres <= 0 ||
      b.neighbor_spacing_metres <= 0 || !near(b.sample_spacing_metres, spacing))
    return error(E::invalid_input, "terrain neighbor coordinate or spacing mismatch");
  if (b.seam_resource && !name(b.seam_resource->value))
    return error(E::invalid_input, "invalid terrain seam resource identity");
  if (!contains(request.required_capabilities, b.policy))
    return error(E::unsupported_capability, "terrain boundary policy must be declared by request");
  return require_capabilities({b.policy}, available);
}
} // namespace

data::ValidationResult validate(const JobEnvelope &e) {
  if (e.request_id.empty() || e.source_revision.empty() || !e.generation_epoch ||
      e.request_id.size() > 65536 || e.source_revision.size() > 65536)
    return error(E::invalid_input, "job requires bounded nonempty request/revision and nonzero epoch");
  return data::validate_value(data::Value::Array{e.request_id, e.source_revision});
}
data::ValidationResult require_capabilities(const std::vector<data::Capability> &required,
                                            const std::vector<data::Capability> &available) {
  data::ValidationResult r;
  if (required.size() > 10000 || available.size() > 10000)
    return error(E::resource_limit, "capability count exceeds v1 ceiling");
  std::set<std::string> seen;
  for (const auto &c : required) {
    if (!name(c.name) || !c.version || !seen.insert(c.name).second)
      append(r, error(E::invalid_input, "invalid or duplicate required capability"));
    else if (!contains(available, c))
      append(r, error(E::unsupported_capability, "unsupported capability " + c.name + "@" +
                                                        std::to_string(c.version)));
  }
  return r;
}
data::ValidationResult validate(const ResourceBudget &b) {
  if (!b.operations || !b.vertices || !b.indices || !b.output_bytes || !b.working_bytes ||
      !b.execution_microseconds || !b.queries || !b.dependency_depth ||
      b.execution_microseconds > uint64_t(INT64_MAX))
    return error(E::invalid_input, "finite nonzero resource ceilings required");
  return {};
}
JobMonitor::JobMonitor(ResourceBudget b, JobControl c)
    : budget_(b), control_(std::move(c)), started_(std::chrono::steady_clock::now()),
      terminal_(validate(b)) {}
data::ValidationResult JobMonitor::checkpoint(const ResourceUsage &u) {
  if (!terminal_.ok()) return terminal_;
  if (control_.cancellation && control_.cancellation->load(std::memory_order_relaxed))
    terminal_ = error(E::cancelled, "job cancelled");
  else if (u.operations < previous_.operations || u.vertices < previous_.vertices ||
           u.indices < previous_.indices || u.output_bytes < previous_.output_bytes ||
           u.queries < previous_.queries)
    terminal_ = error(E::invalid_input, "cumulative resource usage cannot decrease");
  else if (u.operations > budget_.operations || u.vertices > budget_.vertices ||
           u.indices > budget_.indices || u.output_bytes > budget_.output_bytes ||
           u.working_bytes > budget_.working_bytes || u.queries > budget_.queries ||
           u.dependency_depth > budget_.dependency_depth ||
           std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - started_).count() >=
               static_cast<int64_t>(budget_.execution_microseconds))
    terminal_ = error(E::resource_limit, "job resource or execution budget exceeded");
  previous_ = u;
  return terminal_;
}
data::ValidationResult JobMonitor::report(Progress progress) {
  auto checked = checkpoint(previous_);
  if (!checked.ok()) return checked;
  if (!std::isfinite(progress.fraction) || progress.fraction < fraction_ ||
      progress.fraction > 1 || progress.stage.size() > 65536)
    terminal_ = error(E::invalid_input, "invalid progress value");
  else if (control_.progress) {
    if (reports_ >= budget_.progress_events)
      terminal_ = error(E::resource_limit, "progress notification budget exceeded");
    else {
      ++reports_;
      fraction_ = progress.fraction;
      try { control_.progress(progress); }
      catch (...) { terminal_ = error(E::backend_failure, "progress callback threw"); }
    }
  }
  return terminal_;
}
data::ValidationResult validate(const data::RecipeDocument &recipe, const ResolvedInputs &inputs,
                                const data::ValidationOptions &options) {
  auto r = data::validate(recipe, options);
  if (!r.ok()) return r;
  append(r, resources(inputs, options));
  for (const auto &d : recipe.resources) {
    auto found = std::find_if(inputs.resources.begin(), inputs.resources.end(), [&](const auto &a) {
      return a.description.id == d.id;
    });
    if (found == inputs.resources.end()) append(r, error(E::missing_dependency, "missing resolved resource"));
    else if (found->description.content.hex != d.content.hex || found->description.byte_length != d.byte_length ||
             found->description.element_count != d.element_count || found->description.components != d.components ||
             found->description.scalar_type != d.scalar_type)
      append(r, error(E::invalid_input, "resolved resource description mismatch"));
  }
  append(r, dependencies(recipe.asset_dependencies, inputs));
  // Descriptions alone cannot validate indexed meshes. Once all payloads are
  // verified, check the actual little-endian uint32 indices before evaluation.
  // Do not inspect bytes after a failed length/type/digest boundary check.
  if (!r.ok()) return r;
  for (const auto &node : recipe.nodes) {
    if (node.operation != "mesh.input") continue;
    const ResolvedResource *positions = nullptr, *indices = nullptr;
    for (const auto &input : node.inputs) {
      auto found = std::find_if(inputs.resources.begin(), inputs.resources.end(), [&](const auto &entry) {
        return entry.description.id == input.reference;
      });
      if (found == inputs.resources.end()) continue; // Already rejected above.
      if (input.port == "positions") positions = &*found;
      if (input.port == "indices") indices = &*found;
    }
    if (!positions || !indices) continue; // Structural validation requires both.
    const auto &bytes = *indices->bytes;
    for (size_t offset = 0; offset < bytes.size(); offset += 4) {
      uint32_t index = uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
                       (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
      if (index >= positions->description.element_count) {
        data::Diagnostic d{data::Severity::error, E::invalid_input,
                           "resolved mesh index outside positions", node.id.value, {}, {}};
        auto source = recipe.source_map.find(node.id.value);
        if (source != recipe.source_map.end()) d.source = source->second;
        append(r, {{std::move(d)}});
        break;
      }
    }
  }
  return r;
}
data::ValidationResult validate(const QueryBatch &batch, const data::AssetDocument &asset,
                                const EvaluationOptions &options) {
  auto r = data::validate(asset, options.validation);
  append(r, validate(options.budget));
  if (batch.samples.size() > options.budget.queries)
    return error(E::resource_limit, "surface query count exceeds budget");
  for (const auto &q : batch.samples) {
    const auto &p = q.position;
    const auto &d = q.direction;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
        !std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z) ||
        !std::isfinite(q.max_distance_metres) || q.max_distance_metres <= 0 ||
        (q.kind != QueryKind::closest_surface && q.kind != QueryKind::ray_surface) ||
        (q.kind == QueryKind::ray_surface && d.x == 0 && d.y == 0 && d.z == 0))
      append(r, error(E::invalid_input, "invalid surface query"));
    if (std::none_of(asset.meshes.begin(), asset.meshes.end(), [&](const auto &m) { return m.id == q.prototype; }))
      append(r, error(E::missing_dependency, "surface query prototype missing"));
  }
  return r;
}
data::ValidationResult validate(const AssetProduct &product, const data::ValidationOptions &options) {
  auto r = data::validate(product.document, options);
  if (!r.ok()) return r;
  append(r, resources(product.resources, options));
  for (const auto &required : product.document.resources) {
    auto found = std::find_if(product.resources.resources.begin(), product.resources.resources.end(), [&](const auto &a) {
      const auto &d = a.description;
      return d.id == required.id && d.content.hex == required.content.hex &&
             d.byte_length == required.byte_length && d.element_count == required.element_count &&
             d.components == required.components && d.scalar_type == required.scalar_type;
    });
    if (found == product.resources.resources.end()) append(r, error(E::missing_dependency, "asset output payload missing"));
  }
  return r;
}
AssetResult finish_asset(const JobEnvelope &job, data::AssetDocument asset, const EvaluationOptions &options,
                          JobMonitor &monitor, ResourceUsage usage, ResolvedInputs output_resources) {
  auto r = validate(options.budget);
  if (r.ok()) r = monitor.checkpoint();
  if (!r.ok()) return AssetResult::failure(job, r.diagnostics);
  AssetProduct product{std::move(asset), std::move(output_resources)};
  r = validate(product, options.validation);
  if (!r.ok()) return AssetResult::failure(job, r.diagnostics);
  uint64_t payload_bytes = 0;
  for (const auto &resource : product.resources.resources) {
    if (resource.description.byte_length > options.budget.output_bytes - payload_bytes)
      return AssetResult::failure(job, E::resource_limit, "asset payload output exceeds budget");
    payload_bytes += resource.description.byte_length;
  }
  uint64_t vertices = 0, indices = 0;
  for (const auto &m : product.document.meshes) {
    if (m.positions.size() > options.budget.vertices - vertices ||
        m.indices.size() > options.budget.indices - indices)
      return AssetResult::failure(job, E::resource_limit, "asset mesh output exceeds budget");
    vertices += m.positions.size();
    indices += m.indices.size();
  }
  // Checked canonical encoding bounds all owned metadata/attribute output too.
  auto encoding_options = options.validation;
  encoding_options.limits.max_document_bytes = std::min(encoding_options.limits.max_document_bytes,
                                                       options.budget.output_bytes - payload_bytes);
  std::string encoded;
  r = data::encode(product.document, encoded, encoding_options);
  if (!r.ok()) return AssetResult::failure(job, r.diagnostics);
  usage.vertices = std::max(usage.vertices, vertices);
  usage.indices = std::max(usage.indices, indices);
  usage.output_bytes = std::max(usage.output_bytes, uint64_t(encoded.size()) + payload_bytes);
  r = monitor.checkpoint(usage);
  if (!r.ok()) return AssetResult::failure(job, r.diagnostics);
  return AssetResult::success(job, std::move(product));
}
data::ValidationResult validate(const TerrainRequest &t, const std::vector<data::Capability> &available) {
  auto r = validate(t.job);
  if (t.boundaries.size() > 6) return error(E::invalid_input, "terrain has more than six boundary faces");
  append(r, validate(t.options.budget));
  append(r, require_capabilities(t.required_capabilities, available));
  append(r, require_capabilities(t.options.algorithms, available));
  if (!contains(t.required_capabilities, {"matter.terrain.generate", 1}))
    append(r, error(E::unsupported_capability, "terrain generation capability must be declared"));
  if (!name(t.region.value) || !name(t.field.id.value) || t.field.revision.empty() ||
      !t.field.content || !digest(*t.field.content) || !finite_bounds(t.sample_domain))
    append(r, error(E::invalid_input, "invalid terrain field, region or sample domain"));
  for (auto n : t.origin_metres)
    if (!std::isfinite(n)) append(r, error(E::invalid_input, "nonfinite terrain origin"));
  const float lo[] = {t.sample_domain.min.x, t.sample_domain.min.y, t.sample_domain.min.z};
  const float hi[] = {t.sample_domain.max.x, t.sample_domain.max.y, t.sample_domain.max.z};
  double spacing = 0;
  uint64_t samples = 1;
  for (unsigned i = 0; i < 3; ++i) {
    if (!t.cell_counts[i] || !(hi[i] > lo[i])) {
      append(r, error(E::invalid_input, "terrain domain and cell counts must be positive"));
      continue;
    }
    double s = (double(hi[i]) - double(lo[i])) / t.cell_counts[i];
    if (!i) spacing = s;
    else if (!near(s, spacing)) append(r, error(E::invalid_input, "v1 boundary envelope requires uniform sample spacing"));
    uint64_t count = uint64_t(t.cell_counts[i]) + 1;
    if (samples > t.options.budget.operations / count) {
      append(r, error(E::resource_limit, "terrain sample count exceeds operation budget"));
      samples = t.options.budget.operations;
    } else samples *= count;
  }
  std::set<uint32_t> faces;
  for (const auto &b : t.boundaries) {
    if (!faces.insert(static_cast<uint32_t>(b.face)).second)
      append(r, error(E::invalid_input, "duplicate terrain boundary face"));
    append(r, boundary(b, t, available, spacing));
  }
  append(r, resources(t.resolved, t.options.validation));
  append(r, dependencies(t.dependencies, t.resolved));
  return r;
}
data::ValidationResult validate(const TerrainProduct &p, const TerrainRequest &t,
                                const std::vector<data::Capability> &available) {
  auto r = validate(t, available);
  if (!r.ok()) return r;
  if (p.products.size() > t.options.validation.limits.max_nodes)
    return error(E::resource_limit, "terrain extension count exceeds budget");
  uint64_t output_bytes = 0;
  if (!(p.job == t.job) || p.region != t.region || p.region_key != t.region_key ||
      p.origin_metres != t.origin_metres || p.field_content.hex != t.field.content->hex ||
      p.cell_counts != t.cell_counts || p.detail_level != t.detail_level)
    append(r, error(E::stale_result, "terrain product no longer matches requested field/region/detail"));
  if (!p.geometry || !finite_bounds(p.bounds))
    append(r, error(E::invalid_input, "terrain geometry owner and finite bounds required"));
  if (p.geometry) {
    append(r, data::validate(*p.geometry, t.options.validation));
    auto bounded = t.options.validation;
    bounded.limits.max_document_bytes = std::min(bounded.limits.max_document_bytes, t.options.budget.output_bytes);
    std::string encoded;
    auto encoded_valid = data::encode(*p.geometry, encoded, bounded);
    append(r, encoded_valid);
    if (encoded_valid.ok()) output_bytes = encoded.size();
    const auto &b = p.geometry->bounds;
    if (p.bounds.min.x > b.min.x || p.bounds.min.y > b.min.y || p.bounds.min.z > b.min.z ||
        p.bounds.max.x < b.max.x || p.bounds.max.y < b.max.y || p.bounds.max.z < b.max.z)
      append(r, error(E::invalid_input, "terrain product bounds must enclose geometry bounds"));
    // Only resources are checked here; this is not a geometry recipe invocation.
    uint64_t vertices = 0, indices = 0;
    for (const auto &m : p.geometry->meshes) {
      if (m.positions.size() > t.options.budget.vertices - vertices ||
          m.indices.size() > t.options.budget.indices - indices) {
        append(r, error(E::resource_limit, "terrain geometry exceeds output budget"));
        break;
      }
      vertices += m.positions.size(); indices += m.indices.size();
    }
    for (const auto &d : p.geometry->resources) {
      auto found = std::find_if(p.resources.resources.begin(), p.resources.resources.end(), [&](const auto &a) {
        return a.description.id == d.id && a.description.content.hex == d.content.hex &&
               a.description.byte_length == d.byte_length && a.description.scalar_type == d.scalar_type &&
               a.description.components == d.components && a.description.element_count == d.element_count;
      });
      if (found == p.resources.resources.end()) append(r, error(E::missing_dependency, "terrain geometry resource missing"));
    }
  }
  append(r, resources(p.resources, t.options.validation));
  for (const auto &resource : p.resources.resources) {
    if (resource.description.byte_length > t.options.budget.output_bytes - output_bytes) {
      append(r, error(E::resource_limit, "terrain payload output exceeds budget"));
      break;
    }
    output_bytes += resource.description.byte_length;
  }
  if (p.boundaries.size() != t.boundaries.size())
    append(r, error(E::invalid_input, "terrain boundary set mismatch"));
  std::set<uint32_t> seen;
  double spacing = (double(t.sample_domain.max.x) - t.sample_domain.min.x) / t.cell_counts[0];
  for (const auto &b : p.boundaries) {
    if (!seen.insert(static_cast<uint32_t>(b.face)).second)
      append(r, error(E::invalid_input, "duplicate product terrain boundary"));
    append(r, boundary(b, t, available, spacing));
    auto found = std::find_if(t.boundaries.begin(), t.boundaries.end(), [&](const auto &a) { return a.face == b.face; });
    if (found == t.boundaries.end() || b.neighbor_key != found->neighbor_key ||
        b.sample_spacing_metres != found->sample_spacing_metres ||
        b.neighbor_spacing_metres != found->neighbor_spacing_metres || !capability(b.policy, found->policy) ||
        (found->seam_resource && found->seam_resource != b.seam_resource))
      append(r, error(E::invalid_input, "terrain boundary request mismatch"));
    if (b.seam_resource && std::none_of(p.resources.resources.begin(), p.resources.resources.end(),
        [&](const auto &a) { return a.description.id == *b.seam_resource; }))
      append(r, error(E::missing_dependency, "terrain seam payload missing"));
  }
  std::vector<data::Capability> products;
  for (const auto &e : p.products) {
    append(r, data::validate_value(e.payload, t.options.validation.limits));
    auto bounded = t.options.validation.limits;
    bounded.max_document_bytes = std::min(bounded.max_document_bytes, t.options.budget.output_bytes - output_bytes);
    std::string encoded;
    auto encoded_valid = data::canonical_json(e.payload, encoded, bounded);
    append(r, encoded_valid);
    if (encoded_valid.ok()) output_bytes += encoded.size();
    if (e.required) products.push_back(e.capability);
  }
  append(r, require_capabilities(products, available));
  return r;
}
data::ValidationResult admit_terrain(const TerrainResult &result, const TerrainRequest &expected,
                                     const std::vector<data::Capability> &available,
                                     std::shared_ptr<const TerrainProduct> &destination) {
  if (!(result.envelope() == expected.job)) return error(E::stale_result, "stale terrain result envelope");
  if (!result.ok()) return {result.diagnostics()};
  auto valid = validate(*result.value(), expected, available);
  if (!valid.ok()) return valid;
  return admit(result, expected.job, destination);
}
} // namespace matter::services
