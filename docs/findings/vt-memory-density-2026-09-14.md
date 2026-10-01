# Virtual texture memory density

Date: 2026-09-14. Native Windows/MSVC, RTX 4090. Diagnostic measurements, not full VT acceptance.

## Finding

There are two separate inefficiencies: the allocated physical pool is much larger than the occupied working sets in these captures, and small charts use only a small part of their page-aligned blocks. A tightly arranged physical page grid does not imply densely packed surface data.

The pool has 33,024 slots and approximately 4 GiB of format-accounted image storage. After correcting feedback visibility, the castle validation capture ends with **1,510 pages (186.4 MiB, 4.57% of the pool)** and passes the strict 600-frame settled gate. After removing unnecessary CPU world-tracer reservations that stopped the first attempt, the terrain validation capture ends with **1,452 pages (179.3 MiB, 4.40%)** and passes the same 600-frame stability gate. The previous successful terrain revision held 1,631 pages (201.4 MiB, 4.94%); these different startup-retained sets do not establish a packing improvement. These are narrow paths, not exhaustive traversal requirements; the unused pool capacity is reserve, not occupied surface data. These figures cover physical page images, not source tilesets, preparation buffers or other renderer memory.

The subsequent production-mode runs, with validation and page-event logging disabled, retain **1,853 castle pages (228.8 MiB, 5.61%)** and **1,452 terrain pages (179.3 MiB, 4.40%)**. Both pass the strict settled gate. Source, packing and pool allocation are unchanged from the preceding terrain validation run. Castle's different retained set reinforces that these individual occupancy snapshots are not controlled memory-savings comparisons. Geometric coverage was not remeasured. See [production timing and artifact manifests](../agent/evidence/2026-09-15-vt-feedback/README.md).

The preceding staged-upload castle capture ended with 2,976 pages (367.5 MiB, 9.01%) and one late fill. The visibility fix removes hidden-surface requests in a native overlap fixture; it does not change chart packing, page formats or the allocated pool. Its full-resolution request attachment adds 15.82 MiB at 1920×1080, replacing the former 0.247 MiB compact feedback image; compact CPU readback size remains unchanged at that resolution. Fewer occupied pages do not by themselves release the unused physical pool allocation.

The preceding worker-validation captures retained 4,353 castle pages (537.5 MiB, 13.2%) and 1,630 terrain pages (201 MiB, 4.94%); both passed the strict settled counter gate. Packing, formats and pool capacity have not changed. Different retained sets across revisions and repeated launches are not evidence of a packing improvement or controlled memory savings.

The subsequent castle geometric diagnostic measures 2,704 occupied slots (334 MiB, 8.19% of capacity). Only **2.93% of occupied payload texel centers cover projected surface triangles**. Approximately **92.75% lies inside chart blocks but outside content bounds expanded by the existing gutter**. Thus whole-page rounding of small charts dominates geometric packing loss in this sample. Different startup-retained sets explain why each capture needs its own occupancy denominator; the detailed diagnostic is not interchangeable with the earlier baseline's slot count.

The terrain diagnostic measures 1,630 occupied slots (201 MiB, 4.94% of capacity), including 789 pinned tails. Triangle-center coverage is **24.09% for detail pages**, **0.68% for tails**, and **12.76% overall**. Of all occupied payload area, 42.54% lies beyond the atlas dimensions at that mip, mostly because small tails occupy full-size slots. Another 32.78% is chart-block space outside the gutter-expanded bounds. Terrain therefore benefits from both denser chart packing and compact tail storage.

| Earlier geometric diagnostic | Occupied slot storage | Pool occupied | Triangle-center coverage of occupied payload |
| --- | ---: | ---: | ---: |
| Castle | 334 MiB | 8.19% | 2.93% |
| Terrain | 201 MiB | 4.94% | 12.76% |

The original fixed-camera baseline used 212 MiB for castle after return, and terrain used 201 MiB when settled or 266 MiB after a camera turn/return. Those earlier retained sets are distinct from both the geometric diagnostics and the latest integration captures.

The later fixed-camera integration capture, after safe page replacement was implemented, retains 2,904 pages (359 MiB, 8.79% of the same pool) and records no new page work during its final 605 frames. Its [artifact manifest](../agent/evidence/2026-09-14-vt-01/castle-snapshot-fixed-artifacts.json) identifies the newer source revision and camera path. Geometric coverage was not rerun for that retained set; the 2.93% above belongs to the earlier dedicated density capture. Chart packing and channel formats have not changed. An intervening capture with camera drift was rejected as a matched comparison.

The earlier event-logged staged-upload capture ends with 2,895 pages and records 3,995 page fills with zero repeated owner/generation/revision/mip/coordinate identities. Its single late fill is a first request in that same frame. The new visibility-corrected capture records 2,461 fills with zero repeated identities and no page events during its settled interval. The native overlap fixture proves that direct shader feedback requested hidden owners, but these individual scene captures do not isolate why the earlier exact late page appeared. Packing coverage was not remeasured, so the earlier 2.93% must not be applied to any newer retained set. See the [capture report and feedback investigation](../agent/evidence/2026-09-15-vt-feedback/README.md).

## What the numbers mean

- Physical slots have no gaps between them: a 16×16 grid per array layer.
- Each slot stores 136×136 texels around a 128×128 payload. Physical borders are 11.42% of stored area; keep them separate from payload utilization.
- Three compressed appearance channels use one byte per texel each. The uncompressed material-ID/blend channel uses four bytes per texel, or 57.14% of page-format storage. Exact IDs constrain alternative encodings.
- Every owner reserves a whole physical slot for a coarse tail whose atlas dimensions are at most 64×64. Terrain's 789 pinned tails reserve about 97 MiB of slot capacity in the baseline.
- Triangle-center coverage is a geometric measure. Gutters, dilation and coarse filtering require some uncovered texels. Tiny charts can contain no coarse texel center while still receiving a valid nearest-surface fallback. Do not translate low center coverage directly into an equal percentage of recoverable memory.

## Optimization order

1. **Evaluate tighter chart packing.** Pack several small charts within pages while preserving gutters and chart identity. Measure occupied page count, composition work, seams, coarse filtering, POM and raster/RT agreement. Addressing or packing changes need explicit versioning and compatible-LOD validation.
2. **Evaluate compact coarse-tail storage.** Preserve always-available coverage while reducing the full-page cost per owner. Tail subregions need their own safe filtering margins and retirement rules.
3. **Right-size initial pool allocation.** Size against measured traversal working sets plus replacement/retirement headroom and other renderer allocations. Retain bounded growth or a tested capacity policy; these snapshots alone do not justify a universal tiny cap.
4. **Evaluate auxiliary-channel representation.** Preserve exact material identities and blend semantics, and measure device-format support and shader costs before adopting a smaller representation.

These are implementation candidates; this investigation does not apply a pool-size, page-format, chart-packing or quality change. VT replacement/latency work remains active, and layered materials remain a separate follow-on.

## Reproduction and evidence

Set `MATTER_VT_DENSITY_FRAME` to a positive VT serial in a dedicated settled-scene capture. The runtime logs a complete physical-slot census and geometric coverage. Analyze with:

```sh
python tools/vt_density.py --log run/log.txt --output density.json
```

Native tests verify analytic coverage, duplicate triangles, page clipping, rectangular tails, rotated chart frames and missing geometry. Parser tests reject incomplete or inconsistent snapshots. The expensive diagnostic runs once on the CPU and is excluded from performance acceptance.

Both native capture drivers exit 0 with verified 1920×1080 screenshots. Every occupied slot is reported once, all geometry is available, all tails are filled, and matching frame counters agree. One-shot CPU measurement costs approximately 721 ms for castle and 812 ms for terrain; neither run is timing acceptance evidence.

See [native evidence and limitations](../agent/evidence/2026-09-14-vt-01/README.md), [castle density counts](../agent/evidence/2026-09-14-vt-01/castle-density-summary.json), [terrain density counts](../agent/evidence/2026-09-14-vt-01/stream-density-summary.json), their [castle](../agent/evidence/2026-09-14-vt-01/castle-density-artifacts.json) and [terrain](../agent/evidence/2026-09-14-vt-01/stream-density-artifacts.json) artifact manifests, and [source/build/test identity](../agent/evidence/2026-09-14-vt-01/density-source.json).
