#pragma once
#include <filesystem>
#include <matter/asset/archive.h>
#include <matter/contracts/hosts.h>

namespace matter::asset {
// Native, thread-confined adapter. Namespace/key paths are derived internally;
// callers cannot choose cache filenames. Store validates before touching disk,
// flushes a private stage, then atomically replaces one entry. Cancellation and
// all pre-commit errors leave the previous entry unchanged.
class FileCache final : public services::CacheService {
public:
  explicit FileCache(std::filesystem::path directory,
                     services::EvaluationOptions = {});
  std::vector<data::Capability> capabilities() const override;
  services::CacheResult lookup(const services::JobEnvelope &,
                               const services::CacheKey &,
                               const services::JobControl &) override;
  services::StatusResult store(const services::JobEnvelope &,
                               const services::CacheKey &,
                               std::shared_ptr<const services::AssetProduct>,
                               const services::JobControl &) override;
  // Safe convenience boundary: failed/stale jobs cannot write a cache entry.
  services::StatusResult store_result(const services::JobEnvelope &expected,
                                      const services::CacheKey &,
                                      const services::AssetResult &,
                                      const services::JobControl & = {});

private:
  std::filesystem::path directory_;
  services::EvaluationOptions options_;
};
// Explicit single-file export. Existing destinations are refused atomically.
services::StatusResult write_new_file(const services::JobEnvelope &,
                                      const std::filesystem::path &,
                                      const Bytes &,
                                      const services::ResourceBudget & = {},
                                      const services::JobControl & = {});
} // namespace matter::asset
