# Primary receiver light culling

This work adds conservative GPU light lists for primary raster and RT direct
lighting. It continues the [lighting optimization work](lighting-quality-and-adaptive-shadows-2026-09-12.md)
in the `castle/astra-assembly` integration worktree. Primary RT lighting is about
6% cheaper in the measured castle hall. DLSS Quality improves by 2.6–5.1% in two
matched comparisons; Native frame cadence is less consistent despite lower GPU
cost. The feature remains opt-in globally and is enabled in the live DLSS
Quality castle launcher. It preserves contributing lights and shadow samples.

## Implementation

A compute dispatch reconstructs receiver positions from the final, current
jittered G-buffer depth and builds one AABB per 16×16 internal-resolution tile.
Each tile rejects lights whose finite radius or spotlight cone cannot reach
that AABB. Floating-point padding and conservative handling of malformed data
protect boundary receivers. Partial edge tiles participate in all barriers;
empty tiles produce zero masks. The spot bound also covers obtuse cones and
the attenuation function's hard-edge branch.

The compute pass expands surviving bits into a compact ID list once per tile.
A monotonic merge emits ordinary ascending IDs followed by oversized ascending
IDs. Primary raster and RT shading use that list only when it is shorter than
the receiver's world list; otherwise they use the original world candidates
without an extra mask test. Each shading loop needs one index load per light,
instead of bit-iterator state or repeated per-light validation. A zero-length
tile list is a valid empty set. Both paths keep all shadow samples for contributing lights.
This removes candidate evaluation work; it does not reduce the number
of necessary shadow rays. Reflections, transmission and secondary GI keep
world-space lookup, including when their dispatch resolution matches primary.
The receiver bounds use shading depth, rather than the POM proxy position used
to move ray origins outside geometry. Normal-cone culling is deliberately
absent because water can replace its lighting normal after the G-buffer pass.

The original five light descriptor bindings and their consumer stage flags
remain intact. Masks extend the existing metadata buffer after its original
32-byte prefix and a new 16-byte tile header. Fixed-capacity ID storage follows
all masks, with one count plus `light_count` ID slots per tile; unused capacity
is not read. The producer uses a private compute descriptor layout. Camera changes rebuild masks and lists each frame; scene
records remain cached by publication generation. Resize rebuilds the mask
allocation. At 136 lights, masks and list capacity together occupy about
1.95 MiB at 1280×720 and 4.42 MiB at 1920×1080 per frame slot, with host-visible
device-local memory preferred. Allocation is capped at 64 MiB and device limits;
over-budget layouts use exact world lists without truncating light counts.

`MATTER_PRIMARY_LIGHT_CULLING=1` enables the feature at process startup; the
default `0` provides the comparison path. `MATTER_RT_PRIMARY_ONLY=1` independently selects
the smaller fixed primary shader without culling, for attribution and comparison.
The diagnostic `MATTER_PRIMARY_LIGHT_CULL_AUDIT=1` checks actual chosen-list
membership for every original world candidate, evaluates rejected primary RT BRDFs and
checks for any positive transmission-weighted contribution. Both diagnostic
controls default to `0`. Counts accumulate
privately before at most four atomics per receiver. Every retired audit frame
is checked, with status logged periodically and any positive rejection always
reported. Audit is disabled for timing measurements.

The optional `primary_light_cull` GPU zone is appended at index 24. It uses
`MATTER_GPU_LIGHTING_DETAIL_TIMERS=1`, alongside the previous HDR/split-GI
profiling zones. Disabled detail timers remain unavailable in raw statistics,
rather than reporting invented zero-cost measurements. The UI, perf JSON and
issue reports carry the new timing field. Detail timers can affect overlap and
stay off in normal comparison runs.

## Validation

The portable reference tests cover finite support, source radius, narrow and
obtuse cones, hard edges, tangency, large coordinates, nonfinite inputs,
zero-light layouts, overflow and memory caps. A deterministic sweep tests
20,000 boxes with 16 sampled receiver points each; 19,313 positive-attenuation
witnesses must survive. The native CPU run passed.

The visible `primary-light-cull` Vulkan fixture compares unculled and culled
raster HDR and raw RT direct lighting. It includes 65 lights across mask-word
boundaries, oversized world-list entries, away-facing spots, partial tiles,
internal resize, a same-size moving-light revision and a zero-light publication.
It also reads generated IDs back and compares them with masks and expected
ordinary/oversized ordering. The fixture passed with
zero validation errors and clean teardown. It caught a descriptor-pool
lifetime bug during development, now fixed: renderer-owned pools are released
before device destruction instead of leaving raw-device deleters in retained
frame resources. A separate test expectation was corrected so the fixture's
light radius actually covers the wider resize view.

Evidence is under `C:/tmp/castle-primary-culling/`. `baseline/` preserves the
previous executable and runtime DLLs; `baseline-source/` preserves the exact
previous renderer and shaders for controlled comparison. Final `final-reference/`
and `final-audit/` captures cover the hall, glass/gold, a 16-step camera pan,
settled history, GI disabled, equal GI extents and return to Native resolution.
All seven pairs have matching 1288×811 viewport dimensions. They are functional
captures, separate from the verified 1920×1080 performance runs. The visual
checks retain gold, glass, material detail and lighting; independently sampled
GI and histories mean these are not bit-exact image comparisons.

The final audit checks every retired frame and reports zero positive rejections,
with periodic rows through retired frame 720. Logged candidate rejection rates
range from 33.7% to 46.4%. No audit readback failures or Vulkan validation errors
were logged; both capture processes exited normally. The visible GPU fixture
also passes with adaptive primary shadows. Transmission and independent fixed
primary-only lighting pass their visible smoke tests. The original secondary
GI shader remains byte-identical, and its earlier secondary-RIS gate passed.
The CPU timing-statistics test passes the new zone's validity/null/percentile
contract.

Final receipts: `gates-gpu-list-visible/results.json`,
`gates-gpu-list-remaining/results.json`, `final-audit/log.txt`,
`acceptance.json` and the seven-pair `comparison.html`, all under the evidence
directory above. Final source/executable snapshots are in
`candidate-gpu-list-final/`. The final editor SHA-256 is
`5d2824def140a337da98c614ed3fd21204347e0b34c4a582b79c4536da8fd55f`.

## Measurements and acceptance

RTX 4090, NVIDIA 610.74, visible foreground windows, VSync off/MAILBOX,
CastleUpgraded hall, 1920×1080 output. DLSS Quality uses 1280×720 internally;
Native uses 1920×1080. Diffuse scale 0.128, reflection scale 1, four primary
area samples, secondary stochastic sampling, POM and output-aware material
sampling enabled. Each run uses 15 seconds of warmup and 20 seconds of samples.
All off/on comparisons use the same executable, with audit and extra detail
timers disabled. CPU compilation and other GPU tests do not run concurrently.
Runtime output size, active DLSS mode and visible/unminimized state are verified.

GPU columns are raw per-pass medians, rather than UI moving averages. Run 1
measures culling first; run 2 reverses that order.

| Mode / run | Culling | Median frame ms | FPS | GPU total ms | Primary direct ms | Combined GI ms |
|---|---|---:|---:|---:|---:|---:|
| DLSS Quality / 1 | Off | 9.132 | 109.50 | 8.865 | 3.463 | 1.222 |
| DLSS Quality / 1 | On | 8.689 | 115.09 | 8.277 | 3.248 | 1.291 |
| DLSS Quality / 2 | Off | 8.850 | 112.99 | 8.403 | 3.472 | 1.199 |
| DLSS Quality / 2 | On | 8.629 | 115.89 | 8.231 | 3.247 | 1.283 |
| Native / 1 | Off | 15.296 | 65.37 | 15.059 | 7.331 | 2.616 |
| Native / 1 | On | 15.775 | 63.39 | 14.585 | 6.904 | 2.649 |
| Native / 2 | Off | 15.871 | 63.01 | 15.202 | 7.329 | 2.659 |
| Native / 2 | On | 14.813 | 67.51 | 14.384 | 6.907 | 2.658 |

Primary direct cost falls 6.2–6.5% with Quality and 5.8% with Native. Total GPU
cost falls in both rounds and modes, but Native FPS changes from −3.0% to +7.1%
across the two comparisons. This is not an established universal frame-rate
win. Existing p95 frame cadence remains around 20 ms with Quality and 31–32 ms
with Native; this work does not fix presentation or frame pacing.

An attribution run selecting only the smaller fixed primary shader, with
culling disabled, measured 3.398 ms primary / 8.850 ms total / 110.26 FPS in
Quality. Native measured 7.201 ms primary / 15.010 ms total / 63.99 FPS; its
window observer recorded 145 visible polls out of 146 and zero minimized
polls. This ablation is secondary evidence, not one of the main off/on pairs.

With detail timers enabled, the tile producer costs **0.0247 ms median**
(0.0254 ms p95) at Quality. That diagnostic run measures 0.0740 ms HDR lighting
and 3.242 ms primary direct; its total is not substituted into the normal
comparison table. Raster-only Native measures 3.164 → 3.157 ms total GPU and
299.34 → 297.95 FPS: effectively unchanged, with no demonstrated raster FPS win.

Reproduce one Quality run from the worktree with native Windows Python:

```powershell
py -3 tools/castle_rt_perf.py `
  --editor MatterEditor/build/windows-msvc-dlss/editor.exe `
  --out-dir C:/tmp/castle-primary-culling/repro-quality-on `
  --width 1920 --height 1080 --dlss-mode quality `
  --primary-light-culling 1 --primary-cull-audit 0 --primary-only 0 `
  --area-samples 4 --material-footprint 1 --lighting-detail-timers 0 `
  --vsync 0 --gi-trace-scale 0.128 --reflection-trace-scale 1 `
  --secondary-light-sampling 1 --capture-frames 128
```

Use a fresh output directory and `--primary-light-culling 0` for the comparison;
`--dlss-mode native` selects Native. The helper records the complete environment,
camera/settings timeline, executable hash, window observations and raw timings.

## Development findings

The initial per-candidate filter was slower despite rejecting 35–45% of
world-list candidates in sampled castle views. With DLSS Quality it measured
87.49 FPS / 5.906 ms primary lighting; moving mask storage to host-visible
GPU-local memory only improved that to 89.91 FPS / 5.758 ms. Moving repeated
validation outside the loop and caching mask words reduced overhead, but did
not establish a win. A resized 1728×1084 run is excluded from matched results.

An additional regression affected the disabled path: shader specialization
alone did not recover the previous code generation. Compile-time isolation
restored the original fixed `rt_lighting.rgen.spv` byte for byte (SHA-256
`095c53423924db2ca806eb39f4cf00479f010aaae59a256e3bb71e2f43bdf92f`). The old
executable measured 113.97 FPS / 3.455 ms primary lighting in the same session;
the isolated disabled path measured 113.11 FPS / 3.454 ms. These comparisons
verify recovery of the baseline rather than a culling speedup.

Normal culling and audit have separate compiled wrappers, so the audit's
additional BRDF evaluation and private counters are absent from normal
culling. The final implementation also separates fixed primary lighting into
optional RT stage 9/group 7, using a 146,464-byte culled shader. Stage 2 retains the
original full GI shader and five-entry specialization map. Fixed primary adds
no history descriptors; only adaptive sampling uses history. Existing stage,
group and SBT offsets remain intact; the optional primary record is appended.
This prevents secondary lighting from inheriting primary culling logic.

## Next work

Keep the global default off pending broader views and hardware measurements;
the preferred DLSS Quality castle launch explicitly enables culling. The list
producer scans all lights and reserves capacity proportional to tiles × lights.
Prefix-compacted allocation or a hierarchy may help substantially larger light
sets; the current 64 MiB cap safely falls back to world lists. Multi-layer
depth clusters could improve loose tiles at doorways, if measured benefit
exceeds construction cost.

The main remaining direct-light expense is shadow visibility for lights that
actually contribute. Static local-shadow caching, with geometry/light revision
invalidation and dynamic/translucent handling, is the next substantial
opportunity. This change does not lower contributing-light ray counts, change
GI quality, simplify castle geometry or fix cold pipeline startup.
