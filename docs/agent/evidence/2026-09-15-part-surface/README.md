# Part-local finite material recipes

This checkpoint adds the JS-to-native declaration needed to attach reusable
geometry-derived brick materials to cheap wall receivers. It does not yet
connect provider publication or change `ClayBrickWallProof` to low-poly walls.

## Implementation

- `ClayBrickSurface.js` declares `static finiteSurface(p)` and builds a separate
  twelve-triangle receiver. Geometry and receiver share physical bounds; the
  modular depth defaults to 117.5 mm. `ClayBrickSource` remains the detailed
  inspection part.
- `ScriptHost::evaluate_finite_surface` returns owned solid operations and
  compiled appearance/base programs without running the Part constructor or
  `build()`, invoking the source mesher, or writing cache artifacts.
- Part and World share `surface_base.js.inc`. There is one surface language,
  with isolated register numbering for each compiled material. The solid
  operation decoder is also shared with the existing `solidSource` verb.
- Generic six-face planning derives outward frames and datum planes, adds a
  two-pixel guard and bounds aggregate source pixels. Projection checks ray
  clipping; the padded mesher lattice cannot prove exact solid enclosure.
- Geometry identity remains independent of clay-color changes. Invalid,
  cancelled, stale or oversized declarations preserve the previous complete
  result. Local GPU programs are validated before source preparation.
- The existing 48-face native GPU test now gets geometry and appearance from
  this declaration. It retains the earlier 112 mm inspection dimensions and
  sample domains so its acceptance comparisons remain comparable.

## Initial failure and correction

`recipe-v1` built successfully but exposed acceptance of a `NaN` material
constant by the general surface parser. The new finite-source reader now
rejects every nonfinite literal operand before GPU packing. The initial
failure and source manifest are retained. Subsequent failure-preservation
assertions in that first run were consequences of the same accepted bad
recipe replacing the test's prior value.

## Validation

All 13 sequential native build/run commands in
[recipe-v2-checks.json](recipe-v2-checks.json) passed with unchanged source
snapshots. The final [source/binary manifest](recipe-v2-source.json) and
[summary](summary.json) retain the exact inputs and results.

| Check | Result |
| --- | --- |
| `finite_surface_recipe_tests` | All eight actual recipes, malformed input, local-program contract, cancellation/deadline/staleness, independent material identities, six physical datum planes, and actual twelve-triangle receiver build passed. |
| `solid_source_evaluation_tests` | Existing recipe-only evaluator and shared solid parser passed. |
| `eval_world_tests` | Existing material recorder, ordered layers, terrain and brick proof tapes passed. |
| `script_host_tests` | Existing native Part/DSL regression suite passed. |
| `solid_face_projection_gpu_tests` | All 48 clay geometry/material/projection faces passed using the new declaration. Existing analytic and sampling oracles remained enabled. |
| `vt_compositor_tests` | All eight real brick variants passed source-to-VT comparison; zero Vulkan validation errors. |
| `matter_editor` | Canonical MSVC editor build passed. |

The eight recipe evaluations measured 4.97–6.52 ms each (single run, including
validation and six-face planning). They performed no source meshing, GPU work
or artifact writes. This measures declaration evaluation, **not total texture
generation latency**. The native receiver also built without calling the
detailed source mesher or writing an artifact.

All eight decoded VT channel arrays are **byte-for-byte identical** to the
prior validated native outputs. [readback-comparison.json](readback-comparison.json)
records each hash and comparison; the prior
[readback archive](../2026-09-15-stamp-vt/clay-vt-readback.zip) therefore also
contains the exact current decoded arrays. This establishes preservation of
the baked appearance through the new declaration, not a new visual improvement
or a rendered whole-wall acceptance capture. The GPU acceptance specimen keeps
its 112 mm depth; the modular 117.5 mm recipe/receiver pair is separately checked
by the native recipe test.

The unchanged whole-brick JS layout suite also passed: 40 wall sizes/bonds and
780 complete bricks, including resize, mortar and metric-frame invariants.

Reproduce the native sequence with `python3 run_native.py <new-prefix>` followed
by the `build:`, `rootcpu:`, `cpu:`, `projection:` and `clay:` arguments shown in
the retained runner invocation/checks. The script uses this WSL checkout and
native Windows toolchain, freezes source hashes across every command, and stops
on the first failure. No editor or GPU test should run concurrently.

## Remaining integration

The provider must prepare/cache all six faces, then publish a complete catalog
and base material with the receiver. Both eager and deferred VT admission need
the correct per-vertex face selector before preparation. The whole-wall layout
must retain those source parts' physical frames and reuse their catalogs across
instances. Final compressed source storage, safe resident replacement, chipped
silhouette handling, weathering/splats, terrain work and visual/performance
acceptance remain open. The full material goal remains active.
The existing composed-POM internal-chart seam gate also remains open; no
parallax implementation changed in this checkpoint.
