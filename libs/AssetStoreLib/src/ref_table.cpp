/* ref_table.cpp -- semantic keys over a BlobStore, LRU against a disk budget.
 *
 * The key is an opaque byte string. This layer never parses it, never assigns
 * it meaning and never invents one. Whoever sits above (the engine's cache)
 * folds a part hash, an artifact kind, a rep index and the version vector into
 * that string -- in exactly one place, which is the point of the design.
 *
 * Two refs may name the same blob; the budget counts the blob once. That is
 * why eviction frees bytes only when the LAST ref to a blob goes away, and why
 * evict_to_budget() and compact() are separate calls: dropping refs is cheap
 * bookkeeping, reclaiming disk is a pack rewrite.
 */

#include "../include/asset_store.h"

#include "store_format.h"
#include "store_hash.h"
#include "store_os.h"

#include <algorithm>
#include <map>
#include <stdio.h>
#include <string.h>
#include <unordered_map>

namespace asset_store {

namespace {

std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char last = dir[dir.size() - 1];
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

struct HashCount {
    uint64_t size = 0;
    uint32_t refs = 0;
};

struct HashKeyHash {
    size_t operator()(const BlobHash& h) const {
        return (size_t)(h.lo ^ (h.hi * 0x9E3779B97F4A7C15ull));
    }
};

}  // namespace

struct RefTable::Impl {
    BlobStore* store = nullptr;
    RefTableConfig cfg;
    std::string dir;

    /* Sorted: the file is written in key order, so the same set of refs always
     * produces the same refs.bin bytes. */
    std::map<std::string, RefInfo> refs;
    std::unordered_map<BlobHash, HashCount, HashKeyHash> by_hash;

    uint64_t tick = 1;
    bool dirty = false;

    std::string refs_path() const { return join(dir, "refs.bin"); }
    std::string refs_tmp_path() const { return join(dir, "refs.tmp"); }

    void reindex_hashes() {
        by_hash.clear();
        for (auto& kv : refs) {
            HashCount& hc = by_hash[kv.second.hash];
            hc.size = kv.second.size;
            hc.refs += 1;
        }
    }

    uint64_t live_bytes() const {
        uint64_t total = 0;
        for (auto& kv : by_hash) total += kv.second.size;
        return total;
    }

    bool load(std::string* err);
};

bool RefTable::Impl::load(std::string* err) {
    std::string path = refs_path();
    if (!os::file_exists(path)) return true;

    os::File* f = os::open_read(path);
    if (!f) { if (err) *err = "cannot open refs: " + os::last_error(); return false; }
    uint64_t sz = os::file_size(f);
    if (sz < kRefsHeaderBytes + 4) { os::close(f); if (err) *err = "refs truncated"; return false; }
    std::vector<uint8_t> buf((size_t)sz);
    bool ok = os::read_at(f, 0, buf.data(), buf.size());
    os::close(f);
    if (!ok) { if (err) *err = "refs read failed"; return false; }

    if (get_u32(buf.data() + buf.size() - 4) != crc32(buf.data(), buf.size() - 4)) {
        if (err) *err = "refs checksum mismatch";
        return false;
    }
    if (get_u32(buf.data()) != kRefsMagic) { if (err) *err = "refs magic mismatch"; return false; }
    if (get_u32(buf.data() + 4) != kFormatVersion) { if (err) *err = "refs version mismatch"; return false; }

    uint32_t count = get_u32(buf.data() + 8);
    tick = get_u64(buf.data() + 16);

    std::map<std::string, RefInfo> loaded;
    size_t p = kRefsHeaderBytes;
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 4 > buf.size() - 4) { if (err) *err = "refs truncated entry"; return false; }
        uint32_t klen = get_u32(buf.data() + p); p += 4;
        size_t kpad = (klen + 3u) & ~3u;
        if (p + kpad + 40 > buf.size() - 4) { if (err) *err = "refs truncated entry"; return false; }
        std::string key((const char*)buf.data() + p, klen);
        p += kpad;
        RefInfo ri;
        ri.hash.lo = get_u64(buf.data() + p); p += 8;
        ri.hash.hi = get_u64(buf.data() + p); p += 8;
        ri.size = get_u64(buf.data() + p); p += 8;
        ri.last_access = get_u64(buf.data() + p); p += 8;
        ri.kind = get_u32(buf.data() + p); p += 4;
        p += 4;  /* reserved */
        loaded[key] = ri;
    }

    refs.swap(loaded);
    reindex_hashes();
    return true;
}

/* ========================================================= RefTable public */

RefTable::RefTable() : d_(new Impl()) {}
RefTable::~RefTable() = default;

std::unique_ptr<RefTable> RefTable::open(BlobStore& store, const RefTableConfig& cfg,
                                         std::string* err) {
    std::unique_ptr<RefTable> t(new RefTable());
    t->d_->store = &store;
    t->d_->cfg = cfg;
    t->d_->dir = store.dir();
    if (!t->d_->load(err)) return nullptr;
    return t;
}

bool RefTable::put(const std::string& key, const BlobHash& h, uint32_t kind, uint64_t size) {
    Impl& d = *d_;
    if (!h.valid()) return false;
    /* A reader must never mutate the table. Without this a reader thread could
     * quietly overwrite the writer's refs.bin. */
    if (d.store->read_only()) return false;
    auto it = d.refs.find(key);
    if (it != d.refs.end()) {
        /* Re-pointing an existing key: the old blob loses a reference. */
        auto oh = d.by_hash.find(it->second.hash);
        if (oh != d.by_hash.end() && --oh->second.refs == 0) d.by_hash.erase(oh);
    }
    RefInfo ri;
    ri.hash = h;
    ri.kind = kind;
    ri.size = size;
    ri.last_access = d.tick++;
    d.refs[key] = ri;
    HashCount& hc = d.by_hash[h];
    hc.size = size;
    hc.refs += 1;
    d.dirty = true;
    return true;
}

bool RefTable::lookup(const std::string& key, RefInfo* out) {
    Impl& d = *d_;
    auto it = d.refs.find(key);
    if (it == d.refs.end()) return false;
    it->second.last_access = d.tick++;
    d.dirty = true;
    if (out) *out = it->second;
    return true;
}

bool RefTable::peek(const std::string& key, RefInfo* out) const {
    auto it = d_->refs.find(key);
    if (it == d_->refs.end()) return false;
    if (out) *out = it->second;
    return true;
}

bool RefTable::erase(const std::string& key) {
    Impl& d = *d_;
    if (d.store->read_only()) return false;
    auto it = d.refs.find(key);
    if (it == d.refs.end()) return false;
    auto oh = d.by_hash.find(it->second.hash);
    if (oh != d.by_hash.end() && --oh->second.refs == 0) d.by_hash.erase(oh);
    d.refs.erase(it);
    d.dirty = true;
    return true;
}

size_t RefTable::count() const { return d_->refs.size(); }
uint64_t RefTable::live_bytes() const { return d_->live_bytes(); }
uint64_t RefTable::budget_bytes() const { return d_->cfg.budget_bytes; }
void RefTable::set_budget_bytes(uint64_t b) { d_->cfg.budget_bytes = b; }

EvictStats RefTable::evict_to_budget() {
    Impl& d = *d_;
    EvictStats st;
    st.bytes_live_after = d.live_bytes();
    if (d.store->read_only()) return st;
    if (d.cfg.budget_bytes == 0 || st.bytes_live_after <= d.cfg.budget_bytes) return st;

    /* Oldest touch first. */
    std::vector<const std::pair<const std::string, RefInfo>*> order;
    order.reserve(d.refs.size());
    for (auto& kv : d.refs) order.push_back(&kv);
    std::sort(order.begin(), order.end(),
              [](const std::pair<const std::string, RefInfo>* a,
                 const std::pair<const std::string, RefInfo>* b) {
                  if (a->second.last_access != b->second.last_access)
                      return a->second.last_access < b->second.last_access;
                  return a->first < b->first;   /* deterministic ties */
              });

    uint64_t live = st.bytes_live_after;
    for (const auto* kv : order) {
        if (live <= d.cfg.budget_bytes) break;
        BlobHash h = kv->second.hash;
        uint64_t sz = kv->second.size;
        std::string key = kv->first;
        auto oh = d.by_hash.find(h);
        bool last_ref = (oh != d.by_hash.end() && oh->second.refs == 1);
        d.refs.erase(key);
        if (oh != d.by_hash.end() && --oh->second.refs == 0) d.by_hash.erase(oh);
        ++st.refs_evicted;
        if (last_ref) { live -= sz; st.bytes_freed += sz; }
    }
    st.bytes_live_after = live;
    d.dirty = true;
    return st;
}

bool RefTable::compact(CompactStats* out) {
    Impl& d = *d_;
    if (d.store->read_only()) return false;
    /* Survivors in key order: deterministic, and it puts blobs whose keys share
     * a prefix -- one sector's artifacts, under the caller's naming -- next to
     * each other in the pack. */
    std::vector<BlobHash> keep;
    keep.reserve(d.refs.size());
    for (auto& kv : d.refs) keep.push_back(kv.second.hash);
    if (!d.store->compact(keep.empty() ? nullptr : keep.data(), keep.size(), out))
        return false;
    return flush();
}

bool RefTable::flush() {
    Impl& d = *d_;
    /* The last line of defence: whatever a reader did to its in-memory copy --
     * lookup() bumps last-access, harmlessly -- none of it reaches the disk. */
    if (d.store->read_only()) return false;

    std::vector<uint8_t> buf;
    push_u32(buf, kRefsMagic);
    push_u32(buf, kFormatVersion);
    push_u32(buf, (uint32_t)d.refs.size());
    push_u32(buf, 0);
    push_u64(buf, d.tick);
    for (auto& kv : d.refs) {
        const std::string& k = kv.first;
        push_u32(buf, (uint32_t)k.size());
        size_t kpad = (k.size() + 3u) & ~3u;
        size_t n = buf.size();
        buf.resize(n + kpad, 0);
        memcpy(buf.data() + n, k.data(), k.size());
        push_u64(buf, kv.second.hash.lo);
        push_u64(buf, kv.second.hash.hi);
        push_u64(buf, kv.second.size);
        push_u64(buf, kv.second.last_access);
        push_u32(buf, kv.second.kind);
        push_u32(buf, 0);
    }
    push_u32(buf, crc32(buf.data(), buf.size()));

    std::string tmp = d.refs_tmp_path();
    os::File* f = os::open_rw(tmp, true);
    if (!f) return false;
    bool ok = os::truncate(f, 0) && os::write_at(f, 0, buf.data(), buf.size()) && os::sync(f);
    os::close(f);
    if (!ok) { os::remove_file(tmp); return false; }
    if (!os::rename_over(tmp, d.refs_path())) { os::remove_file(tmp); return false; }
    d.dirty = false;
    return true;
}

std::vector<std::string> RefTable::keys() const {
    std::vector<std::string> out;
    out.reserve(d_->refs.size());
    for (auto& kv : d_->refs) out.push_back(kv.first);
    return out;
}

}  // namespace asset_store
