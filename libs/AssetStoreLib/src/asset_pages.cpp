#include "asset_pages.h"
#include "store_format.h"
#include "store_os.h"
#include <algorithm>
#include <map>
#include <iterator>
#include <set>
#include <utility>

namespace asset_store {
namespace {
constexpr uint32_t page_magic = 0x31475041; // APG1, little endian
constexpr size_t header_bytes = 32, section_bytes = 32;
size_t align8(size_t n) { return (n + 7) & ~size_t(7); }
bool fail(std::string& error, const char* message) { error = message; return false; }
}
const PageSectionView* PageView::find(uint32_t type) const {
    for (const auto& section : sections) if (section.type == type) return &section;
    return nullptr;
}
bool encode_page(uint32_t kind, const std::vector<PageSection>& sections,
                 const std::vector<BlobHash>& dependencies, const PageLimits& limits,
                 std::vector<uint8_t>& out, std::string& error) {
    if (!kind || sections.size() > limits.max_sections || dependencies.size() > limits.max_dependencies)
        return fail(error, "page kind or directory limits invalid");
    uint64_t length = header_bytes + uint64_t(sections.size()) * section_bytes + uint64_t(dependencies.size()) * 16;
    std::set<uint32_t> types;
    std::set<BlobHash> deps;
    for (const auto& dep : dependencies)
        if (!dep.valid() || !deps.insert(dep).second) return fail(error, "invalid or duplicate dependency");
    for (const auto& section : sections) {
        if (!section.type || !section.schema || !section.stride || section.bytes.empty() ||
            section.bytes.size() % section.stride || !types.insert(section.type).second)
            return fail(error, "invalid or duplicate page section");
        length += (uint64_t(section.bytes.size()) + 7) & ~uint64_t(7);
        if (length > limits.max_bytes) return fail(error, "page exceeds byte limit");
    }
    if (length > limits.max_bytes || length > UINT32_MAX) return fail(error, "page exceeds byte limit");
    std::vector<uint8_t> bytes(static_cast<size_t>(length), 0);
    put_u32(bytes.data(), page_magic); put_u32(bytes.data() + 4, 1);
    put_u32(bytes.data() + 8, kind); put_u32(bytes.data() + 12, static_cast<uint32_t>(length));
    put_u32(bytes.data() + 16, static_cast<uint32_t>(sections.size()));
    put_u32(bytes.data() + 20, static_cast<uint32_t>(dependencies.size()));
    size_t offset = header_bytes + sections.size() * section_bytes + dependencies.size() * 16;
    for (size_t i = 0; i < sections.size(); ++i) {
        const auto& s = sections[i]; auto* row = bytes.data() + header_bytes + i * section_bytes;
        put_u32(row, s.type); put_u32(row + 4, s.schema); put_u32(row + 8, static_cast<uint32_t>(offset));
        put_u32(row + 12, static_cast<uint32_t>(s.bytes.size())); put_u32(row + 16, s.stride);
        put_u32(row + 20, static_cast<uint32_t>(s.bytes.size() / s.stride));
        std::copy(s.bytes.begin(), s.bytes.end(), bytes.begin() + offset);
        offset += align8(s.bytes.size());
    }
    auto* dep = bytes.data() + header_bytes + sections.size() * section_bytes;
    for (const auto& h : dependencies) { put_u64(dep, h.lo); put_u64(dep + 8, h.hi); dep += 16; }
    out = std::move(bytes); error.clear(); return true;
}
bool decode_page(const uint8_t* bytes, size_t size, const PageLimits& limits,
                 PageView& out, std::string& error) {
    if (!bytes || size < header_bytes || size > limits.max_bytes || size > UINT32_MAX)
        return fail(error, "page byte limit or truncated header");
    if (get_u32(bytes) != page_magic || get_u32(bytes + 4) != 1 || !get_u32(bytes + 8) ||
        get_u32(bytes + 12) != size || get_u32(bytes + 24) || get_u32(bytes + 28))
        return fail(error, "unsupported or corrupt page envelope");
    const uint32_t ns = get_u32(bytes + 16), nd = get_u32(bytes + 20);
    const uint64_t directory_end = header_bytes + uint64_t(ns) * section_bytes + uint64_t(nd) * 16;
    if (ns > limits.max_sections || nd > limits.max_dependencies || directory_end > size)
        return fail(error, "page directory exceeds limits");
    PageView view; view.kind = get_u32(bytes + 8);
    std::set<uint32_t> types; std::set<BlobHash> deps;
    size_t expected = static_cast<size_t>(directory_end);
    for (uint32_t i = 0; i < ns; ++i) {
        const auto* row = bytes + header_bytes + size_t(i) * section_bytes;
        PageSectionView s;
        s.type = get_u32(row); s.schema = get_u32(row + 4);
        const size_t offset = get_u32(row + 8); s.size = get_u32(row + 12);
        s.stride = get_u32(row + 16); s.count = get_u32(row + 20);
        if (!s.type || !s.schema || !s.stride || !s.size || !types.insert(s.type).second ||
            offset != expected || offset > size || s.size > size - offset ||
            uint64_t(s.stride) * s.count != s.size || get_u64(row + 24))
            return fail(error, "invalid page section extent or schema");
        expected = offset + align8(s.size);
        if (expected > size) return fail(error, "truncated section padding");
        for (size_t p = offset + s.size; p < expected; ++p)
            if (bytes[p]) return fail(error, "noncanonical section padding");
        s.data = bytes + offset; view.sections.push_back(s);
    }
    if (expected != size) return fail(error, "unclaimed page payload");
    const auto* dep = bytes + header_bytes + size_t(ns) * section_bytes;
    for (uint32_t i = 0; i < nd; ++i, dep += 16) {
        BlobHash h{get_u64(dep), get_u64(dep + 8)};
        if (!h.valid() || !deps.insert(h).second) return fail(error, "invalid page dependency");
        view.dependencies.push_back(h);
    }
    out = std::move(view); error.clear(); return true;
}

std::shared_ptr<PageBank> PageBank::create(size_t capacity, size_t quantum, size_t max_leases) {
    MemBankConfig config{capacity, quantum, quantum ? std::min(max_leases, capacity/quantum) : 0, 0, nullptr};
    auto* bank = mem_bank_create(&config);
    return bank ? std::shared_ptr<PageBank>(new PageBank(bank, quantum)) : nullptr;
}
PageBank::~PageBank() { mem_bank_destroy(bank_); }
bool PageBank::acquire(size_t bytes, MemBankLease& lease) {
    std::lock_guard<std::mutex> lock(mutex_); return mem_bank_acquire(bank_, bytes, &lease) != 0;
}
void PageBank::release(MemBankLease lease) {
    std::lock_guard<std::mutex> lock(mutex_); mem_bank_release(bank_, lease);
}
MemBankStats PageBank::stats() const {
    std::lock_guard<std::mutex> lock(mutex_); return mem_bank_stats(bank_);
}
struct PageCache::Impl {
    struct Ledger { std::atomic<uint64_t> bytes{0}; };
    struct Allocation {
        std::shared_ptr<PageBank> bank;
        MemBankLease lease{};
        uint64_t bytes = 0;
        std::shared_ptr<Ledger> ledger;
        ~Allocation() { if (bank) bank->release(lease); if (ledger) ledger->bytes.fetch_sub(bytes); }
    };
    struct Entry { PageHandle page; uint64_t touch; };
    PageCacheConfig cfg;
    std::unique_ptr<BlobStore> store;
    std::unique_ptr<RefTable> references;
    uint64_t reference_stamp = 0;
    std::map<BlobHash, Entry> entries;
    std::shared_ptr<Ledger> ledger = std::make_shared<Ledger>();
    PageCacheStats counters;
    uint64_t tick = 0;
    uint64_t locality_revision = UINT64_MAX;
    std::map<BlobHash, size_t> locality_group;
    std::vector<std::vector<BlobHash>> locality;
    void refresh_locality() {
        if (!cfg.read_ahead_bytes || locality_revision == store->snapshot_revision()) return;
        locality.clear(); locality_group.clear();
        BlobLocation previous{}; uint64_t begin = 0, end = 0;
        for (const auto& hash : store->all_hashes()) {
            BlobLocation current{}; if (!store->locate(hash, &current)) continue;
            if (current.length > cfg.limits.max_bytes) continue;
            const uint64_t next_end = current.offset + current.length;
            if (locality.empty() || current.pack != previous.pack || current.offset < end ||
                current.offset-end > cfg.store.batch_gap_bytes || next_end-begin > cfg.read_ahead_bytes ||
                locality.back().size() >= cfg.max_requests) {
                locality.emplace_back(); begin = current.offset;
            }
            locality.back().push_back(hash); locality_group[hash] = locality.size()-1;
            previous = current; end = next_end;
        }
        locality_revision = store->snapshot_revision();
    }
    bool reserve(uint64_t bytes, bool evict_one = false) {
        if (bytes > cfg.resident_bytes) return false;
        while ((evict_one || ledger->bytes.load() > cfg.resident_bytes - bytes) && !entries.empty()) {
            // Evict allocations, not individual views: removing one view from
            // a pinned coalesced range frees nothing and causes avoidable
            // re-reads. Keep externally pinned ranges discoverable as hits.
            struct Group { size_t pages = 0; long owners = 0; uint64_t touch = 0; bool pinned = false; };
            std::map<const void*, Group> groups;
            for (const auto& item : entries) {
                const auto& page = item.second.page;
                auto& group = groups[page->allocation.get()];
                ++group.pages; group.owners = page->allocation.use_count();
                group.touch = std::max(group.touch, item.second.touch);
                group.pinned = group.pinned || page.use_count() > 1;
            }
            const void* victim = nullptr; uint64_t oldest = UINT64_MAX;
            for (const auto& group : groups) {
                const auto& value = group.second;
                if (!value.pinned && value.owners == static_cast<long>(value.pages) && value.touch < oldest) {
                    victim = group.first; oldest = value.touch;
                }
            }
            if (!victim) return false;
            evict_one = false;
            for (auto it = entries.begin(); it != entries.end();) {
                if (it->second.page->allocation.get() == victim) it = entries.erase(it);
                else ++it;
            }
        }
        return !evict_one && ledger->bytes.load() <= cfg.resident_bytes - bytes;
    }
};
PageCache::PageCache() : d_(new Impl) {}
PageCache::~PageCache() = default;
std::unique_ptr<PageCache> PageCache::open(const PageCacheConfig& config, std::string& error) {
    if (config.resident_bytes > SIZE_MAX || !config.resident_bytes || !config.max_read_bytes || !config.max_requests ||
        config.max_requests > 65536 || config.limits.max_bytes < header_bytes) {
        fail(error, "invalid page cache limits"); return {};
    }
    auto cache = std::unique_ptr<PageCache>(new PageCache);
    cache->d_->cfg = config;
    auto cfg = config.store; cfg.read_only = true;
    cache->d_->store = BlobStore::open(cfg, &error);
    if (!cache->d_->store) return {};
    if (!cache->d_->cfg.bank)
        cache->d_->cfg.bank = PageBank::create(static_cast<size_t>(config.resident_bytes));
    if (!cache->d_->cfg.bank) { fail(error, "page bank initialization failed"); return {}; }
    return cache;
}
std::vector<PageResult> PageCache::read(const std::vector<BlobHash>& hashes, const std::atomic<bool>* cancel) {
    auto& d = *d_;
    std::vector<PageResult> results(hashes.size());
    const auto cancelled = [&] { return cancel && cancel->load(); };
    if (hashes.size() > d.cfg.max_requests || cancelled()) {
        for (auto& r : results) r.status = cancelled() ? PageStatus::Cancelled : PageStatus::BudgetExceeded;
        return results;
    }
    d.counters.requests += hashes.size();
    std::map<BlobHash, std::vector<size_t>> pending;
    for (size_t i = 0; i < hashes.size(); ++i) {
        auto it = d.entries.find(hashes[i]);
        if (it != d.entries.end()) {
            it->second.touch = ++d.tick; results[i] = {PageStatus::Ok, it->second.page}; ++d.counters.hits;
        } else pending[hashes[i]].push_back(i);
    }
    ReadBatch batch(*d.store);
    std::vector<BlobHash> requested;
    for (const auto& item : pending) {
        const size_t length = d.store->size_of(item.first);
        if (!length) continue;
        if (length > d.cfg.limits.max_bytes) {
            for (size_t slot : item.second) results[slot].status = PageStatus::Corrupt;
            continue;
        }
        requested.push_back(item.first); batch.add(item.first);
    }
    const size_t demanded = requested.size();
    d.refresh_locality();
    if (d.cfg.read_ahead_bytes && demanded) {
        // Expand the first missing demand's physical group. Other demands retain
        // priority; speculative work is dropped if the complete batch will not fit.
        const auto group = d.locality_group.find(requested.front());
        if (group != d.locality_group.end()) {
            std::set<BlobHash> included(requested.begin(), requested.end());
            for (const auto& hash : d.locality[group->second]) {
                if (requested.size() >= d.cfg.max_requests) break;
                if (d.entries.count(hash) || !included.insert(hash).second) continue;
                requested.push_back(hash); batch.add(hash);
            }
        }
    }
    size_t bytes = batch.allocation_bytes();
    const size_t quantum = d.cfg.bank->quantum();
    const auto rounded = [&](size_t n) { return n > SIZE_MAX-(quantum-1) ? SIZE_MAX : (n+quantum-1)&~(quantum-1); };
    size_t charge = rounded(bytes);
    if (requested.size() > demanded) {
        const auto bank = d.cfg.bank->stats();
        if (charge > d.cfg.max_read_bytes || charge > bank.largest_free ||
            d.ledger->bytes.load() > d.cfg.resident_bytes - std::min<uint64_t>(charge, d.cfg.resident_bytes) ||
            charge > d.cfg.resident_bytes) {
            requested.resize(demanded); batch.clear();
            for (const auto& hash : requested) batch.add(hash);
            bytes = batch.allocation_bytes(); charge = rounded(bytes);
        }
    }
    if (!requested.empty() && (charge == SIZE_MAX || bytes > d.cfg.max_read_bytes || !d.reserve(charge))) {
        ++d.counters.budget_rejections;
        for (const auto& h : requested) for (size_t slot : pending[h]) results[slot].status = PageStatus::BudgetExceeded;
        return results;
    }
    std::vector<std::pair<BlobHash, PageHandle>> staged;
    if (!requested.empty()) {
        auto allocation = std::make_shared<Impl::Allocation>();
        allocation->bank = d.cfg.bank;
        while (!allocation->bank->acquire(bytes, allocation->lease)) {
            if (d.reserve(charge, true)) continue;
            ++d.counters.budget_rejections;
            for (const auto& h : requested) for (size_t slot : pending[h]) results[slot].status = PageStatus::BudgetExceeded;
            return results;
        }
        allocation->bytes = charge; allocation->ledger = d.ledger; d.ledger->bytes.fetch_add(charge);
        batch.submit(allocation->lease.data, allocation->lease.capacity);
        d.counters.disk_reads += batch.stats().chunk_reads;
        d.counters.disk_bytes += batch.stats().bytes_read;
        d.counters.checksums += batch.stats().checksum_count;
        for (size_t i = 0; i < requested.size(); ++i) {
            const auto& read = batch.result(i);
            PageResult result;
            if (read.status == Status::Ok) {
                auto page = std::make_shared<CachedPage>();
                std::string error;
                if (decode_page(read.data, read.size, d.cfg.limits, page->view, error)) {
                    page->hash = requested[i]; page->bytes = read.data; page->size = read.size;
                    page->allocation = allocation;
                    result = {PageStatus::Ok, page}; staged.emplace_back(requested[i], page);
                    if (i >= demanded) ++d.counters.prefetched_pages;
                } else result.status = PageStatus::Corrupt;
            } else if (read.status == Status::Corrupt) result.status = PageStatus::Corrupt;
            else if (read.status != Status::Missing) result.status = PageStatus::IoError;
            for (size_t slot : pending[requested[i]]) results[slot] = result;
        }
    }
    if (cancelled()) {
        for (auto& r : results) r = {PageStatus::Cancelled, {}};
        return results;
    }
    for (auto& item : staged) d.entries[item.first] = {std::move(item.second), ++d.tick};
    return results;
}
std::vector<PageResult> PageCache::read_partitioned(const std::vector<BlobHash>& hashes, const std::atomic<bool>* cancel) {
    auto results=read(hashes,cancel);
    if(hashes.size()<=1 || hashes.size()>d_->cfg.max_requests ||
       std::none_of(results.begin(),results.end(),[](const PageResult& result){return result.status==PageStatus::BudgetExceeded;}))return results;
    // Drop temporary pins before retrying. The cache still owns valid pages;
    // accepted sub-batches then pin their results while later sub-batches run.
    results.clear();
    const auto middle=hashes.begin()+hashes.size()/2;
    auto left=read_partitioned({hashes.begin(),middle},cancel);
    auto right=read_partitioned({middle,hashes.end()},cancel);
    left.insert(left.end(),std::make_move_iterator(right.begin()),std::make_move_iterator(right.end()));
    if(cancel && cancel->load())for(auto& result:left)result={PageStatus::Cancelled,{}};
    return left;
}
bool PageCache::refresh() { return d_->store->reload_index(); }
PageResult PageCache::read_manifest(const std::string& key) {
    std::string error;
    // Atomic reference publication changes this stamp. Capture it BEFORE
    // opening the table: a racing commit then causes another reload next
    // time, rather than labeling an older snapshot with a newer stamp.
    uint64_t stamp = 0;
    const bool stamped = os::stamp_of(d_->store->dir()+"/refs.bin", &stamp);
    if (!d_->references || !stamped || stamp != d_->reference_stamp) {
        auto refs = RefTable::open(*d_->store, {}, &error);
        if (!refs) return {PageStatus::IoError, {}};
        d_->references = std::move(refs);
        d_->reference_stamp = stamp;
        ++d_->counters.reference_reloads;
    }
    RefInfo ref;
    if (!d_->references->peek(key, &ref)) return {};
    if (!refresh()) return {PageStatus::IoError, {}};
    auto result = read({ref.hash})[0];
    if (result.page) {
        for (const auto& dependency : result.page->view.dependencies)
            if (!d_->store->contains(dependency)) return {PageStatus::Missing, {}};
    }
    return result;
}
void PageCache::clear() { d_->entries.clear(); }
PageCacheStats PageCache::stats() const {
    auto result = d_->counters; result.resident_payload_bytes = d_->ledger->bytes.load();
    result.bank = d_->cfg.bank->stats(); return result;
}
bool publish_page_manifest(BlobStore& store, RefTable& refs, const std::string& key,
                           const std::vector<uint8_t>& manifest, const PageLimits& limits,
                           BlobHash& out, std::string& error) {
    PageView view;
    if (!decode_page(manifest.data(), manifest.size(), limits, view, error)) return false;
    for (const auto& h : view.dependencies)
        if (!store.contains(h)) return fail(error, "manifest dependency is not present");
    BlobHash hash;
    if (store.put(manifest.data(), manifest.size(), &hash) != Status::Ok || !store.flush_index()) {
        error = store.last_error(); return false;
    }
    RefInfo previous;
    const bool had_previous = refs.peek(key, &previous);
    if (!refs.put(key, hash, view.kind, manifest.size()) || !refs.flush()) {
        if (had_previous) refs.put(key, previous.hash, previous.kind, previous.size);
        else refs.erase(key);
        return fail(error, "manifest reference publication failed");
    }
    out = hash; error.clear(); return true;
}
bool compact_page_store(BlobStore& store, const RefTable& refs, const std::vector<BlobHash>& retained_roots,
                        const PageLimits& limits, size_t max_pages, CompactStats& out, std::string& error) {
    if (store.read_only() || !max_pages) return fail(error, "invalid page maintenance request");
    std::vector<BlobHash> pending = retained_roots, keep;
    for (const auto& key : refs.keys()) { RefInfo ref; if (refs.peek(key, &ref)) pending.push_back(ref.hash); }
    std::set<BlobHash> visited;
    std::unique_ptr<MemArena, void(*)(MemArena*)> arena(mem_arena_create(limits.max_bytes), mem_arena_destroy);
    if (!arena) return fail(error, "maintenance arena allocation failed");
    while (!pending.empty()) {
        const auto hash = pending.back(); pending.pop_back();
        if (!visited.insert(hash).second) continue;
        if (visited.size() > max_pages) return fail(error, "maintenance traversal limit exceeded");
        const size_t length = store.size_of(hash);
        if (!length || length > limits.max_bytes) return fail(error, "missing or oversized maintenance dependency");
        mem_arena_reset(arena.get()); const uint8_t* data = nullptr; size_t size = 0;
        if (store.read(hash, arena.get(), &data, &size) != Status::Ok) return fail(error, "maintenance page read failed");
        PageView view;
        if (!decode_page(data, size, limits, view, error)) return false;
        keep.push_back(hash);
        for (const auto& dependency : view.dependencies) if (!visited.count(dependency)) pending.push_back(dependency);
        if (pending.size() > max_pages) return fail(error, "maintenance dependency queue limit exceeded");
    }
    CompactStats stats;
    if (!store.compact(keep.data(), keep.size(), &stats, true)) { error = store.last_error(); return false; }
    out = stats; error.clear(); return true;
}

} // namespace asset_store
