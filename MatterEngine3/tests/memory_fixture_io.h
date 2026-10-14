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
        files_[SourceSnapshot::key(path)] = std::move(next);
        return true;
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
private:
    mutable std::mutex lock_;
    std::map<std::string, ReadHandle> files_;
    bool fail_publication_ = false;
};
} // namespace matter::test
