#pragma once
#include "terrain_collision_artifact.h"
#include "matter/fixture_io.h"
#include <algorithm>
#include <memory>
#include <map>
#include <mutex>
#include <thread>

namespace matter::terrain_collision::detail {
// Adapt the explicitly retained fixture cache to the production collision
// artifact loader. The cache owns synchronization and atomic move semantics;
// the adapter performs no filesystem operations. Each file_size begins one
// immutable read generation for that caller/path; header and payload reads
// retain it across concurrent publication and removal. A later file_size
// starts a fresh generation, including a missing-key result.
class FixtureArtifactStorage final : public ArtifactStorage {
public:
    explicit FixtureArtifactStorage(std::shared_ptr<CacheByteStorage> cache)
        : cache_(std::move(cache)) {}
    bool exists(const std::filesystem::path& path, std::error_code& error) override {
        error.clear();
        return bool(cache_->open(key(path)));
    }
    std::uintmax_t file_size(const std::filesystem::path& path,
                            std::error_code& error) override {
        auto bytes = read_generation(path, true);
        error.clear();
        if (!bytes) {
            error = std::make_error_code(std::errc::no_such_file_or_directory);
            return std::uintmax_t(-1);
        }
        return bytes->size();
    }
    bool read(const std::filesystem::path& path, std::size_t offset,
              std::size_t count, std::uint8_t* destination) override {
        const auto bytes = read_generation(path, false);
        if (!bytes || offset > bytes->size() || count > bytes->size() - offset ||
            (count && !destination)) return false;
        if (count) std::copy_n(bytes->data() + offset, count, destination);
        return true;
    }
    bool write(const std::filesystem::path& path,
               const std::vector<std::uint8_t>& bytes) override {
        return cache_->publish(key(path), bytes);
    }
    void remove(const std::filesystem::path& path) noexcept override {
        try { cache_->erase(key(path)); } catch (...) {}
    }
    std::uint32_t move(const std::filesystem::path& source,
                       const std::filesystem::path& destination, bool replace) override {
        return cache_->move(key(source), key(destination), replace);
    }
private:
    static std::string key(const std::filesystem::path& path) {
        return path.lexically_normal().generic_string();
    }
    CacheByteStorage::ReadHandle read_generation(const std::filesystem::path& path,
                                                 bool refresh) {
        const auto caller_key = std::make_pair(std::this_thread::get_id(), key(path));
        std::lock_guard<std::mutex> guard(read_mutex_);
        const auto found = reads_.find(caller_key);
        if (!refresh && found != reads_.end()) return found->second;
        auto bytes = cache_->open(caller_key.second);
        reads_[caller_key] = bytes;
        return bytes;
    }
    std::shared_ptr<CacheByteStorage> cache_;
    std::mutex read_mutex_;
    std::map<std::pair<std::thread::id, std::string>, CacheByteStorage::ReadHandle> reads_;
};
} // namespace matter::terrain_collision::detail
