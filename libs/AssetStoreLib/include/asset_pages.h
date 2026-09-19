#pragma once
// Versioned binary pages over AssetStoreLib. All objects except immutable page
// handles and PageBank are thread-confined. Execute disk work on a streaming worker.
#include "asset_store.h"
#include <atomic>
#include <functional>
#include <mutex>
#include "mem_bank.h"

namespace asset_store {

struct PageLimits {
    uint32_t max_bytes = 1024 * 1024;
    uint32_t max_sections = 256;
    uint32_t max_dependencies = 4096;
};
struct PageSection {
    uint32_t type = 0, schema = 1, stride = 1;
    std::vector<uint8_t> bytes;
};
struct PageSectionView {
    uint32_t type = 0, schema = 0, stride = 0, count = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
};
// Borrowed, validated byte views. No native struct casts or alignment promises
// beyond 8 bytes. Retain the PageHandle when taking a view from a cached page.
struct PageView {
    uint32_t kind = 0;
    std::vector<PageSectionView> sections;
    std::vector<BlobHash> dependencies;
    const PageSectionView* find(uint32_t type) const;
};
bool encode_page(uint32_t kind, const std::vector<PageSection>& sections,
                 const std::vector<BlobHash>& dependencies, const PageLimits& limits,
                 std::vector<uint8_t>& out, std::string& error);
bool decode_page(const uint8_t* bytes, size_t size, const PageLimits& limits,
                 PageView& out, std::string& error);

struct CachedPage {
    BlobHash hash;
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    PageView view;
    // Keeps the entire coalesced allocation alive, including gaps. Copies of
    // the immutable handle may safely outlive cache eviction or cache teardown.
    std::shared_ptr<const void> allocation;
};
using PageHandle = std::shared_ptr<const CachedPage>;
enum class PageStatus { Ok, Missing, Corrupt, IoError, BudgetExceeded, Cancelled };
struct PageResult { PageStatus status = PageStatus::Missing; PageHandle page; };
// Shared, synchronized bank; create at session initialization and reuse across
// cache reopen/reset. Immutable page leases may release from any thread.
class PageBank {
public:
    static std::shared_ptr<PageBank> create(size_t capacity, size_t quantum = 8,
                                           size_t max_leases = 65536);
    ~PageBank();
    MemBankStats stats() const;
    size_t quantum() const { return quantum_; }
private:
    friend class PageCache;
    bool acquire(size_t bytes, MemBankLease&);
    void release(MemBankLease);
    explicit PageBank(MemBank* bank, size_t quantum) : bank_(bank), quantum_(quantum) {}
    MemBank* bank_;
    size_t quantum_;
    mutable std::mutex mutex_;
};
struct PageCacheConfig {
    StoreConfig store;
    PageLimits limits;
    std::shared_ptr<PageBank> bank;
    uint64_t resident_bytes = 64ull * 1024 * 1024;
    uint64_t max_read_bytes = 8ull * 1024 * 1024;
    uint32_t max_requests = 4096;
    // Optional bounded physical-neighbor prefetch. Zero preserves demand-only
    // reads. Never evicts pinned bytes or displaces mandatory read admission.
    uint32_t read_ahead_bytes = 0;
};
struct PageCacheStats {
    uint64_t requests = 0, hits = 0, disk_reads = 0, disk_bytes = 0;
    uint64_t reference_reloads = 0;
    uint64_t checksums = 0, budget_rejections = 0, prefetched_pages = 0;
    // Payload allocations, including holes and padding, also while externally
    // pinned after eviction. Excludes index/directory/container metadata.
    uint64_t resident_payload_bytes = 0;
    MemBankStats bank{}; // Global bank occupancy, fragmentation and backing count.
};

class PageCache {
public:
    static std::unique_ptr<PageCache> open(const PageCacheConfig&, std::string& error);
    ~PageCache();
    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;
    // A cancelled batch publishes nothing new into the cache. Cancellation
    // does not interrupt an OS read or destroy its buffers early.
    std::vector<PageResult> read(const std::vector<BlobHash>&,
                                const std::atomic<bool>* cancel = nullptr);
    // Preserve input order while bisecting batches rejected by byte/space
    // budgets. Individual pages that still cannot fit remain BudgetExceeded.
    // Sub-batches commit independently; cancellation suppresses returned handles
    // but may leave earlier valid sub-batches cached.
    std::vector<PageResult> read_partitioned(const std::vector<BlobHash>&,
                                            const std::atomic<bool>* cancel = nullptr);
    bool refresh();
    // Read refs first, then reload the blob index. A newly published ref can
    // never be accepted against an older index. Failure leaves caller-owned
    // previous handles intact; no partially visible revision is returned.
    PageResult read_manifest(const std::string& key);
    void clear();
    PageCacheStats stats() const;
private:
    PageCache();
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Write complete pages first, then the manifest, commit the blob index, and
// finally publish the semantic reference. No destructor commits. The caller
// owns the sole writer and must not compact while readers can use old indexes.
// All dependencies must already be present (including this writer's pending
// pages). Generic RefTable::compact is NOT dependency-aware and must not be
// used on a page store; traverse retained manifests before offline maintenance.
bool publish_page_manifest(BlobStore& store, RefTable& refs, const std::string& key,
                           const std::vector<uint8_t>& manifest, const PageLimits& limits,
                           BlobHash& out, std::string& error);

// Offline maintenance: follow dependencies from EVERY semantic reference and
// explicit retained revision. Fails closed on corruption/missing dependencies
// or a traversal limit. BlobStore's maintenance lease rejects active readers.
bool compact_page_store(BlobStore&, const RefTable&, const std::vector<BlobHash>& retained_roots,
                        const PageLimits&, size_t max_pages, CompactStats&, std::string& error);

} // namespace asset_store
