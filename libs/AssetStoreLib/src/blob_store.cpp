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
 *
 * Path: libs/AssetStoreLib/src/blob_store.cpp. Implements everything in
 * ../include/asset_store.h except the RefTable (ref_table.cpp) and the hash
 * and checksum functions (store_hash.cpp).
 *
 * File map, in order: the private Impl (all of BlobStore's state, pimpl'd out
 * of the public header), index load and index write, the recovery truncate and
 * the stale-pack sweep, the public BlobStore surface, compaction, and finally
 * ReadBatch.
 *
 * What lives in the store directory:
 *   p<gen>_<id>.pack   append-only blob records; <gen> bumps on every compact()
 *   index.bin          the sole authority; rewritten whole and renamed in
 *   index.<pid>.tmp    the staging file for that rename, pid-qualified
 *   store.lock         the cross-process writer lock, held open for a session
 *   readers.lock       shared reader / exclusive maintenance lease
 *
 * Threading: none. Nothing here locks, and nothing here is atomic. One
 * BlobStore per thread is the contract; the only concurrency this file handles
 * is between store instances, and it handles it with file leases plus the fact
 * that a rename is all-or-nothing.
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

/* One row of the in-memory index and, minus `pending`, one 40-byte row of
 * index.bin. `offset` points at the PAYLOAD, not at the record header that
 * precedes it: reads seek straight there and never parse a header. `crc` is
 * the payload's CRC-32 and is checked on every read. */
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
    /* The two sizes are the crash-safety invariant in miniature: everything
     * between committed_size and write_size is on disk but addressed by
     * nothing, and the next writer open truncates it away. `f` stays null
     * until the first read or write actually touches this pack. */
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

/* All of BlobStore's state, pimpl'd so that neither os::File nor
 * <unordered_map> nor any OS type appears in the public header.
 *
 * No synchronisation anywhere in here -- a BlobStore is single-threaded by
 * contract. Cross-instance safety uses the writer lock, shared reader leases,
 * exclusive maintenance leases, and atomic index publication.
 *
 * `index` holds committed entries and this writer's uncommitted ones together;
 * `pending` distinguishes them and `dirty` says whether any exist. */
struct BlobStore::Impl {
    StoreConfig cfg;
    std::string dir;
    bool read_only = true;

    os::Lock* lock = nullptr;
    os::Lock* reader_lease = nullptr;

    uint32_t generation = 0;
    uint64_t snapshot_revision = 0;
    std::vector<PackFile> packs;
    std::unordered_map<BlobHash, IndexEntry, HashKeyHash> index;

    uint64_t index_stamp = 0;   /* to detect a writer's commit from a reader */
    bool dirty = false;         /* pending appends awaiting flush_index()    */
    std::string last_err;

    ~Impl() {
        for (PackFile& p : packs) if (p.f) os::close(p.f);
        if (reader_lease) os::unlock(reader_lease);
        if (lock) os::unlock(lock);
    }

    std::string index_path() const { return join(dir, "index.bin"); }
    /* The staging index is pid-qualified so that a leftover from a writer that
     * died can never be confused with, or collide with, this one's. (refs.tmp
     * in ref_table.cpp is not pid-qualified; the writer lock is what keeps
     * that safe.) */
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

    /* Lazily opens pack `id` of the CURRENT generation and caches the handle
     * for the rest of the session. Returns null for an out-of-range id or an
     * open failure, which every caller turns into IoError. A read-only store
     * opens the pack read-only, so a reader cannot write through a mistake
     * here. */
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

/* Reads index.bin whole, validates it -- trailing CRC first, then magic,
 * version, declared size, and every entry naming a pack that is actually
 * listed -- and only then swaps it in. A failure at any point returns false
 * with the previously loaded index left completely untouched, which is what
 * lets reload_index() fail safely on a live reader.
 *
 * A missing index.bin is not a failure: that is a brand-new store, and the
 * result is generation 0 with no packs and no entries. */
bool BlobStore::Impl::load_index(std::string* err) {
    std::string path = index_path();
    if (!os::file_exists(path)) {
        /* A fresh store: generation 0, one empty pack. */
        generation = 0;
        packs.clear();
        index.clear();
        return true;
    }

    // Capture BEFORE opening/reading. A concurrent rename may make us read a
    // newer index (which only causes a redundant refresh), but must never mark
    // an older buffer with the stamp of a commit it has not actually read.
    uint64_t read_stamp = 0;
    os::stamp_of(path, &read_stamp);
    os::File* f = os::open_read(path);
    if (!f) { if (err) *err = "cannot open index: " + os::last_error(); return false; }
    uint64_t sz = os::file_size(f);
    if (sz < kIndexHeaderBytes + 4 || sz > cfg.max_index_bytes || sz > SIZE_MAX) {
        os::close(f);
        if (err) *err = "index truncated or exceeds byte limit";
        return false;
    }
    std::vector<uint8_t> buf((size_t)sz);
    bool ok = os::read_at(f, 0, buf.data(), buf.size());
    os::close(f);
    if (!ok) { if (err) *err = "index read failed: " + os::last_error(); return false; }
    if (cfg.debug_after_index_read) cfg.debug_after_index_read(cfg.debug_after_index_read_context);

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
    new_index.reserve(size_t(nentries) * 2 + 8);
    uint32_t previous_pack = 0;
    uint64_t previous_end = 0;
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
        const uint64_t committed = new_packs[ie.pack].committed_size;
        if (!ie.hash.valid() || !ie.length || ie.offset < kRecordHeaderBytes || ie.offset % 8 ||
            ie.offset > committed || align_up8(ie.length) > committed - ie.offset ||
            (i && (ie.pack < previous_pack ||
                   (ie.pack == previous_pack && ie.offset - kRecordHeaderBytes < previous_end))) ||
            !new_index.emplace(ie.hash, ie).second) {
            if (err) *err = "invalid, overlapping or duplicate index record";
            return false;
        }
        previous_pack = ie.pack;
        previous_end = ie.offset + align_up8(ie.length);
    }

    for (PackFile& p : packs) if (p.f) os::close(p.f);
    packs.swap(new_packs);
    index.swap(new_index);
    ++snapshot_revision;
    generation = gen;
    index_stamp = read_stamp;
    return true;
}

/* ------------------------------------------------------------ index write */

/* Serialises a complete index to `path` and fsyncs it. This writes a STAGING
 * file only -- the caller performs the rename that makes it authoritative, and
 * that rename is the commit. `entries` is taken by value because it is sorted
 * in place. */
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

    const uint64_t bytes = kIndexHeaderBytes + uint64_t(pack_sizes.size()) * 8 +
                           uint64_t(entries.size()) * kIndexEntryBytes + 4;
    if (bytes > cfg.max_index_bytes || bytes > SIZE_MAX || pack_sizes.size() > UINT32_MAX || entries.size() > UINT32_MAX) {
        if (err) *err = "index exceeds configured byte/count limit";
        return false;
    }
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

/* Open order matters, and it is: create the directory, take the writer lock,
 * load the index, truncate the packs back to what it vouches for, sweep packs
 * from other generations. A writer therefore finishes all of its crash
 * recovery before any caller can read a single byte.
 *
 * A read-only open holds a shared maintenance lease before loading its index.
 * It never truncates or sweeps packs and fails if maintenance is in progress
 * or the directory does not already exist. */
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

    if (cfg.read_only) {
        d.reader_lease = os::lock_shared(join(cfg.dir, "readers.lock"));
        if (!d.reader_lease) {
            if (err) *err = "store maintenance is active: " + os::last_error();
            return nullptr;
        }
    }
    if (!d.load_index(err)) return nullptr;

    if (!cfg.read_only) {
        if (!d.truncate_to_committed()) {
            if (err) *err = "recovery truncate failed: " + os::last_error();
            return nullptr;
        }
        // A reader may still lazily open any pack in its snapshot. Skip
        // sweeping unless every read-only handle has retired.
        if (auto* lease = os::lock_exclusive(join(cfg.dir, "readers.lock"), false)) {
            d.sweep_stale_packs();
            os::unlock(lease);
        }
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

/* Append one blob. The bytes are hashed before anything else, so re-putting
 * identical content costs a hash and nothing more. Everything written here is
 * unaddressable by any other process until flush_index() renames a new index
 * over the old one; this handle can read it back immediately only because its
 * own in-memory index already names it. */
Status BlobStore::put(const void* data, size_t len, BlobHash* out_hash) {
    Impl& d = *d_;
    if (d.read_only) return Status::ReadOnly;
    /* A rejected length is a caller mistake, not an IO failure, but Status has
     * no argument-error member and adding one would change the public enum.
     * Say so in last_error() instead, so an IoError from put() can at least be
     * told apart from a disk that actually failed. */
    if (len == 0) {
        d.last_err = "put: zero-length blob";
        return Status::IoError;
    }
    if (len > 0xFFFFFFFFull) {
        d.last_err = "put: blob exceeds the 4 GiB record-header length limit";
        return Status::IoError;
    }

    BlobHash h = hash_bytes(data, len);
    if (out_hash) *out_hash = h;

    auto it = d.index.find(h);
    if (it != d.index.end()) return Status::Ok;   /* dedup: already stored */

    /* Choose a pack: the last one, unless the record would overflow it. */
    /* The `write_size != 0` guard below means a blob larger than
     * max_pack_bytes still lands, alone, in a fresh empty pack instead of
     * rolling over forever looking for room: max_pack_bytes is a rollover
     * threshold, not a hard cap on file size. */
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

Status BlobStore::repair_blob(const BlobHash& expected, const void* data, size_t len) {
    auto& d = *d_;
    if (d.read_only) return Status::ReadOnly;
    if (!data || !len || len > UINT32_MAX || !expected.valid() || hash_bytes(data, len) != expected) {
        d.last_err = "repair_blob: replacement does not match requested content";
        return Status::IoError;
    }
    const auto found = d.index.find(expected);
    const bool existed = found != d.index.end();
    IndexEntry previous;
    if (existed) { previous = found->second; d.index.erase(found); }
    const auto result = put(data, len, nullptr);
    if (result != Status::Ok && existed) d.index[expected] = previous;
    return result;
}

Status BlobStore::put_batch(const std::vector<BlobInput>& inputs, size_t max_bytes,
                            std::vector<BlobHash>& hashes, WriteBatchStats* stats) {
    auto& d = *d_;
    if (stats) *stats = {};
    if (d.read_only) return Status::ReadOnly;
    struct Pending { BlobInput input; BlobHash hash; };
    std::vector<Pending> pending;
    std::vector<BlobHash> result;
    std::unordered_map<BlobHash, bool, HashKeyHash> seen;
    size_t total = 0;
    // Reject the entire oversized/invalid request before touching a pack.
    for (const auto& input : inputs) {
        if (!input.data || !input.size || input.size > UINT32_MAX) {
            d.last_err = "put_batch: invalid payload"; return Status::IoError;
        }
        const auto hash = hash_bytes(input.data, input.size);
        result.push_back(hash);
        if (d.index.count(hash) || !seen.emplace(hash, true).second) continue;
        const uint64_t bytes = align_up8(kRecordHeaderBytes + uint64_t(input.size));
        if (bytes > max_bytes - total) {
            d.last_err = "put_batch: staging limit exceeded"; return Status::IoError;
        }
        total += static_cast<size_t>(bytes);
        pending.push_back({input, hash});
    }
    WriteBatchStats st;
    for (size_t i = 0; i < pending.size();) {
        if (d.packs.empty()) d.packs.resize(1);
        uint32_t pid = static_cast<uint32_t>(d.packs.size() - 1);
        const uint64_t first_bytes = align_up8(kRecordHeaderBytes + uint64_t(pending[i].input.size));
        if (d.packs[pid].write_size &&
            (d.packs[pid].write_size >= d.cfg.max_pack_bytes ||
             first_bytes > d.cfg.max_pack_bytes - d.packs[pid].write_size)) {
            d.packs.push_back(PackFile()); ++pid;
        }
        const uint64_t base = d.packs[pid].write_size;
        std::vector<uint8_t> bytes;
        bytes.reserve(total); // keep staging growth within the admitted byte bound
        std::vector<IndexEntry> entries;
        do {
            const auto& p = pending[i];
            const size_t record_bytes = static_cast<size_t>(align_up8(kRecordHeaderBytes + uint64_t(p.input.size)));
            if (!bytes.empty() && (base + bytes.size() >= d.cfg.max_pack_bytes ||
                record_bytes > d.cfg.max_pack_bytes - base - bytes.size())) break;
            const size_t offset = bytes.size(); bytes.resize(offset + record_bytes, 0);
            auto* hdr = bytes.data() + offset;
            const uint32_t crc = crc32(p.input.data, p.input.size);
            put_u32(hdr, kBlobMagic); put_u32(hdr + 4, static_cast<uint32_t>(p.input.size));
            put_u64(hdr + 8, p.hash.lo); put_u64(hdr + 16, p.hash.hi);
            put_u32(hdr + 24, crc); put_u32(hdr + 28, crc32(hdr, 28));
            memcpy(hdr + kRecordHeaderBytes, p.input.data, p.input.size);
            IndexEntry e; e.hash = p.hash; e.offset = base + offset + kRecordHeaderBytes;
            e.pack = pid; e.length = static_cast<uint32_t>(p.input.size); e.crc = crc; e.pending = true;
            entries.push_back(e); ++i;
        } while (i < pending.size());
        auto* file = d.pack_handle(pid);
        if (!file || !os::write_at(file, base, bytes.data(), bytes.size())) {
            d.last_err = "put_batch: pack append failed: " + os::last_error();
            if (stats) *stats = st;
            return Status::IoError;
        }
        d.packs[pid].write_size += bytes.size();
        for (const auto& e : entries) d.index[e.hash] = e;
        d.dirty = true;
        st.bytes_written += bytes.size(); ++st.write_calls;
        st.blobs_written += static_cast<uint32_t>(entries.size());
    }
    if (stats) *stats = st;
    hashes = std::move(result);
    return Status::Ok;
}

/* The commit point. fsync every open pack first -- committing an index that
 * named bytes still sitting in the page cache would be the one way to break
 * the invariant -- then write index.<pid>.tmp, fsync that, and rename it over
 * index.bin.
 *
 * Returns true immediately when nothing is pending. A false return leaves the
 * store exactly as it was: the previous index is still the authority, and the
 * pending appends stay pending. */
bool BlobStore::flush_index() {
    Impl& d = *d_;
    if (d.read_only) return false;
    if (!d.dirty) return true;
    if (d.cfg.debug_fail_pack_flush) { d.last_err = "injected pack flush failure"; return false; }

    for (PackFile& p : d.packs) {
        if (p.f && !os::sync(p.f)) {
            d.last_err = "pack flush failed: " + os::last_error();
            return false;
        }
    }

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

/* Cheap when nothing changed: one stat of index.bin, and the whole load is
 * skipped when the stamp matches. The stamp -- see os::stamp_of -- mixes the
 * finest available mtime with the size, and on POSIX also with the inode,
 * which the commit rename always changes; that inode term is what makes two
 * commits inside one second visible here even when they produce an
 * identically sized index. Returns true both for "reloaded" and for "nothing
 * to do"; false means the file was there but unreadable or failed its CRC, in
 * which case the previous index is retained. */
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
uint64_t BlobStore::snapshot_revision() const { return d_->snapshot_revision; }

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

/* Rewrite the store into generation+1, keeping only `keep` and laying it out
 * in exactly that order.
 *
 * Sequence: flush pending puts, stream each survivor through a CRC check into
 * fresh p<gen+1>_*.pack files, write and rename the new index (that rename is
 * the commit), adopt the new generation in memory, then sweep the old packs.
 *
 * Duplicates in `keep` are ignored, hashes absent from the index are skipped,
 * and a survivor whose payload fails its CRC is dropped instead of copied --
 * compaction is therefore also the point at which bit rot leaves the store.
 *
 * Any failure before the rename leaves the old generation completely intact;
 * the half-written new packs are unreachable and get swept at a later open.
 * After the rename the old packs are deleted best-effort. Every BlobLocation
 * and pack id handed out before this call is stale afterwards, and a reader
 * cannot exist during compaction: the maintenance lease requires all read-only
 * handles to retire first. require_all refuses missing/corrupt survivors. */
bool BlobStore::compact(const BlobHash* keep, size_t keep_count, CompactStats* out, bool require_all) {
    Impl& d = *d_;
    std::unique_ptr<os::Lock, void(*)(os::Lock*)> maintenance(
        d.read_only ? nullptr : os::lock_exclusive(join(d.dir, "readers.lock"), false), os::unlock);
    if (!maintenance) { d.last_err = "compaction requires quiescent readers"; return false; }
    if (keep_count && !keep) { d.last_err = "missing survivor array"; return false; }
    if (d.generation == UINT32_MAX) { d.last_err = "pack generation exhausted"; return false; }
    if (require_all) for (size_t i = 0; i < keep_count; ++i)
        if (!d.index.count(keep[i])) { d.last_err = "required compaction survivor is missing"; return false; }
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
        if (!os::truncate(dst, 0)) { os::close(dst); dst = nullptr; return false; }
        new_paths.push_back(p);
        dst_size = 0;
        return true;
    };

    if (!open_dst(0)) return false;

    std::vector<uint8_t> buf;
    for (const IndexEntry& src : survivors) {
        uint64_t record_bytes = align_up8(kRecordHeaderBytes + (uint64_t)src.length);
        if (dst_size != 0 && dst_size + record_bytes > d.cfg.max_pack_bytes) {
            if (!os::sync(dst)) { os::close(dst); return false; }
            os::close(dst); dst = nullptr;
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
        if (crc32(buf.data(), buf.size()) != src.crc) {
            if (require_all) { os::close(dst); d.last_err = "required compaction survivor is corrupt"; return false; }
            continue;
        }

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
            if (!os::write_at(dst, dst_size + kRecordHeaderBytes + src.length, zeros,
                              (size_t)(padded - src.length))) { os::close(dst); return false; }
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

    if (!os::sync(dst)) { os::close(dst); return false; }
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
    struct Item { size_t slot; IndexEntry e; };
    struct Chunk { size_t first, last; uint64_t begin, end; uint32_t pack; };
    struct Plan { std::vector<Item> items; std::vector<Chunk> chunks; };

    Plan plan() const {
        Plan p;
        const auto& d = *store->d_;
        p.items.reserve(requests.size());
        for (size_t i = 0; i < requests.size(); ++i) {
            auto it = d.index.find(requests[i]);
            if (it != d.index.end()) p.items.push_back({i, it->second});
        }
        std::sort(p.items.begin(), p.items.end(), [](const Item& a, const Item& b) {
            if (a.e.pack != b.e.pack) return a.e.pack < b.e.pack;
            if (a.e.offset != b.e.offset) return a.e.offset < b.e.offset;
            if (a.e.hash != b.e.hash) return a.e.hash < b.e.hash;
            return a.slot < b.slot;
        });
        for (size_t i = 0; i < p.items.size();) {
            const auto& first = p.items[i].e;
            Chunk c{i, i + 1, first.offset, first.offset + first.length, first.pack};
            uint64_t holes = 0;
            while (c.last < p.items.size()) {
                const auto& e = p.items[c.last].e;
                if (e.pack != c.pack) break;
                // Duplicate requests never grow the allocation or the holes.
                if (e.hash == p.items[c.last - 1].e.hash) { ++c.last; continue; }
                const uint64_t hole = e.offset > c.end ? e.offset - c.end : 0;
                const uint64_t end = std::max(c.end, e.offset + e.length);
                if (hole > d.cfg.batch_gap_bytes || end - c.begin > d.cfg.batch_max_bytes ||
                    hole > d.cfg.batch_max_overread_bytes - holes) break;
                holes += hole;
                c.end = end;
                ++c.last;
            }
            p.chunks.push_back(c);
            i = c.last;
        }
        return p;
    }
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

/* Synchronous. Resolves every request against the store's CURRENT in-memory
 * index -- submit() never reloads it for you -- sorts the hits into physical
 * (pack, offset) order, coalesces near neighbours, and reads each coalesced
 * chunk exactly once.
 *
 * Returns true even when nothing could be read: per-blob outcomes live in each
 * ReadResult::status. A null `arena`, or a pack that will not open, surfaces as
 * IoError on every affected result, not as a false return. Results are rebuilt
 * from scratch on each call, so re-submitting the same batch reads everything
 * again and allocates from the arena again. */
size_t ReadBatch::allocation_bytes() const {
    size_t total = 0;
    for (const auto& c : d_->plan().chunks) {
        const uint64_t span = c.end - c.begin;
        if (span > SIZE_MAX - 7) return SIZE_MAX;
        const size_t padded = (static_cast<size_t>(span) + 7) & ~size_t(7);
        if (padded > SIZE_MAX - total) return SIZE_MAX;
        total += padded;
    }
    return total;
}

bool ReadBatch::submit(MemArena* arena) { return submit_impl(arena, nullptr, 0); }
bool ReadBatch::submit(void* buffer, size_t capacity) {
    const size_t needed = allocation_bytes();
    if (needed == SIZE_MAX || needed > capacity || (needed && !buffer) ||
        reinterpret_cast<uintptr_t>(buffer) % 8) {
        d_->results.assign(d_->requests.size(), ReadResult());
        d_->stats = BatchStats();
        for (size_t i=0; i<d_->requests.size(); ++i) {
            d_->results[i].hash = d_->requests[i]; d_->results[i].status = Status::IoError;
        }
        return false;
    }
    return submit_impl(nullptr, buffer, capacity);
}
bool ReadBatch::submit_impl(MemArena* arena, void* buffer, size_t capacity) {
    size_t used = 0;
    Impl& b = *d_;
    BlobStore::Impl& d = *b.store->d_;
    b.results.assign(b.requests.size(), ReadResult());
    b.stats = BatchStats();
    b.stats.requests = static_cast<uint32_t>(b.requests.size());
    for (size_t i = 0; i < b.requests.size(); ++i) b.results[i].hash = b.requests[i];
    const auto plan = b.plan();
    b.stats.misses = static_cast<uint32_t>(b.requests.size() - plan.items.size());
    for (const auto& c : plan.chunks) {
        const uint64_t extent = c.end - c.begin;
        os::File* f = d.pack_handle(c.pack);
        const size_t span = extent <= SIZE_MAX ? static_cast<size_t>(extent) : 0;
        uint8_t* chunk = arena && span ? static_cast<uint8_t*>(mem_arena_alloc(arena, span)) : nullptr;
        if (!arena && buffer && span && span <= SIZE_MAX-7) {
            const size_t padded = (span+7) & ~size_t(7);
            if (padded <= capacity-used) {
                chunk = static_cast<uint8_t*>(buffer)+used; used += padded;
            }
        }
        bool ok = false;
        if (f && chunk) {
            ++b.stats.chunk_reads;
            b.stats.bytes_read += span;
            ok = os::read_at(f, c.begin, chunk, span);
        }
        for (size_t k = c.first; k < c.last; ++k) {
            const auto& item = plan.items[k];
            ReadResult& r = b.results[item.slot];
            if (k > c.first && item.e.hash == plan.items[k - 1].e.hash) {
                r = b.results[plan.items[k - 1].slot];
                ++b.stats.duplicate_requests;
            } else if (!ok) {
                r.status = Status::IoError;
            } else {
                const uint8_t* payload = chunk + static_cast<size_t>(item.e.offset - c.begin);
                ++b.stats.checksum_count;
                if (crc32(payload, item.e.length) != item.e.crc) {
                    r.status = Status::Corrupt;
                } else {
                    r.data = payload;
                    r.size = item.e.length;
                    r.status = Status::Ok;
                    b.stats.unique_bytes_delivered += r.size;
                }
            }
            if (r.status == Status::Corrupt) ++b.stats.corrupt;
            if (r.status == Status::Ok) b.stats.bytes_delivered += r.size;
        }
    }
    return true;
}

}  // namespace asset_store
