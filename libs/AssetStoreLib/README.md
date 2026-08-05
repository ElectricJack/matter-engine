# AssetStoreLib (MatterStore)

Content-addressed blob storage in append-only pack files, with a semantic ref
table and LRU eviction against a disk budget.

Design: `docs/lod-vt-redesign-2026-08-04.md` §9. Milestone M5, first half.

**This library stores bytes.** It knows nothing about parts, worlds, LODs,
representations or textures, and it depends on nothing but `libs/MemoryLib`. No
engine headers, no raylib, no Vulkan. It builds and tests standalone.

## Why it exists

Today the engine's cache is a tree of thousands of small files: one open, one
seek and one close per artifact. The design's claim is that grouping artifacts
into pack files and reading them in coalesced batches beats that. The benchmark
in `tests/store_bench.cpp` measures the claim rather than assuming it — see
"Measured" below.

## Shape

    BlobStore    content hash -> bytes, in append-only packs
    RefTable     opaque key -> blob hash + (kind, size, last-access)
    ReadBatch    a batch of reads, sorted into physical order and coalesced

### The one invariant

> **The committed index is the only authority on what exists.**

Appends go to the end of a pack. They are unreachable until an index naming them
is renamed into place, so a crash mid-append leaves bytes that no reader can
address — and the next writer truncates each pack back to the extent the index
recorded. There is no recovery scan, no journal replay and no repair path,
because there is nothing to repair. `test_crash_mid_write` kills a real child
process partway through a real append and proves all of that end to end.

### Corruption is a miss, never a crash

Every blob carries a CRC32 of its payload and of its own record header; the index
and ref files carry a CRC32 of themselves. A damaged blob reads back as
`Status::Corrupt` with a null pointer, and its neighbours in the same pack are
unaffected. Callers treat `Corrupt` exactly as `Missing`: a cache miss, re-bake.

### Concurrency

Single writer, many readers, enforced by a cross-process lock on
`<dir>/store.lock`. A `BlobStore` object is not thread-safe — open one read-only
handle per reader thread. `reload_index()` is how a reader picks up the writer's
commits, and it is all-or-nothing against the writer's rename.

### Async

`ReadBatch::submit()` is **synchronous**. The batching, physical-order sort and
coalescing — the parts that carry the performance — are all present, and the API
shape is the one an async implementation needs. Nothing pretends to be async:
there is no future, no callback and no completion queue that would have to be
redesigned. Adding `submit_async()`/`poll()` (IOCP on Windows, per the design) is
an additive change to the same class.

## On-disk layout

    <dir>/store.lock        writer lock
    <dir>/p<gen>_<id>.pack  append-only blob records
    <dir>/index.bin         the authority: hash -> (pack, offset, length, crc)
    <dir>/refs.bin          key -> hash + metadata

Byte layouts are documented in `src/store_format.h`. Every integer is
little-endian and written byte at a time, and no structure contains a timestamp,
so the same sequence of writes produces byte-identical files — asserted by
`test_determinism`.

## Build

    make -C libs/AssetStoreLib          # build/libasset_store.a
    make -C libs/AssetStoreLib test     # the correctness suite
    make -C libs/AssetStoreLib bench    # the pack-vs-small-files measurement

The archive deliberately does **not** contain MemoryLib's objects: consumers that
already compile `mem_arena.c` would get duplicate symbols. Link the `.a` and keep
compiling MemoryLib from where it lives, per CLAUDE.md's "Code Sharing Between
Projects". `tests/Makefile` shows the pattern.

## Measured

See `docs/asset-store-benchmark-2026-08-05.md` for the numbers this machine
produced and what they do and do not justify.

## Not yet adopted

M5's second half — moving the engine's PartBundle blobs, resolve/settle refs and
decoded-BC `.gtex` mirror into the store — has **not** been done. Nothing in
`MatterEngine3` or `MatterEditor` references this library yet; it is purely
additive.
