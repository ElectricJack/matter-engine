#pragma once
#include "terrain_collision_artifact.h"
#include "matter/fixture_io.h"
#include <algorithm>
#include <memory>

namespace matter::terrain_collision::detail {
// Adapt the explicitly retained fixture cache to the production collision
// artifact loader. The cache owns synchronization and atomic move semantics;
// the adapter performs no filesystem operations and adds no storage policy.
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
        auto bytes = cache_->open(key(path));
        error.clear();
        if (!bytes) {
            error = std::make_error_code(std::errc::no_such_file_or_directory);
            return std::uintmax_t(-1);
        }
        return bytes->size();
    }
    bool read(const std::filesystem::path& path, std::size_t offset,
              std::size_t count, std::uint8_t* destination) override {
        const auto bytes = cache_->open(key(path));
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
    std::shared_ptr<CacheByteStorage> cache_;
};
} // namespace matter::terrain_collision::detail
