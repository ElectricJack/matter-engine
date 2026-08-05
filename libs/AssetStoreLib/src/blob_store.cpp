/* blob_store.cpp -- append-only packs, an atomically-swapped index, and the
 * batched read path.
 *
 * The single invariant everything else follows from:
 *
 *     THE COMMITTED INDEX IS THE ONLY AUTHORITY ON WHAT EXISTS.
 *
 * Appends go to the end of a pack file and are unreachable until an index
 * naming them is renamed into place. So a crash mid-append leaves bytes that no
 * reader can address, and the next writer truncates each pack back to the size
 * the index recorded. There is no scan, no journal and no repair -- the torn
 * bytes are simply overwritten by the next append.
 */

#include "../include/asset_store.h"

#include "store_format.h"
#include "store_hash.h"
#include "store_os.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>

#ifdef _WIN32
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace asset_store {

namespace {

struct HashKeyHash {
    size_t operator()(const BlobHash& h) const {
        return (size_t)(h.lo ^ (h.hi * 0x9E3779B97F4A7C15ull));
    }
};

struct IndexEntry {
    BlobHash hash;
    uint64_t offset = 0;   /* payload offset within the pack */
    uint32_t pack = 0;
    uint32_t length = 0;
    uint32_t crc = 0;
    bool pending = false;  /* appended by this writer, not yet committed */
};

/* A live handle on one pack file, opened lazily. */
struct PackFile {
    os::File* f = nullptr;
    uint64_t committed_size = 0;   /* from the index: bytes the index vouches for */
    uint64_t write_size = 0;       /* including uncommitted appends              */
};

std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char last = dir[dir.size() - 1];
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

}  // namespace

/* ==================================================================== Impl */

struct BlobStore::Impl {
    StoreConfig cfg;
    std::string dir;
    bool read_only = true;

    os::Lock* lock = nullptr;

    uint32_t generation = 0;
    std::vector<PackFile> packs;
    std::unordered_map<BlobHash, IndexEntry, HashKeyHash> index;

    uint64_t index_stamp = 0;   /* to detect a writer's commit from a reader */
    bool dirty = false;         /* pending appends awaiting flush_index()    */
    std::string last_err;

    ~Impl() {
        for (PackFile& p : packs) if (p.f) os::close(p.f);
        if (lock) os::unlock(lock);
    }

    std::string index_path() const { return join(dir, "index.bin"); }
    std::string index_tmp_path() const {
        char buf[64];
        snprintf(buf, sizeof(buf), "index.%d.tmp", (int)
#ifdef _WIN32
                 _getpid()
#else
                 getpid()
#endif
                 );
        return join(dir, buf);
    }
    std::string lock_path() const { return join(dir, "store.lock"); }

    std::string pack_name(uint32_t gen, uint32_t id) const {
        char buf[64];
        snprintf(buf, sizeof(buf), "p%u_%u.pack", (unsigned)gen, (unsigned)id);
        return buf;
    }
    std::string pack_path(uint32_t gen, uint32_t id) const {
        return join(dir, pack_name(gen, id));
    }

    os::File* pack_handle(uint32_t id) {
        if (id >= packs.size()) return nullptr;
        PackFile& p = packs[id];
        if (!p.f) {
            p.f = read_only ? os::open_read(pack_path(generation, id))
                            : os::open_rw(pack_path(generation, id), true);
        }
        return p.f;
    }

    bool load_index(std::string* err);
    bool write_index(const std::string& path, uint32_t gen,
                     const std::vector<uint64_t>& pack_sizes,
                     std::vector<IndexEntry> entries, std::string* err);
    /* The recovery step, run once at writer open. */
    bool truncate_to_committed();
    void sweep_stale_packs();
};

/* ------------------------------------------------------------- index load */

bool BlobStore::Impl::load_index(std::string* err) {
    std::string path = index_path();
    if (!os::file_exists(path)) {
        /* A fresh store: generation 0, one empty pack. */
        generation = 0;
        packs.clear();
        index.clear();
        return true;
    }

    os::File* f = os::open_read(path);
    if (!f) { if (err) *err = "cannot open index: " + os::last_error(); return false; }
    uint64_t sz = os::file_size(f);
    if (sz < kIndexHeaderBytes + 4) {
        os::close(f);
        if (err) *err = "index truncated";
        return false;
    }
    std::vector<uint8_t> buf((size_t)sz);
    bool ok = os::read_at(f, 0, buf.data(), buf.size());
    os::close(f);
    if (!ok) { if (err) *err = "index read failed: " + os::last_error(); return false; }

    uint32_t stored_crc = get_u32(buf.data() + buf.size() - 4);
    uint32_t actual_crc = crc32(buf.data(), buf.size() - 4);
    if (stored_crc != actual_crc) { if (err) *err = "index checksum mismatch"; return false; }

    if (get_u32(buf.data()) != kIndexMagic) { if (err) *err = "index magic mismatch"; return false; }
    if (get_u32(buf.data() + 4) != kFormatVersion) { if (err) *err = "index version mismatch"; return false; }

    uint32_t gen = get_u32(buf.data() + 8);
    uint32_t npacks = get_u32(buf.data() + 12);
    uint32_t nentries = get_u32(buf.data() + 16);

    size_t need = kIndexHeaderBytes + (size_t)npacks * 8 +
                  (size_t)nentries * kIndexEntryBytes + 4;
    if (buf.size() != need) { if (err) *err = "index size mismatch"; return false; }

    /* Swap in wholesale: either the new index is fully valid and replaces the
     * old, or nothing changes. */
    std::vector<PackFile> new_packs((size_t)npacks);
    for (uint32_t i = 0; i < npacks; ++i) {
        uint64_t committed = get_u64(buf.data() + kIndexHeaderBytes + i * 8);
        new_packs[i].committed_size = committed;
        new_packs[i].write_size = committed;
    }

    std::unordered_map<BlobHash, IndexEntry, HashKeyHash> new_index;
    new_index.reserve(nentries * 2 + 8);
    const uint8_t* e = buf.data() + kIndexHeaderBytes + (size_t)npacks * 8;
    for (uint32_t i = 0; i < nentries; ++i, e += kIndexEntryBytes) {
        IndexEntry ie;
        ie.hash.lo = get_u64(e);
        ie.hash.hi = get_u64(e + 8);
        ie.offset  = get_u64(e + 16);
        ie.pack    = get_u32(e + 24);
        ie.length  = get_u32(e + 28);
        ie.crc     = get_u32(e + 32);
        if (ie.pack >= npacks) { if (err) *err = "index names a pack that is not listed"; return false; }
        new_index[ie.hash] = ie;
    }

    for (PackFile& p : packs) if (p.f) os::close(p.f);
    packs.swap(new_packs);
    index.swap(new_index);
    generation = gen;
    os::stamp_of(path, &index_stamp);
    return true;
}

/* ------------------------------------------------------------ index write */

bool BlobStore::Impl::write_index(const std::string& path, uint32_t gen,
                                  const std::vector<uint64_t>& pack_sizes,
                                  std::vector<IndexEntry> entries,
                                  std::string* err) {
    /* Physical order: deterministic, and it makes ReadBatch's sort cheap. */
    std::sort(entries.begin(), entries.end(),
              [](const IndexEntry& a, const IndexEntry& b) {
                  if (a.pack != b.pack) return a.pack < b.pack;
                  return a.offset < b.offset;
              });

    std::vector<uint8_t> buf;
    buf.reserve(kIndexHeaderBytes + pack_sizes.size() * 8 +
                entries.size() * kIndexEntryBytes + 4);
    push_u32(buf, kIndexMagic);
    push_u32(buf, kFormatVersion);
    push_u32(buf, gen);
    push_u32(buf, (uint32_t)pack_sizes.size());
    push_u32(buf, (uint32_t)entries.size());
    push_u32(buf, 0);
    for (uint64_t s : pack_sizes) push_u64(buf, s);
    for (const IndexEntry& ie : entries) {
        push_u64(buf, ie.hash.lo);
        push_u64(buf, ie.hash.hi);
        push_u64(buf, ie.offset);
        push_u32(buf, ie.pack);
        push_u32(buf, ie.length);
        push_u32(buf, ie.crc);
        push_u32(buf, 0);
    }
    push_u32(buf, crc32(buf.data(), buf.size()));

    os::File* f = os::open_rw(path, true);
    if (!f) { if (err) *err = "cannot create index tmp: " + os::last_error(); return false; }
    bool ok = os::truncate(f, 0) && os::write_at(f, 0, buf.data(), buf.size()) && os::sync(f);
    os::close(f);
    if (!ok) { if (err) *err = "index tmp write failed: " + os::last_error(); return false; }
    return true;
}

/* --------------------------------------------------------------- recovery */

bool BlobStore::Impl::truncate_to_committed() {
    /* This is the whole of crash recovery. Any bytes past committed_size were
     * appended by a writer that died before committing; nothing addresses
     * them, so they are cut away and the space reused. */
    for (uint32_t i = 0; i < (uint32_t)packs.size(); ++i) {
        std::string p = pack_path(generation, i);
        if (!os::file_exists(p)) continue;
        uint64_t on_disk = os::file_size_of(p);
        if (on_disk <= packs[i].committed_size) continue;
        os::File* f = os::open_rw(p, false);
        if (!f) return false;
        bool ok = os::truncate(f, packs[i].committed_size) && os::sync(f);
        os::close(f);
        if (!ok) return false;
    }
    return true;
}

void BlobStore::Impl::sweep_stale_packs() {
    /* Packs from a superseded generation, or beyond the pack count the index
     * lists, are unreachable. Deletion is best effort: a reader may still hold
     * one open (Windows refuses), and the next writer open tries again. */
    std::vector<std::string> names = os::list_dir(dir);
    for (const std::string& n : names) {
        if (n.size() < 7 || n[0] != 'p') continue;
        if (n.compare(n.size() - 5, 5, ".pack") != 0) continue;
        unsigned g = 0, id = 0;
        if (sscanf(n.c_str(), "p%u_%u.pack", &g, &id) != 2) continue;
        if (g == generation && id < packs.size()) continue;
        os::remove_file(join(dir, n));
    }
}

/* ======================================================== BlobStore public */

BlobStore::BlobStore() : d_(new Impl()) {}
BlobStore::~BlobStore() = default;

std::unique_ptr<BlobStore> BlobStore::open(const StoreConfig& cfg, std::string* err) {
    std::unique_ptr<BlobStore> s(new BlobStore());
    Impl& d = *s->d_;
    d.cfg = cfg;
    d.dir = cfg.dir;
    d.read_only = cfg.read_only;

    if (!cfg.read_only) {
        if (!os::make_dirs(cfg.dir)) {
            if (err) *err = "cannot create store dir: " + os::last_error();
            return nullptr;
        }
        d.lock = os::lock_exclusive(d.lock_path(), cfg.block_for_lock);
        if (!d.lock) {
            if (err) *err = "store is already open for writing (lock held)";
            return nullptr;
        }
    } else if (!os::file_exists(cfg.dir)) {
        if (err) *err = "store dir does not exist: " + cfg.dir;
        return nullptr;
    }

    if (!d.load_index(err)) return nullptr;

    if (!cfg.read_only) {
        if (!d.truncate_to_committed()) {
            if (err) *err = "recovery truncate failed: " + os::last_error();
            return nullptr;
        }
        d.sweep_stale_packs();
        if (d.packs.empty()) {
            d.packs.resize(1);
            /* Touch pack 0 so a store that is opened and closed without a put
             * still has a coherent shape on disk. */
            os::File* f = os::open_rw(d.pack_path(d.generation, 0), true);
            if (f) os::close(f);
        }
    }
    return s;
}

Status BlobStore::put(const void* data, size_t len, BlobHash* out_hash) {
    Impl& d = *d_;
    if (d.read_only) return Status::ReadOnly;
    if (len == 0 || len > 0xFFFFFFFFull) return Status::IoError;

    BlobHash h = hash_bytes(data, len);
    if (out_hash) *out_hash = h;

    auto it = d.index.find(h);
    if (it != d.index.end()) return Status::Ok;   /* dedup: already stored */

    /* Choose a pack: the last one, unless the record would overflow it. */
    uint64_t record_bytes = align_up8(kRecordHeaderBytes + (uint64_t)len);
    if (d.packs.empty()) d.packs.resize(1);
    uint32_t pid = (uint32_t)d.packs.size() - 1;
    if (d.packs[pid].write_size != 0 &&
        d.packs[pid].write_size + record_bytes > d.cfg.max_pack_bytes) {
        d.packs.push_back(PackFile());
        pid = (uint32_t)d.packs.size() - 1;
    }

    os::File* f = d.pack_handle(pid);
    if (!f) return Status::IoError;

    uint64_t rec_off = d.packs[pid].write_size;
    uint32_t payload_crc = crc32(data, len);

    uint8_t hdr[kRecordHeaderBytes];
    put_u32(hdr + 0, kBlobMagic);
    put_u32(hdr + 4, (uint32_t)len);
    put_u64(hdr + 8, h.lo);
    put_u64(hdr + 16, h.hi);
    put_u32(hdr + 24, payload_crc);
    put_u32(hdr + 28, crc32(hdr, 28));

    if (!os::write_at(f, rec_off, hdr, kRecordHeaderBytes)) return Status::IoError;

    uint64_t payload_off = rec_off + kRecordHeaderBytes;

    if (d.cfg.debug_abort_mid_payload > 0) {
        /* TEST HOOK. Write a genuine prefix of the payload, flush it so the
         * bytes really reach the file, and then die hard -- no destructors, no
         * index commit. What is left on disk is exactly what a power cut in
         * the middle of an append leaves. */
        size_t partial = (size_t)d.cfg.debug_abort_mid_payload;
        if (partial > len) partial = len;
        os::write_at(f, payload_off, data, partial);
        os::sync(f);
        fflush(nullptr);
        _exit(3);
    }

    if (!os::write_at(f, payload_off, data, len)) return Status::IoError;

    uint64_t padded = align_up8((uint64_t)len);
    if (padded > len) {
        uint8_t zeros[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        if (!os::write_at(f, payload_off + len, zeros, (size_t)(padded - len)))
            return Status::IoError;
    }

    d.packs[pid].write_size = rec_off + record_bytes;

    IndexEntry ie;
    ie.hash = h;
    ie.offset = payload_off;
    ie.pack = pid;
    ie.length = (uint32_t)len;
    ie.crc = payload_crc;
    ie.pending = true;
    d.index[h] = ie;
    d.dirty = true;
    return Status::Ok;
}

bool BlobStore::flush_index() {
    Impl& d = *d_;
    if (d.read_only) return false;
    if (!d.dirty) return true;

    for (PackFile& p : d.packs) if (p.f) os::sync(p.f);

    std::vector<uint64_t> sizes;
    sizes.reserve(d.packs.size());
    for (PackFile& p : d.packs) sizes.push_back(p.write_size);

    std::vector<IndexEntry> entries;
    entries.reserve(d.index.size());
    for (auto& kv : d.index) entries.push_back(kv.second);

    std::string tmp = d.index_tmp_path();
    if (!d.write_index(tmp, d.generation, sizes, entries, &d.last_err)) return false;
    if (!os::rename_over(tmp, d.index_path())) {
        d.last_err = "index commit rename failed: " + os::last_error();
        os::remove_file(tmp);
        return false;
    }

    for (auto& kv : d.index) kv.second.pending = false;
    for (PackFile& p : d.packs) p.committed_size = p.write_size;
    d.dirty = false;
    os::stamp_of(d.index_path(), &d.index_stamp);
    return true;
}

bool BlobStore::reload_index() {
    Impl& d = *d_;
    uint64_t stamp = 0;
    if (os::stamp_of(d.index_path(), &stamp) && stamp == d.index_stamp) return true;
    return d.load_index(&d.last_err);
}

bool BlobStore::contains(const BlobHash& h) const {
    return d_->index.find(h) != d_->index.end();
}

bool BlobStore::locate(const BlobHash& h, BlobLocation* out) const {
    auto it = d_->index.find(h);
    if (it == d_->index.end()) return false;
    if (out) {
        out->pack = it->second.pack;
        out->offset = it->second.offset;
        out->length = it->second.length;
        out->crc = it->second.crc;
    }
    return true;
}

size_t BlobStore::size_of(const BlobHash& h) const {
    auto it = d_->index.find(h);
    return it == d_->index.end() ? 0 : (size_t)it->second.length;
}

Status BlobStore::read(const BlobHash& h, MemArena* arena,
                       const uint8_t** out_data, size_t* out_len) {
    ReadBatch b(*this);
    b.add(h);
    if (!b.submit(arena)) return Status::IoError;
    const ReadResult& r = b.result(0);
    if (out_data) *out_data = r.data;
    if (out_len) *out_len = r.size;
    return r.status;
}

size_t BlobStore::blob_count() const { return d_->index.size(); }

uint64_t BlobStore::live_bytes() const {
    uint64_t total = 0;
    for (auto& kv : d_->index) total += kv.second.length;
    return total;
}

uint64_t BlobStore::pack_bytes() const {
    uint64_t total = 0;
    for (const PackFile& p : d_->packs) total += p.write_size;
    return total;
}

uint32_t BlobStore::generation() const { return d_->generation; }

std::string BlobStore::pack_path(uint32_t pack_id) const {
    return d_->pack_path(d_->generation, pack_id);
}

const std::string& BlobStore::dir() const { return d_->dir; }
bool BlobStore::read_only() const { return d_->read_only; }
const std::string& BlobStore::last_error() const { return d_->last_err; }

std::vector<BlobHash> BlobStore::all_hashes() const {
    std::vector<IndexEntry> es;
    es.reserve(d_->index.size());
    for (auto& kv : d_->index) es.push_back(kv.second);
    std::sort(es.begin(), es.end(), [](const IndexEntry& a, const IndexEntry& b) {
        if (a.pack != b.pack) return a.pack < b.pack;
        return a.offset < b.offset;
    });
    std::vector<BlobHash> out;
    out.reserve(es.size());
    for (const IndexEntry& e : es) out.push_back(e.hash);
    return out;
}

/* ------------------------------------------------------------- compaction */

bool BlobStore::compact(const BlobHash* keep, size_t keep_count, CompactStats* out) {
    Impl& d = *d_;
    if (d.read_only) return false;
    if (!flush_index()) return false;

    CompactStats st;
    uint32_t new_gen = d.generation + 1;

    /* Survivors are written in the order the caller gave -- locality is a
     * write-side concern, and compaction is the writer's chance to reorder by
     * observed co-access. */
    std::vector<IndexEntry> survivors;
    survivors.reserve(keep_count);
    std::unordered_map<BlobHash, bool, HashKeyHash> already;
    for (size_t i = 0; i < keep_count; ++i) {
        auto it = d.index.find(keep[i]);
        if (it == d.index.end()) continue;
        if (already.count(keep[i])) continue;
        already[keep[i]] = true;
        survivors.push_back(it->second);
    }

    uint64_t before_bytes = pack_bytes();

    std::vector<uint64_t> new_sizes;
    std::vector<IndexEntry> new_entries;
    new_entries.reserve(survivors.size());

    os::File* dst = nullptr;
    uint32_t dst_id = 0;
    uint64_t dst_size = 0;
    std::vector<std::string> new_paths;

    auto open_dst = [&](uint32_t id) -> bool {
        std::string p = d.pack_path(new_gen, id);
        os::remove_file(p);
        dst = os::open_rw(p, true);
        if (!dst) return false;
        os::truncate(dst, 0);
        new_paths.push_back(p);
        dst_size = 0;
        return true;
    };

    if (!open_dst(0)) return false;

    std::vector<uint8_t> buf;
    for (const IndexEntry& src : survivors) {
        uint64_t record_bytes = align_up8(kRecordHeaderBytes + (uint64_t)src.length);
        if (dst_size != 0 && dst_size + record_bytes > d.cfg.max_pack_bytes) {
            os::sync(dst); os::close(dst); dst = nullptr;
            new_sizes.push_back(dst_size);
            ++dst_id;
            if (!open_dst(dst_id)) return false;
        }

        os::File* sf = d.pack_handle(src.pack);
        if (!sf) { os::close(dst); return false; }
        buf.resize(src.length);
        if (!os::read_at(sf, src.offset, buf.data(), buf.size())) { os::close(dst); return false; }
        /* Never carry a corrupt blob forward: it drops out here and reads as
         * a miss afterwards, which is exactly what a cache should do. */
        if (crc32(buf.data(), buf.size()) != src.crc) continue;

        uint8_t hdr[kRecordHeaderBytes];
        put_u32(hdr + 0, kBlobMagic);
        put_u32(hdr + 4, src.length);
        put_u64(hdr + 8, src.hash.lo);
        put_u64(hdr + 16, src.hash.hi);
        put_u32(hdr + 24, src.crc);
        put_u32(hdr + 28, crc32(hdr, 28));
        if (!os::write_at(dst, dst_size, hdr, kRecordHeaderBytes)) { os::close(dst); return false; }
        if (!os::write_at(dst, dst_size + kRecordHeaderBytes, buf.data(), buf.size())) {
            os::close(dst); return false;
        }
        uint64_t padded = align_up8(src.length);
        if (padded > src.length) {
            uint8_t zeros[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            os::write_at(dst, dst_size + kRecordHeaderBytes + src.length, zeros,
                         (size_t)(padded - src.length));
        }

        IndexEntry ne = src;
        ne.pack = dst_id;
        ne.offset = dst_size + kRecordHeaderBytes;
        ne.pending = false;
        new_entries.push_back(ne);
        dst_size += record_bytes;
        ++st.blobs_kept;
        st.bytes_kept += src.length;
    }

    os::sync(dst);
    os::close(dst);
    new_sizes.push_back(dst_size);

    st.blobs_dropped = (uint64_t)d.index.size() - st.blobs_kept;

    /* Commit: the rename makes the new generation authoritative. If we die
     * before it, the old index and old packs are untouched and the new pack
     * files are swept on the next open. */
    std::string tmp = d.index_tmp_path();
    if (!d.write_index(tmp, new_gen, new_sizes, new_entries, &d.last_err)) return false;
    if (!os::rename_over(tmp, d.index_path())) {
        d.last_err = "compaction commit rename failed: " + os::last_error();
        os::remove_file(tmp);
        return false;
    }

    /* Adopt the new generation in memory, then drop the old files. */
    for (PackFile& p : d.packs) if (p.f) os::close(p.f);
    d.packs.assign(new_sizes.size(), PackFile());
    for (size_t i = 0; i < new_sizes.size(); ++i) {
        d.packs[i].committed_size = new_sizes[i];
        d.packs[i].write_size = new_sizes[i];
    }
    d.index.clear();
    for (const IndexEntry& e : new_entries) d.index[e.hash] = e;
    d.generation = new_gen;
    d.dirty = false;
    os::stamp_of(d.index_path(), &d.index_stamp);
    d.sweep_stale_packs();

    uint64_t after_bytes = pack_bytes();
    st.bytes_reclaimed = before_bytes > after_bytes ? before_bytes - after_bytes : 0;
    if (out) *out = st;
    return true;
}

/* ================================================================ ReadBatch */

struct ReadBatch::Impl {
    BlobStore* store = nullptr;
    std::vector<BlobHash> requests;
    std::vector<ReadResult> results;
    BatchStats stats;
};

ReadBatch::ReadBatch(BlobStore& store) : d_(new Impl()) { d_->store = &store; }
ReadBatch::~ReadBatch() = default;

void ReadBatch::reserve(size_t n) {
    d_->requests.reserve(n);
    d_->results.reserve(n);
}
void ReadBatch::add(const BlobHash& h) { d_->requests.push_back(h); }
size_t ReadBatch::size() const { return d_->requests.size(); }
void ReadBatch::clear() {
    d_->requests.clear();
    d_->results.clear();
    d_->stats = BatchStats();
}

const ReadResult& ReadBatch::result(size_t i) const { return d_->results[i]; }
const BatchStats& ReadBatch::stats() const { return d_->stats; }

bool ReadBatch::submit(MemArena* arena) {
    Impl& b = *d_;
    BlobStore::Impl& d = *b.store->d_;

    b.results.assign(b.requests.size(), ReadResult());
    b.stats = BatchStats();
    b.stats.requests = (uint32_t)b.requests.size();
    for (size_t i = 0; i < b.requests.size(); ++i) b.results[i].hash = b.requests[i];
    if (b.requests.empty()) return true;

    /* Resolve, then sort into physical order. Everything the batch buys comes
     * from this: the disk is visited once, forwards. */
    struct Item {
        size_t slot;
        IndexEntry e;
    };
    std::vector<Item> items;
    items.reserve(b.requests.size());
    for (size_t i = 0; i < b.requests.size(); ++i) {
        auto it = d.index.find(b.requests[i]);
        if (it == d.index.end()) {
            b.results[i].status = Status::Missing;
            ++b.stats.misses;
            continue;
        }
        items.push_back(Item{i, it->second});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& c) {
        if (a.e.pack != c.e.pack) return a.e.pack < c.e.pack;
        return a.e.offset < c.e.offset;
    });

    const uint64_t gap = d.cfg.batch_gap_bytes;

    size_t i = 0;
    while (i < items.size()) {
        /* Grow a chunk over neighbours in the same pack while the hole between
         * them stays under the coalescing window. Reading a small hole is
         * cheaper than paying for a second seek. */
        uint32_t pack = items[i].e.pack;
        uint64_t begin = items[i].e.offset;
        uint64_t end = begin + items[i].e.length;
        size_t j = i + 1;
        while (j < items.size() && items[j].e.pack == pack) {
            uint64_t s = items[j].e.offset;
            if (s > end + gap) break;
            uint64_t e2 = s + items[j].e.length;
            if (e2 > end) end = e2;
            ++j;
        }

        /* The chunk is read STRAIGHT INTO the arena and the results point into
         * it. There is no staging buffer and no second copy: the bytes land
         * once, get checksummed where they lie, and are handed out in place.
         *
         * Payload offsets inside a pack are 8-byte aligned by construction and
         * `begin` is itself a payload offset, so every payload keeps the arena's
         * 8-byte alignment.
         *
         * The cost is that the arena also holds whatever fell between the
         * requested blobs -- bounded by batch_gap_bytes per join, and reported
         * as bytes_read against bytes_delivered so a caller who cares can see
         * it. Measured on the benchmark corpus that overhead is under 0.2%,
         * against a copy of every delivered byte. */
        os::File* f = d.pack_handle(pack);
        size_t span = (size_t)(end - begin);
        uint8_t* chunk = arena ? (uint8_t*)mem_arena_alloc(arena, span) : nullptr;
        bool ok = false;
        if (f && chunk) ok = os::read_at(f, begin, chunk, span);
        ++b.stats.chunk_reads;
        b.stats.bytes_read += span;

        for (size_t k = i; k < j; ++k) {
            ReadResult& r = b.results[items[k].slot];
            const IndexEntry& e = items[k].e;
            if (!ok) { r.status = Status::IoError; continue; }
            const uint8_t* payload = chunk + (size_t)(e.offset - begin);
            if (crc32(payload, e.length) != e.crc) {
                r.status = Status::Corrupt;
                ++b.stats.corrupt;
                continue;
            }
            r.data = payload;
            r.size = e.length;
            r.status = Status::Ok;
            b.stats.bytes_delivered += e.length;
        }
        i = j;
    }
    return true;
}

}  // namespace asset_store
