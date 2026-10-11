#include <matter/terrain_coordinator/coordinator.h>
#include <algorithm>
#include <deque>
#include <limits>

namespace matter::terrain_coordinator {
namespace {
using V = data::Value;
using E = data::ErrorCode;
data::ValidationResult error(E code, const char *message) {
  return {{{data::Severity::error, code, message, {}, {}, {}}}};
}
bool digest(const data::ContentDigest &d) {
  return d.hex.size() == 64 && std::all_of(d.hex.begin(), d.hex.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
V u64(uint64_t n) { return V(std::to_string(n)); }
V caps(const std::vector<data::Capability> &values) {
  V::Array a;
  for (const auto &v : values) a.emplace_back(V::Array{v.name, v.version});
  return a;
}
V vec(data::Vec3 v) { return V::Array{v.x, v.y, v.z}; }
template<class T, size_t N> V array(const std::array<T, N> &values) {
  V::Array a;
  for (auto v : values) a.emplace_back(double(v));
  return a;
}
std::vector<data::Capability> declared(const services::TerrainRequest &r) {
  auto c = r.required_capabilities;
  c.insert(c.end(), r.options.algorithms.begin(), r.options.algorithms.end());
  for (const auto &b : r.boundaries) c.push_back(b.policy);
  return c;
}
uint64_t reservation(const services::TerrainRequest &r) {
  uint64_t n = r.options.budget.output_bytes;
  for (auto extra : {r.options.validation.limits.max_resource_bytes,
                     r.options.validation.limits.max_document_bytes}) {
    if (extra > UINT64_MAX - n) return UINT64_MAX;
    n += extra;
  }
  return n;
}
} // namespace

data::ValidationResult cache_key(const services::TerrainRequest &r,
                                const data::ContentDigest &implementation, CacheKey &out) {
  // This checks structure; supported capabilities are checked at define/execute.
  auto valid = services::validate(r, declared(r));
  if (!valid.ok()) return valid;
  if (!digest(implementation)) return error(E::invalid_input, "implementation SHA-256 is required");
  V::Array deps, resources, assets, boundaries, legacy;
  for (const auto &d : r.dependencies)
    deps.emplace_back(V::Array{d.id.value, d.content.hex, d.schema_version.major, d.schema_version.minor});
  for (const auto &d : r.resolved.resources) {
    const auto &v = d.description;
    resources.emplace_back(V::Array{v.id.value, v.content.hex, uint32_t(v.scalar_type),
                                    v.components, u64(v.element_count), u64(v.byte_length)});
  }
  for (const auto &d : r.resolved.assets) {
    data::ContentDigest content;
    valid = data::content_digest(*d.asset, content, r.options.validation);
    if (!valid.ok()) return valid;
    assets.emplace_back(V::Array{d.description.id.value, d.description.content.hex,
        d.description.schema_version.major, d.description.schema_version.minor, content.hex});
  }
  for (const auto &b : r.boundaries)
    boundaries.emplace_back(V::Array{uint32_t(b.face), array(b.neighbor_key), b.sample_spacing_metres,
        b.neighbor_spacing_metres, b.policy.name, b.policy.version,
        b.seam_resource ? V(b.seam_resource->value) : V(nullptr)});
  for (const auto &h : r.field.legacy_hashes)
    legacy.emplace_back(V::Array{h.name_space, u64(h.value)});
  const auto &b = r.options.budget;
  const auto &l = r.options.validation.limits;
  V value(V::Object{
      {"profile", "matter.terrain.coordinator.cache.v1"}, {"implementation", implementation.hex},
      {"contract", V::Array{1, 0}}, {"source_revision", r.job.source_revision},
      {"field", V::Array{r.field.id.value, r.field.uri, r.field.revision, r.field.content->hex, legacy}},
      {"dependencies", deps}, {"resources", resources}, {"assets", assets},
      {"region", r.region.value}, {"key", array(r.region_key)}, {"origin", array(r.origin_metres)},
      {"domain", V::Array{vec(r.sample_domain.min), vec(r.sample_domain.max)}},
      {"cells", array(r.cell_counts)}, {"detail", r.detail_level}, {"boundaries", boundaries},
      {"required", caps(r.required_capabilities)}, {"algorithms", caps(r.options.algorithms)},
      {"consumer", caps(r.options.validation.supported_capabilities)},
      {"budget", V::Array{u64(b.operations), u64(b.vertices), u64(b.indices), u64(b.output_bytes),
          u64(b.working_bytes), u64(b.execution_microseconds), u64(b.queries), b.dependency_depth, b.progress_events}},
      {"validation", V::Array{u64(l.max_document_bytes), u64(l.max_resource_bytes), u64(l.max_vertices),
          u64(l.max_indices), u64(l.max_value_nodes), l.max_nodes, l.max_instances, l.max_depth,
          l.max_string_bytes, l.max_diagnostics}}});
  std::string canonical;
  valid = data::canonical_json(value, canonical, r.options.validation.limits);
  if (valid.ok()) out.content = data::sha256("matter.terrain.coordinator.cache.v1\n" + canonical);
  return valid;
}

struct MemoryCache::Impl {
  struct Item { CacheKey key; std::shared_ptr<const CacheEntry> entry; uint64_t reserved; };
  size_t max_entries;
  uint64_t capacity, used = 0;
  std::deque<Item> items;
};
MemoryCache::MemoryCache(size_t entries, uint64_t bytes)
    : impl_(new Impl{entries, bytes, 0, {}}) {}
MemoryCache::~MemoryCache() = default;
CacheResult MemoryCache::lookup(const services::JobEnvelope &job, const CacheKey &key) {
  auto valid = services::validate(job);
  if (!valid.ok()) return CacheResult::failure(job, valid.diagnostics);
  if (!digest(key.content)) return CacheResult::failure(job, E::invalid_input, "invalid cache digest");
  for (const auto &i : impl_->items)
    if (i.key.content.hex == key.content.hex) return CacheResult::success(job, {i.entry});
  return CacheResult::success(job, {});
}
Status MemoryCache::store(const services::JobEnvelope &job, const CacheKey &key,
                          std::shared_ptr<const CacheEntry> entry) {
  auto valid = services::validate(job);
  if (!valid.ok()) return Status::failure(job, valid.diagnostics);
  if (!entry || !entry->product || !(entry->request.job == job) || !digest(key.content))
    return Status::failure(job, E::invalid_input, "invalid terrain cache entry");
  auto available = declared(entry->request);
  // Transport supports extension envelopes; it does not implement their algorithms.
  for (const auto &p : entry->product->products) available.push_back(p.capability);
  valid = services::validate(*entry->product, entry->request, available);
  if (!valid.ok()) return Status::failure(job, valid.diagnostics);
  auto bytes = reservation(entry->request);
  auto &s = *impl_;
  if (!s.max_entries || bytes == UINT64_MAX || bytes > s.capacity)
    return Status::failure(job, E::resource_limit, "terrain cache entry exceeds capacity");
  // Allocate before removing old entries, so allocation failure is atomic too.
  s.items.push_back({key, std::move(entry), bytes});
  for (auto i = s.items.begin(); i != s.items.end() - 1;) {
    if (i->key.content.hex == key.content.hex) { s.used -= i->reserved; i = s.items.erase(i); }
    else ++i;
  }
  while (s.items.size() > s.max_entries || bytes > s.capacity - s.used) {
    s.used -= s.items.front().reserved;
    s.items.pop_front();
  }
  s.used += bytes;
  return Status::success(job, {});
}
void MemoryCache::clear() { impl_->items.clear(); impl_->used = 0; }
size_t MemoryCache::size() const { return impl_->items.size(); }
uint64_t MemoryCache::reserved_bytes() const { return impl_->used; }
} // namespace matter::terrain_coordinator
