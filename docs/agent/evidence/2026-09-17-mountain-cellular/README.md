# StreamMountain cellular material development

Status: the third candidate passes native checks and the capture audit, and is
retained as the next development baseline. Its rougher stone profiles improve
on the first two candidates, but overall terrain realism remains unaccepted.
This follows the validated POM fade optimization.
The complete terrain/building layered-texturing goal remains open.

Follow-up: [compiler-directed cellular reuse](../2026-09-17-cellular-reuse/README.md)
removes duplicate neighborhood searches while retaining this recipe. Its
`v4/` capture verifies close/overview material output within one 8-bit level;
see the follow-up for the full grazing comparison and modest page-fill gain.

## Material and reusable operator

The previous terrain recipe's smoothly thresholded noise produced soft domes.
The candidate replaces that aggregate with oblique 3D cellular profiles: flatter
crowns, planar flank segments and a stable value per stone for correlated
height and color. Soil is warmer; organic patches use darker moss colors and
bury more of the stones. Broad relief is shallower, while fine grain remains
sub-millimetre detail. Large world-space fields retain regional variation.

`s.cellular3(seed, x, y, z, feature)` is a generic surface-program operator,
available to both base materials and splat contents through the same evaluator.
It is not a brick/terrain-specific DSL function. It supports nearest distance,
squared-distance gap and site attribute, with matching CPU and GPU semantics.
The revised kernel searches 27 sites first and extends to a bounded 125-site
window when needed. A separate 343-site double-precision search tests competitor
selection, including negative coordinates, outer-ring competitors and
full-range uint32 seeds. Coordinates are supplied explicitly so authors
can use local, world or transformed domains. The design spec documents the
grammar, units and filtering responsibility.

The current terrain recipe uses 260 GPU operations (previously 184), within the
existing 512-operation / 96-live-register compiler. It requires no geometry
source bake or physics settling. The three bark atlas jobs remain separate;
the five removed terrain atlas jobs remain absent. The source height envelope
is [-0.064, 0] metres. The fixed native probe samples [-0.058565, -0.015047] m,
about 4.35 cm of POM relief.
Paired 1,024-position probes at 1/64 m versus 2 m footprint report mean heights
-0.049495 / -0.050000 m and red albedo 0.096363 / 0.095058. Those are measurements
of one patch, not a guarantee of identical averages in every biome.

## Evidence so far

Native manifests/logs use `mountain-cellular-*` prefixes in the sibling
`2026-09-16-shared-vt-pixels` directory. All validation is native MSVC.

- `v1`: retained test compile failure from passing `std::string` to a local
  `const char*` test helper. Fixed by using the parser's string interface.
- `v2`: surface and mountain tests pass. The larger cellular reference agrees
  within 0.000000298 cell units and every sampled site identity matches.
  The first GPU fixture run fails: its variant identity `0x710003` collides
  with an existing cached continuous-chart fixture. Its raw outputs came from
  that older cached source; this was an invalid test identity.
- `v3`: the corrected identity gives GPU height error below 0.7 micrometres
  and byte-identical regeneration. A coarse color assertion fails (RMSE
  0.037630) because the fixture writes three unrelated raw fields into RGB,
  including discontinuous site attributes at cell edges. BC7 cannot represent
  those colors exactly with one endpoint line per block.
- `v4`: validates all three operator outputs through the physical UNORM16
  height channel, at both tested mips, with maximum error 0.000000684 m after
  scaling the fields by 0.02 m. This checks operator accuracy separately from
  lossy color storage. Constant-site BC7 blocks retain the site value within
  0.005895; full-image raw-field color RMSE remains recorded, not hidden by a
  looser threshold. Color, height and normal bytes are identical after
  preparation eviction and regeneration for each feature. All compositor
  checks pass, with zero Vulkan validation errors.

The first candidate's v2 native `surface`, `mountain`, `vt-direct-source` and
`vt-composed-parallax` checks pass, with no skipped RT gate. Source manifests
prove v2 through v4 changed only the compositor test fixture; production code,
shader and terrain recipe are identical. The final compositor test and editor
builds use v4. No production-source changes occurred during those runs.

The revised candidate's v5 builds and all five checks pass: `surface`,
`mountain`, `compositor`, `vt-direct-source` and `vt-composed-parallax`.
The CPU reference error is at most 0.000000387 cell units, all sampled site
attributes match, and three explicit positions require the outer search ring.
GPU field tests at both mips have height error below 0.7 micrometres; the
explicit outer-ring GPU case differs by 0.000000018 m. Constant-site BC7 value
error is at most 0.006090. Regeneration preserves all channel bytes. All runs
retain unchanged source/binary identities and report zero validation errors;
the RT-capable direct-source check is not skipped.

Native v6 changes only `mountain_surface.js` relative to v5. Its editor and
world-test binaries are identical to v5; the wrapper confirms both targets are
up to date. The actual JS recipe still parses, compiles entirely to the GPU
evaluator, stays within its height envelope and preserves world anchoring in
the native mountain test. The unchanged operator/compositor/POM evidence above
continues to apply. The new recipe is exercised by the third native capture.

`mountain_surface-before.js` preserves the previous authored material.
`capture.py` waits for world activation, at least the baseline 789 variants,
stable counts and an empty page queue before overview, grazing and close
captures. It includes raw albedo/normals and a close POM-off control, checks
immutable source/binary identities, and restores original scene props.

## Open work

Distinct rock/soil/moss realism, distance
transitions, contact layers, motion review and isolated performance acceptance
remain open. The independent frozen r2 asset editor and export handoff remain
unchanged.

## First visual review and revision

`v1/` captures all ten requested images with a clean native exit, unchanged
source/binary hashes, no validation errors and no missing shots. It settles at
789 variants. The view shows a clear problem: low-jitter, nearly filled cells
look like paving, and the high-contrast broad moss reads as camouflage.
This candidate is **rejected as terrain realism**, despite its technical checks.
The previous recipe is retained in `mountain_surface-paving-v1.js`; the initial
kernel and shader are retained alongside it.

The second candidate gives sites their full cell range, with a rare bounded
outer-ring search to preserve correct neighbors. Variable-radius stone masks
leave more exposed soil. Moss/soil colors are closer, their broad transition
is softer, and a shallow face variation reduces perfectly flat stone crowns.
`estimate_profile.py` records the mean used for footprint filtering. Its
131,072-position probe has 23.7% stone coverage and runs the extra ring for
0.373% of samples; three samples actually require an outer competitor. Those
positions are now explicit CPU/GPU regression cases. Native v5 validation
passes.

## Second visual review

`v2/` retains ten captures and a clean native exit. Sources and binary remain
unchanged during capture, all shots arrive and Vulkan reports no validation
errors. The overview settles at 789 variants with an empty page queue after
211.0 s. The capture restores the exact original scene props and closes its
own editor.

Separated stones remove the obvious paving grid, and the POM-on/off pair
clearly changes their apparent depth. However, their almost circular, smooth
crowns look like buttons. This is useful POM structure, **not accepted terrain
realism**. `mountain_surface-round-v2.js` preserves this recipe. Distant ground
still loses too much material character, and the scene's blue lighting and
other visible geometry/foliage issues limit the overall presentation.

Thirty-sample G-buffer medians are 27.7705 ms overview, 18.163 ms grazing and
5.979 ms close. The log reports 56.718 s root bake (42.226 s publish) and
171.30 s for 2,586 streamed sectors. These are non-isolated observations with
the independent frozen r2 editor open, **not a bake-speed acceptance claim**.

## Third recipe

An additional one-octave field breaks the stone outlines, wears their crowns,
varies face height and pigment together, and adds shallow soil relief. Stone
brightness is reduced. Its full-amplitude height bound remains inside the
existing [-0.064, 0] m envelope; no POM scale, terrain density or quality setting
is reduced. `profile-estimate-v3.json` records the approximate profile mean
0.11926 and 24.15% coverage. That diagnostic omits the small weather warp; the
native paired-footprint probe above measures the actual authored recipe.

## Third scene review and retained baseline

`v3/` retains all ten native images, exact source/binary hashes, commands,
timings and logs. The editor exits 0, sources and binary stay unchanged, no
commands fail and Vulkan reports zero validation errors. Original scene props
are restored. All views settle at 789 variants with an empty page queue;
overview settles after 206.0 s. The last close sample has 1,327 resident pages,
zero rejected variants and zero evictions.

The new outlines and face wear are visible, and the close POM-on/off pair
clearly distinguishes relief from flat pigment. This is retained as a
development improvement, **not final visual approval**. Stone scale and coverage
remain rather uniform, soil is still soft, distant ground loses character,
and cliffs need their own convincing material structure. Motion, terrain/contact
layers, actual scene RT comparisons and isolated performance gates remain open.
Differences in lit brightness between separate runs are not attributed solely
to the material; these captures do not freeze every temporal lighting effect.

- [Close, POM enabled](v3/close-lit.png)
- [Close, POM disabled](v3/close-flat.png)
- [Grazing view](v3/grazing-lit.png)
- [Overview](v3/overview-lit.png)
- [Raw albedo](v3/close-albedo.png) and [normals](v3/close-normal.png)
- [Capture audit](v3/audit.json)

Thirty-sample G-buffer medians are 29.1575 ms overview, 18.053 ms grazing and
5.947 ms close. Root bake is 55.355 s (42.424 s publish); streaming reports
2,586 sectors in 168.43 s. Total capture duration is 284.955 s. The frozen r2
editor remains independently open. These non-isolated observations do not
establish a frame-time or full-load improvement. The extra cellular searches
and 260-op recipe still need measured preparation/fill latency work; removing
physics from the terrain recipe does not imply instant whole-scene loading.
