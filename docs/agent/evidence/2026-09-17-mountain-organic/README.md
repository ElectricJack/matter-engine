# StreamMountain organic ground development

Status: candidate v3 retained as an incremental material-development checkpoint.
Native checks and the nineteen-image scene audit pass; final visual acceptance
and the full goal remain open.
Previous goal turn made progress: explicit receiver binding, its native proof,
and the eager initialization fix are retained and validated. This pass returns
to the requested mountain material appearance; the full goal remains active.

Candidate v1 reuses the existing two cellular neighborhoods and noise fields.
It varies stone crown shape, reduces fine outline chipping and pale stone
contrast, and introduces shallow soil/clod and moss relief with height-aware
burial. Regional weather breaks up stone presence and moss coverage. This adds
no source atlas, source geometry, physics settling or shader/API changes.

`mountain_surface-before.js` is the exact retained recipe from the preceding
world-receiver binding checkpoint. Fixed-camera baseline images are
`../2026-09-17-mountain-layering/v3`. Terrain remains 64 texels/m and declared
height remains [-0.090, 0] metres. POM quality is unchanged.

## Native iteration before visual capture

`mountain-organic-v1` passes actual JS/native source loading and GPU packing:
398 operations, two cellular searches/three reuse reads, sampled height
[-0.069257, -0.023170] m. Its near/far mean height is -0.047894 / -0.050000 m.
The added soil shifts the mean by 2.106 mm, so it is not the reviewed candidate.

Candidate v2 clamps soil thickness nonnegative and accounts for its expected
mean in the base datum without scaling relief. Native checks pass: 402 ops,
height [-0.070217, -0.024211] m; near/far mean -0.049401 / -0.050000 m.
Red albedo is 0.095552 / 0.095388. The mean reference is a statistical
approximation checked in one patch, not a general nonlinear filtering proof.

Candidate v3 additionally separates centimetre chip/clod filtering from the
larger stone-body fade: footprint 0.016–0.09 m versus 0.025–0.28 m. This keeps
fine shapes from persisting after they become unresolved. No resolution, height
envelope or POM settings change. Native checks and actual capture results follow.

### Independent unclamped height bounds

Clod detail lies in [-0.48, 0.52], body profile in [0, 1.05], and all footprint
fades in [0,1]. Soil thickness before footprint/relief attenuation is bounded
by [0, 0.00908] m after its explicit nonnegative thickness operation. Moss is
above that soil by at most 0.00356 m (its loose bound permits independently
varying fades).

The old base/stone lower bound -0.079915625 m widens by at most 0.003 m for
soil mean centering, giving -0.082915625 m. Soil and stones add nonnegative
height. With separate fades the moss addition can be as low as -0.00144 m,
giving the conservative complete lower bound -0.084355625 m. The previous
stone upper bound -0.0058125 m remains valid, since its new crown/profile and
mean-centering terms only lower that envelope. A loose independent bound on
base + soil + moss is -0.05 + 0.0033 + 0.00955*0.8125 + 0.00908 + 0.00356
= -0.026300625 m. All `s.layer` results are convex height blends.
Thus no height clamp is needed to hide overshoot of the unchanged [-0.090,0] m
source envelope. These bounds concern the authored displacement, not silhouette
or cross-LOD acceptance.


Candidate v3's native check passes with 404 packed GPU operations, still two
cellular searches and three reuses. Sampled height is [-0.070217,-0.024211] m;
paired means match v2 above. All runtime implementations and the native editor
binary remain unchanged. The actual scene capture is
`../2026-09-17-mountain-layering/v4` against prefix `mountain-organic-v3`.
It uses exactly the preceding v3 baseline's cameras and lighting. POM-on/off,
albedo/normal and thirty timing samples per view will determine whether to
retain this candidate. Whole-scene startup performance remains a separate open
issue even though no terrain texture bake or physics job is requested.


## Retained visual result

The nineteen-image v4 capture completes with no missing images, command failures
or Vulkan validation errors. Sources and native editor remain immutable; original
props are restored and the capture editor exits. The same executable SHA as the
baseline is retained: `c503c7d16294eddc8048f1628a692d745bb09b1cd86d5a91f5491bcfe60479c5`.
Only the authored JS recipe differs. The frozen r2 asset editor is untouched.

Retain candidate v3 as an incremental improvement: stones are less pale and
less uniformly flat-topped; some lower stones are buried by uneven soil. The
near ground has shallow clod structure and less fine outline speckling. Moss
coverage is more broken up. Close/grazing POM-off controls still visibly flatten
stones and soil. Close POM-on/off images differ by >3 byte levels on 65.6% of
pixels; that image metric is not an independent height/depth oracle.

This is **not final realism acceptance**. Soil and moss still look soft, cliff
forms still read as joined plates, and chart/LOD seams remain visible. Blue
lighting and the forest presentation also constrain the evaluation. Millimetre
structure cannot be assumed resolved at the current 64 texels/m; check actual
near-page resolution/filtering before adding more high-frequency noise. Motion,
actual-scene RT and user visual approval remain open.

- [New close view](../2026-09-17-mountain-layering/v4/close-lit.png) / [previous](../2026-09-17-mountain-layering/v3/close-lit.png)
- [New POM-off control](../2026-09-17-mountain-layering/v4/close-flat.png)
- [Grazing view](../2026-09-17-mountain-layering/v4/grazing-lit.png)
- [Cliff view](../2026-09-17-mountain-layering/v4/cliff-grazing-lit.png)
- [Capture audit](../2026-09-17-mountain-layering/v4/audit.json) / [native scene summary](../2026-09-17-mountain-layering/v4-summary.json)

### Performance remains open

Thirty-sample G-buffer medians, ms (baseline → candidate):

| View | Before | Candidate |
| --- | ---: | ---: |
| Overview | 32.9625 | 33.7565 |
| Grazing | 16.837 | 18.0125 |
| Frontal cliff | 27.866 | 27.525 |
| Oblique cliff | 36.7255 | 36.0785 |
| Close | 5.5275 | 6.098 |

Close and grazing are approximately 10.3% / 7.0% slower in this run, while cliff
views are slightly faster. Preserve that regression signal. The frozen editor
is independently open, so these measurements do not establish causal attribution
or acceptance. Cached rendering reads pages, not the recipe instruction stream;
do not infer a frame-time cause from the added source operations alone.

Root setup is 55.623 s, including 42.100 s publish. Initial streaming fills 2,586
sectors in 168.23 s. Terrain still requests no atlas or physics jobs, but this
pass does not fix whole-scene startup. The existing trace attributes 6.1 s of the
preceding 42.3 s publish to tilesets and leaves most of that stage unsplit; a
future startup investigation needs finer attribution rather than blaming terrain
material generation. No density, envelope or POM settings were lowered.
