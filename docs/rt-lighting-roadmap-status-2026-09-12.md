# Status of the original RT/GI optimization roadmap

Audited 2026-09-12 against the current `castle/astra-assembly` source and the
[implementation/acceptance record](rt-lighting-implementation-2026-09-12.md).
The original list is the eight ordered items in the
[September 11 review](rt-gi-lighting-performance-review-2026-09-11.md#recommendation).
Updated later the same day after the
[GI reconstruction work](gi-reconstruction-2026-09-12.md). The historical
measurements below explain the original priorities; use the updated remaining
implementation order for the preferred reduced-resolution GI configuration.
The subsequent [material and adaptive-shadow implementation](lighting-quality-and-adaptive-shadows-2026-09-12.md)
adds output-aware DLSS material sampling, experimental adaptive shadow counts and
the previously missing HDR/split-dispatch timing zones. The extra timers are
opt-in; adaptive is slower than fixed four samples in the measured DLSS Quality
view and must not be treated as an accepted universal performance improvement.
The subsequent [primary light culling work](primary-light-culling-2026-09-12.md)
adds conservative depth-derived tile lists for raster and primary RT receivers.
Primary direct GPU cost falls about 6% in the castle hall; DLSS Quality FPS
improves 2.6–5.1% in two comparisons, while Native frame cadence is mixed.
Culling remains opt-in globally and is enabled in the live Quality launcher.

The first milestone substantially reduced repeated ray work; it did not finish
the proposed lighting architecture. The final matched hall run improved from
211.203ms to 42.541ms per frame (4.735 to 23.507 FPS). Its raw GPU medians were
42.031ms total, 34.194ms GI, 3.437ms primary local direct and 1.464ms denoising.
GI was approximately 81% of total GPU time in that historical full-GI view.
That is no longer the bottleneck at the preferred diffuse scale of 0.128 with
full internal-resolution reflections and guided reconstruction. The later
1280×720 hall measurement was 9.621ms / 103.94 FPS: raw GPU total 9.337ms,
primary local direct 4.433ms (47%), combined GI 1.294ms (14%), and denoise
0.579ms. Evidence: `C:/tmp/castle-gi-reconstruct/perf-after/perf.json`.

## Original list, item by item

| Original item | Current status | What remains |
|---|---|---|
| 1. Measure primary local RT and controlled ablations | Primary, HDR lighting and split-dispatch measurements implemented | Split resolutions expose diffuse and combined reflection/transmission timers while retaining aggregate GI. Equal resolutions still share one dispatch. Individual reflection/transmission costs, remaining ablations, ray/candidate distributions and GPU register/spill profiling remain. |
| 2. Remove avoidable work | Main fixes implemented | Diagnostic atomics are off normally, exactly-zero lanes are skipped, unused GI normal reconstruction is removed, and effective light publication is cached. Smaller visibility material loads remain unaudited as an optimization; castle-specific radius/cell policy is still hardcoded by world name. |
| 3. Reduce primary area shadows from four samples to one | Fixed and adaptive modes implemented | Four remains default. Adaptive 1/4 uses mature presented history, prior variance and strict geometry/material/motion checks; cuts, scene changes and untrusted pixels retain four. Visible smoke tests and a short castle camera-pan sequence pass. Broader continuous-motion and unequal-light penumbra acceptance remain before changing the default. |
| 4. Bounded stochastic light sampling | Partially implemented; secondary path enabled | Diffuse/reflection hits sample one shadow light with probability weighting. Primary direct and transmission still evaluate/shadow every contributing light at their existing sample rates. All secondary candidate BRDFs are still scored. No spatial/temporal reservoir reuse, visibility guiding, proposal hierarchy or distribution cache exists. |
| 5. Cheaper secondary materials and footprint-aware POM | Experimental controls implemented; broad policy unfinished | Reference/adaptive/no-march modes exist. Adaptive POM did not improve the tested hall and remains off. No distinct cheap diffuse/rough-reflection hit policy or sharp-reflection budget, no deferred shading of only the selected transmission hit, and the identified texture-coordinate/mip-query cleanup remains. |
| 6. Runtime diffuse-light cache and transport ownership | Not implemented | Choose and implement one room-scale probe or instance-aware surface cache with progressive updates, directional HDR lighting, visibility/leak protection, confidence and dirty-region invalidation. Resolve sky/sun/diffuse energy ownership first. Material atlases and temporal histories are not this cache. |
| 7. Classify reflection/glass and denoise useful work only | Independent rates and guided diffuse reconstruction implemented | Diffuse GI now has a separate trace scale from reflection/glass; reduced diffuse stores incident lighting and applies material response at the raster sample. Rough-reflection fallback, compact ray/active-tile lists, selective spatial filters and truthful supported quality settings remain. Screen-space reflection reuse is also still a proposal. |
| 8. Screen-space light clusters, geometry and host publication | Primary screen/depth tile lists implemented; geometry/host work remains | Conservative 16×16 depth-derived light lists serve primary raster/RT and preserve world lists for off-screen hits. Actual-list audits, resize/light-revision/fallback tests and visible benchmarks pass. Culling remains opt-in; broader views/hardware, tighter depth clustering and larger-light-set construction remain to evaluate. Static fast-trace TLAS experiment, lower CPU part/instance publication cost, regional history generations, dedicated conservative RT proxies, and measured payload reduction remain. Static TLAS reuse already existed and must not be counted as new work. |

## Specific unfinished contracts confirmed in source

- [`set_gi_settings`](../MatterEngine3/src/render/vk_scene_renderer.cpp:2572)
  still forces `max_bounces` and `samples_per_pixel` to one. The shader still
  has a fixed two-vertex diffuse walk. Make supported controls truthful by
  implementing their meaning or removing/relabeling unsupported choices.
- [Spatial scheduling](../MatterEngine3/src/render/vk_scene_renderer.cpp:11717)
  still runs fixed 5 diffuse / 3 reflection / 3 transmission iterations, plus
  one primary-direct iteration. The advertised `denoiser_iterations = 0`
  contract is still not honored.
- [The sampler](../MatterEngine3/shaders_vk/rt_lighting_impl.glsl:565) scans every
  candidate. Its [dispatch guard](../MatterEngine3/shaders_vk/rt_lighting_impl.glsl:640)
  applies only to GI secondary calls; it is not a complete many-light solution
  across all paths. The existing diffuse/reflection luminance caps and temporal
  clipping can bias rare high-weight samples. Raw-estimator tests passed, but
  final linear-HDR energy under these nonlinear stages still needs evaluation.
- [Finished-surface sampling](../MatterEngine3/shaders_vk/surface_detail.glsl:22)
  repeats Wang coordinate resolution and mip-count queries for final material
  channels. Tiny triplanar weights still use the `1e-5` threshold. The
  [RT footprint bound](../MatterEngine3/shaders_vk/rt_surface_common.glsl:385)
  still uses a Frobenius norm. These are candidates to profile, not proven
  frame-rate wins.
- [Primary ambient](../MatterEngine3/shaders_vk/composite.frag:308) and the
  [secondary hit model](../MatterEngine3/shaders_vk/rt_lighting_impl.glsl:765) retain
  the prior sky approximations. Reflections use hit lighting without the
  explicit sun query that transmission uses. The original potential sky/sun
  overlap and diffuse-continuation energy concerns remain unresolved; caching
  the present approximation would preserve those inconsistencies.
- [TLAS policy](../MatterEngine3/src/render/vk_scene_renderer.cpp:15229) still
  prefers fast build, while BLAS prefers fast trace. The
  [castle radius/cell policy](../MatterEngine3/src/matter_engine.cpp:13255)
  remains tied to `CastleUpgraded` rather than a generic authored setting.

## Recommended remaining implementation order

The [frame-pacing investigation](frame-pacing-2026-09-12.md) separately removes
repeatable acquisition stalls in the measured configuration and adds an
optional live FPS limiter. A 90 FPS Quality run reduces interval standard
deviation from 6.25 to 0.38 ms without a lighting-quality change. This is CPU
present cadence on a 32 Hz Remote Desktop session, not proof of displayed
90 Hz motion. Local-display acceptance and remaining CPU outliers are separate
from the shader/lighting priorities below.

1. **Keep adaptive shadows experimental.** The new optional policy
   preserves four samples for untrusted/noisy pixels and chooses one for stable
   history, but is slower with DLSS Quality in the hall. Improve its cost and
   confidence model before broader continuous doorway walkthroughs, moving
   lights/occluders and unequal-light penumbra acceptance. Keep four as default;
   primary candidate culling and static shadow caching can proceed independently.
2. **Use the new timing boundaries to select further work.** `hdr_lighting`
   measures the HDR lighting/reconstruction draw; the split-resolution GI
   dispatches have separate diffuse and combined reflection/transmission zones.
   The legacy `composite` zone remains the final display transform. Do not sum
   GI children with the aggregate or treat absent children as zero-cost work.
3. **Broaden primary light-list acceptance.** Screen/depth tile lists now remove
   zero-support candidate work and keep world lists for off-screen hits. The
   tile producer costs about 0.025 ms at DLSS Quality in the hall. Test exterior
   and doorway views and other hardware before changing the global default.
   Contributing-light shadow rays are unchanged; candidate rejection must not
   be presented as an equivalent ray-count reduction. Primary importance
   sampling and temporal/spatial reuse remain larger, quality-sensitive options.
4. **Prototype cached static local shadows if direct lighting still dominates.**
   Castle lights and most occluders are static. Bound memory and update work,
   invalidate on light/geometry revisions, and retain dynamic and transparent
   visibility handling. A room-scale diffuse cache remains available later,
   but replacing a 1.3ms combined GI pass is now less urgent than direct shadows.
5. **Specialize secondary work only where profiling justifies it.** Independent
   rates currently share a compiled raygen with dispatch masks. Test separate
   compiled programs, cheaper diffuse/rough-reflection materials and selected
   transmission-hit shading. Preserve gold/glass; adaptive POM previously did
   not improve the hall. Audit texture fetches and registers rather than
   assuming these changes will help.
6. **Make denoising and coarse GI scheduling selective.** Honor iteration
   controls and avoid unused/converged work with explicit coverage and history
   invalidation. Reduced diffuse must still provide incident lighting for
   neighboring pixels, including when its representative surface is dark or
   metallic. Do not restore material-zero early-outs that make holes in this
   lighting field. Fixed 5/3/3 spatial iterations remain a smaller measured cost.

DLSS Super Resolution is a complementary way to reduce internal raster and RT
work. The native MSVC build now supports an explicit Streamline option; see
the [DLSS build and next-steps record](dlss-build-and-next-optimizations-2026-09-12.md)
for runtime verification and the matched comparison.

## Acceptance still owed by the broader roadmap

The existing native tests, static images and short camera-pan comparisons are
useful and passed. They do not cover the original complete acceptance matrix:
matched courtyard/exterior and continuous doorway walkthroughs; moving lights
and occluders; isolated per-lane cost; linear-HDR energy after clipping and
denoising; roughness transitions; and, once a cache exists, convergence, age,
leakage, invalidation and recovery after door/light/material changes.

Optional original research proposals remain unimplemented: cached static local
shadow maps, room/portal scheduling, distant chandelier light grouping,
reusable directional transfer for kit parts, room lighting bases and coarse
voxel assistance. These are alternatives to evaluate when useful, not a
requirement to build several overlapping lighting systems.
