# StreamMountain interactive load — 2026-09-16

## Result

The frozen r2 editor (PID 12832) loaded StreamMountain and rendered the terrain
and mixed forest with the editor panes visible. The scene remained open for
user review. This is a load investigation, not a settled performance benchmark
or full visual/VT acceptance.

## Cause and evidence

- Scene discovery resolved `scenes/streaming/StreamMountain`; Console reported
  connection, successful surface/habitat compilation and 20 required child
  variants. No scene path failure was observed.
- The packaged scene, sector, shared libraries, vegetation and terrain sources
  matched the development copy in all 96 files checked.
- The packaged cache was filling during the blank viewport: 40, then 51, then
  67 generated bundles. A non-invasive debugger sample found the bake worker
  in `fp_sdCapsule -> primitive_sdf -> GenerateMeshInternal -> mesh_sdf_ops
  -> HostBaker::bake -> install_world`. The main thread was presenting frames.
  Both debugger samples detached immediately after collecting stacks.
- Streaming initialization installs its dependency variants before enabling
  terrain streaming. This first-load work had almost no visible progress.
  Console's connection marker was about 06:46 after process start; the loaded
  screenshot's latest messages were about 17:17. These observations include
  cache investigation/seeding and are not a clean timing measurement.
- Of overlapping source/destination bundles compared, 78 were byte-identical;
  16 differed. All existing destination files were preserved.

## Recovery

Copied 462 missing cache files (2,282.8 MiB) from the development StreamMountain
cache into r2. Each file was copied to a unique temporary file, checked for a
concurrent source change, and published using an atomic link that fails if the
destination already exists. No existing destination was replaced. See
[cache-seed.json](cache-seed.json) for every copied entry and counts.

A subsequent stack sample showed the install worker idle and terrain bake
workers active. [mountain-loaded-ui.png](mountain-loaded-ui.png) confirms the
terrain and mixed forest drawing. Outer terrain was still filling; the Console
contained empty-LOD warnings, so this does not assert a clean warning log or a
fully settled scene.

The frozen executable remains SHA-256
`3aac176acbea521dfc6f1c0b017c5f376a2dc5ae7e1b4c7e18bdfc5db932b615`.
No engine source, executable, authored scene or rendering settings changed.

## Remaining work

- Measure and reduce the CPU dependency bake on an empty cache.
- Show current dependency, stage, elapsed time and cancellation while loading.
- Permit terrain to appear before optional vegetation/detail dependencies finish,
  with correct publication/lifetime handling.
- Prewarm showcase scenes when preparing a review build; retain native cache
  identity/version checks and avoid treating copied caches as a cold-start fix.
- Investigate the earlier approximately 2.5-minute process initialization
  separately; its exact bottleneck was not captured in this run.
