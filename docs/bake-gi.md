# Fully baked GI lighting: per-Part lightmaps (`matter bake gi`)

Status: shipped 2026-09-15 (task eager-bridge). Code: `MatterEngine3/src/gi_bake*.{h,cpp}`,
`MatterEngine3/cli/matter_cli.cpp`, tests `MatterEngine3/tests/gi_bake_tests.cpp`
(`make -C MatterEngine3/tests run-gi-bake`, CTest `gi_bake_tests`). Baseline:
`docs/perf/gi-bake-baseline-2026-09-15.md`. Plan: `docs/superpowers/plans/2026-09-15-gi-lightmap-bake.md`.

## What it does

An offline bake that captures the engine's lighting — directional sun with a
finite angular diameter (soft shadows, column shadows, light shafts through
openings), the sky, multi-bounce diffuse interreflection over the material
albedo, and the occlusion those imply — into one texture per **placed Part
instance**, in a chart UV set, so a renderer with no GI of its own (three.js on
the web) shows the scene with the engine's lighting baked in.

It traces on the CPU against the assembled scene through the engine's own
GL-free world tracer (`world_tracer.h`), so inter-part occlusion and bounce are
captured: a courtyard lights the room beside it through its doorway, a tower
shadows the wall behind it. It is deterministic: two bakes of the same scene
with the same settings produce byte-identical files.

## Running it

```bash
# canonical (Windows MSVC): tools/build-windows-from-wsl.sh RelWithDebInfo matter_cli
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/matter.exe bake gi CastleCourtyard --out C:/tmp/gi-castle
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/matter.exe bake gi Primitives --out C:/tmp/gi-prims --samples 128 --bounces 3 --texel-density 16 --prelit
```

```
matter bake gi <scene> --out <dir> [--samples N] [--bounces N] [--texel-density N]
                [--seed N] [--prelit] [--no-denoise] [--no-firefly] [--dilate N]
                [--threads N] [--max-atlas N] [--no-cache]
                [--projects-root DIR] [--shared-lib DIR]
```

`<scene>` is a scene or world name resolved exactly like `MATTER_WORLD`
(`projects/*/scenes/<S>/<S>.js`, then `projects/*/worlds/<W>.js`,
case-insensitive), or a path to that `.js` file. The tool runs the same
`LocalProvider::connect()` the editor runs when it opens the world, so parts
already baked by the editor are cache hits and the world's materials are
registered identically. Worlds whose parts need a GPU baker must be baked by
the editor first: the tool never creates a Vulkan device.

| flag | default | meaning |
| --- | --- | --- |
| `--samples N` | 64 | paths per texel; noise falls as 1/sqrt(N) |
| `--bounces N` | 2 | diffuse interreflections; 0 = direct sun + sky visibility only |
| `--texel-density N` | 8 | lightmap texels per metre (see census) |
| `--seed N` | 0x5EED | changes the noise pattern, nothing else |
| `--prelit` | off | also write `prelit.png` = albedo x lightmap, for unlit renderers |
| `--no-denoise` / `--no-firefly` | on | skip a post pass |
| `--dilate N` | 4 | seam padding rounds into the chart gutter |
| `--max-atlas N` | 2048 | atlas edge cap; density halves per part until it fits |
| `--no-cache` | | ignore the lightmap store |

### `giBake({...})` in the world script (the DSL call)

A world declares its bake defaults with a call anywhere in its script — module
scope, a static initializer or a lifecycle hook — because it declares data
rather than driving a build:

```js
class CastleCourtyard extends World {
  static gi = giBake({ samples: 128, bounces: 3, texelDensity: 16, prelit: true, out: 'gi' });
  ...
}
```

Every field is optional (`samples` 1..4096, `bounces` 0..8, `texelDensity` > 0,
`seed` u32, `denoise`/`prelit` booleans, `out` a directory, relative to the
project). A second call is an error. The call returns the effective settings.
CLI flags override the declaration; `--out` may be omitted when the world sets
`out`. The declaration reaches the tool through `WorldDefinition::gi_bake` and
`LocalProvider::gi_bake()`.

## Outputs

```
<out>/manifest.json                      settings, lighting, hashes, per-part and per-instance census
<out>/parts/<part-hash>.lmuv             the lightmap UV set of that part (see "UV set")
<out>/<NNNN>_<name>/lightmap.hdr         linear HDR lightmap, Radiance RGBE (new-style RLE scanlines)
<out>/<NNNN>_<name>/lightmap16.png       linear, 16-bit RGB, value / 8 (png16_scale in the manifest)
<out>/<NNNN>_<name>/lightmap.png         8-bit, tone-mapped (Reinhard, white 4.0) and sRGB-encoded
<out>/<NNNN>_<name>/prelit.png           --prelit only: albedo x lightmap, tone-mapped like lightmap.png
```

`NNNN` is the instance index in manifest order; `<name>` is the module name.
Every instance of the same Part shares one `.lmuv` (one chart table per Part)
and gets its own lightmap.

**Units.** A lightmap texel is linear RGB **irradiance / pi** — the outgoing
radiance of a white Lambertian receiver. Under the engine defaults a fully open
horizontal floor reads `sun_color * cos(theta_sun) + sky_color`, e.g.
`(2.2, 2.05, 1.8) * 0.59 + (0.38, 0.43, 0.52) = (1.68, 1.64, 1.58)` with the sun
36 degrees up. That is what three.js multiplies the diffuse colour by, and it
matches the engine composite (`ambient = diffuse * sky_irradiance`, `sun * ndotl`).

## Scene semantics: instances, not parts

The bake runs against the **full assembled scene** (every instance the world
manifest places, compositional children expanded) but writes output **per
instance**. A downstream tool that places the same instance at the same
transform gets the same look. Because every lightmap depends on every other
instance (occlusion and bounce), **moving, adding or removing any instance
invalidates every bake in the scene** — the manifest's `scene_hash` changes
and the store misses. Re-run the tool; unchanged scenes are cache hits.

The lightmap store lives at `<project>/.cache/<world>/gi_store` (AssetStoreLib
`BlobStore` + `RefTable`). Keys fold the bake version, part hash, instance
placement, scene hash, lighting hash, settings hash and the packed atlas
dimensions, so any change to any of them is a miss and nothing stale is ever
returned.

## Quality controls, in order of application

1. **Estimator** (`gi_bake.h` header comment): per texel, N samples of
   `sun(x, n) + path(x, n)`; the sun direction is jittered inside the sun disc
   (`sun.size` in the world script, default 0.53 degrees) so shadow edges are
   soft; the path is a cosine-weighted random walk that adds `sky_color` on
   escape and, at each bounce, the hit's own emission plus `albedo * sun`
   (emission is not scaled by the emitter's albedo, as in `composite.frag`).
   Albedo and emission colour are `resolveBaseColor(material colour,
   TriEx.tint)` — the same blend `material_common.glsl` and
   `rt_lighting_impl.glsl` use.
2. **Firefly ceiling**: the `gi-firefly-filtering` policy ported to texel space
   (centre-excluded 3x3 luminance median/MAD over same-chart neighbours, outer
   5x5 ring when fewer than three, ceiling `max(0.25, 4*median, median + 6*MAD)`,
   RGB ratios preserved).
3. **Denoise**: a variance-guided a-trous filter (steps 1/2/4) guided by normal,
   world distance and chart identity. The luminance weight is measured against
   the two texels' own noise (the variance of their means, propagated per
   iteration), so it averages within the noise and leaves real gradients and
   shadow edges alone. Taps that fall outside the chart are odd-reflected
   (`2*centre - mirror`), which keeps the support symmetric at chart borders: a
   one-sided kernel on a lit gradient otherwise drags the border toward the
   chart interior and shows up as a step across the seam — the vault fixture in
   `gi_bake_tests` measured 0.19 before this, 0.019 after.
4. **Dilation**: content is padded `--dilate` texels into each chart's gutter
   (the gutter is 4 texels, chart contents are at least 8 apart) so bilinear
   fetches across chart edges never blend against empty texels.

## UV set and the OBJ exporter (ticket prime-flare)

Chart UVs are not stored in `.part` artifacts — the part store builds the
chart ladder at load time (`part_store.cpp`, `lod_bake::build_chart_rung`), so
the bake builds its own chart atlas with the same MeshChartingLib
segmentation and planar projection at the requested density and writes it
out. The one difference from the renderer's atlas is the packer: the VT
packer page-aligns every chart to 128-texel pages because its pool is paged,
which turned a rock's forty tiny charts into a 3-megatexel atlas that was
0.2 % covered; the lightmap shelf-packs charts with just the 4-texel gutter
(`gi_bake::build_atlas`), and the `chart_atlas.h` texel mapping is unchanged.
The sidecar:

```
parts/<hash>.lmuv : "LMUV", u32 version(1), u32 tri_count, u32 atlas_w, u32 atlas_h,
                    f32 texels_per_meter, tri_count x { f32 u0 v0 u1 v1 u2 v2 }
```

Triangle order is the Part's LOD0 order: `part_asset::load_v2` ->
`lods[0].blas_indices` (every entry when the part has no ladder) -> each
entry's triangles in file order. UVs are normalized over the atlas with v
pointing down the image (row 0 is the top of the PNG/HDR). An exporter that
iterates the same entries emits one `vt` per corner from this file.

MTL convention (`manifest.json` "mtl"):

```
map_Ka lightmap.png                    # 8-bit tone-mapped, any OBJ reader
map_matter_lightmap_hdr lightmap.hdr   # custom key: the linear HDR lightmap
map_matter_prelit prelit.png           # custom key: albedo x lightmap
```

OBJ carries one `vt` set. Since the engine's albedo is procedural (chart-space
VT), emit the lightmap UVs as the `vt` set and let the consumer bind the same
UVs to both maps.

### three.js

```js
// geometry.attributes.uv holds the lightmap UVs from the .lmuv sidecar
geometry.setAttribute('uv1', geometry.attributes.uv);   // lightMap/aoMap read uv1 (r152+; 'uv2' before)
const lightMap = new RGBELoader().load('0003_Tower/lightmap.hdr');
lightMap.flipY = false;                                  // v is measured from the top row
const material = new THREE.MeshStandardMaterial({
  map: albedoTextureOrNull, color: baseColour,
  lightMap, lightMapIntensity: 1.0,   // texel = irradiance / pi, so 1.0 is physically matched
  aoMap: aoTextureOrNull,             // AO is already inside the lightmap; bind one only if you want extra contact darkening
});
```

For an unlit renderer use `MeshBasicMaterial({ map: prelit.png })` with the
same UVs.

## Validation

`gi_bake_tests` (headless, synthetic geometry, no project cache):

- **Two-room fixture**: a 40 m courtyard floor and a closed 8x4x8 m room with
  one doorway on the sun-facing wall. Asserts the open courtyard floor equals
  `sun*cos + sky` within 8 % (measured 1.687/1.647/1.588 vs 1.680/1.642/1.583),
  the deep room floor is below 15 % of it (1.5 %), the sunlit strip through the
  doorway carries the direct term (79 %), the room's cast shadow on the
  courtyard is sky-lit only (23 %), the door-facing interior wall is more than
  1.5x brighter with bounces than direct-only (7x), and dilation only adds
  texels.
- **Seam test**: a half-cylinder vault the 45-degree normal cone splits into
  charts; the p95 luminance step across chart-boundary edges must stay within
  2x the interior edge-to-edge p95 or 2 % of the level, the max within 3x or
  4 % (measured 0.019 on 2.34, interior p95 0.019).
- **Determinism**: threads 1 vs 2 give identical content hashes, texels and
  `.hdr` bytes; the cache key ignores thread count, changes with the seed, and
  changes for every instance when any instance moves.
- **Cache**: second bake hits both lightmaps, round-trips texels, albedo and
  content hash; a corrupted blob is a miss.
- **Image writers**: CRC-32/Adler-32 check vectors, PNG framing and IHDR CRC,
  RGBE round trip on both scanline encodings.

`world_definition_tests` covers `giBake()` acceptance, defaults and every
rejection path.

## Limitations

- The sky is the `WorldSettings.sky_color` flat radiance (the `WorldLights`
  contract). The atmosphere's SH sky and cloud shadows live in GLSL and are not
  reproduced. Local point/spot lights are not baked.
- Albedo is the material base colour with the TriEx tint; the chart-space VT
  surface detail (brick, mortar, POM) is not part of the pre-lit texture.
- CPU only. Minutes per scene is the design point; see the perf baseline.
- A texel claimed by overlapping triangles inside one chart keeps the first
  triangle in Part order.
