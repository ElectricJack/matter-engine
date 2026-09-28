# 16K virtual atlas capacity for near terrain

Status: retained detail-capacity checkpoint. Six native builds, all fourteen
focused checks and both full-scene captures (58 images) pass. Full visual and
performance acceptance remains open.
Previous goal turn made progress: two immutable 29-image StreamMountain runs
proved the settled near views stay near 64 t/m despite requesting 128, while
indirection use rises. See `../2026-09-17-vt-resolution/README.md`.

## Change and contracts

Raise the supported virtual atlas edge from 8192 to 16384. The physical page
payload/border/pool budget and 64-texel pinned tail remain unchanged. Nine mips
reach that tail; the GPU record appends the ninth offset after the old fields,
preserving their member offsets. CPU/GPU array stride changes from 64 to 68
bytes and all consumers must use the rebuilt executable/shaders together.
The bounded indirection allocator admits 32768-word blocks for a worst-case
21846-entry table; its total 64 MiB budget is unchanged. Exhaustion still rejects
registration safely. Upload staging grows with the maximum table size; this is
additional metadata memory, not free capacity.

Existing paired feedback already carries 16-bit coordinates and four mip bits,
so high page coordinates and mip eight need no transport-width changes. Ordinary
chart atlases gain capacity. Periodic material domains retain their existing 8K
extent contract. Export accepts long/thin 16K atlases within its existing total
pixel budget; it does not promise unrestricted dense 16K-square image export.

StreamMountain now requests 128 t/m as a candidate. This is not accepted merely
because a number changed. A real 64 m sector staging check must retain 128 t/m
including gutters; actual scene diagnostics and controls must then demonstrate
higher effective density and useful material detail with stable POM/residency.
Other default budgets are not raised to hide failures.

## Verification scope

- CPU layouts: retain old 8K/512 layouts, exercise 16K and odd large sizes,
  parent fallback and exact resident membership through edits.
- Allocator: admit largest table, refuse reuse before reader retirement,
  recycle after retirement and preserve fixed capacity.
- Chart staging: a real serialized 64 m terrain mesh retains 128 t/m and its
  geometry ladder; legacy density and nested scaling continue to pass.
- GPU domain fixture: actual sampler reads a 16K receiver tail, highest fine
  coordinates and the appended ninth offset with tagged feedback intact.
- Existing composed POM/seam, feedback, queue, export and material-domain checks
  must continue to pass with the new record stride.
- Full StreamMountain capture reuses prior cameras/material/POM/lighting and
  records actual density, resident mips, page/indirection use and frame samples.
  The separate frozen r2 editor remains untouched; timing is not isolated.

The full terrain/building goal remains active. This capacity step does not
replace chart/LOD seam work, sparse per-instance layers, material art, movement
and RT/user visual approval, or original memory/performance acceptance.

## First native results

MSVC builds for residency, charting, asset writer, world definition, Vulkan
smoke and editor pass under prefix `vt-atlas16k-v1` in the shared evidence
folder. CPU residency/chart/writer/mountain checks pass. The real 64 m sector
reports **128 t/m, 8320×8320 atlas**, preserving the original geometry ladder.
The production GPU material-domain and paired-feedback checks pass, including
the synthetic 16K receiver and highest mip, with zero validation errors.
The subsequent GPU checks and full-scene results are recorded below.

Metadata cost is explicit: at the default eight-fill budget, raising the table
upload allowance adds 2 MiB per frame slot, or 6 MiB across the three slots.
At 32768 maximum variants, the appended offset adds 128 KiB each to the GPU
record buffer and its CPU record vector. These are in addition to any increase
in actually used indirection tables and CPU layout mirrors. The physical page
pool and the 64 MiB indirection arena allocations remain bounded as before.

All fourteen native checks now pass on the immutable v1 source set: CPU
residency, charting, asset writer, mountain source, GPU material domains,
paired/visible feedback, source snapshots, queue, composed POM, composed seams,
receiver materials, module residency and VT export. The domain fixture executes
743 probes (including highest 16K coordinates and mip eight), maximum channel
error 0.000023872, height error 0.000000002, zero failed probes. Existing POM and
seam numeric outputs remain unchanged from the preceding build. All GPU checks
report zero Vulkan validation errors.

Full-scene candidate output is `../2026-09-17-vt-resolution/v3`, using the same
capture driver and fixed views as v1/v2. No scene/runtime source may change
until that capture completes and audits its inputs.

## First full-scene result (v3)

The native editor (`192d79c5c8469db71c3d62f08c4b0de28bc167f0572dbdfac13f747f3633b1c3`)
completes all 29 images/markers with zero validation/command errors, unchanged
sources and executable, restored props and exit 0. The views actually gain
resolution: close ground finest density is about 129 t/m (PNG-quantized), with
about 125 t/m resident in the bottom crop; frontal cliff is about 121 t/m versus
59 previously. Grazing/distant portions still select coarser requested mips.
This is not a blanket forced-mip-zero quality setting.

Used physical-page high water rises 1386 → 1437 (3.7%), while table storage in
use rises 11.36 → 36.96 MiB within the unchanged 64 MiB arena. Peak variants
remain 998; evictions and capacity rejection stay zero. Page fills rise
2247 → 2330. Table-use values are not increases in the already allocated arena
size; additional upload/record allocations are described above.

G-buffer medians, ms (old-cap request-64 v1 → new-cap request-128 v3):

| View | Before | Candidate |
| --- | ---: | ---: |
| Overview | 34.2865 | 34.5105 |
| Grazing ground | 18.403 | 18.9535 |
| Frontal cliff | 28.085 | 28.1475 |
| Oblique cliff | 35.621 | 35.799 |
| Close ground | 6.8965 | 7.0025 |

These are non-isolated observations with another editor open, not performance
acceptance or proof of no regression. No POM settings or source recipe changed.
The new resolution reduces the visibly coarse edges on nearby stones and adds
normal detail, especially on cliffs. The result remains too soft/flat in some
materials and strongly blue in lit views. This is capacity/near-detail progress,
not final realistic material art or resolved terrain chart/LOD seams.

Root setup is 126.417 s in v3, including 114.825 s publish. The log attributes
75.937 s to the first ConiferBarkDetail GPU job. Its prior v1/v2 values were
24.849/2.923 s; the scene preparation/reset job stays around 35–40 s. A warm
repeat v4 is in progress on identical source and binary. Its startup already
reports 53.883 s root setup, 42.256 s publish, 2.763 s ConiferBarkDetail and
35.957 s reset. This points to first-use initialization/cache work in the bark
job; finer attribution is still required before blaming shader compilation or
claiming a bake-speed improvement. Both first-use and roughly 54 s warm setup
remain unresolved costs.

## Completed warm repeat (v4)

The identical executable/source set completes all 29 images and markers, with
zero Vulkan validation errors or command failures, unchanged input hashes and
editor exit 0. Together v3/v4 retain 58 images. Close ground again resolves
about 125 resident texels/metre in the bottom crop; frontal cliff about 121.
Peak used pages (1437), fills (2330), pinned variants (998) and indirection use
(36.96 MiB) match v3, with zero evictions. Warm G-buffer medians are 32.276,
17.502, 28.6465, 34.064 and 6.568 ms for the five views respectively. These
remain non-isolated measurements, not a performance acceptance result.

Warm root setup is 53.883 s; initial streaming fills 2586 sectors in 168.84 s.
Those timers overlap and must not be added. First-use root setup was 126.417 s.
The first bark job accounts for much of the difference (75.937 versus 2.763 s);
initialization/cache attribution still needs finer profiling. No whole-scene
bake-speed improvement is claimed.

Retain 16K virtual capacity and the 128 t/m StreamMountain request. The physical
pool budget stays unchanged; metadata costs are listed above. Near-resolution
progress does not close material realism, lighting reproducibility, terrain
chart/LOD seams, motion/RT approval, sparse overlays or performance requirements.

Evidence: [warm capture audit](../2026-09-17-vt-resolution/v4/audit.json),
[decoded channels](../2026-09-17-vt-resolution/v4-analysis.json),
[four-run comparison](../2026-09-17-vt-resolution/comparison.json),
[final source/binary audit](final-source-audit.json),
[close ground](../2026-09-17-vt-resolution/v4/close-lit.png),
[frontal cliff](../2026-09-17-vt-resolution/v4/cliff-lit.png).
