#include "internal.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <matter/asset/prepared.h>
#include <set>

// Kernel-free preparation records. Nothing here links MatterSurfaceLib,
// SpatialQueryLib or MatterEngine3: a consumer of baked assets reads and checks
// what generation published without being able to generate.
namespace matter::asset {
namespace {
using namespace detail;
using V = data::Value;
using A = V::Array;
using O = V::Object;

// Mirrors the contracts identifier rule (MatterContracts/src/internal.h:21),
// which is private to that library. Derived and requested ids are checked here
// so a bad id fails before generation rather than at publication.
bool valid_id(const std::string &s) {
  if (s.empty() || s.size() > 256)
    return false;
  auto alnum = [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
  };
  if (!alnum(static_cast<unsigned char>(s[0])))
    return false;
  for (unsigned char c : s)
    if (!alnum(c) && c != '.' && c != '_' && c != ':' && c != '/' && c != '-')
      return false;
  return true;
}
data::ValidationResult failed(E code, std::string message) {
  data::ValidationResult r;
  r.diagnostics.push_back(
      {data::Severity::error, code, std::move(message), {}, {}, {}});
  return r;
}
template <class F> data::ValidationResult guarded(F &&fn) {
  try {
    fn();
    return {};
  } catch (const Failure &e) {
    return failed(e.code, e.what());
  } catch (const std::bad_alloc &) {
    return failed(E::resource_limit, "preparation record allocation failed");
  } catch (const std::exception &e) {
    return failed(E::backend_failure, e.what());
  } catch (...) {
    return failed(E::backend_failure, "unknown preparation record exception");
  }
}
const O &object(const V &v, const char *what) {
  auto p = std::get_if<O>(&v.data);
  need(p != nullptr, std::string(what) + " must be an object");
  return *p;
}
const A &array(const V &v, const char *what) {
  auto p = std::get_if<A>(&v.data);
  need(p != nullptr, std::string(what) + " must be an array");
  return *p;
}
const V *find(const O &o, const char *key) {
  auto it = o.find(key);
  return it == o.end() ? nullptr : &it->second;
}
const V &get(const O &o, const char *key) {
  auto p = find(o, key);
  need(p != nullptr, std::string("missing preparation field: ") + key);
  return *p;
}
void only(const O &o, std::initializer_list<const char *> keys,
          const char *what) {
  for (const auto &item : o)
    need(std::any_of(keys.begin(), keys.end(),
                     [&](const char *k) { return item.first == k; }),
         std::string("unknown ") + what + " field: " + item.first);
}
double real(const V &v, const char *what) {
  auto p = std::get_if<double>(&v.data);
  need(p && std::isfinite(*p), std::string(what) + " must be a finite number");
  return *p;
}
uint64_t count(const V &v, const char *what) {
  const double n = real(v, what);
  need(n >= 0 && n <= 9007199254740992.0 && std::floor(n) == n,
       std::string(what) + " must be an exact unsigned integer");
  return static_cast<uint64_t>(n);
}
std::string text(const V &v, const char *what) {
  auto p = std::get_if<std::string>(&v.data);
  need(p != nullptr, std::string(what) + " must be a string");
  return *p;
}
bool flag(const V &v, const char *what) {
  auto p = std::get_if<bool>(&v.data);
  need(p != nullptr, std::string(what) + " must be a boolean");
  return *p;
}
// A positive float32 error target. Rejects values that round to zero or
// overflow, so the stored float is exactly what generation will use.
float error_target(const V &v, const char *what) {
  const double d = real(v, what);
  need(d > 0 && d <= double(std::numeric_limits<float>::max()),
       std::string(what) + " must be a positive float32");
  const float f = static_cast<float>(d);
  need(f > 0, std::string(what) + " must be a positive float32");
  return f;
}
data::ContentDigest hashed(const V &v) {
  std::string json;
  checked(data::canonical_json(v, json));
  return data::sha256(std::string(data::canonical_profile) + "\n" + json);
}
V capability(const data::Capability &c) {
  return O{{"name", c.name}, {"version", c.version}};
}
void extend(std::vector<data::Capability> &caps) {
  for (const auto &c : prepared_capabilities())
    if (std::none_of(caps.begin(), caps.end(), [&](const data::Capability &x) {
          return x.name == c.name && x.version == c.version;
        }))
      caps.push_back(c);
}
// Requirement check shared by verify_prepared and load_prepared; the product
// has already been validated against the extended capability set.
void require_baked(const data::AssetDocument &doc,
                   const PreparedRequirements &req) {
  if (req.lod_sources.empty() && !req.lod_settings)
    return;
  if (req.lod_settings)
    need(digest(req.lod_settings->hex), "invalid required LOD settings digest");
  LodChainRecord record;
  checked(read_lod_chain(doc, record));
  if (req.lod_settings)
    need(record.settings.hex == req.lod_settings->hex,
         "baked LOD chains were generated with different settings",
         E::missing_dependency);
  std::set<std::string, std::less<>> baked;
  for (const auto &c : record.chains)
    baked.insert(c.source.value);
  for (const auto &s : req.lod_sources)
    need(baked.count(s.value) != 0,
         "LOD representations for " + s.value + " are not baked",
         E::missing_dependency);
}
} // namespace

data::ValidationResult parse_lod_settings(const data::Value &v,
                                          LodSettings &out) {
  return guarded([&] {
    const auto &o = object(v, "LOD settings");
    only(o, {"prototypes", "max_error_metres", "verification_tolerance_metres"},
         "LOD setting");
    LodSettings s;
    const auto &ids = array(get(o, "prototypes"), "LOD prototypes");
    need(!ids.empty(), "LOD settings require at least one prototype");
    std::set<std::string> sorted;
    for (const auto &item : ids) {
      auto id = text(item, "LOD prototype");
      need(valid_id(id), "invalid LOD prototype id");
      need(sorted.insert(std::move(id)).second, "duplicate LOD prototype");
    }
    for (const auto &id : sorted)
      s.prototypes.push_back({id});
    const auto &errors = array(get(o, "max_error_metres"), "LOD error targets");
    need(!errors.empty() && errors.size() <= max_lod_levels,
         "LOD settings require 1-16 error targets");
    for (const auto &item : errors) {
      const float e = error_target(item, "LOD error target");
      need(s.max_error_metres.empty() || e > s.max_error_metres.back(),
           "LOD error targets must strictly increase");
      s.max_error_metres.push_back(e);
    }
    if (auto t = find(o, "verification_tolerance_metres")) {
      const double tolerance = real(*t, "LOD verification tolerance");
      need(tolerance > 0, "LOD verification tolerance must be positive");
      s.verification_tolerance_metres = tolerance;
    }
    out = std::move(s);
  });
}

data::Value to_value(const LodSettings &s) {
  A ids, errors;
  // emplace_back throughout: moving a temporary Value into the vector trips a
  // GCC 13 -O2 maybe-uninitialized false positive in std::variant.
  for (const auto &id : s.prototypes)
    ids.emplace_back(id.value);
  for (float e : s.max_error_metres)
    errors.emplace_back(e);
  return O{{"prototypes", std::move(ids)},
           {"max_error_metres", std::move(errors)},
           {"verification_tolerance_metres", s.verification_tolerance_metres}};
}

data::ValidationResult settings_digest(const LodSettings &s,
                                       data::ContentDigest &out) {
  LodSettings normalized;
  auto r = parse_lod_settings(to_value(s), normalized);
  if (!r.ok())
    return r;
  return guarded([&] { out = hashed(to_value(normalized)); });
}

data::Value to_value(const LodChainRecord &r) {
  A chains;
  for (const auto &c : r.chains) {
    A levels;
    for (const auto &l : c.levels)
      levels.emplace_back(O{{"prototype", l.prototype.value},
                            {"requested_error_metres", l.requested_error_metres},
                            {"measured_lower_metres", l.measured_lower_metres},
                            {"measured_upper_metres", l.measured_upper_metres},
                            {"triangles", double(l.triangles)},
                            {"queries", double(l.queries)},
                            {"depth_limited", l.depth_limited}});
    chains.emplace_back(O{{"source", c.source.value},
                          {"source_triangles", double(c.source_triangles)},
                          {"levels", std::move(levels)}});
  }
  return O{{"algorithm", capability(r.algorithm)},
           {"settings", r.settings.hex},
           {"chains", std::move(chains)}};
}

data::ValidationResult read_lod_chain(const data::AssetDocument &doc,
                                      LodChainRecord &out) {
  return guarded([&] {
    const data::ExtensionEnvelope *found = nullptr;
    for (const auto &e : doc.extensions)
      if (e.capability.name == lod_chain_record) {
        need(found == nullptr, "duplicate LOD chain record");
        found = &e;
      }
    need(found != nullptr, "asset has no baked LOD chain record",
         E::missing_dependency);
    need(found->capability.version == 1, "unsupported LOD chain record version",
         E::unsupported_version);
    need(!found->required, "LOD chain record must be optional metadata");
    const auto &o = object(found->payload, "LOD chain record");
    only(o, {"algorithm", "settings", "chains"}, "LOD chain record");
    LodChainRecord record;
    const auto &algorithm = object(get(o, "algorithm"), "LOD algorithm");
    only(algorithm, {"name", "version"}, "LOD algorithm");
    record.algorithm.name = text(get(algorithm, "name"), "LOD algorithm name");
    const uint64_t version = count(get(algorithm, "version"), "LOD algorithm version");
    need(record.algorithm.name == lod_simplify,
         "unknown LOD algorithm " + record.algorithm.name,
         E::unsupported_capability);
    need(version == 1, "unsupported LOD algorithm version",
         E::unsupported_version);
    record.algorithm.version = 1;
    record.settings.hex = text(get(o, "settings"), "LOD settings digest");
    need(digest(record.settings.hex), "invalid LOD settings digest");

    std::map<std::string, const data::MeshPrototype *, std::less<>> meshes;
    for (const auto &m : doc.meshes)
      meshes.emplace(m.id.value, &m);
    std::map<std::string, float, std::less<>> lods;
    for (const auto &l : doc.lods)
      lods.emplace(l.prototype.value, l.max_error_metres);
    auto mesh = [&](const std::string &id, const char *what) {
      auto it = meshes.find(id);
      need(it != meshes.end(), std::string(what) + " is not a mesh prototype: " + id);
      return it->second;
    };
    std::set<std::string, std::less<>> sources, levels;
    const auto &chains = array(get(o, "chains"), "LOD chains");
    need(!chains.empty(), "LOD chain record has no chains");
    for (const auto &item : chains) {
      const auto &c = object(item, "LOD chain");
      only(c, {"source", "source_triangles", "levels"}, "LOD chain");
      LodChain chain;
      chain.source.value = text(get(c, "source"), "LOD chain source");
      const auto *source = mesh(chain.source.value, "LOD chain source");
      need(sources.insert(chain.source.value).second, "duplicate LOD chain source");
      chain.source_triangles = count(get(c, "source_triangles"), "LOD source triangles");
      need(chain.source_triangles == source->indices.size() / 3,
           "LOD chain source triangle count does not match its mesh");
      auto zero = lods.find(chain.source.value);
      need(zero != lods.end() && zero->second == 0,
           "LOD chain source must be a zero-error LOD representation");
      const auto &list = array(get(c, "levels"), "LOD levels");
      need(!list.empty() && list.size() <= max_lod_levels,
           "LOD chain requires 1-16 levels");
      for (const auto &entry : list) {
        const auto &l = object(entry, "LOD level");
        only(l, {"prototype", "requested_error_metres", "measured_lower_metres",
                 "measured_upper_metres", "triangles", "queries", "depth_limited"},
             "LOD level");
        LodLevelRecord level;
        level.prototype.value = text(get(l, "prototype"), "LOD level prototype");
        const auto *target = mesh(level.prototype.value, "LOD level");
        need(levels.insert(level.prototype.value).second, "LOD level appears twice");
        level.requested_error_metres =
            error_target(get(l, "requested_error_metres"), "LOD requested error");
        need(chain.levels.empty() ||
                 level.requested_error_metres > chain.levels.back().requested_error_metres,
             "LOD requested errors must strictly increase");
        level.measured_lower_metres = real(get(l, "measured_lower_metres"), "LOD measured error");
        level.measured_upper_metres = real(get(l, "measured_upper_metres"), "LOD measured error");
        need(level.measured_lower_metres >= 0 &&
                 level.measured_lower_metres <= level.measured_upper_metres,
             "invalid measured LOD error interval");
        level.triangles = count(get(l, "triangles"), "LOD level triangles");
        need(level.triangles == target->indices.size() / 3,
             "LOD level triangle count does not match its mesh");
        level.queries = count(get(l, "queries"), "LOD error queries");
        level.depth_limited = flag(get(l, "depth_limited"), "LOD depth limit flag");
        auto bound = lods.find(level.prototype.value);
        need(bound != lods.end() && double(bound->second) >= level.measured_upper_metres,
             "LOD representation error must cover the measured upper bound");
        chain.levels.push_back(std::move(level));
      }
      record.chains.push_back(std::move(chain));
    }
    for (const auto &s : sources)
      need(levels.count(s) == 0, "LOD chain source is also a level: " + s);
    out = std::move(record);
  });
}

data::ValidationResult processing_cache_key(const services::AssetProduct &source,
                                            const services::ProcessingRequest &request,
                                            const data::Capability &algorithm,
                                            services::CacheKey &out,
                                            const data::ValidationOptions &options) {
  auto r = services::validate(source, options);
  if (!r.ok())
    return r;
  return guarded([&] {
    const std::vector<data::Capability> supported{{lod_simplify, 1}};
    checked(services::require_capabilities({algorithm}, supported));
    checked(services::require_capabilities(request.required_capabilities, supported));
    need(std::any_of(request.required_capabilities.begin(),
                     request.required_capabilities.end(),
                     [&](const data::Capability &c) {
                       return c.name == algorithm.name && c.version == algorithm.version;
                     }),
         "processing request does not declare " + algorithm.name);
    LodSettings settings;
    checked(parse_lod_settings(request.settings, settings));
    data::ContentDigest content;
    checked(data::content_digest(source.document, content, options));
    auto caps = request.required_capabilities;
    std::sort(caps.begin(), caps.end(),
              [](const data::Capability &a, const data::Capability &b) {
                return a.name != b.name ? a.name < b.name : a.version < b.version;
              });
    A declared;
    for (const auto &c : caps)
      declared.push_back(capability(c));
    services::CacheKey key;
    key.content = hashed(O{{"namespace", key.name_space},
                           {"algorithm", capability(algorithm)},
                           {"source", content.hex},
                           {"capabilities", std::move(declared)},
                           {"settings", to_value(settings)}});
    out = std::move(key);
  });
}

std::vector<data::Capability> prepared_capabilities() {
  return {{lod_product, 1}};
}

data::ValidationResult verify_prepared(const services::AssetProduct &product,
                                       const PreparedRequirements &req,
                                       const data::ValidationOptions &options) {
  auto extended = options;
  auto r = guarded([&] { extend(extended.supported_capabilities); });
  if (r.ok())
    r = services::validate(product, extended);
  if (!r.ok())
    return r;
  return guarded([&] { require_baked(product.document, req); });
}

services::AssetResult load_prepared(const services::JobEnvelope &job,
                                    const Bytes &bytes,
                                    const PreparedRequirements &req,
                                    const services::EvaluationOptions &options,
                                    const services::JobControl &control) {
  return boundary<services::AssetResult>(job, [&] {
    auto extended = options;
    extend(extended.validation.supported_capabilities);
    auto result = decode_prepared(job, bytes, extended, control);
    if (!result.ok())
      return result;
    require_baked(result.value()->document, req);
    return result;
  });
}
} // namespace matter::asset
