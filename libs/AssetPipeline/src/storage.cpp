#include "internal.h"
#include <atomic>
#include <cstdio>
#include <fstream>
#include <matter/asset/storage.h>
#ifdef _WIN32
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace matter::asset {
namespace {
using namespace detail;
namespace fs = std::filesystem;
void key_valid(const services::CacheKey &k) {
  need(k.name_space == "matter.portable.asset.v1",
       "unsupported portable cache namespace", E::unsupported_version);
  need(digest(k.content.hex), "cache key requires lower-case SHA-256");
}
fs::path entry(const fs::path &root, const services::CacheKey &k) {
  return root / (k.content.hex + ".mapa");
}
struct Stage {
  fs::path directory;
  explicit Stage(const fs::path &parent) {
    static std::atomic<uint64_t> counter{0};
    for (unsigned i = 0; i < 100; ++i) {
      auto tag =
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()) +
          "-" + std::to_string(counter++);
      directory = parent / (".matter-stage-" + tag);
      if (fs::create_directory(directory))
        return;
    }
    throw Failure(E::backend_failure,
                  "cannot allocate private staging directory");
  }
  ~Stage() {
    std::error_code ec;
    fs::remove_all(directory, ec);
  }
};
void publish(const fs::path &path, const Bytes &bytes, bool replace,
             services::JobMonitor &monitor) {
  fs::path parent = path.parent_path();
  if (parent.empty())
    parent = ".";
  Stage stage(parent);
  const auto temp = stage.directory / "payload";
#ifdef _WIN32
  FILE *file = nullptr;
  if (_wfopen_s(&file, temp.c_str(), L"wb") != 0)
    file = nullptr;
#else
  FILE *file = std::fopen(temp.c_str(), "wb");
#endif
  need(file != nullptr, "cannot open staged cache/export file",
       E::backend_failure);
  bool good =
      std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() &&
      std::fflush(file) == 0;
#ifdef _WIN32
  if (good)
    good = _commit(_fileno(file)) == 0;
#else
  if (good)
    good = ::fsync(fileno(file)) == 0;
#endif
  if (std::fclose(file) != 0)
    good = false;
  need(good, "cannot flush staged cache/export file", E::backend_failure);
  checked(monitor.report({1, "ready_to_commit"}));
  checked(monitor.checkpoint());
#ifdef _WIN32
  const DWORD flags = replace
                          ? MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
                          : MOVEFILE_WRITE_THROUGH;
  need(MoveFileExW(temp.c_str(), path.c_str(), flags) != 0,
       "atomic cache/export publication failed", E::backend_failure);
#else
  if (replace)
    fs::rename(temp, path);
  else {
    // link is an atomic create-if-absent on the same filesystem; the private
    // stage is removed after publication, leaving the destination's link.
    need(::link(temp.c_str(), path.c_str()) == 0,
         "export destination exists or cannot be published",
         E::backend_failure);
  }
#endif
}
Bytes load(const fs::path &path, uint64_t limit) {
  auto n = fs::file_size(path);
  need(n <= limit && n <= SIZE_MAX &&
           n <= uint64_t(std::numeric_limits<std::streamsize>::max()),
       "cache entry exceeds budget", E::resource_limit);
  Bytes bytes(static_cast<size_t>(n));
  std::ifstream file(path, std::ios::binary);
  need(bool(file) &&
           bool(file.read(reinterpret_cast<char *>(bytes.data()),
                          static_cast<std::streamsize>(n))) &&
           file.peek() == std::char_traits<char>::eof(),
       "cannot read complete cache entry", E::backend_failure);
  return bytes;
}
} // namespace
FileCache::FileCache(fs::path directory, services::EvaluationOptions options)
    : directory_(std::move(directory)), options_(std::move(options)) {}
std::vector<data::Capability> FileCache::capabilities() const {
  return {{"matter.cache.prepared", 1}};
}
services::CacheResult FileCache::lookup(const services::JobEnvelope &job,
                                        const services::CacheKey &key,
                                        const services::JobControl &control) {
  return detail::boundary<services::CacheResult>(job, [&] {
    key_valid(key);
    services::JobMonitor monitor(options_.budget, control);
    detail::checked(monitor.checkpoint());
    const auto path = entry(directory_, key);
    if (!fs::exists(path))
      return services::CacheResult::success(job, {});
    auto bytes = load(path, options_.budget.working_bytes);
    // Read the stored recipe key rather than inferring it from the filename:
    // moving another valid entry here is still a mismatch. The prepared
    // archive verifies its own document/payload checksum independently.
    detail::need(bytes.size() >= 64 &&
                     detail::view(bytes).substr(0, 64) == key.content.hex,
                 "cache recipe digest mismatch");
    Bytes archive(bytes.begin() + 64, bytes.end());
    auto result = decode_prepared(job, archive, options_, control);
    if (!result.ok())
      return services::CacheResult::failure(job, result.diagnostics());
    detail::checked(monitor.checkpoint());
    return services::CacheResult::success(job, {result.value()});
  });
}
services::StatusResult
FileCache::store(const services::JobEnvelope &job,
                 const services::CacheKey &key,
                 std::shared_ptr<const services::AssetProduct> asset,
                 const services::JobControl &control) {
  return detail::boundary<services::StatusResult>(job, [&] {
    key_valid(key);
    detail::need(bool(asset), "cannot cache a null asset");
    services::JobMonitor monitor(options_.budget, control);
    detail::checked(monitor.checkpoint());
    auto result = encode_prepared(job, *asset, options_, control);
    if (!result.ok())
      return services::StatusResult::failure(job, result.diagnostics());
    Bytes bytes(key.content.hex.begin(), key.content.hex.end());
    bytes.insert(bytes.end(), result.value()->begin(), result.value()->end());
    services::ResourceUsage usage;
    usage.output_bytes = bytes.size();
    usage.working_bytes = bytes.size();
    detail::checked(monitor.checkpoint(usage));
    fs::create_directories(directory_);
    publish(entry(directory_, key), bytes, true, monitor);
    return services::StatusResult::success(job, {});
  });
}
services::StatusResult FileCache::store_result(
    const services::JobEnvelope &expected, const services::CacheKey &key,
    const services::AssetResult &result, const services::JobControl &control) {
  std::shared_ptr<const services::AssetProduct> product;
  auto valid = services::admit(result, expected, product);
  if (!valid.ok())
    return services::StatusResult::failure(expected, valid.diagnostics);
  return store(expected, key, std::move(product), control);
}
services::StatusResult write_new_file(const services::JobEnvelope &job,
                                      const fs::path &path, const Bytes &bytes,
                                      const services::ResourceBudget &budget,
                                      const services::JobControl &control) {
  return detail::boundary<services::StatusResult>(job, [&] {
    services::JobMonitor monitor(budget, control);
    services::ResourceUsage usage;
    usage.output_bytes = bytes.size();
    usage.working_bytes = bytes.size();
    detail::checked(monitor.checkpoint(usage));
    detail::need(!path.filename().empty(), "export filename is empty");
    publish(path, bytes, false, monitor);
    return services::StatusResult::success(job, {});
  });
}
} // namespace matter::asset
