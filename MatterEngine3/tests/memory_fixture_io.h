#pragma once
#include "matter/fixture_io.h"
#include <mutex>

namespace matter::test {
// Isolated per fixture; publication swaps immutable generations atomically.
// Tests can publish corrupt bytes through this same operation and can model
// failed publication without replacing a previously readable generation.
class MemoryCacheByteStorage final : public CacheByteStorage {
public:
    ReadHandle open(const std::string& path) const override {
        std::lock_guard<std::mutex> guard(lock_);
        const auto found = files_.find(SourceSnapshot::key(path));
        return found == files_.end() ? ReadHandle{} : found->second;
    }
    bool publish(const std::string& path, const Bytes& bytes) override {
        auto next = std::make_shared<const Bytes>(bytes);
        std::lock_guard<std::mutex> guard(lock_);
        if (fail_publication_) return false;
        ++publication_count_;
        files_[SourceSnapshot::key(path)] = std::move(next);
        return true;
    }
    std::map<std::string, ReadHandle> entries() const {
        std::lock_guard<std::mutex> guard(lock_);
        return files_;
    }
    std::size_t publication_count() const {
        std::lock_guard<std::mutex> guard(lock_);
        return publication_count_;
    }
    void fail_publication(bool fail) {
        std::lock_guard<std::mutex> guard(lock_);
        fail_publication_ = fail;
    }
    bool erase(const std::string& path) override {
        std::lock_guard<std::mutex> guard(lock_);
        if (fail_publication_) return false;
        files_.erase(SourceSnapshot::key(path));
        return true;
    }
    std::uint32_t move(const std::string& source, const std::string& destination,
                       bool replace) override {
        std::lock_guard<std::mutex> guard(lock_);
        if (fail_publication_) return 5u;
        const auto from_key = SourceSnapshot::key(source);
        const auto to_key = SourceSnapshot::key(destination);
        const auto from = files_.find(from_key);
        if (from == files_.end()) return 2u;
        if (from_key == to_key) return 0u;
        if (!replace && files_.count(to_key)) return 183u;
        files_[to_key] = from->second;
        files_.erase(from);
        ++publication_count_;
        return 0u;
    }
private:
    mutable std::mutex lock_;
    std::map<std::string, ReadHandle> files_;
    bool fail_publication_ = false;
    std::size_t publication_count_ = 0;
};
} // namespace matter::test
