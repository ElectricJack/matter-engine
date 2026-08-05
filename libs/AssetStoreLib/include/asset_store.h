/* asset_store.h -- MatterStore: content-addressed blob storage in append-only packs.
 *
 * Design: docs/lod-vt-redesign-2026-08-04.md section 9.
 *
 * This library stores BYTES. It knows nothing about parts, worlds, LODs or any
 * other engine concept, and it depends on nothing but MemoryLib. Two layers:
 *
 *   BlobStore -- content hash -> bytes. Blobs are appended to pack files. An
 *                in-memory index (hash -> pack, offset, length, checksum) is
 *                loaded from an index file at open and committed by writing a
 *                temp file and renaming it over the old one.
 *
 *                CRASH SAFETY IS BY CONSTRUCTION: the index is the only
 *                authority on what exists. Bytes appended to a pack but not
 *                named by a committed index are invisible to every reader, and
 *                the next writer to open the store truncates them away. There
 *                is no recovery scan, no journal replay and no repair path,
 *                because there is nothing to repair.
 *
 *   RefTable  -- semantic key (an opaque byte string; the caller gives it
 *                meaning) -> blob hash + metadata (kind, size, last-access).
 *                Eviction is LRU against a disk budget. Compaction rewrites the
 *                surviving blobs into fresh packs and drops the orphans.
 *
 * Threading and process model
 *   Single writer, many readers, enforced by a cross-process lock on
 *   <dir>/store.lock. A BlobStore or RefTable object is NOT thread-safe: open
 *   one read-only BlobStore per reader thread. Readers see the writer's work
 *   when they call reload_index(), which is atomic with respect to the writer's
 *   rename -- a reader either sees the whole previous index or the whole new
 *   one, never a mixture.
 *
 * Corruption
 *   Every blob carries a CRC32 of its payload and a CRC32 of its own record
 *   header; the index file carries a CRC32 of itself. A torn or bit-rotted blob
 *   reads back as Status::Corrupt with a null pointer. It is never a crash and
 *   never garbage handed to the caller. Callers are expected to treat Corrupt
 *   exactly as they treat Missing: as a cache miss, and re-bake.
 */
#ifndef ASSET_STORE_H
#define ASSET_STORE_H

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

/* MemoryLib -- the only dependency. Reads land in a caller-supplied arena. */
#include "mem_arena.h"

namespace asset_store {

/* ---------------------------------------------------------------- hashing -- */

/* 128-bit content hash (MurmurHash3 x64 128). Content-addressed: two identical
 * byte strings always produce the same hash, on every machine, forever. */
struct BlobHash {
    uint64_t lo = 0;
    uint64_t hi = 0;

    bool valid() const { return lo != 0 || hi != 0; }
    bool operator==(const BlobHash& o) const { return lo == o.lo && hi == o.hi; }
    bool operator!=(const BlobHash& o) const { return !(*this == o); }
    bool operator<(const BlobHash& o) const {
        return hi != o.hi ? hi < o.hi : lo < o.lo;
    }
};

BlobHash hash_bytes(const void* data, size_t len);

/* 32 hex chars + NUL. */
void hash_to_hex(const BlobHash& h, char out[33]);
std::string hash_to_string(const BlobHash& h);

/* --------------------------------------------------------------- statuses -- */

enum class Status {
    Ok = 0,
    Missing,   /* no such hash in the committed index */
    Corrupt,   /* found, but the bytes on disk failed their checksum */
    IoError,   /* the read itself failed */
    ReadOnly,  /* a write was attempted on a read-only handle */
    Locked,    /* another process holds the writer lock */
};

const char* status_name(Status s);

/* ------------------------------------------------------------- BlobStore --- */

struct StoreConfig {
    std::string dir;

    /* A pack rolls over once it would exceed this. The design calls for
     * 64-256 MB; the default is 64 MB. Tests use small values. */
    uint64_t max_pack_bytes = 64ull * 1024 * 1024;

    /* Read-only handles take no writer lock and never modify the store. Open
     * one per reader thread. */
    bool read_only = false;

    /* When non-zero, block on the writer lock instead of failing with
     * Status::Locked. */
    bool block_for_lock = false;

    /* Coalescing window for ReadBatch: two records whose extents are separated
     * by no more than this many bytes are fetched in a single read. */
    uint32_t batch_gap_bytes = 64 * 1024;

    /* TEST HOOK. When non-zero, the next put() writes exactly this many bytes
     * of payload and then calls _exit(3) -- a real, hard process death partway
     * through a real append, with the pack file left torn on disk. Used by the
     * crash-mid-write test. Zero (the default) in every non-test build. */
    uint64_t debug_abort_mid_payload = 0;
};

/* Where a blob physically lives. Exposed so a benchmark or a locality-aware
 * writer can reason about placement; not needed for ordinary use. */
struct BlobLocation {
    uint32_t pack = 0;
    uint64_t offset = 0;   /* offset of the payload, not of the record header */
    uint32_t length = 0;
    uint32_t crc = 0;
};

struct CompactStats {
    uint64_t blobs_kept = 0;
    uint64_t blobs_dropped = 0;
    uint64_t bytes_kept = 0;
    uint64_t bytes_reclaimed = 0;
};

class ReadBatch;

class BlobStore {
public:
    /* Opens (creating if needed) the store in cfg.dir. Returns null and fills
     * *err on failure. A writer handle takes the cross-process lock and
     * truncates every pack back to the extent the committed index names --
     * this is the entire crash-recovery path. */
    static std::unique_ptr<BlobStore> open(const StoreConfig& cfg, std::string* err);
    ~BlobStore();

    BlobStore(const BlobStore&) = delete;
    BlobStore& operator=(const BlobStore&) = delete;

    /* ---- write side (writer handles only) ---- */

    /* Appends the bytes and returns their content hash. Deduplicating: storing
     * bytes already present is a no-op that returns the existing hash. The blob
     * is readable through THIS handle immediately, but is invisible to every
     * other process, and is lost on a crash, until flush_index() commits it. */
    Status put(const void* data, size_t len, BlobHash* out_hash);

    /* Commits every pending put: writes a fresh index to <dir>/index.tmp,
     * flushes the packs and the temp file, then renames it over
     * <dir>/index.bin. That rename is the commit point. */
    bool flush_index();

    /* Rewrites the surviving blobs into a fresh pack generation, in the order
     * given -- so the caller controls locality -- and drops everything else.
     * Commits by index rename, then deletes the old packs (best effort; any
     * pack a reader still holds open is swept on the next writer open). */
    bool compact(const BlobHash* keep, size_t keep_count, CompactStats* out);

    /* ---- read side ---- */

    bool contains(const BlobHash& h) const;
    bool locate(const BlobHash& h, BlobLocation* out) const;
    size_t size_of(const BlobHash& h) const;   /* 0 if absent */

    /* Reads one blob into `arena`. On Missing/Corrupt/IoError *out_data is left
     * null and nothing is allocated. */
    Status read(const BlobHash& h, MemArena* arena,
                const uint8_t** out_data, size_t* out_len);

    /* Re-reads the index file from disk if it changed. This is how a reader
     * picks up the writer's commits. Returns false only on IO/CRC failure, in
     * which case the previously loaded index is retained. */
    bool reload_index();

    /* ---- accounting ---- */

    size_t blob_count() const;
    uint64_t live_bytes() const;   /* sum of indexed payload lengths */
    uint64_t pack_bytes() const;   /* bytes actually occupied on disk */
    uint32_t generation() const;
    std::string pack_path(uint32_t pack_id) const;
    const std::string& dir() const;
    bool read_only() const;

    /* Why the last call that returned false or a non-Ok status failed. Empty
     * if nothing has failed. */
    const std::string& last_error() const;

    /* All hashes in the committed index, in physical (pack, offset) order. */
    std::vector<BlobHash> all_hashes() const;

private:
    BlobStore();
    struct Impl;
    std::unique_ptr<Impl> d_;
    friend class ReadBatch;
};

/* -------------------------------------------------------------- ReadBatch -- */

struct ReadResult {
    BlobHash hash;
    const uint8_t* data = nullptr;   /* arena-owned; valid until arena reset */
    size_t size = 0;
    Status status = Status::Missing;
};

struct BatchStats {
    uint32_t requests = 0;
    uint32_t chunk_reads = 0;   /* actual read() calls issued */
    uint64_t bytes_read = 0;    /* including coalesced padding and headers */
    uint64_t bytes_delivered = 0;
    uint32_t misses = 0;
    uint32_t corrupt = 0;
};

/* A batch of reads, submitted together.
 *
 * The batch is the unit of work on purpose: submit() sorts the requests into
 * physical (pack, offset) order and merges records that are near each other
 * into single large reads. That is where the win over per-file access lives --
 * one seek and one big sequential read instead of N opens and N seeks.
 *
 * NOTE ON ASYNC. The design (section 9.2) calls for overlapped/IOCP completion.
 * submit() here is SYNCHRONOUS: it returns when every payload has landed. The
 * batching, ordering and coalescing -- the parts that carry the performance --
 * are all present, and the API shape is the one an async implementation needs
 * (a batch object, a submit, results retrieved afterwards). Nothing here
 * pretends to be async: there is no future, no callback and no completion
 * queue that would have to be redesigned later. Adding submit_async()/poll()
 * alongside submit() is an additive change to this same class. */
class ReadBatch {
public:
    explicit ReadBatch(BlobStore& store);
    ~ReadBatch();

    ReadBatch(const ReadBatch&) = delete;
    ReadBatch& operator=(const ReadBatch&) = delete;

    void reserve(size_t n);
    void add(const BlobHash& h);
    size_t size() const;
    void clear();

    /* Executes every queued read. Payloads are allocated from `arena` -- one
     * allocation per delivered blob, no heap churn -- and checksummed. Returns
     * false only if the batch could not be executed at all; per-blob failures
     * are reported in each result's status. */
    bool submit(MemArena* arena);

    /* Results are in add() order, not in the physical order they were read. */
    const ReadResult& result(size_t i) const;
    const BatchStats& stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

/* --------------------------------------------------------------- RefTable -- */

struct RefInfo {
    BlobHash hash;
    uint32_t kind = 0;
    uint64_t size = 0;
    uint64_t last_access = 0;   /* monotonic tick, not wall clock */
};

struct RefTableConfig {
    /* LRU eviction target, in bytes of distinct referenced blob payload.
     * Zero means unlimited. */
    uint64_t budget_bytes = 0;
};

struct EvictStats {
    uint64_t refs_evicted = 0;
    uint64_t bytes_freed = 0;
    uint64_t bytes_live_after = 0;
};

/* Semantic keys over a BlobStore. The key is an opaque byte string: this
 * library never parses it. The engine's cache layer is what folds a part hash,
 * an artifact kind, a rep index and the version vector into one. */
class RefTable {
public:
    static std::unique_ptr<RefTable> open(BlobStore& store,
                                          const RefTableConfig& cfg,
                                          std::string* err);
    ~RefTable();

    RefTable(const RefTable&) = delete;
    RefTable& operator=(const RefTable&) = delete;

    bool put(const std::string& key, const BlobHash& h, uint32_t kind, uint64_t size);

    /* Bumps last-access. This is the LRU touch. */
    bool lookup(const std::string& key, RefInfo* out);
    /* Does not bump last-access -- for reporting and for tests. */
    bool peek(const std::string& key, RefInfo* out) const;

    bool erase(const std::string& key);

    size_t count() const;
    /* Distinct referenced payload bytes -- two keys sharing a blob count once. */
    uint64_t live_bytes() const;
    uint64_t budget_bytes() const;
    void set_budget_bytes(uint64_t b);

    /* Drops least-recently-used refs until live_bytes() <= budget. Blobs whose
     * last ref went away become orphans; compact() is what reclaims their
     * disk space. */
    EvictStats evict_to_budget();

    /* Compacts the underlying BlobStore down to exactly the referenced blobs,
     * ordered by key so the result is deterministic. */
    bool compact(CompactStats* out);

    /* Atomic tmp+rename of <dir>/refs.bin. */
    bool flush();

    /* Every key, sorted. */
    std::vector<std::string> keys() const;

private:
    RefTable();
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace asset_store

#endif /* ASSET_STORE_H */
