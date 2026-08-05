# AssetStoreLib benchmark: packs vs the small-file storm

2026-08-05. Harness: `libs/AssetStoreLib/tests/store_bench.cpp`
(`make -C libs/AssetStoreLib bench`). Milestone M5, first half.

## The claim being tested

Design §9.2:

> A warm sector revisit is then one or two large sequential reads, not today's
> per-file open/seek storm over thousands of small cache files.

Migration plan, M5 acceptance:

> Benchmark: warm-sector read via store ≥ 5× faster than the small-file storm on
> the same data (the number that justifies the subsystem — publish the measured
> figure in the doc).

## Headline

**The 5× was not met in any configuration.** The measured figures, on this
machine, are:

| Case | small files | store packs | ratio |
|---|---|---|---|
| Cold, whole corpus (400 blobs, 98.5 MiB) | 190–200 ms | 42–45 ms | **4.3–4.7×** |
| Cold, one sector revisit (40 contiguous blobs) | 9.4–10.5 ms | 3.15 ms | **3.0–3.3×** |
| Cold, scattered access (40 blobs spread wide) | 9.7–10.5 ms | 6.4–6.5 ms | **1.5–1.6×** |
| Warm, whole corpus (OS page cache hot) | 28.9–29.5 ms | 52.0–53.5 ms | **0.54–0.57×** |

The case the acceptance criterion names — a *sector revisit* — is **3.0–3.3×**,
not 5×. The best number the subsystem produces anywhere is 4.7×, and that is the
bulk-load case, not the revisit case.

Reading the table honestly: **the ratio falls as the request gets smaller and as
locality gets worse, and it inverts entirely when the page cache is hot.** Those
three sentences are the finding.

## Method

**Corpus.** 400 blobs, 98.5 MiB, drawn to reproduce the measured size quantiles
of `projects/world_demo/.cache` (426 files, 413 MiB; p25 29 KB, median 131 KB,
p75 251 KB, p90 761 KB). The one real outlier — a single 66 MB `.gtex` — is
excluded, because one huge file is already one large sequential read and would
flatter both paths equally.

**Layout A** is one file per artifact in a two-level directory tree, read with
`std::ifstream`, which is what `MatterEngine3` does today. It performs **no
integrity checking**.

**Layout B** is the same bytes in a pack, read through `ReadBatch` (or, in the
unbuffered cases, through the identical raw primitive with the same coalescing
applied by hand, so the only difference measured is the *shape* of the access).
It CRC32s every payload it delivers.

**Cold** means `FILE_FLAG_NO_BUFFERING` on both paths. The Windows page cache
cannot be dropped without administrator rights, so this is a substitute for a
genuinely cold cache, and it is labelled as one. It is applied identically to
both paths.

## Where the time goes

### Warm: the store loses, and it is the checksum

Of the store's 52 ms warm, **35 ms is CRC32** over 98.5 MiB. Strip it and the
store's warm read is ~17 ms against the small-file path's 29 ms — i.e. the pack
path *is* about 1.8× faster at moving warm bytes, and then spends that advantage
and more on integrity the small-file path simply does not provide.

This was worse before. The first measurement had CRC32 at 171 ms — a
byte-at-a-time table loop running at ~575 MB/s, **82% of the store's total warm
time**. Rewriting it slice-by-eight (same polynomial, identical checksums, no
format change) took it to ~2.8 GB/s. A second, unrelated 2× came from deleting a
copy: `ReadBatch` originally read each coalesced chunk into a staging buffer and
then memcpy'd each payload into the arena. It now reads the chunk *straight into*
the arena and hands out pointers in place.

If warm-path throughput ever becomes the binding constraint, the next move is
hardware CRC32C (`_mm_crc32_u64`, SSE4.2) with a software fallback. That changes
the polynomial and therefore the on-disk format, which is cheap now — nothing has
adopted the format yet — and expensive later.

### Cold: the store wins, and it is the file operations

| | file operations |
|---|---|
| small files, 400 blobs | 1200 (400 open + 400 read + 400 close) |
| store, 400 blobs contiguous | 3 |
| store, 40 blobs scattered | 42 |

The scattered case is the instructive one. Coalescing buys nothing there — the
pack issues 40 separate reads, exactly as many as the file path — and it still
wins 1.5×. That residual is purely the cost of `open`/`close` per artifact. So
the subsystem's floor benefit is the syscall shape, and everything above 1.5× is
locality that the writer has to earn by placing co-accessed blobs together.

## What this does and does not justify

**It justifies the subsystem, but not on the number the plan asked for.** The
throughput case is real but smaller than assumed: 3× on the target case, and
negative when the cache is hot. The strong arguments for AssetStoreLib are the
ones the benchmark was not measuring:

- **Crash safety by construction.** The committed index is the only authority on
  what exists, so there is no torn-artifact state to detect or repair. The
  current per-file cache has been bitten by silent corruption before; this
  removes the class rather than handling it.
- **Per-blob checksums**, which the small-file path does not have at any price —
  and which, measured above, cost ~0.35 ms per 10 MiB delivered.
- **Eviction against a disk budget**, which a file tree cannot do without walking
  it.
- **600× fewer file operations.** This machine is a fast local NVMe with no
  antivirus filter on the temp directory. Every one of those 1200 operations is
  where a filter driver, a network share or a spinning disc extracts its toll,
  and none of that is in these numbers. StreamMountain's disc is 6547 sectors,
  not 400 blobs.

**The warm regression is a genuine adoption risk and should be watched.** If the
engine's revisit path is mostly page-cache-hot, M5's second half could make it
slower, not faster, until the checksum gets cheaper. The instant-revisit
acceptance test in the plan (fly StreamMountain out and back, record cold vs warm
enter times) is the measurement that will settle it, and it should be read with
this table in hand.

## Reproducing

    export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
    make -C libs/AssetStoreLib bench \
      TMP="C:/Users/webde/AppData/Local/Temp" TEMP="C:/Users/webde/AppData/Local/Temp"

Figures above are the spread across five runs on a Windows 11 machine, MSYS2
UCRT64 GCC, `-O2`, local NVMe, scratch under `%TEMP%`.
