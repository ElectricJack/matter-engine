# Ground Tileset: Vulkan Port, Parallax & Macro Layer — Design

**Date:** 2026-07-21
**Status:** Draft for review

## Summary

Bring the `.gtex` ground tileset (2026-07-05 design) into the Vulkan hybrid
pipeline and fix the two problems observed in the GL v1: visible tile seams at
distance and a flat, unparallaxed ground. Four phases: **(0)** migrate the
Vulkan pipeline to reversed-Z depth so all new depth-dependent work (the
parallax depth write in particular) is authored once against the final
convention, **(1)** port tileset consumption to the Vulkan renderer with a
texture-array layout that structurally eliminates the mip-bleed seams,
**(2)** height-driven world-space parallax occlusion mapping with a
conservative depth write and height self-shadowing, **(3)** a coarser second
("macro") tileset composited as a frequency split — low-frequency variation
at all distances, sole survivor in the far field — which also breaks
near-field tiling repetition.

The bake pipeline (`Tileset` DSL → settle → GPU bake → `.gtex`) is unchanged.
All work is runtime-side plus one small material-schema extension and one new
authored tileset per world that wants a macro layer.

## Background: why v1 seams were visible

The bake-side seam invariant holds (edge strips are byte-equal per boundary
color; tested in `tileset_seam_tests`). The visible seams come from runtime
filtering:

- `tileset_provider.cpp` uploads the whole 4×4 atlas as one texture and calls
  `glGenerateMipmap` — no gutters, no per-tile mip chains. Edge strips are
  identical only for `edgeStripWidth` (0.15 m ≈ 77 texels at 512 t/m); once a
  mip's filter footprint exceeds that (~mip 6–7, i.e. moderate camera
  distance), border texels blend interior content from the *atlas* neighbor,
  which is generally not the *runtime* neighbor. Result: a 2 m grid visible
  exactly where trilinear engages coarse mips. (Called out as a "known
  approximation … revisit only if visible" in the v1 spec; it was visible.)
- No anisotropic filtering. Ground at grazing angles is the worst case; the
  resulting over-blur pulls the bad coarse mips even closer to the camera.
- The height channel is baked and uploaded but never sampled ("Height unused
  in v1") — the ground shades with a normal map on a flat plane.

## Goals

- Reversed-Z depth throughout the Vulkan pipeline: near-uniform depth
  precision across the whole view distance, ahead of the infinite-world /
  sector-streaming direction where standard-Z float visibly z-fights.
- `.gtex` tilesets render in the Vulkan hybrid pipeline (GBuffer + RT passes);
  the GL raster path is frozen — no backport.
- No visible seams at any distance: per-tile mip chains via texture arrays,
  anisotropic filtering, macro-layer far field.
- Ground relief from the baked height channel: parallax occlusion mapping that
  is seam-transparent (marches across Wang cell boundaries correctly), writes
  depth so RT passes see the displaced surface, and self-shadows toward the
  sun.
- Macro tileset: a second, coarser `.gtex` (authored with the existing
  `Tileset` DSL, e.g. 16 m tiles) composited under the detail layer at all
  distances and carrying the far field alone.
- RT hit shaders sample tileset channels (flat, no parallax) so GI,
  reflections, and shadows agree with the textured GBuffer instead of falling
  back to the scalar material.

## Non-Goals

- Any change to the bake pipeline, settle physics, or `.gtex` format.
- True heightfield intersection for secondary rays (hit-shader sampling is
  flat; revisit only if visibly wrong in reflections).
- Macro-height displacement of the terrain mesh (noted as future work).
- Slope-aware / triplanar mapping (planar XZ projection retained; slope
  stretch accepted as in v1).
- Hex tiles, >2 edge colors, more than 4 runtime slots.

## Phase 0 — Reversed-Z depth migration

Standard-Z + float stacks two precision losses: perspective projection crams
everything beyond a few tens of meters into a sliver near NDC 1.0, and float32
is coarsest near 1.0. Reversed-Z (far→0, near→1) aligns the hyperbolic
pile-up with float's densest region, giving near-uniform relative precision
over the full view distance. On Vulkan it is a convention flip, not a feature.

Current state (all standard-Z, `D32_SFLOAT`): depth clear `1.0`
(`vk_scene_renderer.cpp:396`), `VK_COMPARE_OP_LESS_OR_EQUAL` (`:1502`),
software-path `pixel.depth = 1.0f` (`:6340`).

Work items:

- **Projection:** flip the projection matrix depth mapping (near→1, far→0).
  Prefer the standard trick of swapping near/far in the projection build so
  clip-space `w` handling stays untouched.
- **Pipeline state:** depth clear → `0.0`; compare ops →
  `VK_COMPARE_OP_GREATER_OR_EQUAL`; audit every `VkPipelineDepthStencilStateCreateInfo`
  and `vkCmdClearDepthStencilImage` call site.
- **Shader audit:** every shader that linearizes depth or reconstructs
  position from the depth buffer — the volumetric froxel code (recently fixed
  for standard-Z math in `fa454c52`; its linearization inverts), the RT
  passes seeded from GBuffer depth (`rt_lighting.rgen`, `rt_shadow.rgen`
  position reconstruction), sky/background full-screen passes that test
  `depth == far`, and any `gl_FragDepth` writers.
- **Streamline/DLSS bridge:** set the depth-inverted flag
  (`streamline_bridge.cpp`, depth resource setup ~`:412`) so DLSS receives
  the correct convention.
- **Software reference path:** flip the `pixel.depth` seed and comparisons in
  the CPU raytracer reference so parity tests keep passing.

Exit criteria: image parity with pre-migration captures (allowing for reduced
far-field z-fighting — the point), plus a new regression shot with two
coplanar-ish distant surfaces (~2 km) that z-fights under standard-Z and must
not under reversed-Z.

## Phase 1 — Vulkan consumption with per-tile mips

### Texture layout: slice the atlas into arrays

At load, `load_gtex` output is sliced CPU-side from the 4×4 atlas into
**16-layer texture arrays**, one per channel:

| channel | format | 2 m @ 512 t/m per layer |
|---|---|---|
| albedo | `VK_FORMAT_R8G8B8A8_UNORM` (RGB used) | 1024² |
| normal | `VK_FORMAT_R8G8_UNORM` | 1024² |
| ORM | `VK_FORMAT_R8G8B8A8_UNORM` (RGB used) | 1024² |
| height | `VK_FORMAT_R16_UNORM` | 1024² |

Full mip chains are generated **per layer** (`vkCmdBlitImage` chain per array
layer, standard mip loop). Sampler: trilinear, `maxAnisotropy = 8` (clamped to
device limit), address mode repeat (harmless; cell UV is always in [0,1]).

Why this kills the seams: within a layer, every border texel row/column is
byte-equal to the matching strip of any color-legal runtime neighbor, so
bilinear/trilinear filtering is continuous across cells at every mip until a
tile collapses toward its per-tile average. The residual per-tile average
difference at the coarsest mips is exactly what the macro layer (Phase 3)
hides; until Phase 3 lands it is a faint low-frequency patchwork, strictly
better than v1's hard grid.

RGB channels upload as RGBA with opaque alpha (wide device compatibility for
`R8G8B8` is poor); memory cost at 4096² source: albedo+ORM 2×5.3 MB, normal
2.7 MB, height 2.7 MB ≈ **16 MB per slot with mips** — negligible.

### Descriptor layout

New bindings in the raster per-frame set (set 1, alongside the material buffer
at binding 5) and mirrored in the RT set 0:

- `sampler2DArray tilesetTex[16]` — one binding as a descriptor array,
  `descriptorCount = 16`, indexed `slot * 4 + channel` (4 slots × 4 channels).
  Unused entries bound to a 1×1 dummy (no `PARTIALLY_BOUND` requirement).
  Indexing uses `nonuniformEXT` (descriptor indexing is core in Vulkan 1.2,
  which the RT pipeline already requires) since slot varies per material
  within a draw.
- `TilesetParams` UBO: per slot `{ tile_size_m, texels_per_meter, height_min,
  height_max, mean_albedo (vec3, computed at load), valid }` plus global
  parallax tunables (see Phase 2).

Exact binding numbers assigned at implementation (next free in each set).

### Shader port

`shaders_vk/tileset_common.glsl` — a port of `tileset_sampling.glsl`'s Wang
machinery (edge-color hash, de Bruijn pair LUT, cell coords). Differences from
the GL version:

- Atlas-UV computation is replaced by `(layer_index, cellUV)`: the
  (top,bottom,left,right) → 4×4 cell mapping now selects an array layer
  (`layer = row * 4 + col`) instead of offsetting UV. `textureGrad` gradients
  are cell-local (`dWorldXZ / tileSize`), no more ÷4 — and no cross-tile
  gradient hazard at all.
- The per-slot `if (slot == N)` sampler dispatch is replaced by descriptor
  array indexing.

### GBuffer integration

`gbuffer.frag` gains a tileset branch. Plumbing required:

- `raster.vert` passes **world-space position** to the fragment stage (new
  varying; transforms are already available in the vertex stage via
  `DrawTransforms`). Camera world position for Phase 2 comes from the existing
  frame UBO.
- Material → slot: `MaterialGpu.flags_misc.y` packs
  `(detailSlot + 1) | ((macroSlot + 1) << 8)` (0 = untextured; macro unused
  until Phase 3). Populated from `MaterialDef` (see below).
- When detail slot > 0: sample albedo/normal/ORM via Wang lookup at
  `worldPos.xz`; albedo replaces `resolveBaseColor`'s output (vertex tint
  multiplies on top, preserving authored tinting), tangent-space normal is
  rotated into the interpolated surface frame, baked AO multiplies vertex AO,
  roughness/metallic come from the ORM texture.

### Material schema

`MaterialDef` (schema v4): rename semantics of `groundTilesetSlot` to "detail
slot" (field name kept for source compatibility) and add
`int groundMacroSlot` (−1 = none). `MaterialRegistrySetGroundTilesetSlot()`
gains a macro sibling. `MaterialGpuRecord` packs both into `flags_misc[1]` as
above; the GL path never reads `flags_misc`, so the frozen path is unaffected.

### RT hit-shader sampling

`rt_surface_common.glsl` gains the same flat Wang sample (no parallax): when
the hit material's detail slot ≥ 0, `RtSurface` albedo/normal/roughness are
overridden from the tileset at the hit point's world XZ. Ray-diff gradients
are approximated by hit distance × cone spread (the existing RT passes already
carry enough for a serviceable LOD estimate; exact ray differentials are not
required for ground texture LOD). This keeps GI bounce color, reflections,
and shadow tinting consistent with the GBuffer.

## Phase 2 — Parallax occlusion mapping

### World-space march (the seam-critical decision)

Classic POM marches in tangent-space UV and would walk off the current Wang
cell into the wrong tile. Instead the march runs in **world XZ**, re-resolving
cell → layer → UV per step via the same Wang lookup used for flat sampling.
Crossing a cell boundary mid-march therefore samples the true runtime
neighbor, whose edge strip is byte-equal — the march is seam-transparent by
construction.

Frame/datum: the fragment's interpolated triangle plane is the height datum
(plane point = fragment world position, plane normal = interpolated normal).
Because the bake is a planar top-down projection, the tangent frame is
world-aligned (`T = +X`, `B = +Z`) and the baked height is single-valued by
construction (no overhangs) — the ideal POM input. The view ray is stepped in
world space; at each step the displaced surface height is
`plane_height(p.xz) + decode_height(p.xz)` (height decoded via the slot's
`height_min/max`), compared against the ray's height above the plane. Sloped
terrain inherits the same planar-projection stretch already accepted for flat
sampling.

March parameters (in `TilesetParams`, tunable): `pomSteps` 24 linear steps +
4 binary-refine steps, `pomMaxDistance` 25 m with a fade band over the last
5 m (step count also scales down with distance). Early-out on first crossing.
Beyond the fade band the flat Phase-1 sample is used unchanged — "parallax
vs. ray-marched" is thus a quality knob (step count / refine count), not two
implementations.

### Depth write

The fragment writes the marched intersection's depth so the RT passes —
which reconstruct positions from the GBuffer — see the displaced surface:
contact shadows land in crevices, reflections anchor correctly. Parallax only
ever pushes the surface **away** from the camera, so conservative depth keeps
early/hierarchical Z: under the reversed-Z convention established in Phase 0
(far→0), pushed-away means a *smaller* depth value, so the declaration is
`layout(depth_less) out float gl_FragDepth`. Velocity output is left at the
un-displaced surface's motion (sub-texel error under camera motion; accepted,
revisit only if TAA shows ground smear).

### Height self-shadow

After the primary march finds the displaced point, a short secondary march
toward the sun (8 steps, same world-space Wang resolution, capped at
`edgeStripWidth`-scale distance ≈ 0.3 m) produces a soft occlusion factor
multiplied into the GBuffer AO channel. The cap mirrors the bake-time AO
rationale: near-seam results stay arrangement-independent, and short-range is
perceptually right for ground litter. This is what makes sun-lit litter stop
reading as flat.

### What secondary rays see

RT hit-shader sampling (Phase 1) stays flat — a secondary ray hitting terrain
shades with un-parallaxed channels at the geometric hit point. Combined with
the depth-written primary surface this is visually consistent for rough
ground; true heightfield intersection for secondary rays is explicitly
deferred until something visibly disagrees (mirror-like wet ground would be
the trigger).

## Phase 3 — Macro tileset (frequency split)

### Authoring

A macro layer is just another `Tileset` root with coarser parameters —
authored, baked, cached, and loaded by the entirely unchanged v1 pipeline:

```js
export default class ForestFloorMacro extends Tileset {
  build() {
    this.tile({ size: 16.0, texelsPerMeter: 32, seed: 77 });   // 2048² atlas
    this.base((x, z) => broadMottling(x, z), MAT.dirt);
    // large-scale features: damp patches, leaf drifts, moss tint regions —
    // authored with tint patches / broad heightfield features rather than
    // individual litter
  }
}
```

Manifest gains a second `[tileset]` entry; the world loader assigns it a slot
and binds it to the terrain material's `groundMacroSlot`. The 4-slot budget
holds two full ground types (detail + macro each); raising `kMaxSlots` is
trivial post-port since the descriptor array is sized by a constant.

### Compositing

The macro layer is sampled at **all** distances and composited in the GBuffer
shader (RT hit shaders use the same function), with a detail weight
`w = detailFade(distance)` (1 near → 0 past the detail fade distance,
default fade centered ~40 m):

```
macroRatio = macroAlbedo / max(meanAlbedo(macroSlot), 0.02)   // clamped ≈ [0.25, 4]
albedo     = mix(macroAlbedo, detailAlbedo * macroRatio, w)
normal_ts  = normalize(mix(macroN, vec3(detailN.xy + macroN.xy, detailN.z), w))  // RNM-style add
occlusion  = mix(macroAO, detailAO * macroAO, w)
roughness  = mix(macroRough, detailRough, w)
metallic   = detail (w > 0) else macro                          // ground: effectively 0
```

Near field: the ratio term modulates detail albedo by macro's deviation from
its own mean — damp/mossy/dry patches make repeated detail tiles read as
different ground, attacking tiling repetition directly. Far field: `w = 0`
leaves pure macro with its own healthy per-tile mips; the coarsest-mip
per-tile average patchwork of the detail layer is gone before it becomes
visible, and no "global average color" fallback exists in the design.

Parallax and self-shadow apply to the detail height only and fade with `w`;
macro height is baked but unused (future: feed the terrain mesher as
displacement — out of scope).

## Error handling

Fail-closed, matching existing conventions:

- `.gtex` load / slice / upload failure → material renders untextured (slot
  cleared, `flags_misc` stays 0) + console warning; never a crash. Mirrors
  the GL provider contract.
- Slot out of range or macro slot referencing an empty slot → treated as −1
  at material-table pack time, warning once.
- Device lacks required limits (16-entry sampler-array descriptor count,
  anisotropy) → anisotropy degrades to device max; descriptor count 16 is far
  below any real minimum (`maxPerStageDescriptorSamplers` ≥ 16 everywhere the
  RT pipeline can run).

## Testing

- **Reversed-Z (Phase 0):** pre/post image parity on the existing viewer shot
  suite; software-reference parity tests still pass; the new ~2 km coplanar
  z-fight shot is clean; DLSS path visually verified with the inverted-depth
  flag.
- **Slice/mips (CPU):** atlas → 16 layers slicing is exact (byte-compare
  layer 0 against source rect); per-layer mip generation preserves the edge
  invariant at mip 0 (border rows byte-equal across color-matched layers).
- **Seam invariant (GPU, gpu_tests-style):** render a synthetic two-cell
  boundary at several mip-forcing distances; assert no texel row differs
  across the boundary beyond filtering epsilon — the test v1 lacked
  (v1 only tested the bake-side strips, not runtime filtering).
- **POM:** flat-plane fixture with a known step-function height: marched hit
  position matches analytic expectation within a step; depth-write value
  matches the marched point; march across a cell boundary is continuous
  (regression for the world-space-march rationale).
- **Compositing:** `w = 0` equals pure macro sample; `w = 1`, uniform macro
  (macro == its mean) equals pure detail — identity checks.
- **Integration:** Meadow with `ForestFloor` + `ForestFloorMacro`; scripted
  viewer shots along the v1 seam-heavy camera path at near/mid/far distances
  plus a grazing-angle sunset shot (self-shadow visible); Windows binary
  rebuilt (`make windows`).

## Implementation phases

0. **Reversed-Z migration:** projection flip, clear/compare ops, shader
   audit (volumetrics linearization, RT depth reconstruction, sky pass),
   Streamline depth-inverted flag, software-path parity, distant z-fight
   regression shot. *Ship: identical images with far-field z-fighting gone;
   all later depth-dependent work authored against the final convention.*
1. **Vulkan tileset consumption:** slicing loader + per-layer mips + sampler,
   descriptor bindings, `tileset_common.glsl`, `gbuffer.frag` flat sampling,
   `MaterialDef` schema v4 + `flags_misc` packing, RT hit-shader flat
   sampling, seam-invariant GPU test, Meadow shots. *Ship: textured ground in
   the Vulkan viewer, seams gone.*
2. **Parallax:** world-space POM + conservative depth write + height
   self-shadow + distance fade, POM tests, sunset shot. *Ship: ground reads
   as 3D.*
3. **Macro layer:** `ForestFloorMacro` authoring, manifest/loader/material
   binding for the second slot, frequency-split compositing, compositing
   tests, far-distance shots. *Ship: no far-field tiling, near-field
   repetition broken.*

## Future work (out of scope)

- Macro height as terrain-mesher displacement input (moves 16 m-wavelength
  relief from texture into geometry, where it belongs).
- True heightfield intersection for secondary rays (wet-ground trigger).
- Additional tileset slots / additional ground types per world; slope-aware
  or triplanar projection for steep terrain.
- Tileset interaction with the voxel-box impostor tier (shared far-field
  strategy for scattered mid-size props).
