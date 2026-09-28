# StreamMountain frame attribution — 2026-09-27

Task 17 of `docs/superpowers/plans/2026-09-27-vg-vt-stabilization-and-measurement.md`.
The runs took place on 2026-09-28; the file keeps the plan's date. Capture script:
`tools/streammountain_attribution.sh`. Raw outputs (perf JSON, Chrome traces, logs)
stay in `C:/tmp/attr*` and are not committed. The traces alone are about 4 MB each.

## 0. Headline

| Question | Measured answer |
|---|---|
| Where does the frame go? | The GPU. Median GPU frame is 228–262 ms after 45 s of warmup and 417–694 ms after 300 s. The render thread's `loop_render_ms` is 30–59 ms. |
| Largest GPU zone | `gbuffer`: 146 ms median with POM off at 45 s, 302 ms at 300 s. |
| POM | +23 ms `gbuffer` median at 45 s, which is about 1.6× the run-to-run spread. +221 ms `gbuffer` and +56 ms `rt_gi` at 300 s. |
| Second GPU zone | `rt_gi`: 50 ms at 45 s, 93 ms at 300 s (POM off). |
| VT | The `vt` GPU zone brackets `VtResidency::record_frame` in `vt_record_pre_pass`: page fills, table uploads and tier-2 AO. Its median is about 0 ms, but its p99 is 158–525 ms in all eight runs. The VT cost is occasional frames that do heavy page work, not a steady per-frame charge. |
| Five CPU traversals | Summed per-frame mean of the six named zones is 2.7–2.9 ms. `rt.rung_select` is 1.7–1.8 ms of that. |
| Hitches | Every 20 s sample at 45 s has two frames over 1 s, the worst 2.3–3.2 s. Seven of those eight frames are a single 2.29–2.74 s `pf.static` or `publish.vulkan` stall on the render thread. |
| Geometry runtime | It does not run on the default path: `geometry.*` zones are absent from all six default traces. See §3.3 for the paged profile. |

These numbers describe a different world from the one the 2026-09-19 analysis
measured. Since 2026-09-19 the default StreamMountain includes the geometry
stress profile (`docs/agent/evidence/2026-09-19-geometry-stress/README.md`):
0.25 m voxels in the finest sector, and 1,283 dense rock placements (about 252 M
placed source triangles) in front of the default camera. So this is not a
same-scene regression measurement. §5 compares each earlier inference with what
the measurements here can and cannot support.

## 1. Setup

| Item | Value |
|---|---|
| Git SHA | `78a5a994de66e8331ad0ae40d53696b263dd8e14` (branch `vg-vt-improvements` tip). `editor.exe` built from it with `./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor` (MSVC). |
| GPU / driver | NVIDIA GeForce RTX 4090, driver 610.74 (WDDM), Vulkan API 1.4.341 |
| CPU / RAM | AMD Ryzen 9 5900X (24 logical cores); stream bake pool 12 workers |
| Resolution | 1920×1080 native. DLSS is unavailable in this build (Streamline SDK absent). Borderless visible window, UI hidden. |
| Presentation | Requested and effective IMMEDIATE, frame limit 0 (`MATTER_PRESENT_MODE=immediate`, `MATTER_FRAME_LIMIT=0`) |
| Camera | World default: eye (380, 90, 1600), target (420, 55, 1420) |
| World settings | `scenes/streaming/StreamMountain/props.json` as saved: POM off, volumetrics on, `Rock/hide`. RT on (`vk_rt_effective`), GI on. |
| Warmup / sample | Sampling starts after static uploads hold still for 30 frames. Then 45 s warmup and 20 s sample (four-variant set), or 300 s warmup and 20 s sample (pair). |
| Validation errors | 0 in every run |
| GPU sharing | The script waits until nothing else holds more than 2 GiB of VRAM before each launch and logs `nvidia-smi` every 5 s. Every launch started at 600–1001 MiB used. |

**POM mechanism.** The world saves `render.pom.enabled=false`, and no environment
variable toggles POM (`MATTER_RENDER_POM` does not exist). So every run sets POM
through the command FIFO with `set render.pom.enabled true|false`, which is not
persisted. The first attempt was invalid: a `set` dispatched before
`bake.finished` is overwritten when `EditorProps::on_world_connected` applies the
world props at BakeFinished (`MatterEditor/src/main.cpp` ~6840–6925). The script
now queues `wait_event bake.finished` and `wait_frames 2` before the `set`, and
fails a run whose `perf.json` `pom_enabled` differs from the request. Those
invalid runs still count as two extra POM-off repeats, and §2 uses them for
noise.

**GPU busy check.** During a 300 s-warmup run, the Windows `GPU Engine`
counter showed the editor's `graphics_1` engine 99.9 % busy over a 10 s window.
`nvidia-smi` read 100 % utilization at 2745 MHz and 128.8 W of a 450 W limit.
The GPU is saturated but draws little power: it stays busy without working
hard. That fits latency-bound shaders (dependent fetch chains, low occupancy).
It is an inference, not an occupancy measurement; measure-first item #3 is still
needed.

## 2. GPU zones

### 2.1 Matched four-variant set (45 s warmup, 20 s sample)

`tools/frame_attribution.py` output (`C:/tmp/attr/attribution.md`). Each cell
is median / p95 / p99 in ms. Zones with no samples in any run are omitted
(atmosphere, hdr_lighting, primary_light_cull, rt_gi_diffuse,
rt_gi_reflection_transmission, rt_local_direct, water_*).

| zone | pom_reference | pom_chart_only | pom_work | pom_off |
|---|---|---|---|---|
| frame_interval | 241.38 / 467.52 / 3002.85 | 233.53 / 733.94 / 2359.55 | 275.83 / 688.17 / 3214.16 | 229.73 / 605.25 / 2951.88 |
| total | 241.14 / 346.47 / 398.62 | 235.97 / 459.37 / 563.77 | 261.81 / 538.16 / 600.20 | 227.78 / 497.99 / 601.63 |
| gbuffer | 168.67 / 190.30 / 262.78 | 161.91 / 169.49 / 172.70 | 193.93 / 218.29 / 261.34 | 145.53 / 164.87 / 203.36 |
| vt | 0.00 / 106.06 / 161.51 | 0.00 / 249.83 / 351.68 | 2.80 / 299.66 / 348.49 | 9.21 / 320.78 / 399.50 |
| rt_gi | 54.50 / 65.16 / 104.40 | 57.15 / 63.52 / 66.46 | 34.45 / 41.68 / 56.45 | 50.38 / 56.25 / 57.81 |
| blas | 10.05 / 12.55 / 12.55 | 9.08 / 13.49 / 13.49 | 8.69 / 16.41 / 16.41 | 10.84 / 21.40 / 21.40 |
| tlas | 1.33 / 1.85 / 1.85 | 1.36 / 1.53 / 1.53 | 1.33 / 1.76 / 1.76 | 1.29 / 1.53 / 1.53 |
| denoise | 1.59 / 1.65 / 1.87 | 1.64 / 1.87 / 2.02 | 1.61 / 1.75 / 7.99 | 1.59 / 1.65 / 1.80 |
| cull | 0.19 / 0.23 / 0.27 | 0.16 / 0.20 / 0.22 | 0.19 / 0.25 / 0.27 | 0.18 / 0.22 / 0.36 |
| volumetrics | 0.18 / 0.18 / 0.39 | 0.16 / 0.18 / 0.18 | 0.18 / 0.19 / 0.62 | 0.16 / 0.18 / 0.19 |
| rt_sun_shadow | 0.14 / 0.15 / 0.72 | 0.15 / 0.15 / 0.15 | 0.15 / 0.15 / 0.67 | 0.14 / 0.15 / 0.54 |
| vt_feedback_readback | 0.11 / 0.12 / 0.13 | 0.11 / 0.12 / 0.14 | 0.11 / 0.12 / 0.13 | 0.11 / 0.12 / 0.13 |
| vol_scatter | 0.11 / 0.11 / 0.32 | 0.10 / 0.11 / 0.11 | 0.11 / 0.12 / 0.37 | 0.10 / 0.11 / 0.12 |
| cloud_shadows | 0.08 / 0.08 / 0.24 | 0.07 / 0.08 / 0.08 | 0.08 / 0.22 / 0.28 | 0.07 / 0.08 / 0.27 |
| vol_density | 0.05 / 0.05 / 0.06 | 0.04 / 0.05 / 0.05 | 0.05 / 0.05 / 0.15 | 0.04 / 0.05 / 0.06 |
| vol_integrate | 0.02 / 0.02 / 0.02 | 0.02 / 0.02 / 0.02 | 0.02 / 0.02 / 0.10 | 0.02 / 0.02 / 0.02 |
| composite | 0.01 / 0.01 / 0.02 | 0.01 / 0.01 / 0.01 | 0.01 / 0.02 / 0.04 | 0.01 / 0.01 / 0.02 |
| dlss | 0.00 / 0.00 / 0.00 | 0.00 / 0.00 / 0.00 | 0.00 / 0.00 / 0.00 | 0.00 / 0.00 / 0.00 |

Sample counts: 60 / 66 / 50 / 57 frames. `blas` and `tlas` have 7–12 samples
per run because those passes run only on some frames.

**Noise.** Three runs sampled POM off at 45 s: the two invalid first-attempt
runs and `pom_off`. Their `gbuffer` medians were 131.16, 143.50 and 145.53 ms,
a 14.4 ms spread. `rt_gi` was 45.10, 49.98 and 50.38 ms. `total` was 193.75,
203.04 and 227.78 ms. Against that spread:

- `pom_reference` adds 23.1 ms of `gbuffer` over `pom_off` (1.6× the spread) and 4.1 ms of `rt_gi`.
- `pom_chart_only` adds 16.4 ms `gbuffer`, about the size of the spread. Connected-relief handling is not separable from noise at 45 s.
- `pom_work` adds 48.4 ms `gbuffer`. It is a diagnostic path whose albedo encodes walk counts, so this is not a cost of the shipped path.
- `vt` p95 is unstable across POM-off repeats: 0.02, 308.47 and 320.78 ms. Its p99 is 158–525 ms in all eight runs (six at 45 s, two at 300 s). VT fill frames cost 100–525 ms each, and they are at least 1 % of frames in all eight runs and at least 5 % in seven of them.

### 2.2 Longer warmup pair (300 s warmup, 20 s sample)

`C:/tmp/attr_settled/attribution.md`:

| zone | pom_off | pom_reference | delta (median) |
|---|---|---|---|
| frame_interval | 416.61 / 683.59 / 871.02 | 696.39 / 1003.63 / 1948.02 | +279.8 |
| total | 416.55 / 568.46 / 680.55 | 693.55 / 955.39 / 999.94 | +277.0 |
| gbuffer | 302.24 / 329.28 / 334.61 | 523.22 / 538.75 / 545.36 | +221.0 |
| vt | 0.00 / 174.43 / 235.12 | 0.00 / 278.36 / 300.55 | — |
| rt_gi | 92.67 / 103.19 / 104.98 | 149.04 / 154.33 / 155.45 | +56.4 |
| blas | 13.78 / 17.49 / 17.49 | — | — |
| denoise | 2.21 / 2.40 / 2.66 | 2.64 / 2.75 / 2.80 | +0.4 |

Samples: 46 and 27 frames. Both runs had the same static geometry at the last
capacity report: 17.03 M vs 17.02 M vertices and 86 vs 85 parts. Both still
recorded uploads during sampling (static 2 and 1, instance 7 and 1), so neither
is fully settled.

What changed between 45 s and 300 s: POM-off `gbuffer` doubled (146 → 302 ms)
and `rt_gi` nearly doubled (50 → 93 ms). Static vertices were already at 17.0 M
in the 45 s runs, so the static set did not grow; instances did. RT-scanned
instances went from 1,641 to 2,129 and LOD-scanned from 161 to 272. The POM
delta grew about tenfold (+23 → +221 ms). One likely reading, not isolated here,
is that POM cost follows the amount of resident fine terrain and filled VT
pages. The 45 s set compares variants fairly, but it understates every
steady-state cost, and POM most of all. The `rt_gi` delta (+56 ms) is
consistent with the RT secondary path height-marching too. The blas-cache plan
says both raster and RT height marching use the shared zero-step setting.

## 3. CPU zones

### 3.1 Named traversals

Per-frame mean / p95 over each run's perf sampling window, which is the last
`frames` records of the trace. ProfileLib's trace is its 512-frame ring at exit,
load frames included, so the script trims it to that window. Times are in ms;
counters are instances per frame.

| zone | 45 s pom_ref | 45 s chart_only | 45 s work | 45 s pom_off | 300 s pom_off | 300 s pom_ref |
|---|---|---|---|---|---|---|
| frame wall | 334.28 / 455.88 | 304.90 / 733.20 | 400.26 / 685.24 | 353.46 / 601.87 | 441.11 / 681.76 | 763.48 / 1001.70 |
| resolve.sector_lod | 0.196 / 0.470 | 0.208 / 0.437 | 0.213 / 0.699 | 0.193 / 0.331 | 0.356 / 0.817 | 0.345 / 0.702 |
| resolve.emit | 0.177 / 0.412 | 0.188 / 0.353 | 0.210 / 0.523 | 0.176 / 0.414 | 0.327 / 0.675 | 0.258 / 0.502 |
| instance_cache.match | 0.029 / 0.051 | 0.037 / 0.055 | 0.030 / 0.054 | 0.036 / 0.062 | 0.059 / 0.122 | 0.061 / 0.082 |
| cull.vt_demand | 0.368 / 1.586 | 0.271 / 1.461 | 0.458 / 1.711 | 0.307 / 1.488 | 0.167 / 1.221 | 0.008 / 0.012 |
| rt.rung_select | 1.553 / 3.556 | 1.703 / 3.382 | 1.456 / 2.953 | 1.744 / 4.341 | 1.818 / 3.654 | 1.876 / 2.881 |
| rt.tlas_hash | 0.191 / 0.298 | 0.203 / 0.297 | 0.208 / 0.432 | 0.205 / 0.461 | 0.212 / 0.282 | 0.226 / 0.293 |
| geometry.update | absent | absent | absent | absent | absent | absent |
| geometry.scene_completion | absent | absent | absent | absent | absent | absent |
| geometry.scene_assembly | absent | absent | absent | absent | absent | absent |
| instances.sector_lod_scanned | 159 / 168 | 165 / 169 | 165 / 170 | 161 / 168 | 272 / 274 | 276 / 276 |
| instances.rt_scanned | 1633 / 1687 | 1669 / 1687 | 1672 / 1692 | 1641 / 1687 | 2129 / 2155 | 2196 / 2196 |
| instances.vt_scanned | 328 / 1661 | 327 / 1687 | 531 / 1692 | 289 / 1686 | 279 / 2155 | absent |

The six named traversals sum to 2.66 ms per frame on average in the 45 s
`pom_off` run and 2.94 ms in the 300 s one. `rt.rung_select` is 60–65 % of
that, at 1,600–2,200 RT instances. In the 300 s `pom_reference` window,
`cull.vt_demand` and `instances.vt_scanned` were idle: the demand pass had
nothing new to scan.

Other render-lane zones (45 s `pom_off` window, mean / p95 / max ms). These
zones nest, so the rows do not sum:

| zone | mean | p95 | max |
|---|---|---|---|
| build (⊃ build.prepare_frame ⊃ pf.static) | 46.07 | 23.04 | 2395.1 |
| pf.static | 42.93 | 16.23 | 2390.3 |
| publish.vulkan | 44.66 | 79.14 | 2281.6 |
| draw | 13.74 | 58.30 | 75.0 |
| cull.raster | 9.75 | 27.33 | 73.0 |
| raster.rt | 6.33 | 18.04 | 30.7 |
| raster.gbuffer | 3.13 | 6.52 | 68.7 |
| rt.instances | 2.47 | 11.74 | 23.6 |
| draw.vt_requests | 1.98 | 20.47 | 38.4 |
| vt.enrich | 1.34 | 0.82 | 66.9 |
| vt.fill_select | 0.95 | 1.89 | 2.6 |
| vt.residency_begin | 0.80 | 1.27 | 1.5 |
| vt.drain_feedback | 0.72 | 1.09 | 1.2 |
| vt.fb_scan | 0.63 | 0.98 | 1.1 |

### 3.2 Hitches

Every 45 s run has two frames over 1 s in its 20 s window, the worst
2.34–3.20 s. The trace attributes seven of those eight frames to a single zone
chain:

- `pf.static` at 2,289–2,733 ms (static geometry buffer rewrite), in four frames, or
- `build.prepare_frame` ⊃ `publish.vulkan` at 2,282–2,486 ms, in three frames.

The eighth frame (`pom_chart_only`, 1,010 ms) has no render-lane zone above
60 ms.

Each log reports three `STATIC CAPACITY OVERFLOW -- full O(world) rewrite
ahead` events. The vertex capacity steps from 512 MiB to 1 GiB to 2 GiB, and the
last event reports 1.50 GB of vertices and 269 MB of indices (17.0 M vertices,
67.2 M indices, 86 parts). The log has no frame numbers, so it does not show
whether the two sampled hitches are two of those rewrites. Every 45 s run
records 7–8 static vertex uploads inside its sample window. In the 300 s windows
`pf.static` is 0.64–0.73 ms mean with no multi-second frame. The worst frames
are 871 ms (`pom_off`, `publish` max 150 ms) and 1,948 ms (`pom_reference`,
`publish` max 921 ms). The rewrites belong to the fill, but the fill is still
running minutes after the perf gate lets sampling begin.

### 3.3 Geometry runtime

`geometry.update`, `geometry.scene_completion` and `geometry.scene_assembly`
are absent from all six default traces. The virtual-geometry runtime only runs
when the `MATTER_GEOMETRY_*` variables are set, and the default launch does not
set them. The paged profile in `tools/terrain_cache_audit.py` does set them, but
on this world it never reached a state where its runtime could be measured
(§4.2). Both `prepare` runs ran out of device memory before completing, and
`reopen` / `load` need a complete key set from a valid `prepare`. The cold
`prepare` trace (last 512 frames, 12 workers cooking) shows `geometry.update` at
0.91 ms mean / 1.49 ms p95. `prepare` skips runtime hierarchy-page registration,
so that figure is not the runtime's steady-state cost. **The 24–26 ms figure was
neither reproduced nor refuted here.**

### 3.4 Snippet

`tools/streammountain_attribution.sh` runs this over each variant's trace and
writes `cpu_zones.md`. It prints the tables above plus the top 15 render-lane
zones of the first variant:

```python
import json, math, pathlib, sys
ZONES = ["resolve.sector_lod", "resolve.emit", "instance_cache.match", "cull.vt_demand", "rt.rung_select",
         "rt.tlas_hash", "geometry.update", "geometry.scene_completion", "geometry.scene_assembly"]
COUNTERS = ["instances.sector_lod_scanned", "instances.rt_scanned", "instances.vt_scanned"]
def frames(path):
    out, pending, lanes, window = [], {}, {}, json.loads(pathlib.Path(path.replace(".trace.json", ".json")).read_text())["frames"]
    for e in json.loads(pathlib.Path(path).read_text())["traceEvents"]:
        if e["ph"] == "X":
            pending[e["name"]] = pending.get(e["name"], 0.0) + e["dur"] / 1000.0; lanes[e["name"]] = e["tid"]
        elif e["ph"] == "C" and e["name"] == "frame_ms":
            out.append({"wall": e["args"]["ms"], "z": pending, "c": {}}); pending = {}
        elif e["ph"] == "C" and out: out[-1]["c"][e["name"]] = e["args"]["n"]
    return out[-window:], lanes
def p95(v): s = sorted(v); return s[max(0, math.ceil(0.95 * len(s)) - 1)]
def cell(v, seen, fmt): return f"{sum(v) / len(v):{fmt}} / {p95(v):{fmt}}" if seen and v else "absent"
runs = [(pathlib.Path(p).name.split(".")[0], *frames(p)) for p in sys.argv[1:]]
print("| per-frame mean / p95 | " + " | ".join(n for n, _, _ in runs) + " |\n|" + "---|" * (len(runs) + 1))
print("| frames (sampling window) | " + " | ".join(str(len(f)) for _, f, _ in runs) + " |")
print("| frame wall ms | " + " | ".join(cell([x["wall"] for x in f], True, ".2f") for _, f, _ in runs) + " |")
for z in ZONES:
    print(f"| {z} ms | " + " | ".join(cell([x["z"].get(z, 0.0) for x in f], z in l, ".3f") for _, f, l in runs) + " |")
for c in COUNTERS:
    print(f"| {c} | " + " | ".join(cell([x["c"].get(c, 0) for x in f], any(c in x["c"] for x in f), ".0f") for _, f, _ in runs) + " |")
name, f, lanes = runs[0]
top = sorted((z for z in lanes if lanes[z] == 1), key=lambda z: -sum(x["z"].get(z, 0.0) for x in f))[:15]
print(f"\nTop render-lane zones in {name} (nested zones overlap; do not sum):\n\n| zone | mean ms | p95 ms |\n|---|---|---|")
for z in top:
    v = [x["z"].get(z, 0.0) for x in f]; print(f"| {z} | {sum(v) / len(v):.3f} | {p95(v):.3f} |")
```

## 4. Load

### 4.1 Default path (the four captures)

Every default launch pays a warm "roots only" bake before streaming starts:

| run | install | world | publish | total | launch → exit |
|---|---|---|---|---|---|
| 45 s pom_reference | 23 ms | 92.1 s | 99.8 s | 192.0 s | 361.1 s |
| 45 s pom_chart_only | 21 ms | 67.9 s | 106.8 s | 174.8 s | 324.5 s |
| 45 s pom_work | 20 ms | 53.0 s | 153.8 s | 206.9 s | 378.4 s |
| 45 s pom_off | 21 ms | 49.0 s | 97.9 s | 149.2 s | 299.2 s |
| 300 s pom_off | 20 ms | 47.7 s | 98.5 s | 146.3 s | 560.9 s |
| 300 s pom_reference | 23 ms | 97.5 s | 101.6 s | 199.1 s | 641.7 s |

Launch → exit minus warmup and sample (65 s) bounds load plus settle plus
shutdown: 234–313 s for the 45 s runs.

The first default launch in this worktree started from a 2026-09-09 `parts`
cache and had no geometry caches. It was stopped by the script's 40-minute
timeout while still baking. Nineteen `MountainDetailRock` parts (196,608
triangles each; 16 variants plus 3 fixed samples in
`shared-lib/mountain_geometry_site.js`) baked one after another. Their bundles
landed about 73 s apart while the process used about 1.9 of 24 cores. They then
flattened at about 30 s each. The next launch finished the root
bake at install 24 ms, world 315.7 s, publish 165.9 s, total 481.7 s. It then
lost the Vulkan device while a 20.7 GB ollama model shared the GPU, so the
script now waits for idle VRAM before each launch.

### 4.2 Paged profile: `tools/terrain_cache_audit.py prepare`, cold then warm

The plan's command line does not match the tool, which takes a mode argument and
has no `--world` flag. The runs used:

- `py -3 tools/terrain_cache_audit.py prepare --out C:/tmp/attr/audit-prepare-cold --timeout 5400`
- the same command with `--out C:/tmp/attr/audit-prepare-warm`

The cold run started with `geometry-pages`, `prepared-sectors` and `blas` all
absent from `projects/world_demo/.cache/StreamMountain` (none had ever been
written in this worktree), and with a warm `parts` cache. The audit profile is
1280×720, raster only, with RT, GI, POM, volumetrics and cloud shadows off, and
camera (425, 25, 1465) → (419, 23, 1455).

| | cold prepare | warm prepare |
|---|---|---|
| roots bake | world 60.1 s, publish 614.9 s, total 675.1 s | world 96.4 s, publish 45.5 s, total 144.2 s |
| first terrain cache event | 723.8 s | 151.4 s |
| resident sectors ≥ 100 / 500 / 1000 | 826 / 2149 / 2584 s | 215 / 1113 / 1398 s |
| terrain cache outcomes | 381 missing, all compiled | 125 hit, 100 failed, 2 missing |
| paging failures | 243 root payload budget exceeded, 4 vertex outside bounds, 1 root manifest budget exceeded | 100 root payload budget exceeded, 2 vertex outside bounds |
| lookup ms median / p95 | 0.29 / 1034 | 526 / 1276 (hits) |
| end | **VK_ERROR_OUT_OF_DEVICE_MEMORY** at 4412 s, 1538 resident | **VK_ERROR_OUT_OF_DEVICE_MEMORY** at 1503 s, 1089 resident |
| last static overflow before the fault | 4.30 GB vertices (48.8 M), 0.88 GB indices (220 M), 427 parts | 2.58 GB vertices (29.3 M), 0.54 GB indices (135 M), 275 parts |
| `valid` / `completed` | false / false | false / false |

Neither run is a completed load, so neither gives a load time. The failure
mechanism shows in both. Terrain assets that exceed `MATTER_GEOMETRY_ROOT_MB=1024`
fail paging, log `page compile unavailable, restoring the 3-rung ladder`, and
push full source geometry into the static buffers. Each growth is a full
O(world) rewrite, and the last one fails `vkAllocateMemory`. The cold run
auto-filed `issues/9fec02f1-a0eb-96c3-0dfa-7f1ef17ef6c8` in the main checkout.
`reopen` and `load` were not run. Both forbid compilation and are judged against
`prepare`'s key set, and no `prepare` completed, so both could only report the
same missing and failed keys.

Cold cook composition, from the log's `compile_profile`, `cache_write_profile`
and `page_write_profile` lines. Times are summed over the 12 workers, so they
are worker-seconds, not wall time:

| stage | worker-s | share | median per asset |
|---|---|---|---|
| compile total (400 assets) | 25,346 | 100 % of compile | 54.7 s (max 234 s) |
| of which `mesh_error::measure` (`verify_ms`, `geometry_compiler.cpp:363-366`) | 22,424 | 88.5 % | 48.5 s |
| of which `simplify` | 740 | 2.9 % | 1.4 s |
| of which attribute sampling | 429 | 1.7 % | 1.0 s |
| cache write total (396 assets) | 2,831 | 10 % of compile + write | 5.4 s (max 28.6 s) |
| of which `writer_wait_ms` (waiting for the writer) | 984 | 35 % of write | 0 ms (max 22.0 s) |
| of which `open_ms` (opening the writer BlobStore) | 346 | 12 % of write | 0.82 s |
| of which `hierarchy_write_ms` | 1,342 | 47 % of write | 3.1 s |
| sector mesher (1,552 meshes) | 1,400 | — | density 335 s + surface 905 s + boundary 160 s |

The cook wrote 31.9 GB of pages: median 67 MB per terrain asset, storing a
median 576 k triangles from a median 154 k source triangles. Mean `write_ms` by
commit-order quartile of the 381 terrain assets was 4.3, 4.9, 8.6 and 11.6 s,
while `index.bin` grew to 89.8 MB. That growth is the P0-3 pattern. But the
writer is 10 % of compile-plus-write worker-time, and error measurement is
79.6 % (88.5 % of compile alone). In the warm run a
cache hit still re-meshes its sector upstream: 1,113 sector meshes took 1,206
worker-seconds.

## 5. Against the 2026-09-19 analysis

| 2026-09-19 inference | Measured here | Verdict |
|---|---|---|
| POM costs 10–100 ms/frame (P0-1), from G-buffer A/Bs of 3.19 → 26.08 and 7.02 → 133.73 ms | +23 ms `gbuffer` at 45 s (1.6× run spread). +221 ms `gbuffer` and +56 ms `rt_gi` (+277 ms `total`) at 300 s. `pom_chart_only` is within noise of `pom_reference` at 45 s. | **Confirmed, and exceeded once the stream fills.** The cost grows with resident fine terrain and also lands in `rt_gi`. |
| Geometry runtime 24–26 ms/frame at steady state (P0-2) | Not exercised: the runtime is off on the default path, and the paged profile runs out of memory before steady state (§3.3, §4.2). The tree's own later 2026-09-18 evidence already reported 10.10–12.28 ms (`indexed-geometry`) and 7.50 ms (`packed-terrain-roots`) on the pre-stress world. | **Not measured.** It is also out of date as a baseline. |
| Five traversals cost about 8–15 ms/frame at about 88 k instances (P0-4) | 2.66 / 2.94 ms per frame, summed means of the six named zones, at 160–280 LOD-scanned and 1.6–2.2 k RT-scanned instances. `rt.rung_select` is 1.5–1.9 ms of it. `rt.tlas_hash` is 0.19–0.23 ms. | **Refuted at this scene's instance count.** The 88 k-instance premise does not describe the default StreamMountain. Only `rt.rung_select` exceeds 1 ms. |
| VT is 0.25 ms GPU p95 and 0.81 ms CPU p95 of the frame | GPU `vt` median about 0, but p99 158–525 ms in all eight runs and p95 0.02–321 ms. CPU `vt.fb_scan` 0.63–0.69 ms mean (max 2.5 ms); `vt.fill_select` 0.8–1.1 ms. | **Median confirmed, tail new.** Page-fill bursts are a large GPU tail cost. P1-3's "up to 11 ms CPU" feedback scan was not observed (≤ 2.5 ms). |
| Whole-renderer GPU p95 74–88 ms | 346–498 ms at 45 s; 568–955 ms at 300 s | Different world: the 2026-09-19 geometry stress profile landed after those runs. Not a regression claim. |
| 274 ms mid-frame staging copy (P1-2) | 2.29–2.73 s `pf.static` and 2.28–2.49 s `publish.vulkan` stalls, about two per 20 s during fill. Static buffers overflow three times per launch, reaching 1.5 GB of vertices on the default path and 2.6–4.3 GB on the paged profile, where the last overflow runs out of device memory. | **Confirmed and worse.** |
| Cold cooks of 410 s and 166 s, quadratic writer (P0-3) | No cold cook completed (§4.2). Error measurement is 79.6 % of compile-plus-write worker-time; the writer is 10 %. Writer cost grows with the index (4.3 → 11.6 s per asset by quartile). | **Writer growth confirmed. It is not the dominant cold-load cost.** |

## 6. Re-ranked Tier 1

Ordered by measured impact on this world. Each item names the Tier 1 row it
refers to in `docs/vg-vt-work-queue-2026-09-19.md`.

1. **1.3 POM march in physical page space.** POM is the largest single toggle
   measured: +221 ms `gbuffer` and +56 ms `rt_gi` at 300 s (§2.2).
2. **New: G-buffer cost with POM off.** Its geometry-versus-shading split is
   unknown. `gbuffer` is 146 ms at 45 s and 302 ms at 300 s with POM off (§2.1,
   §2.2), and the GPU is 99.9 % busy at 129 W. Before choosing between a
   geometry fix (1.5/1.7) and a shading fix (1.9), split `gbuffer`
   (measure-first #3 and the 1.1 split this plan did not include).
3. **1.7 One canonical triangle residency, plus a bounded static-buffer
   policy.** Render-thread stalls of 2.3–2.7 s in `pf.static` or
   `publish.vulkan`, about twice per 20 s of fill (§3.2). On the paged profile, `VK_ERROR_OUT_OF_DEVICE_MEMORY`
   at 2.6–4.3 GB of static vertices (§4.2).
4. **New: root-payload-budget fallback.** 243 of 381 cooked terrain assets
   exceed `MATTER_GEOMETRY_ROOT_MB=1024` and fall back to the full-detail
   ladder (§4.2). Until they don't, the paged path cannot load this world, and
   1.5 cannot be measured.
5. **1.9 Tape register pressure and page-fill throughput.** The `vt` GPU zone
   has p99 158–525 ms in all eight runs (§2.1, §2.2).
6. **New: RT GI cost.** `rt_gi` is 50 ms at 45 s and 93 ms at 300 s with POM
   off (§2.1, §2.2). No Tier 1 row covers it.
7. **1.2 Cook complexity, re-scoped.** `mesh_error::measure` is 79.6 % of
   cold-cook compile-plus-write worker-time (22,424 of 28,177 worker-s). The
   shared writer (Task 18) addresses the 10 % write share, whose per-asset cost
   rises from 4.3 to 11.6 s by quartile as the index grows (§4.2).
8. **1.5 Persistent geometry cut.** Not measurable on this world yet (§3.3).
   Keep it gated on item 4 and on measure-first #5.
9. **1.6 VT pool packing.** Its impact is memory. VRAM was not captured per
   consumer here, but both paged runs died of device-memory exhaustion. The log
   line `chart-space VT online: 25600 page pool (4064 MiB)` shows the pool
   competing with static geometry for the same VRAM.
10. **1.4 One shared visibility pass.** The six named traversals sum to
    2.7–2.9 ms per frame (§3.1), under 1 % of the frame. Demote it below
    everything that moves the GPU frame, and keep `rt.rung_select` (1.5–1.9 ms)
    as the only candidate worth an early-out.
11. **1.8 Compact feedback target.** `vt_feedback_readback` is 0.11 ms of GPU
    and `vt.fb_scan` is 0.63–0.69 ms of CPU (§2.1, §3.1). Demote.
