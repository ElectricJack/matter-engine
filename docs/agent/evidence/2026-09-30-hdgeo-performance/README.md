# High-density geometry comparison evidence — 2026-09-30

The [decision report](../../../findings/hdgeo-performance-pass-2026-09-30.md)
compares six historical task-1 runs with twelve final-source static/VG runs.
All setups use POM-off StreamMountain, 1920×1080, its default camera, RT/GI,
visible immediate uncapped presentation, three repeats at each of 45/300 s
warmup and twenty-second samples. Current GPU captures ran serially from
02:50:12 to 04:13:52 PDT. No engine implementation or default changed here.

- `summary.json` retains all 18 runs' cadence samples, per-pass GPU summaries,
  CPU trace-window summaries, coverage/memory observations and SHA-256 hashes
  for the raw inputs. GPU quantiles remain per run; cadence can be pooled.
- `protocol.json` records source/build identity, ordered launch recipes,
  budgets, baseline reuse authorization and absence of inherited driver overrides.
- `reduce_captures.py` revalidates and reduces the original retained dataset.
  Run it from the repository root. It expects the original raw directories
  below and uses `tools/frame_attribution.py` for canonical hitch buckets.
  `--partial` is only for campaign progress; the final reduction requires 18 runs.
- `capture_campaign.py` records the serial twelve-run driver used here. Its
  paths name the preserved original dataset; recapture into fresh directories
  using the report's commands, rather than overwriting those artifacts.
- `inputs_sha256.json` hashes the StreamMountain authoring/props and capture tools.
- `host-load.csv` records WSL load/RAM from 02:58:28 to 04:13:53 PDT; this covers
  load/warmup/sample phases but omits the first static run. Load1 ranges 1.83–9.78.
- `images.json` maps the four unchanged 1920×1080 native PNGs below to their
  original paths, captured sidecars and content hashes. Early images are r1;
  late images are r2. They were requested during warmup, outside timed samples.
- `retained-cr9-*.log` are previous native checks of unchanged final implementation,
  inspected and preserved here; they are not new test runs. RT, transmission,
  local-direct and composed-parallax report `ALL PASS`, validation errors zero.
  The source-string test retains the already-recorded exit-9 failure.

The first image monitor stopped on a transient `OSError: [Errno 61] No data
available` while reading native output. It was restarted with read retries.
Geometry diagnostics also interleaved inside one warmup `printf`; phase/duration
tokens and launch provenance are checked separately. Neither event changed timing
settings or invalidated a capture. All four capture groups exited zero.

Raw JSON/traces/logs/commands/GPU logs remain in
`C:/tmp/clear-ridge10-20260930/{static,vg}-w{45,300}`. Baseline inputs remain in
`C:/tmp/clear-ridge-1-w{45,300}`. The final executable, build log, input hashes,
native-check logs and generated hitch/attribution tables also remain in the
current artifact root, outside the recyclable worker slot. Historical baseline
host load was 8.7–16.5 and its scene/residency differs from current runs.

These captures establish neither identical loaded/detail populations nor visual
parity. Early VG has much less terrain/vegetation and coarser rocks. Late VG
has conspicuous triangular rock patterns and different terrain brightness.
Measured windows exclude cold startup, camera movement and shutdown; zero sampled
intervals above one second is limited to those windows.

| Warmup | Static/source | VG paged |
|---|---|---|
| 45 s, repeat 1 | [Native image](static-w45-r1.png) | [Native image](vg-w45-r1.png) |
| 300 s, repeat 2 | [Native image](static-w300-r2.png) | [Native image](vg-w300-r2.png) |
