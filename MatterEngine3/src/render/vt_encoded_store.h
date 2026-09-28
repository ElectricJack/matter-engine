#pragma once
#include "vt_encoded_pages.h"
#include <cstdio>

namespace vt::encoded {
inline std::string reference_key(const Key& key) {
    char text[112];
    std::snprintf(text, sizeof(text), "vt1/%016llx%016llx/%u/%u/%u/%u",
        static_cast<unsigned long long>(key.content.hi),
        static_cast<unsigned long long>(key.content.lo), key.rung, key.mip, key.x, key.y);
    return text;
}
// Single streaming-worker ownership. The render thread only receives immutable
// bundles. The caller supplies the session's preallocated PageBank; this service
// never creates backing banks, uploads, or touches renderer publication state.
class Store {
public:
    struct Read {
        asset_store::PageStatus status = asset_store::PageStatus::Missing;
        Bundle bundle;
    };
    static std::unique_ptr<Store> open(asset_store::PageCacheConfig config,
                                      bool writable, std::string& error) {
        if (!config.bank) { error = "VT cache requires a preallocated page bank"; return {}; }
        config.limits = limits();
        auto result = std::unique_ptr<Store>(new Store);
        if (writable) {
            config.store.read_only = false;
            result->writer_ = asset_store::BlobStore::open(config.store, &error);
            if (!result->writer_) return {};
            result->refs_ = asset_store::RefTable::open(*result->writer_, {}, &error);
            if (!result->refs_) return {};
        }
        result->reader_ = asset_store::PageCache::open(config, error);
        if (!result->reader_) return {};
        error.clear(); return result;
    }
    Read read(const Key& key) {
        if (!valid(key, {})) return {asset_store::PageStatus::Corrupt, {}};
        auto page = reader_->read_manifest(reference_key(key));
        if (!page.page) return {page.status, {}};
        Read result;
        std::string error;
        if (!decode(std::move(page.page), result.bundle, error) || !result.bundle.find(key))
            return {asset_store::PageStatus::Corrupt, {}};
        result.status = asset_store::PageStatus::Ok;
        return result;
    }
    // Caller groups nearby receiver pages into one contiguous bundle. All refs
    // become durable together after the blob index; reopening cannot observe a
    // published key whose payload was not committed. No destructor commits.
    bool write(const std::vector<Page>& pages, std::string& error, size_t count = SIZE_MAX) {
        if (count == SIZE_MAX) count = pages.size();
        if (!writer_) { error = "VT cache is read-only"; return false; }
        std::vector<uint8_t> bytes;
        if (!encode(pages, bytes, error, count)) return false;
        asset_store::BlobHash hash;
        if (writer_->put(bytes.data(), bytes.size(), &hash) != asset_store::Status::Ok ||
            !writer_->flush_index()) { error = writer_->last_error(); return false; }
        struct Previous { std::string key; bool existed; asset_store::RefInfo info; };
        std::vector<Previous> previous;
        previous.reserve(count);
        const auto rollback = [&] {
            for (const auto& p : previous) {
                if (p.existed) refs_->put(p.key, p.info.hash, p.info.kind, p.info.size);
                else refs_->erase(p.key);
            }
        };
        for (size_t index = 0; index < count; ++index) {
            const auto& page = pages[index];
            Previous old; old.key = reference_key(page.key); old.existed = refs_->peek(old.key, &old.info);
            previous.push_back(std::move(old));
            if (!refs_->put(previous.back().key, hash, kKind, bytes.size())) {
                rollback(); error = "VT cache reference update failed"; return false;
            }
        }
        if (!refs_->flush()) { rollback(); error = "VT cache reference commit failed"; return false; }
        // Existing page handles remain immutable while the index is refreshed.
        // read_manifest performs its own ref-before-index validation on reads.
        reader_->refresh();
        error.clear(); return true;
    }
    asset_store::PageCacheStats stats() const { return reader_->stats(); }
private:
    Store() = default;
    std::unique_ptr<asset_store::BlobStore> writer_;
    std::unique_ptr<asset_store::RefTable> refs_;
    std::unique_ptr<asset_store::PageCache> reader_;
};
} // namespace vt::encoded
