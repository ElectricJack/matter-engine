# Mountain mineral faces and shallow soil relief

Status: v1 retained as a modest surface-detail improvement. Native evaluation
and 33-image capture pass; cliff material selection and final realism remain open.
The previous goal turn completed the 16K capacity repeat, measured actual near
density and documented its memory/startup limits. This pass returns to the
requested material appearance using that retained resolution.

## Candidate

Only `projects/world_demo/shared-lib/mountain_surface.js` changes from the
capacity checkpoint. One continuous 3D noise field supplies creased mineral
faces; it varies loose-stone crowns and contributes correlated bedrock pigment
and shallow height. Weather varies fracture widths so every rock need not have
the same continuous outline. Soil clod relief and mineral grain are stronger,
with the existing physical-footprint filtering retained. All height dependencies
remain position/footprint inputs; receiver slope still affects appearance only.

There are no new source atlases, physics jobs, DSL functions or shader changes.
The two cellular neighborhood searches and three reused features remain.
POM settings, terrain geometry, 128 t/m request and [-0.090,0] m envelope stay
unchanged. This recipe targets surface form; it does not resolve proxy silhouettes
or terrain chart/LOD seams.

## Native evidence

`mountain-facets-v1` MSVC world-definition/editor builds pass. The native
mountain check loads the actual authored source and packs 428 GPU operations
(previously 404). Sampled height is [-0.071750,-0.025862] m, about 4.59 cm relief.
The existing 1/64 m footprint reference gives near/far mean height
-0.049311/-0.050000 m (0.689 mm shift), and red albedo 0.095036/0.095388.
This reference is not a new 128 t/m mean measurement or general filtering proof.
Far unresolved height remains -0.05 m. World translation invariance and channel
bounds pass. The native editor hash remains identical to the capacity build.

The independent interval bound does not depend on the output height clamp:
the facet field is in [-0.30,0.70], and its base-height term is multiplied by
0.006, block-face coverage [0,1] and rock amount at most 0.8125. Its contribution
is [-0.0014625,0.0034125] m. Additional grain contributes at most +/-0.0003 m.
The previous whole-surface lower bound -0.084355625 therefore becomes
-0.086118125 m. Soil thickness remains nonnegative and stone crowns are only
reduced by their new [0.68,1] shape factor. The old stone upper bound
-0.0058125 becomes -0.0021 m. Soil depth's maximum grows by 0.00208 m; the
independent soil/moss upper bound becomes -0.020508125 m. Height-aware layers
are convex blends, so the original [-0.090,0] envelope remains conservative.

## Scene review

Capture v5 in `../2026-09-17-vt-resolution/` uses the unchanged five cameras
and standard settings from v4. Four extra clear-weather lit/POM-off views follow
all comparison captures and timing samples. They disable cloud density/cloud
shadows/fog and reduce sky irradiance to reveal surface form; they are diagnostic
controls, not a replacement production lighting setup. Authored props are
restored on exit.

The capture script now records actual other-editor processes before launching.
The earlier fixed PID annotation was stale; no other editor was running when
this turn checked. Other GPU workloads are uncontrolled, so timings still do
not establish isolated performance acceptance.

Runtime sources stay frozen through capture. The full terrain/building, shared
wall, sparse layer, contact, motion/RT and performance goal remains active.

## Completed review

All 33 images/markers complete in 406.8 s, with no command or Vulkan validation
errors, unchanged runtime sources/executable, restored props and editor exit 0.
Process inspection confirms the capture editor is closed. The revision gives
the soil stronger clod structure and the mineral faces more irregular normals.
The change is modest; cliffs still read as shallow joined plates, and the
distance bands/chart seams remain visible. Do not call this final realism.

Clear-weather controls greatly change the blue appearance, confirming that
the lit color is strongly affected by the lighting/weather setup. Several
controls changed together; this does not isolate a single atmospheric or
shadow cause. The clearer light also exposes small-scale sampling artifacts
and an important material-selection limitation: steep surfaces receive
bedrock pigment while retaining some loose-ground displacement.

The recipe's `rockCover` selects final color/roughness but not height.
`SurfaceProgram::parse` currently rejects receiver normals, slope and field
lanes as height dependencies. That guard is valid: `vt_source_height_at`
offsets position for its normal finite differences but holds the barycentric
coordinates and input normal fixed, so it cannot differentiate those lanes.
Simply lifting the guard would produce incoherent relief/normals. Before more
cliff noise tuning, provide a defined, CPU/GPU-consistent derivative contract
for context-driven height layers, then use it to expose bedrock and suppress
loose stones on cliffs. Preserve cross-chart/world anchoring and evaluate LOD
continuity; an interpolated context is not automatically a rung-independent
world field. The existing full-goal terrain/contact requirements still apply.

### Measurements and artifacts

Used-page, pinned-page and indirection high waters match the previous capture:
1437 used, 998 pinned, 36.96 MiB table use, zero evictions, 2330 fills.
Root setup is 54.401 s, including 42.950 s publish. Initial streaming fills
2586 sectors in 169.59 s; that interval overlaps root setup. No faster total
startup is claimed.

Thirty-sample G-buffer medians (previous v4 → candidate v5), ms:

| View | Before | Candidate |
| --- | ---: | ---: |
| Overview | 32.276 | 33.436 |
| Grazing ground | 17.502 | 18.613 |
| Frontal cliff | 28.6465 | 25.3945 |
| Oblique cliff | 34.064 | 35.0775 |
| Close ground | 6.568 | 6.683 |

These mixed results retain regression signals, including +6.3% grazing time;
they do not establish a causal performance comparison with controlled GPU load.
Raw normals change most on the cliffs (mean absolute differences 2.99/2.40
byte levels), consistent with the authored face changes. Pixel differences are
not a material-quality or independent displacement-depth proof.

- [Capture audit](../2026-09-17-vt-resolution/v5/audit.json)
- [Native checks](../2026-09-16-shared-vt-pixels/mountain-facets-v1-tests.json)
- [Decoded diagnostics](../2026-09-17-vt-resolution/v5-analysis.json)
- [Source comparison](source-comparison.json)
- [Standard close view](../2026-09-17-vt-resolution/v5/close-lit.png)
- [Clear-weather close](../2026-09-17-vt-resolution/v5/close-clear-lit.png)
- [Clear-weather cliff](../2026-09-17-vt-resolution/v5/cliff-clear-lit.png)
- [Clear-weather POM-off cliff](../2026-09-17-vt-resolution/v5/cliff-clear-flat.png)
