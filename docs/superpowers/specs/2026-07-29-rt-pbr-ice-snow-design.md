# RT PBR: Rough Transmission, Secondary Tint, Glints & Snow Overlay — Design

**Date:** 2026-07-29
**Status:** Draft for review
**Companion to:** 2026-07-29-chart-virtual-texturing-design.md (independent
code paths; composes visually — VT pages carry masks/weights, everything here
is view-dependent and stays live in shaders).

## Summary

Close the gap between "the refraction machinery works" and "ice and snow look
incredible" in the Vulkan hybrid RT pipeline. Four phases:
**(1)** microfacet (frosted) transmission — roughness-driven perturbation of
the existing refraction walk, with a proper auxiliary lane for the denoiser,
plus two correctness fixes in the current transmission path,
**(2)** transmissive tint on secondary rays — glass/ice/water seen in
reflections and GI stops rendering as opaque and instead tints what lies
behind it, **(3)** stochastic sun glints — the view-dependent sparkle that
makes ray-traced snow and frost read as crystalline, **(4)** snow/ice
material authoring plus the **world-space dusting overlay** — the runtime
mechanism (deferred out of the chart-VT spec) that puts altitude/orientation
driven snow on *shared* part variants whose baked pages cannot carry
world-dependent data.

What this spec deliberately builds on rather than replaces: `rt_lighting.rgen`
already traces a real refraction walk (entry refraction, up to 4 internal
events with total internal reflection, exit refraction, Beer–Lambert
absorption in transmittance-per-distance form, `:447-551`), the specular path
is a proper two-lobe VNDF-sampled Cook–Torrance with clearcoat (`:369-445`),
and colored shadows through transmissive media already work
(`rt_visibility.rahit:41-55`). Ice is *almost* authoring already; this spec
adds the four things authoring cannot reach.

## Background: what is live, what is broken, what is missing

Live and reusable: `transmission`, `ior`, `absorptionColor`/`absorptionDistance`
(real Beer–Lambert), `thickness` fallback path length, `clearcoat` +
`clearcoatRoughness` second lobe, `specularStrength`/`specularTint`,
`subsurface` + `scatteringColor` thin-walled/backlit approximation in
`composite.frag:158-174`, the VNDF sampler `sample_ggx_vndf`
(`rt_lighting.rgen:95-111`), and the two-mask TLAS layering (opaque 0x01 /
non-opaque 0x02) that gives shadows their tint pass.

Broken or fragile today:

- **The refraction walk is smooth-only.** `refract()` uses the geometric
  normal directly; `roughness` never perturbs transmitted rays. Frosted ice,
  rime, and rough lake ice are unrepresentable.
- **Transmission exists only for primary G-buffer pixels** (`identity.x`
  gate). Every secondary hit shades via `hit_radiance()`
  (`rt_lighting.rgen:172-187`), which treats all surfaces as opaque
  Lambert+sky: glass in a mirror is opaque, a frozen lake reflected in
  another surface is opaque, GI through a window is blocked.
- **`composite.frag:213` lacks the legacy-black guard** that
  `rt_lighting.rgen:535-537` has: a transmissive material with zeroed
  `absorptionColor` that takes the non-RT fallback branch
  (`transmission_coverage < 0.01`) multiplies to black — the historical
  "black glass" failure, still reachable.
- **The walk traces `gl_RayFlagsOpaqueEXT`**, so alpha-tested foliage
  occludes refraction as a solid silhouette.
- **Snow (mat 17) is plain diffuse** (0.90, 0.90, 0.95 @ roughness 0.8): no
  sparkle, no grazing-light glow, no authored ice material at all. The sun
  term in `composite.frag:220` has no specular lobe (`sun * mix(1.0, 0.65,
  roughness)`), so there is nowhere for a sun highlight — let alone sparkle —
  to come from on rough surfaces outside the RT reflection lane.
- **No mechanism for world-driven appearance on shared variants.** Chart-VT
  pages are per-variant and part-local by design; "snow on every rock above
  2,200 m" cannot bake. The chart-VT spec explicitly routes this here.

Explicitly dead fields that stay dead (see Non-Goals): `translucency` (never
packed for RT; CPU meshing-carve only), `MATERIAL_VOLUME_BOUNDARY` (set on 4
materials, tested by nobody), `anisotropy` (backlit exponent only).

## Goals

- Frosted/rough transmission with variance the temporal denoiser can handle;
  smooth glass renders byte-comparably to today.
- Transmissive surfaces tint (not block) reflections and GI, cheaply and
  without recursion.
- Sun glints on materials that opt in: view-dependent, world-stable,
  temporally sane under TAA/DLSS, energy-bounded.
- Snow reads as snow: sparkle in sun, soft glow at grazing light, correct
  high-albedo GI behavior (no firefly regression).
- A `GlacialIce` material and a snow revision, authorable via
  `defineMaterial` (chart-VT Phase 3) or as registry entries if this spec
  lands first — no ordering dependency between the two specs.
- World-space dusting overlay: deterministic, applied identically in the
  G-buffer and RT hit paths, driven by authored world parameters (snow line,
  band width, noise), zero per-variant bake cost.
- Fix the two transmission correctness traps (composite fallback guard,
  alpha-test occlusion) or explicitly bound them.

## Non-Goals

- Volumetric coupling of `MATERIAL_VOLUME_BOUNDARY` media into the froxel
  grid (deep-snow SSS as participating media, fog entering ice). The froxel
  stack stays fog+emitters only.
- Wiring `translucency` into the RT record. It remains a CPU meshing
  concept; documenting that is this spec's only action on it.
- An anisotropic base BRDF (brushed metal, hair). `anisotropy` keeps its
  current backlit-exponent meaning.
- Refraction *bending* on secondary rays (Phase 2 is straight-through tint;
  bent secondary refraction is future work with a real recursion budget).
- Caustics (photon/path-guided). Sun-through-ice light patterns are out.
- Nested dielectrics (ice under water). Single-medium walks only, as today.
- Dispersion, birefringence, sparkle-from-reflection-lane (glints are a sun
  term only in this spec).
- Water animation/foam/waves — water benefits incidentally from Phases 1–2
  but gets no dedicated features.

## Phase 1 — Microfacet transmission + correctness fixes

### Rough refraction

Perturb the walk at the two boundary events that dominate appearance, keep
the interior deterministic (bounded noise, bounded cost):

- **Entry:** sample a half-vector from `sample_ggx_vndf` around the shading
  normal at the material's `roughness` (reuse the existing sampler and its
  spec-constant test counters), refract the view ray through the *sampled*
  half-vector instead of the geometric normal. A sampled direction that
  fails `refract()` (grazing microfacet TIR) falls back to the geometric
  normal's result — bias accepted over a re-sample loop.
- **Exit:** same perturbation on the final exit refraction using the exit
  surface's roughness.
- Internal TIR bounces and the walk structure (4 events, front-face
  termination, `thickness` fallback) are unchanged. Smooth materials
  (roughness < 0.02) skip sampling entirely — bit-compatibility gate for
  existing glass.

### Denoiser lane

`raw_transmission_image` gains a sibling aux target mirroring the specular
lane's contract (`raw_specular_aux`): `(hit_t, roughness)` per pixel, so the
temporal accumulation/filter treats rough transmission like rough reflection
(wider spatial kernel, longer history at high roughness, hit-distance-driven
disocclusion). One sample per pixel per frame, same budget as today; a
firefly clamp on the transmitted radiance matches the GI lane's.

### Correctness fixes

- **Composite fallback guard:** port the legacy-black guard
  (`dot(c,c) < 1e-8 → vec3(1)`) to `composite.frag:213`. One-liner; ends the
  reachable black-glass fallback.
- **Alpha-tested occluders in the walk:** switch walk rays from
  `OpaqueEXT` to the two-mask pattern already used by shadow rays (trace
  0x01 opaque + terminate-first, re-trace 0x02 with any-hit for the
  alpha-test). Cost is bounded by the same argument as shadows. If measured
  cost on foliage-heavy scenes exceeds ~5% of the RT budget, retreat to
  `OpaqueEXT` + document; the fix is behind a spec constant either way.

## Phase 2 — Transmissive tint on secondary rays

`hit_radiance()` and `hit_radiance_sunlit()` gain a bounded continuation:
when the hit material's `transmission > 0.5` (binary gate, no partial-cover
blending on secondary hits):

1. Attenuate throughput by the colored-shadow formula already authored in
   `rt_visibility.rahit`: `albedo * transmission`, plus Beer–Lambert using
   `thickness` as the path length (no interior walk on secondary rays).
2. Continue the ray **straight through** (offset origin past the surface,
   direction unchanged — no bend), at most **2 continuations** per secondary
   ray, then treat the third transmissive hit as opaque.

Approximation is explicit and accepted: a peak reflected in a frozen lake is
correct (the reflection ray hits the peak, not the ice — this phase matters
for ice *edges*, glass props, and GI through transmissive surfaces, which
today go black/opaque). Straight-through means no refraction distortion in
reflections; visually minor for thin ice and window glass, revisit with a
real budget if hero content demands bent secondaries (Future work).

Applies uniformly to GI rays, reflection rays, and the transmission walk's
own radiance lookups — one shared helper, so all lanes agree.

## Phase 3 — Stochastic sun glints

### Model

Procedural discrete-microfacet sparkle in the **direct sun term**, evaluated
where N·L and the traced sun shadow/horizon factors are already known
(`composite.frag` sun lane; the RT hit path applies the same function for
primary-visible pixels only — glints in reflections are a Non-Goal):

- A 3-scale world-space hash grid (cell sizes ~2 mm / 6 mm / 18 mm, scaled
  by `glint_size`) assigns each cell a random microfacet normal in a cone
  about the surface normal.
- A cell fires when its normal aligns with the sun half-vector within a
  threshold; a fired cell contributes an energy-compensated spike
  (contribution ∝ 1 / firing probability, clamped) times the sun radiance,
  shadow, and horizon terms.
- Footprint-aware fade: as the pixel footprint (from existing derivative
  data) covers many cells, the term converges to a small constant added
  gloss and finally to zero — glints are a near/mid-field feature by
  construction, no LOD popping.
- Temporal stability: cells are world-anchored (stable under camera motion;
  sparkle *changes* with view angle, which is the desired phenomenon, not
  noise). Under TAA/DLSS the spike is pre-clamped (`glint_max` × sun) so
  history rejection doesn't strobe. No stochastic per-frame jitter — the
  hash is time-invariant.

### Material plumbing

One new surface flag `MATERIAL_GLINT` plus two packed params — density and
intensity — in the spare `absorption_pad.a` and a `scattering_shape` reuse
audit (final lane assignment at implementation; the GPU record stays 9
vec4s, no schema growth). Authored via `defineMaterial` or registry entries:
snow gets `glint density high / intensity moderate`; frost and glacial ice
surfaces get sparser, brighter settings.

### Snow's diffuse revision (rides this phase)

- Grazing-light glow: enable the existing wrapped-diffuse/backlit subsurface
  path for snow (`subsurface` ≈ 0.3, `scatteringColor` slightly blue) —
  parameter authoring plus verifying the non-thin-walled branch in
  `composite.frag:158-174` behaves at terrain scale.
- High-albedo GI check: 0.9+ albedo multi-bounce is the classic firefly
  amplifier; verify the existing GI firefly clamp holds on a snowfield
  fixture, tighten the clamp constant only if the fixture shows it.

## Phase 4 — Materials + world-space dusting overlay

### Materials

- **`GlacialIce`**: `transmission 0.9`, `ior 1.31`, `roughness 0.05–0.25`
  (authored per surface), `absorptionColor` pale cyan @ `absorptionDistance`
  2–4 m (deep-blue thickness falloff via the existing Beer–Lambert),
  `clearcoat 0.3` for melt glaze, `MATERIAL_GLINT` sparse. Renders through
  Phases 1–2 machinery; the material itself is a table entry.
- **Snow revision** per Phase 3. Both land as `defineMaterial` declarations
  when chart-VT Phase 3 exists, else as registry entries 30+ (the specs stay
  order-independent).

### Dusting overlay

The runtime answer to world-driven appearance on shared variants — the piece
the chart-VT sharing rule cannot bake:

- **Authoring:** world statics gain
  `dusting: { material, altitude, band, noiseScale, slopeFalloff }` —
  e.g. snow above 2,200 m fading in over a 150 m band, coverage falling off
  as the surface turns from up-facing to vertical, broken by world-space FBM
  so the snow line is ragged, not a contour line.
- **Evaluation:** one shared GLSL function
  `dusting_coverage(world_pos, world_normal)` → [0,1], evaluated in
  `gbuffer.frag` *and* the RT hit surface loader (`rt_surface_common`) so
  primary pixels, GI bounces, and reflections agree — same discipline as
  tileset sampling today. Coverage blends albedo/roughness/ORM toward the
  dusting material (and flattens the detail normal toward the geometric
  normal — snow softens relief), *before* the G-buffer write, so every
  downstream pass (sun, GI, glints — dusted texels naturally inherit
  `MATERIAL_GLINT` behavior through the blended material params) is
  consistent for free.
- **Interaction with VT/tilesets:** overlay applies after VT/detail
  sampling; on world-anchored variants (terrain sectors) whose pages already
  bake snow via the `surfaces()` tape, the tape and overlay must not
  double-apply — the tape's snow weight suppresses the overlay
  (`coverage *= 1 - baked_snow_weight`, aux channel already carries it).
- Deterministic, parameter-driven, zero bake cost, and applies to every
  rock, tree, and building the moment the world sets a snow line.

## Error handling

- All four phases behind spec constants / material opt-ins; a material with
  no new fields set renders byte-identically to today (regression gate).
- Glint energy hard-clamped (`glint_max`); a pathological density/intensity
  authoring cannot exceed sun radiance × clamp.
- Secondary-tint continuation count is a hard bound; hitting it degrades to
  opaque (today's behavior), never loops.
- Dusting with an invalid material index → coverage 0, warn once.
- Rough transmission with `transmission > 0` but `ior < 1` → ior clamped to
  1.0002, warn once (degenerate refract guard).

## Testing

- **Smooth-glass regression:** existing glass/greenGlass fixtures byte-stable
  with all features compiled in but unauthored.
- **Rough transmission:** frosted-slab fixture over a checker — blur grows
  monotonically with roughness; energy within 2% of the smooth slab
  (integration sanity); denoiser lane shows bounded temporal variance on an
  orbit path.
- **Fallback guard:** zeroed-absorption transmissive material through the
  `coverage < 0.01` branch is non-black (the black-glass regression test
  that never existed).
- **Secondary tint:** mirror + glass-slab fixture — the slab in the mirror
  matches direct view within tint tolerance; GI room-through-window fixture
  brightens vs. today's opaque baseline.
- **Glints:** sparkle fixture at fixed sun — world-anchored (camera translate
  → glints track surface points), view-dependent (orbit → population
  changes), footprint fade (walk-away → converges, no shimmer band), TAA
  stability (variance bound over 120 static frames), energy clamp honored.
- **Dusting:** determinism (same world params → identical G-buffer),
  gbuffer/RT agreement test (secondary hit of a dusted rock matches primary
  shading within epsilon), tape-suppression identity on a terrain sector
  with baked snow weight 1.
- **Integration:** frozen-tarn fixture world (ice sheet on terrain,
  snowfield, scattered rocks, low sun) — sunset sparkle shot, tarn
  reflection shot, grazing-glow shot; StreamMountain smoke with a snow line
  set (dusting only — full mountain material work remains the later
  authoring effort). Windows binary rebuilt per phase.

## Implementation phases

1. **Microfacet transmission + fixes:** VNDF entry/exit perturbation,
   transmission aux lane + denoiser plumbing, composite fallback guard,
   two-mask walk rays. *Ship: frosted ice possible; black-glass fallback
   dead; foliage no longer silhouettes refraction.*
2. **Secondary tint:** shared attenuate-and-continue helper in the
   hit-radiance path, continuation bound. *Ship: glass and ice participate
   in reflections and GI.*
3. **Glints + snow diffuse:** hash-grid glint term in the sun lane, material
   flag/params, snow subsurface authoring, firefly verification.
   *Ship: snow sparkles and glows; the signature alpine sun shot works.*
4. **Materials + dusting overlay:** `GlacialIce`, snow revision, world
   dusting params + shared coverage function + tape suppression.
   *Ship: set a snow line and the whole world wears it consistently.*

## Future work (out of scope)

- Bent refraction on secondary rays with a real recursion budget (hero
  ice/glass in mirrors).
- Glints in the RT reflection lane (sparkle visible in the frozen tarn's
  reflection of a snowfield).
- Volumetric interiors for `VOLUME_BOUNDARY` media (god rays inside ice,
  deep-snow SSS as participating media).
- Caustics; nested dielectrics; dispersion.
- Specular sun lobe for the general (non-glint) rough direct term in
  `composite.frag` — the current `mix(1.0, 0.65, roughness)` diffuse-only
  sun lane predates this spec and deserves a proper GGX term of its own.
- Weather dynamics: time-varying snow line / accumulation driving the
  dusting parameters.
