# StreamMountain height-aware soil, stone and moss

Status: second candidate retained as a tested material-development checkpoint.
Native checks and the 19-image scene capture pass; final visual acceptance is open.
The full layered terrain/building goal remains active.

This records the earlier layered-material checkpoint. The current retained JS
recipe and its v4 capture are documented in the [soil/clod iteration](../2026-09-17-mountain-organic/README.md).

## Change

The prior recipe used height amplitude as a translucent stone color weight.
This candidate uses the existing `s.layer` helper to compose soil, embedded
stone and moss, with one height-aware weight controlling each layer's RGB,
squared roughness and height. The normal evaluator differentiates that same
composed height for POM. Moss's lower surface recedes from protruding crowns;
low stone faces retain their mineral color instead of fading solely because
of their relief amplitude.

The stone field has a broader nonlinear radius distribution and unequal axes
in the oblique 3D domain. Cell identity, fractures and body coverage still
agree across material channels and world-space receivers. No texture tile,
source geometry, physics simulation, engine API or shader implementation is
added. Terrain shape, forest, authored lighting, texture density (64 texels/m)
and declared height range [-0.090, 0] m are unchanged.

Receiver slope and snow-facing masks remain appearance controls: source v1
cannot differentiate receiver context through height yet. This candidate does
not claim full slope-specific height composition or cross-object contact
blending. Height and its layer weights use position/footprint inputs only.

## Native checks

`mountain-layering-v1` manifests and logs in the sibling
`2026-09-16-shared-vt-pixels` directory record the native MSVC build and focused
`world_definition_tests --mountain-material` run. It passes with immutable
source/binary identities. The real JS import, native tape parser, GPU packer,
height derivative dependency checks, material bounds and translated-world
anchoring all pass. No engine changes require a new generic POM test run;
those shared paths were verified immediately before this material iteration.

- 365 GPU operations (previously 352); two cellular searches and three reused
  feature reads. The actual tape fits existing GPU op/register budgets.
- Sampled height [-0.069042, -0.020078] m, or 4.90 cm variation.
- Paired 1024-position near/far mean height: -0.049617 / -0.050000 m.
- Paired red albedo: 0.106318 / 0.101146. The larger color difference is an
  unresolved filtering concern, not a mean-preservation acceptance claim.
- Only three forest bark detail atlases remain; no terrain atlas/settling jobs.
- Existing terrain field heights and the derived cliff camera are unchanged.

### Independent unclamped bound

Noise signals are bounded to [-1,1], cell identity to [0,1], body profile to
[0,1.05], organic coverage `o` to [0,1] and outcrop `u` to [0,0.75]. Loose
amount is `(1-0.8o)(1-u)` and bedrock amount is `0.25+0.75u`.

The base height is bounded below by
`-0.050 - 0.0033 - 0.02845*0.8125 - 0.0035 = -0.079915625` m.
Moss can lower that by at most another 0.001 m. The maximum stone contribution
including the centering subtraction is at most `0.0385*(1-u)`; bedrock contributes
at most `0.00955*(0.25+0.75u)`. Together with at most 0.0033 m fine variation,
the maximum is `-0.0058125` m (at `u=0`). Snow attenuation only contracts these
variations about the -0.050 m datum. Moss's upper bound is lower still.

Both `s.layer` replacements are convex height blends. Thus the complete raw
height lies inside approximately [-0.080916, -0.0058125] m before clamps,
within the unchanged declared range. The bounds do not conceal saturation.

## Capture procedure

`capture.py v1 mountain-layering-v1` records overview, grazing, frontal cliff,
oblique cliff and close views. Lit/albedo/normal controls and four POM-disabled
controls produce 19 images. Fixed cameras/lighting match the preceding bedrock
review, and each view waits for stable variants with an empty VT request queue.
Thirty normal-mode GPU timing samples accompany each view. `summarize.py v1`
records baked-stage timings and POM pair pixel differences; lit differences
include temporal lighting and are not themselves a correctness oracle.

The harness preserves exact source/executable identity, restores original
props and closes only its own editor. The independently open frozen r2 asset
editor is untouched. Timings remain non-isolated development observations.

## First visual review and correction

`v1/` passes the capture audit with all 19 images, zero Vulkan/command errors,
unchanged source/binary identities, restored props and its editor closed.
The stones have more varied sizes and crisper boundaries, but are too pale;
overview/grazing images lose too much organic cover and reveal bright fine
speckling. This is not retained as the finished visual result.

A coverage mistake reused the loose-stone amount to attenuate moss. Since
organic ground intentionally reduces loose stones, that also suppressed moss
where it should grow. Candidate v2 uses outcrop exclusion for moss instead,
reduces stone pigment contrast, and gives moss a shallow 0.6 mm fracture field
shared with its color. It also changes the unresolved input coverage from
0.16 to 0.24, accounting better for the nonlinear layer blend's average weight.
That is a statistical approximation, not a general filtered-layer solution.

`mountain-layering-v2` passes the same immutable native build/material gates.
It has 372 GPU operations, still two cellular searches and three reused features.
The sampled range is unchanged: [-0.069042, -0.020078] m. Paired near/far red
albedo is 0.098480 / 0.097802 (about 0.69% difference); mean height is
-0.049405 / -0.050000 m (0.595 mm). The additional moss relief widens the
conservative raw lower bound by 0.0006 m to about -0.081516 m; the upper bound
remains -0.0058125 m. Both remain inside the original [-0.090, 0] m declaration.

Only `projects/world_demo/shared-lib/mountain_surface.js` changed relative to
the verified POM diagnostic baseline. The native editor binary remains
`7bdbdd1e8dd1af6b2e02d39ae25990dea0f1f2ae39a1f030d2d2c840768c1bfa`.
The candidate source snapshots preserve both revisions and the original.

## Second visual review and retained checkpoint

`v2/` passes: exit zero, all 19 images, no command failures or Vulkan validation
errors, and immutable source/executable identities. Props are restored exactly;
the capture editor is closed. Final process inspection finds only the independent
frozen r2 editor (PID 9056). Production matches `mountain_surface-candidate-v2.js`.

Stone bodies are more varied than the previous bedrock baseline, and their
exposed faces now read as a distinct material. The second candidate restores
organic ground coverage and reduces the first candidate's excessive pale
contrast. Matched POM-off controls show the stone relief flattening while the
material markings remain. Moss/soil and stone use the shared height-aware layer
contract, rather than independent hand-written height/color weights.

This is **not final realism acceptance**. Fine pale stones can still look busy;
soil/moss lack convincing small structures; broad organic patches are simplified;
and cliffs still look like joined plates. Blue lighting and forest appearance
remain visible constraints on evaluation. Broad chart/LOD seams, continuous
cross-object contacts, motion/mip review and actual-scene RT comparison remain
open. The next material step should address those visual limitations, not add
another speculative POM lookup cache.

- [Ground with POM](v2/close-lit.png) / [POM disabled](v2/close-flat.png)
- [Ground, grazing](v2/grazing-lit.png) / [POM disabled](v2/grazing-flat.png)
- [Overview](v2/overview-lit.png)
- [Cliff, oblique](v2/cliff-grazing-lit.png) / [POM disabled](v2/cliff-grazing-flat.png)
- [Cliff, frontal](v2/cliff-lit.png)
- [Capture audit](v2/audit.json) / [native scene summary](v2-summary.json)

### Performance and startup remain open

Thirty-sample G-buffer medians (ms), with the same development editor and capture
conditions for the two candidates:

| View | Candidate v1 | Retained v2 |
| --- | ---: | ---: |
| Overview | 33.953 | 33.441 |
| Grazing | 18.8105 | 18.293 |
| Frontal cliff | 23.1725 | 30.0295 |
| Oblique cliff | 35.663 | 36.723 |
| Close | 6.124 | 6.217 |

`timing-comparison.json` retains ranges and residency. Frontal cliff is a
**regression signal**, also above the previous bedrock capture's 25.957 ms.
Both candidate runs have identical settled page/variant counts at each view,
no queued requests, no rejections and no evictions. Do not attribute this to
extra source instructions merely from their count: settled rendering reads the
composed pages. These non-isolated observations cannot establish the cause,
but must be preserved for later matched diagnosis. No speedup or performance
acceptance is claimed; the user's visual-development priority remains in force.

Root bake is 54.573 s, including 41.569 s publish; initial streaming reports
2,586 sectors in 170.50 s. Terrain source geometry/physics is still absent,
but whole-scene startup remains slow. The 395.53 s total session includes five
camera positions and nineteen screenshots and is not an editor load-time metric.


## World-coordinate correction follow-up (v3)

`world-receiver-binding-v2` corrects missing volumetric sector Y in demand and
live VT classification; the worker's initial classification already included Y.
The authored mountain recipe remains identical to candidate v2. Density, height
range and POM quality are unchanged. [Binding evidence and limits](../2026-09-17-world-receiver-binding/README.md).

`v3/` completes all nineteen images, with immutable sources/binary, restored
props, no Vulkan/command errors and its editor closed. The fixed close and
oblique views preserve stone/bedrock displacement when compared with POM-off
controls. The overview retains its broad appearance. Raw channels are not
pixel-identical: correcting the world frame can change source evaluation.
Visible chart/LOD seams, simplified moss/soil, pale fine stones and plate-like
cliff forms remain. This is functional regression evidence, not final art approval.

Thirty-sample G-buffer medians are 32.9625 ms overview, 16.837 grazing,
27.866 frontal cliff, 36.7255 oblique cliff and 5.5275 close. Root setup is
54.619 s (42.275 s publish); 2,586 sectors fill in 168.37 s. These non-isolated
measurements show no clear new regression in the sampled views; they do not
resolve earlier performance concerns or establish a speedup. The independent
frozen r2 asset editor remains open. Full acceptance stays open.

- [Updated close POM view](v3/close-lit.png) / [POM off](v3/close-flat.png)
- [Updated oblique cliff](v3/cliff-grazing-lit.png)
- [Audit](v3/audit.json) / [summary](v3-summary.json) / [comparison](v2-v3-comparison.json)
