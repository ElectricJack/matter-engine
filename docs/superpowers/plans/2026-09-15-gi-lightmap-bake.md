# Fully baked GI lighting: per-Part lightmap bake (eager-bridge)

**Goal:** an offline bake that captures the engine's lighting (directional sun
with soft shadows, sky, multi-bounce indirect, occlusion) into per-Part-instance
lightmap textures in the Part's chart UV set, for renderers with no GI.

**Where it sits:** `MatterEngine3/src/gi_bake*.{h,cpp}` (core + scene loader +
image writers), `MatterEngine3/cli/matter_cli.cpp` (`matter bake gi`), a
`giBake({...})` world-script call in `script/world_definition_loader.cpp`,
tests in `MatterEngine3/tests/gi_bake_tests.cpp`, docs in `docs/bake-gi.md`.

## Design decisions (and why)

1. **CPU path tracer over `world_tracer::WorldTracer`, not the Vulkan RT
   pipeline.** The live RT path is a screen-space, temporally accumulated,
   denoised real-time estimator (docs/gi-reconstruction-2026-09-12.md); a
   texel-space bake is a different dispatch shape either way. The CPU tracer
   already exists, is GL-free, thread-safe for queries and returns
   normal/material/albedo per hit. CPU gives byte-determinism (task item 2)
   and lets the numeric fixture tests run in the headless suite on any box.
   The bake reuses the engine's lighting CONTRACT (WorldSettings sun/sky,
   material albedo, `resolveBaseColor` tint blend, emission), not its shaders.
2. **Chart UV set built by the bake itself** with `lod_bake::build_chart_rung`
   (the same MeshChartingLib pipeline the renderer's VT uses) at the requested
   `--texel-density`. Chart UVs are NOT stored in `.part` artifacts (the part
   store builds them at load time, `part_store.cpp:1381`), so the bake writes
   the UV set out as a sidecar (`lightmap_uv.bin`) the OBJ exporter reads.
3. **Instances = the world manifest** (`LocalProvider::connect` -> `WorldManifest`)
   with the tracer's own artifact policy (flat artifact preferred, compositional
   children expanded). One lightmap per placed instance; a part placed twice
   bakes twice (different world lighting), sharing one chart table.
4. **Units.** Lightmap value = irradiance / pi = the outgoing radiance of a
   white Lambertian receiver. A fully open horizontal floor under the engine
   defaults reads `sun_color * cos(theta) + sky_color`. That is exactly what
   three.js `lightMap` multiplies the diffuse colour by, and it matches the
   engine composite (`ambient = diffuse * sky_irradiance`, `sun * ndotl`).
5. **Sky model = uniform radiance `sky_color`** (the `WorldLights` "flat sky
   ambient" contract). The atmosphere SH sky lives in GLSL and is not
   reproduced; documented as a limitation.
6. **Cache** = AssetStoreLib `BlobStore` + `RefTable` at `<cache_root>/gi_store`,
   key = (bake version, part hash, instance transform, scene hash, lighting
   hash, settings hash). Moving ANY instance changes the scene hash and
   invalidates every bake in the scene (inter-part bounce), documented.

## Estimator (per texel, E/pi units)

```
value = 1/N * sum_s [ sun(x, n) + path(x, n) ]
sun(x, n)  = sun_color * max(0, n.l_s) * V(x, l_s)     l_s jittered in the sun disc
path(x, n) : T = 1; d = 0
            loop: w ~ cosine hemisphere(n); hit = trace(x, w)
                  miss  -> value += T * sky_color; stop
                  hit   -> if d == bounces: stop
                           T *= albedo(hit); value += T * (emission + sun(hit))
                           x = hit; d += 1
```

`bounces = 0` is direct sun + sky visibility (AO-like). Post passes, in order:
firefly ceiling (gi-firefly policy: center-excluded 3x3 median/MAD,
`max(0.25, 4*median, median + 6*MAD)`), edge-aware a-trous denoise (normal,
position and chart guides), then chart-bounded dilation (seam padding).

## Files

- `MatterEngine3/src/gi_bake.h/.cpp` — Settings/Lighting/Scene/BakeResult, chart
  build, texel rasterization, tracer, filters, dilation, sampling helper.
- `MatterEngine3/src/gi_bake_image.h/.cpp` — PNG8 (sRGB tone-mapped), PNG16
  (linear, scaled), RGBE `.hdr`; own zlib-stored encoder (16-bit PNG, no stb).
- `MatterEngine3/src/gi_bake_scene.h/.cpp` — world -> Scene (provider connect,
  part loading, tracer resident source), output writer (manifest.json, UV
  sidecar), store cache.
- `MatterEngine3/cli/matter_cli.cpp` — `matter bake gi <scene> --out <dir> ...`.
- `MatterEngine3/src/world_tracer.{h,cpp}` — Hit gains `tint[4]` + emission colour.
- `MatterEngine3/include/matter/world_definition.h` + loader — `GiBakeSettings`,
  `giBake()` global; `provider/local_provider.h` exposes it.
- Build: `cmake/manifests/engine-core.sources`, `MatterEngine3/Makefile` (cli),
  `MatterEngine3/tests/Makefile` (suite), `cmake/MatterEngine.cmake` (test + exe).
- Docs: `docs/bake-gi.md`, `docs/perf/gi-bake-baseline-2026-09-15.md`.

## Tests (`run-gi-bake` / CTest `gi_bake_tests`)

1. image writers: PNG signature/IHDR/CRC vectors, HDR header, round trip.
2. two-room fixture: courtyard floor ~ sun*cos + sky (+-8%); deep room floor
   < 0.15x courtyard; door-facing interior wall brighter with bounces than
   direct-only; direct-only deep floor ~ sky-through-door only.
3. seam: half-cylinder arch forced into several charts; max delta across
   chart-boundary edges <= 2x median in-chart edge delta + eps.
4. determinism: two bakes (threads 1 vs 2) -> identical hashes and files.
5. store cache: second bake hits; seed change misses.
6. `giBake()` parse + validation in `world_definition_tests`.
