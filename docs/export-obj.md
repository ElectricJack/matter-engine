# Exporting baked Parts as OBJ + MTL

`matter export obj` writes a committed Part — or a whole scene's worth of them —
as Wavefront OBJ, a matching MTL, and a set of PBR texture maps, for consumption
outside the engine. It is aimed at three.js, and the conventions below are
chosen to be what `OBJLoader` + `MTLLoader` already expect.

Implementation: `MatterEngine3/src/export/` (start at `mesh_export.h`, which
explains the layering). Gates: `make -C MatterEngine3/tests run-obj-export` and
`make -C MatterEngine3/tests run-obj-export-golden`.

---

## The command

```
matter export obj <part-id|scene> --out <dir> [--lod N] [--texture-size N]
```

Canonical Windows build: `tools/build-windows.ps1 -Config RelWithDebInfo -Target
matter_cli` → `MatterEngine3/build/windows-msvc/matter.exe`. Unix/rollback build:
`make -C MatterEngine3/tests matter-cli` → `MatterEngine3/tests/build/matter`
(the CLI lives in that Makefile because `MatterEngine3/Makefile`'s archive has
the generated SPIR-V header as a prerequisite and there is no `glslc` on
Linux/WSL).

### Target forms

| Form | Meaning |
| --- | --- |
| `CastleStone` | A part module, resolved against the project's object tiers (scene tier first, then project tier). Baked if the cache misses, then flattened. |
| `CornellBox` | A bare name that is not a module but *is* a scene exports that scene. A module wins when both exist. |
| `scene:CastleMasonry` | Every root of `scenes/<Name>/<Name>.js` (or the legacy `worlds/<Name>.js`). One OBJ per distinct resolved hash; two roots of the same part with the same params export once, with two placements in the manifest. Use the prefix to pick the scene when a module shares its name. |
| `0bc1d80be7ca849d` | Exactly sixteen hex digits: a bundle already in the cache. Nothing is evaluated, baked, flattened or written into that cache. |

A bare name is probed against the project and a **module wins over a scene of
the same name**, because a part is what the command is overwhelmingly asked for.
The `scene:` prefix is always available to say otherwise, so that tie-break never
leaves a scene unreachable. A name that is neither is an error naming every
place it was looked for.

### Options

| Option | Default | Notes |
| --- | --- | --- |
| `--out <dir>` | — | Created if missing. A relative path resolves against `--project`, not the shell's working directory. |
| `--project <dir>` | `projects/world_demo` | Project root. |
| `--world <name>` | the target | Scene name, and the cache bucket `<project>/.cache/<world>`. |
| `--cache-root <dir>` | derived | Overrides the bucket above. |
| `--lod N` | `0` | Rung index, 0 = finest. Out of range is an error, never a silent clamp. |
| `--texture-size N` | `2048` | Atlas edge in texels, 16–8192. |
| `--texture-format F` | `png` | `png`, `ktx2` (reserved) or `none`. |
| `--gutter N` | `4` | Dilated texels around each chart. |
| `--chart-cone D` | `45` | Chart segmentation normal-cone half-angle, degrees. |
| `--normal-space S` | `smooth` | `smooth` or `flat`; see **Normal maps**. |
| `--modules a,b,c` | all | Restrict a scene export to these root modules. |
| `--params <json>` | `{}` | Params override for a module target. |
| `--no-flatten` | off | Export the compositional part instead of flattening first. |

Exit codes: `0` success, `1` a usage error or a failed export, `2` `--help`.
The report goes to stdout; warnings and errors go through the engine logger,
which tees to stderr — so stdout stays parseable.

---

## What geometry you get

The exporter reads **what the bake committed** — it does not re-mesh. A schema
that opted into the autoremesher (`modifier_apply.cpp`'s retopology path) has
its retopologised triangles in the artifact, and those are what come out; a
schema that did not gets the marching-cubes/CSG mesh and, at `--lod N > 0`, the
flatten stage's QEM decimation of it. There is no exporter-side simplification
dial, deliberately: an export that silently differed from what the engine draws
would be useless for checking the engine against a web viewer.

## Coordinate conventions

Nothing is converted. What comes out is what the engine's part-local space is:

* **Units**: metres.
* **Up axis**: +Y.
* **Handedness**: right-handed.
* **Winding**: counter-clockwise seen from outside — front faces. Asserted in
  `obj_export_tests.cpp` against an independently derived outward direction, so
  a flip anywhere in the pipeline fails the gate.
* **Origin**: the part's own origin. A scene export does **not** bake the world
  placement into the geometry; each root is exported in local space and its
  transform is recorded in `manifest.json`, so a consumer can instance it.
* **UV**: one set, `[0, 1]` over the part's atlas, `v` increasing **upward**
  (the OBJ/glTF convention). Image row 0 is the top, so a texel at row `r` of an
  `H`-tall atlas has `v = 1 - r/H`.
* **Vertices**: welded. Two corners merge only on an exact match of position,
  normal, UV, material, tint and baked AO, so UV seams, material boundaries and
  hard shading edges each split a vertex — which is exactly the vertex set OBJ
  needs. Signed zeros are normalised first, so `-0.0` and `+0.0` do not split.

---

## Files

For a part exported as `<base>`:

```
<base>.obj              geometry: one `o`, one `usemtl` group per material
<base>.mtl              one `newmtl` per material the mesh actually uses
<base>_albedo.png       RGB8, sRGB-encoded
<base>_normal.png       RGB8, tangent space, linear
<base>_roughness.png    grey8, linear
<base>_metallic.png     grey8, linear
<base>_emissive.png     RGB8, sRGB-encoded — only when something emits
<base>_ao.png           grey8, linear, 255 = unoccluded
manifest.json           every part written, with hashes, counts and placements
```

All text is LF-terminated and written in binary mode, on every platform.

---

## Material key mapping

| MTL key | Source | Notes |
| --- | --- | --- |
| `Kd` | `MaterialDef::albedo` | sRGB-encoded. **White (`1 1 1`) whenever `map_Kd` is written**, because every consumer multiplies the two; the real value is on the `# base_albedo` comment and in the manifest. |
| `Ks` | — | Always `0 0 0`; specular comes from `Pr`/`Pm`. |
| `Ke` | `emissionColor * emission` | sRGB-encoded. White only when `map_Ke` exists. |
| `Ns` | derived from `roughness` | `alpha = r²`, `Ns = 2/alpha² - 2`, clamped to `[1, 1000]`. Only read by consumers that ignore `Pr`. |
| `Ni` | `MaterialDef::ior` | |
| `d` | `MaterialDef::opacity` | |
| `illum` | — | Always `2`. |
| `Pr` | `MaterialDef::roughness` | PBR extension key. |
| `Pm` | `MaterialDef::metallic` | PBR extension key. |
| `map_Kd` | baked | Albedo. |
| `map_Ka` | baked | **Ambient occlusion.** Not an ambient colour — this is the spelling the export spec asks for and the one most PBR-aware importers look at. |
| `norm` | baked | Tangent-space normal map. Prefer this one. |
| `map_Bump` | baked | The same file again, for readers that only know the classic key. three.js's `MTLLoader` maps `norm` → `normalMap` and `map_Bump` → `bumpMap`; if you load with `MTLLoader` and see a doubled effect, drop `map_Bump`. |
| `map_Pr` | baked | Roughness. |
| `map_Pm` | baked | Metalness. |
| `map_Ke` | baked | Emissive, written only when at least one material emits. |

`# alpha_tested: cutoff <x>` and `# double_sided: 1` comments carry the two
surface flags MTL has no key for.

### Why Ke is treated differently from Kd

White `Kd` with no `map_Kd` is a washed-out surface; white `Ke` with no `map_Ke`
is a **fully self-lit** one. So the exporter skips the emissive bake entirely
for a part where nothing emits, and then writes the real (usually black) `Ke`.

---

## Textures

The engine has no per-part texture images: a material is a set of analytic PBR
scalars, and the per-surface variation a part carries lives in its triangle
attributes. The maps exist so that data survives a format that cannot express
it.

**What is actually in them**, honestly:

* `albedo` — the material colour blended with the per-vertex tint, exactly as
  `shaders_vk/material_common.glsl`'s `resolveBaseColor` does it. OBJ has no
  vertex colours, so this map is the only way tint survives. For a
  single-material, untinted part it is a flat colour.
* `roughness`, `metallic`, `emissive` — per-material constants. They vary across
  the surface exactly when the part mixes materials, which is the case OBJ
  handles worst on its own. For a single-material part they are flat.
* `ao` — the per-vertex baked AO. **Today this is 1.0 everywhere on most parts**:
  the vertex-AO bake was rolled back (`docs/ao-bake-findings-2026-07-16.md`) and
  nothing currently writes it. The channel is wired end to end, so it will carry
  real data the day the bake returns.
* `normal` — see below.

Texels no chart covers keep a neutral background (flat normal, fully rough,
unoccluded, black albedo/emissive), and `--gutter` texels of neighbour-average
dilation run outward from every chart so a bilinear fetch at a chart edge never
reads another chart or the background.

8-bit maps clamp: a material with `emission = 5.0` writes white, not 5×. The
un-clamped floats are in `manifest.json`.

### Normal maps

Under the glTF/three.js convention a normal map is relative to the interpolated
**vertex** normal plus a UV-derived tangent. The exported OBJ already carries
those vertex normals in `vn`, and the engine has no finer normal source than
them, so the default (`--normal-space smooth`) map is neutral `(128, 128, 255)`
by construction. It is correct, it is uninformative, and it is still written so
a pipeline that expects the channel gets it — a constant image costs a couple of
kilobytes.

`--normal-space flat` bakes the other useful thing instead: the shading normal
against the per-triangle **geometric** frame, which is what a consumer that
discards `vn` (flat-shaded, or re-welded after an import) needs to recover the
authored shading. It is **not** the glTF convention — a consumer that also
applies `vn` double-counts it — so it is opt-in.

Transferring detail from a finer rung onto a coarse one (the case where a
standards-correct normal map would carry real information) is not implemented.

### Texture formats

`--texture-format png` is the shipped encoder. `ktx2` is reserved: the flag
parses, reaches the writer, and fails with "not implemented yet" rather than
silently writing a PNG under a `.ktx2` name. `none` skips texture writing and
makes the MTL emit real `Kd`/`Ke` values instead of white.

---

## UV charts

The exporter builds its own UV set with `libs/MeshChartingLib` — the same
`build_adjacency → segment_charts → chart_average_normals → plane_basis`
sequence the chart-space virtual-texturing bake drives — and packs it into one
square atlas of exactly `--texture-size` texels.

It does **not** reuse a part's baked `CHRT` chart sidecar when it has one: that
table is sized and paged for virtual texturing (up to 8192 texels, 128-texel
pages), which is not what a single 2048² OBJ atlas wants.

`manifest.json` records `charts.count`, `charts.texels_per_meter`,
`charts.fill` (covered area / atlas area) and `charts.max_distortion`
(1.0 = isometric). A low fill means the atlas is mostly gutter — usually too
many charts, which a wider `--chart-cone` fixes. Packing failure is an error
naming the atlas size to raise.

---

## LOD

`--lod N` indexes the artifact's rung ladder, 0 = finest. For a flattened part,
rung N means rung `min(N, that cluster's last)` of every cluster, so a cluster
with a shorter ladder contributes its coarsest rung rather than vanishing.

**The coarsest rung is often a billboard.** The flatten stage ends most ladders
with a two-triangle impostor quad per cluster whose appearance lives in the
bundle's impostor atlas, which this exporter does not read. Asking for that rung
gets you untextured quads carrying only their material colour; the exporter
warns when it detects the shape. Use a finer `--lod` for a mesh.

---

## Subtrees

A part is rarely one mesh. Both kinds of reference are followed and merged, with
transforms composed, so the OBJ is the whole part:

* a compositional part's **child-instance table**, and
* a flat artifact's **instance references** — the child subtrees the flatten
  stage declared instance boundaries because inlining them would blow its budget.

Recursion is depth-capped and guarded against revisiting a hash on the current
path. Anything skipped is counted in `skipped_subtree_refs` and spelled out in
the manifest's `warnings`, never dropped silently.

---

## Determinism

The same artifact and the same settings produce **byte-identical** OBJ and MTL
and identical texture content hashes, so exports can be diffed and cached in the
content-addressed store. That rests on:

* a locale-independent, integer-based float formatter (`export_text.h`) — not
  `printf`, whose decimal separator follows the locale and whose tie-rounding is
  unspecified;
* deterministic gather order, chart packing, weld and texel scan order;
* signed-zero canonicalisation before the weld;
* explicitly pinned PNG encoder settings, which are otherwise process globals.

A texture's stable identity is `content_hash` in the manifest: fnv1a64 over the
**decoded** payload, not the encoded file, so it survives an encoder upgrade or
a switch to KTX2.

Byte identity is a contract *within a build*. Two CPU architectures can round
the last digit of a chart UV differently; the golden gate compares numerically
with a tolerance for that reason, and byte identity is asserted where it is
meaningful — the same binary, run twice.

---

## Declaring an export from a scene (the DSL binding)

A scene can record how it wants exporting, so the settings live next to the
scene instead of in someone's shell history:

```js
class CastleMasonry extends World {
  static roots = [ /* ... */ ];

  static exports = {
    out: 'export/castle',     // relative to the PROJECT directory
    lod: 0,
    textureSize: 1024,
    textureFormat: 'png',     // 'png' | 'ktx2' | 'none'
    chartCone: 45,
    normalSpace: 'smooth',    // 'smooth' | 'flat'
    modules: ['CastleMasonryFixture'],   // omit for every root
  };
}
```

An array of entries is accepted too; the exporter uses the first `obj` one. Only
`out` is required. The field is **inert at runtime** — nothing in a world load
or a frame reads it — and it supplies *defaults*: any CLI flag overrides it.

The loader validates strictly and **rejects rather than clamps**: an unknown
key, an unimplemented `format`, an out-of-range `textureSize` or `lod`, an
unknown `textureFormat` or `normalSpace`, a `chartCone` at or past 90° all fail
the world load with the authored property path. A silently substituted value
would produce an export nobody asked for and nobody could account for.

---

## manifest.json

One file per output directory, describing every part written: name, resolved
hash, lod, vertex/triangle counts, chart statistics, the material list with
un-clamped PBR floats, per-map content hashes, every file with its byte size and
hash, the world placements, and any warnings. It carries the coordinate
convention explicitly (`units`, `up_axis`, `handedness`, `front_face`) so a
consumer does not have to know this document.

---

## Consuming the output in three.js

```js
import { OBJLoader } from 'three/addons/loaders/OBJLoader.js';
import { MTLLoader } from 'three/addons/loaders/MTLLoader.js';

const materials = await new MTLLoader().setPath('/export/').loadAsync('CastleStone.mtl');
materials.preload();
const object = await new OBJLoader().setMaterials(materials).setPath('/export/')
  .loadAsync('CastleStone.obj');
scene.add(object);
```

Notes that actually matter:

* `MTLLoader` produces `MeshPhongMaterial`. For a PBR look, walk the result and
  rebuild each material as `MeshStandardMaterial`, reading `Pr`/`Pm` and
  `map_Pr`/`map_Pm` from the MTL yourself (`MTLLoader` ignores them) and wiring
  `map_Ka` to `aoMap`.
* `aoMap` needs a second UV set in three.js: `geometry.setAttribute('uv1',
  geometry.getAttribute('uv'))`. The export has one UV set, which is the right
  one for both.
* Set `map.colorSpace = THREE.SRGBColorSpace` on albedo and emissive, and leave
  roughness/metalness/AO/normal linear. That matches how they are encoded here.
* Place a scene's parts from `manifest.json`'s `instances` (row-major 4×4). In
  three.js: `object.matrix.fromArray(m).transpose(); object.matrixAutoUpdate =
  false;`.

---

## Limits

* **No animation.** A skinned or rigged part exports its bind-pose geometry;
  skin weights, the skeleton and every clip are dropped. OBJ cannot carry them.
* **No per-instance data.** Placements are recorded in the manifest, not applied;
  per-instance tint, material overrides and LOD state are not exported.
* **No glTF/GLB.** A deliberate non-goal for now. The extraction layer
  (`mesh_export.h`) is format-agnostic on purpose, so a glTF writer would be a
  sibling of `obj_writer.cpp` and nothing else.
* **No impostor atlases**, so the terminal billboard rung exports untextured.
* **No volumetric emitters, colliders, physics or props** — geometry and
  materials only.
* **One UV set, one atlas per part.** A part with many charts at a small
  `--texture-size` gets a low texel density; raise the size or widen the cone.
* **Dynamic materials need their world.** A part's material ids beyond the frozen
  builtins come from a world's `defineMaterial` calls, which are not serialized
  into the artifact. Exporting a lone part that uses one yields a neutral grey
  `mat_<id>_missing` placeholder; export it as part of its scene instead.
