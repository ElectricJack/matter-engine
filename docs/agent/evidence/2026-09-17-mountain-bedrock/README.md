# StreamMountain bedrock and deposition iteration

This iteration adds a second physical scale to the direct GPU material:
fractured bedrock below loose stones, regional exposure/deposition, and larger
mineral bedding that remains visible after fine detail filters out.
The full layered terrain/building goal remains open.

## First candidate authoring and POM

`projects/world_demo/shared-lib/mountain_surface.js` remains ordinary JS over
generic surface operators. No new terrain-specific DSL, geometry source,
physics step, terrain geometry change or texture-density reduction is involved.
Two cellular searches produce five features; the three additional feature
reads reuse those searches. The recipe has 352 GPU operations, up from 260,
within the existing 512-operation / 96-live-register limits. This is added
material work, not a claimed generation speedup.

- Flattened oblique blocks give the substrate wider fractures and correlated
  color, roughness and height. Their approximate statistical profile mean is
  0.675; the authored filtering datum is 0.70. Native paired-footprint probes
  below measure the actual recipe, including all warps and masks.
- Position-based mineral/organic fields reduce loose-stone relief and pigment
  together and expose substrate in larger regions. Receiver slope can favor
  rock color, but does not drive height: the current height derivative contract
  rejects receiver normal/slope dependencies. Thus relief is not yet a fully
  slope-specific layered terrain model.
- Coarser mineral bedding survives beyond the fine stone/fracture footprint.
  Sub-millimetre grain stays out of distant POM; all normal derivatives come
  from the same metre-valued height field.
- Declared height range is [-0.074, 0] m. The fixed native sample set measures
  [-0.060064, -0.018655] m, or about 4.14 cm of relief. At 2 m footprint the
  unresolved relief returns to the -0.050 m datum.

## Native validation

`mountain-bedrock-v1` and `mountain-bedrock-v2` in the sibling
`2026-09-16-shared-vt-pixels` directory retain build/source/test manifests.
The only v1-to-v2 change widens a diagnostic field scan to locate a real cliff
camera; production sources and editor executable remain identical.
Both native MSVC world-material checks pass: actual JS import, source parsing,
GPU budgets, exact cellular reuse counts, bounded channels/height and translated
world anchoring. Only the three bark atlas jobs remain; terrain atlas/settling
jobs remain absent. The editor target is up to date.

The paired 1,024-position probes report near/far red albedo 0.108750 / 0.107855
and mean height -0.049835 / -0.050000 m. This is one measured patch, not a
universal mean-preservation guarantee. Terrain field heights are unchanged.
Existing generic cellular, compositor and raster/RT POM evidence from the
previous iteration still applies; those implementations are unchanged here.

The derived cliff eye/target is
`827.960 105.624 1659.563 832.000 104.624 1664.000`.
The capture harness adds that view and its POM-off control to the existing
close/grazing/overview sequence. `v1/` retains all fourteen native captures and a clean audit: exit 0, exact
source/binary identities, no missing shots/command failures/validation errors,
original props restored and capture editor closed. Views settle with empty
queues (789 variants initially, 991/992 after visiting the cliff).

**First candidate rejected as final appearance.** Regular altitude bands read
as contour stripes across every mountain; the ground exposes too much pale
slab-like rock, and the cliff fractures look too smooth. POM is present, but
that does not make the material convincing. The source snapshot is
`mountain_surface-candidate-v1.js`.

A second candidate removes the periodic bands, uses irregular warped 3D mineral
variation, narrows/darkens exposed rock regions, breaks up fracture outlines,
and gives the substrate deeper, sharper recesses with slightly varied face
heights. The native checks and second visual review below record this revision.

## Reproduction

Use the existing immutable `build_checks.py` / `run_checks.py` harnesses with
a new prefix, then `python3 capture.py vN PREFIX` from this directory (or its
repository-relative path). The capture restores exact original scene props,
closes only its own editor, and checks source/binary identity and validation
errors. The independently running frozen r2 asset editor is left alone;
render/load timings are development observations, not isolated acceptance.


## Second candidate native results

`mountain-bedrock-v3` builds and passes the focused native mountain check.
The actual source still uses 352 operations and two searches/three reused
features. Its sampled height range is [-0.069729, -0.014820] m (5.49 cm), within
[-0.090, 0] m. Conservative independent signal bounds put the unclamped height
inside approximately [-0.08015, -0.00113] m; the declared range does not conceal
clipping. Snow attenuation only narrows variation around the -0.050 m datum.
Paired near/far probe means are -0.049353 / -0.050000 m and red albedo
0.096426 / 0.095403. Five former terrain atlas jobs remain absent, and terrain
field heights and the 64 texels/m request remain unchanged.

Only the authored JS material and its native test/diagnostic changed from the
previous validated editor source manifest. No engine/shader implementation was
modified. The native editor SHA256 remains
`10739b70602210c66b7307800c8956e3f2eabc54d97b861cf00e62c912a7a21f`.
The second capture adds an oblique cliff view and POM-off counterpart; it uses
18 shots across five views, including three POM-off controls.

## Second visual review and retained development baseline

`v2/` passes the complete capture audit: exit 0, all eighteen shots present,
unchanged source/binary identities, zero Vulkan validation errors and zero
command failures. Original scene props are restored exactly, and the capture
editor is closed. Only the independent frozen r2 editor (PID 9056) remains.
Overview settles after 201.0 s at 789 variants with an empty queue; after the
cliff/oblique visit, close settles at 998 variants with an empty queue.

The regular mountain stripes are gone. Exposed rock patches are quieter, and
fracture outlines are sharper and less uniform. The close POM-on/off pair
clearly changes embedded stones from flat marks to relief. Cliff parallax is
more subtle; the oblique view is included rather than treating frontal shading
as proof of strong depth. This revision is retained as a development step,
**not final realism acceptance**. Pebbles still look too similar, soil/moss
remain soft, and the cliff pattern still reads too much like connected plates.
The scene's blue lighting and visible forest/geometry issues also remain.
No claim of photographic or UE5-equivalent terrain quality is made.

- [Ground with POM](v2/close-lit.png) / [POM disabled](v2/close-flat.png)
- [Cliff, oblique](v2/cliff-grazing-lit.png) / [POM disabled](v2/cliff-grazing-flat.png)
- [Cliff, frontal](v2/cliff-lit.png)
- [Ground, grazing](v2/grazing-lit.png)
- [Overview](v2/overview-lit.png)
- [Capture audit](v2/audit.json) / [timing and pixel summary](v2-summary.json)

The actual full recipe is `mountain_surface-candidate-v2.js`; it matches the
production shared-lib file byte for byte. The statistical guide in
`block-profile-estimate-v2.json` measures 0.7815 without domain warp/fracture
perturbation, versus the authored 0.77 filter mean. The actual native paired
probes above remain the relevant measurement for the composed material.

### Performance observation requiring follow-up

Thirty-sample G-buffer medians are 33.646 ms overview, 18.368 ms grazing,
25.957 ms frontal cliff, 36.173 ms oblique cliff and 6.0475 ms close. The earlier
cellular-only v4 captures measured 28.911 / 18.528 / 5.989 ms for the three
shared cameras. The **overview observation is about 16.4% slower**. This is a
regression signal to investigate; captures are non-isolated and do not yet
establish its cause. The settled queue is empty, so added source-program
operations alone cannot explain steady-state draw cost without further evidence.
POM work, height bounds, actual chart residency and repeatable timing need
matched checks before broader material rollout.

Root bake is 53.271 s including 40.600 s publish; initial streaming reports
2,586 sectors in 165.49 s. The longer 401.44 s total capture includes five
camera positions and eighteen images, so it is not comparable to the earlier
three-view capture duration. Removed terrain source/physics jobs remain
absent, but whole-scene startup is still far from instant. This iteration does
not establish a full-load speedup or meet deferred performance acceptance.

Motion/mip review, actual scene raster/RT comparison, realistic soil/moss,
terrain/rock/building contact blending and the broader wall/layer goals remain
open. Keep the prior generic native correctness evidence, and investigate the
observed overview cost while continuing material development.
