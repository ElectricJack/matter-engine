#pragma once
#include <matter/contracts/generation.h>

namespace matter::services {
struct SourceModule {
  data::SourceIdentity identity;
  std::string source_utf8;
  std::vector<data::SemanticId> dependencies;
};
// Resolver discovery finishes before authoring starts. No ambient filesystem,
// DOM, Node or QuickJS values. Every transitive module is frozen and identified.
struct SourceSnapshot {
  data::SemanticId entry;
  data::ContentDigest content;
  std::vector<SourceModule> modules;
  ResolvedInputs resources;
};
struct ResolveRequest {
  data::SourceIdentity source;
  std::vector<data::Capability> required_capabilities;
};
using ResolveResult = JobResult<SourceSnapshot>;
class ResolverService {
public:
  virtual ~ResolverService() = default;
  virtual std::vector<data::Capability> capabilities() const = 0;
  virtual ResolveResult resolve(const JobEnvelope &, const ResolveRequest &,
                                const ResourceBudget &, const JobControl &) = 0;
};
struct AuthoringRequest {
  data::Value parameters{data::Value::Object{}};
  uint64_t seed = 0;
  std::vector<data::Capability> required_capabilities;
};
using RecipeResult = JobResult<data::RecipeDocument>;
class AuthoringHost {
public:
  virtual ~AuthoringHost() = default;
  virtual std::vector<data::Capability> capabilities() const = 0;
  virtual RecipeResult author(const JobEnvelope &, const SourceSnapshot &,
                              const AuthoringRequest &, const ResourceBudget &,
                              const JobControl &) = 0;
};
struct CacheKey {
  // Digest covers transitive source snapshot, canonical parameters, seed,
  // contract/algorithm versions and processing settings. Legacy hash adapters
  // use a separate namespace; renderer caches never share this key namespace.
  std::string name_space = "matter.portable.asset.v1";
  data::ContentDigest content;
};
struct CacheLookup { std::shared_ptr<const AssetProduct> asset; }; // Null is a successful miss.
using CacheResult = JobResult<CacheLookup>;
struct Acknowledgement {};
using StatusResult = JobResult<Acknowledgement>;
class CacheService {
public:
  virtual ~CacheService() = default;
  virtual std::vector<data::Capability> capabilities() const = 0;
  virtual CacheResult lookup(const JobEnvelope &, const CacheKey &,
                             const JobControl &) = 0;
  // Explicit consumer action after successful, current-result admission.
  // Atomic store: failure/cancellation leaves the previous entry unchanged.
  virtual StatusResult store(const JobEnvelope &, const CacheKey &, std::shared_ptr<const AssetProduct>,
                              const JobControl &) = 0;
};
struct BuildRequest {
  ResolveRequest source;
  AuthoringRequest authoring;
  EvaluationOptions evaluation;
  std::optional<ProcessingRequest> processing;
};
// Implementations receive injected resolver/cache/authoring/geometry services;
// children-first evaluation, scheduling and cache policy belong here. The
// constructor/wiring is implementation-specific, not a singleton dependency.
class BuildService {
public:
  virtual ~BuildService() = default;
  virtual std::vector<data::Capability> capabilities() const = 0;
  virtual AssetResult build(const JobEnvelope &, const BuildRequest &,
                            const JobControl &) = 0;
};
} // namespace matter::services
