# StreamMountain POM-off baseline — 2026-09-28

Task `clear-ridge.1`, the first task of the high-density geometry pass (epic
`clear-ridge`). **Every later clear-ridge task compares against these
numbers.** Raw outputs (perf JSON, Chrome traces, logs, `nvidia-smi` logs)
are in `C:/tmp/clear-ridge-1-w45/` and `C:/tmp/clear-ridge-1-w300/` and are
not committed. Companion documents: `streammountain-frame-attribution-2026-09-27.md`
("attribution" below) and `docs/vg-vt-work-queue-2026-09-19.md`.

## 0. Headline

| | 45 s warmup (3 runs, 172 frames) | 300 s warmup (3 runs, 141 frames) |
|---|---|---|
| GPU total median, per run | 227.5 / 238.6 / 248.3 ms | 402.1 / 402.4 / 454.2 ms |
| GPU total p99 = max, per run | 394.1 / 421.7 / 594.3 ms | 433.5 / 434.7 / 811.4 ms |
| Frame interval median / p99 / max, pooled | 233.2 / 3807.1 / 3939.9 ms | 409.5 / 922.2 / 1061.8 ms |
| Frames over 100 ms, pooled | 161 of 172 (93.6 %) | 138 of 141 (97.9 %) |
| Frames over 1 s, pooled | 6 (3 / 1 / 2 per run) | 1 (0 / 0 / 1 per run) |
| `gbuffer` median | 159.4–170.2 ms | 307.0–330.0 ms |
| `rt_gi` median | 54.3–55.8 ms | 80.3–102.2 ms |
| `vt` p99 | 162.6–354.6 ms | 0.02–371.1 ms |
| Peak whole-GPU VRAM (`nvidia-smi`) | 12,545–12,688 MiB | 12,927–13,071 MiB |

With the GPU frame at 230–450 ms, nearly every frame exceeds 100 ms. The
over-100 ms count means little until the median falls below 100 ms. Until then,
the spike signal is the over-1 s count and the histogram buckets above 500 ms.
The p99 equals the maximum in every run because each window holds fewer than
100 frames.

## 1. What changed

`3bafe049` makes POM off the default rather than a per-world choice:

- `TilesetPomSettings::enabled` now defaults to `false`
  (`MatterEngine3/include/matter/world_definition.h`). This one flag is the
  `render.pom.enabled` World prop. It gates every POM march: the ground
  tileset, the VT chart, the finished-surface detail, and the RT secondary
  lift. All of them read the uploaded `pom_steps`, which the flag sets to 0
  (`vk_scene_renderer.cpp:6480`). So terrain and world props both render
  without POM. Nothing was deleted. A world opts back in with
  `render.pom.enabled=true` in its `props.json` (VillaDoricColumnStudy already
  does), or with `set render.pom.enabled true` over the FIFO.
- StreamMountain keeps its explicit `render.pom.enabled=false` in
  `scenes/streaming/StreamMountain/props.json`. World-props saves are sparse, so
  the next editor Save drops that key while it equals the default. That is
  harmless until the default flips back.
- `tools/streammountain_attribution.sh`: `pom_off` no longer sends a FIFO
  `set`. It measures the world as it ships, and perf.json's `pom_enabled`
  check proves the default. It reported `false` in all six runs. The script
  also gains `RUNS=N` (runs named `<variant>_r<N>`) and writes `hitches.md`
  from the new `tools/frame_attribution.py --hitches` summary.

Before and after this change are the same configuration for StreamMountain,
which already saved POM off. So the "before" is the attribution session's
`pom_off` runs (§4), and this document is the "after".

## 2. Protocol (repeat it exactly)

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor
VARIANTS=pom_off RUNS=3 WARMUP=45  tools/streammountain_attribution.sh C:/tmp/<task>-w45
VARIANTS=pom_off RUNS=3 WARMUP=300 tools/streammountain_attribution.sh C:/tmp/<task>-w300
```

Compare each output directory's `hitches.md` and `attribution.md` with §3.

| Item | Value |
|---|---|
| Git SHA | `3bafe0495456602f1472f468a50b28272d984b97` (branch `aq/clear-ridge.1` off the `vg-vt-improvements` tip `68ec8030`). MSVC RelWithDebInfo `editor.exe`. |
| GPU / driver | NVIDIA GeForce RTX 4090, driver 610.74. 1,355–1,604 MiB of VRAM in use and 0–4 % utilization before each launch. |
| Resolution / presentation | 1920×1080 borderless visible window, UI hidden, IMMEDIATE, frame limit 0 |
| Camera | World default: eye (380, 90, 1600), target (420, 55, 1420) |
| Window | Starts after static uploads hold still for 30 frames, then a 45 s or 300 s warmup and a 20 s sample |
| World | `props.json` as saved: POM off, volumetrics on, `Rock/hide`. RT and GI on. Stream bake pool 12 workers. Warm `parts` cache (6.7 GiB). |
| Validation errors | 0 in all six runs |
| Host load | Shared with other agent sessions. WSL load average was 8.7–16.5 across the captures. One Windows sample (18:34) read 42–53 % total CPU. The GPU was otherwise idle. |

## 3. Results

### 3.1 45 s warmup (`C:/tmp/clear-ridge-1-w45/hitches.md`)

| run | frames | GPU total median / p99 / max ms | frame interval median / p99 / max ms | > 100 ms | > 1 s | static uploads in window | peak VRAM MiB |
|---|---|---|---|---|---|---|---|
| pom_off_r1 | 47 | 248.29 / 594.33 / 594.33 | 253.03 / 3698.51 / 3698.51 | 44 | 3 | 6 | 12679 |
| pom_off_r2 | 72 | 227.50 / 394.12 / 394.12 | 226.47 / 3807.05 / 3807.05 | 68 | 1 | 5 | 12688 |
| pom_off_r3 | 53 | 238.57 / 421.66 / 421.66 | 238.26 / 3939.87 / 3939.87 | 49 | 2 | 7 | 12545 |
| pooled | 172 | — | 233.23 / 3807.05 / 3939.87 | 161 | 6 | — | — |

| frame interval | r1 | r2 | r3 | pooled |
|---|---|---|---|---|
| 0–16.7 ms | 0 | 0 | 0 | 0 |
| 16.7–33.3 ms | 1 | 0 | 0 | 1 |
| 33.3–50 ms | 0 | 0 | 0 | 0 |
| 50–100 ms | 2 | 4 | 4 | 10 |
| 100–250 ms | 20 | 63 | 36 | 119 |
| 250–500 ms | 19 | 4 | 10 | 33 |
| 500–1000 ms | 2 | 0 | 1 | 3 |
| 1000–2000 ms | 1 | 0 | 0 | 1 |
| > 2000 ms | 2 | 1 | 2 | 5 |

GPU zones, median / p95 / p99 in ms (`attribution.md`):

| zone | r1 | r2 | r3 |
|---|---|---|---|
| total | 248.29 / 463.54 / 594.33 | 227.50 / 246.12 / 394.12 | 238.57 / 356.55 / 421.66 |
| gbuffer | 159.40 / 172.25 / 199.29 | 162.97 / 174.16 / 196.31 | 170.15 / 176.09 / 178.47 |
| rt_gi | 55.04 / 60.89 / 115.59 | 54.28 / 59.13 / 98.98 | 55.76 / 57.92 / 136.94 |
| vt | 11.27 / 246.94 / 354.59 | 0.00 / 0.02 / 162.61 | 0.00 / 106.31 / 167.52 |
| blas | 11.58 / 12.04 / 12.04 | 10.22 / 23.37 / 23.37 | 10.02 / 11.03 / 11.03 |
| tlas | 1.39 / 1.76 / 1.76 | 1.42 / 1.88 / 1.88 | 1.38 / 1.73 / 1.73 |
| denoise | 1.59 / 1.65 / 2.23 | 1.64 / 1.68 / 1.92 | 1.64 / 1.89 / 8.73 |

Every other zone has a median under 0.2 ms.

**Frames over 1 s.** Five of the six are a single render-thread stall, read
from each run's Chrome trace over the perf window:

- `pf.static` at 3,122–3,364 ms, in 2 frames;
- `build.prepare_frame` ⊃ `publish.vulkan` at 2,648–3,099 ms, in 3 frames.

The sixth frame (r1, 1,033 ms) has no render-lane zone over 100 ms. `publish`
was 81 ms. Every log reports three `STATIC CAPACITY OVERFLOW -- full O(world)
rewrite ahead` events, the last at 1.50 GB of vertices and 269 MB of indices
(17.0 M vertices, 67.3 M indices, 86 parts). Each run recorded 5–7 static
vertex uploads inside its window, so the static fill is still running when
sampling starts.

CPU traversals (mean / p95 ms): `rt.rung_select` 1.46–2.06 / 2.25–5.08,
`cull.vt_demand` 0.26–0.48, `resolve.sector_lod` 0.21–0.24, `rt.tlas_hash`
0.20–0.22. `geometry.*` is absent, because the geometry runtime is off on the
default path. Instances: 160–171 LOD-scanned and 1,636–1,690 RT-scanned.

### 3.2 300 s warmup (`C:/tmp/clear-ridge-1-w300/hitches.md`)

| run | frames | GPU total median / p99 / max ms | frame interval median / p99 / max ms | > 100 ms | > 1 s | static uploads in window | peak VRAM MiB |
|---|---|---|---|---|---|---|---|
| pom_off_r1 | 50 | 402.11 / 434.67 / 434.67 | 394.48 / 835.10 / 835.10 | 49 | 0 | 3 | 12955 |
| pom_off_r2 | 50 | 402.35 / 433.52 / 433.52 | 403.53 / 847.15 / 847.15 | 50 | 0 | 2 | 12927 |
| pom_off_r3 | 41 | 454.17 / 811.37 / 811.37 | 451.17 / 1061.75 / 1061.75 | 39 | 1 | 2 | 13071 |
| pooled | 141 | — | 409.49 / 922.18 / 1061.75 | 138 | 1 | — | — |

| frame interval | r1 | r2 | r3 | pooled |
|---|---|---|---|---|
| 0–16.7 ms | 0 | 0 | 0 | 0 |
| 16.7–33.3 ms | 0 | 0 | 1 | 1 |
| 33.3–50 ms | 0 | 0 | 0 | 0 |
| 50–100 ms | 1 | 0 | 1 | 2 |
| 100–250 ms | 1 | 1 | 0 | 2 |
| 250–500 ms | 45 | 47 | 26 | 118 |
| 500–1000 ms | 3 | 2 | 12 | 17 |
| 1000–2000 ms | 0 | 0 | 1 | 1 |
| > 2000 ms | 0 | 0 | 0 | 0 |

| zone | r1 | r2 | r3 |
|---|---|---|---|
| total | 402.11 / 420.55 / 434.67 | 402.35 / 429.51 / 433.52 | 454.17 / 735.81 / 811.37 |
| gbuffer | 306.95 / 321.72 / 331.46 | 307.67 / 322.33 / 324.89 | 329.98 / 346.24 / 356.94 |
| rt_gi | 80.34 / 85.10 / 89.97 | 83.42 / 90.97 / 91.97 | 102.22 / 109.53 / 111.02 |
| vt | 0.00 / 0.02 / 0.02 | 0.00 / 0.00 / 0.02 | 0.00 / 280.43 / 371.12 |
| blas | 16.81 / 19.51 / 19.51 | 28.18 / 28.18 / 28.18 | 35.86 / 61.97 / 61.97 |
| tlas | 1.59 / 1.76 / 1.76 | 1.60 / 1.66 / 1.66 | 1.57 / 1.99 / 1.99 |
| denoise | 1.97 / 2.18 / 2.21 | 1.97 / 2.48 / 3.46 | 2.22 / 2.68 / 3.70 |

No multi-second stall occurred after 300 s. In r1 and r2, every frame over
600 ms (818–842 ms) carries a 154–304 ms `publish.vulkan` stall on top of the
roughly 400 ms GPU frame. So does r3's 923 ms frame, with a 442 ms stall. r3's
other frames over 600 ms have no render-lane zone over 100 ms. Its GPU `total`
p95 and p99 are 736 and 811 ms, and its `vt` p95 and p99 are 280 and 371 ms.
The other two runs show no VT burst. The one frame over 1 s (r3, 1,066 ms) has
no render-lane zone over 100 ms. Static vertex uploads in the window dropped to
2–3. Instances: 248–268 LOD-scanned and 1,925–2,054 RT-scanned.
`rt.rung_select` is 1.91–2.61 ms mean.

### 3.3 Run-to-run spread (the noise floor a later claim must clear)

| | 45 s | 300 s |
|---|---|---|
| GPU total median | 20.8 ms (227.5–248.3) | 52.1 ms (402.1–454.2), 0.2 ms without r3 |
| `gbuffer` median | 10.8 ms (159.4–170.2) | 23.0 ms (307.0–330.0) |
| `rt_gi` median | 1.5 ms (54.3–55.8) | 21.9 ms (80.3–102.2) |
| Frames over 1 s per run | 1–3 | 0–1 |

## 4. Against the attribution's POM-off runs (the "before")

| | attribution 45 s `pom_off` (1 run) | this, 45 s (3 runs) | attribution 300 s `pom_off` (1 run) | this, 300 s (3 runs) |
|---|---|---|---|---|
| GPU total median | 227.78 | 227.50–248.29 | 416.55 | 402.11–454.17 |
| GPU total p99 = max | 601.63 | 394.12–594.33 | 680.55 | 433.52–811.37 |
| Frame interval median | 229.73 | 226.47–253.03 | 416.61 | 394.48–451.17 |
| Frame interval max | 2951.88 | 3698.51–3939.87 | 871.02 | 835.10–1061.75 |
| Frames over 100 ms | 57 of 57 | 161 of 172 | 45 of 46 | 138 of 141 |
| Frames over 1 s | 2 | 6 (1–3 per run) | 0 | 1 |
| `gbuffer` median | 145.53 | 159.40–170.15 | 302.24 | 306.95–329.98 |
| `rt_gi` median | 50.38 | 54.28–55.76 | 92.67 | 80.34–102.22 |
| Worst render-thread stall | 2.29–2.73 s (`pf.static` / `publish.vulkan`) | 2.65–3.36 s | 150 ms `publish` | 442 ms `publish` |
| Root bake total | 149.2 s | 216.7–224.6 s | 146.3 s | 198.0–205.9 s |
| Launch to exit | 299.2 s | 412.5–434.1 s | 560.9 s | 634.1–653.1 s |

The GPU medians are within the attribution's own spread. It saw 131–146 ms
`gbuffer` and 194–228 ms `total` over three POM-off runs at 45 s. This baseline
sits 14–25 ms higher on `gbuffer` at 45 s. That fits the busier host, but it is
not isolated. The render-thread stalls are about 0.4–0.6 s longer, and the root
bake's world phase took 91–98 s against 47.7–49.0 s. The attribution's own
`pom_reference` and `pom_chart_only` launches took 92.1 and 67.9 s, so that
phase varies widely between launches. Both are CPU-side, and they are the
numbers most likely inflated by the concurrent agent load noted in §2.
**Compare later tasks with §3, captured under the same protocol, and not with
the attribution's single runs.**

## 5. What the baseline says to fix first

This section interprets §3; it takes no new measurement.

1. **Frames over 1 s (45 s window):** static-buffer overflow rewrites and the
   publish stall. They are 5 of the 6 frames over 1 s, which is queue row 1.7
   and task `clear-ridge.5`.
2. **Steady GPU frame:** the `gbuffer` median is 64–77 % of the GPU `total`
   median across the six runs.
   Its split (row 1.12, `clear-ridge.2`) decides the next fix.
3. **VT fill bursts:** `vt` p99 is 163–371 ms in four of six runs (row 1.9,
   `clear-ridge.6`).
4. **`rt_gi`:** 54–102 ms (row 1.14, `clear-ridge.9`).
