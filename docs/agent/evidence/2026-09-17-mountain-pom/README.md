# StreamMountain POM material pass

Status: the authored material and native source/POM checks pass. Visual review
and distance-transition acceptance remain open. This is a development build;
the frozen r2 asset/export handoff is unchanged.

## Material changes

`projects/world_demo/shared-lib/mountain_surface.js` keeps continuous world-space
GPU generation and the removal of the five terrain source-atlas jobs. No terrain
physics settling or geometry-source bake is introduced.

- Broader embedded stone profiles (roughly 15–30 cm) remain resolvable at the
  scene's requested 64 texels/m. Common weathering contributes centimetre-scale
  relief; fine grain contributes sub-millimetre relief.
- Pigment, roughness, height and derived normals follow the same stone field.
- Filter variation about the aggregate's spatial mean instead of removing its
  entire positive mask at distant footprints. Widen the filtering interval so
  the material changes over several mips.
- Authored height envelope is [-0.074, 0] local metres. The current 256-point
  native probe samples [-0.065526, -0.017024] m: approximately 4.85 cm of relief.
  These are sampled heights, not the maximum possible range or a silhouette.
- The native 1,024-position paired footprint probe reports average heights
  -0.046967 m nearby and -0.047000 m distant (0.033 mm difference). Average red
  albedo is 0.132382 vs 0.133648. This measures one fixed patch, not every biome.

The recipe compiles to 184 GPU operations. Terrain/forest geometry, habitat,
lighting presets and streaming distances retain their prior authoring.

## Native validation

The `mountain-pom-v2` manifests in the sibling
`2026-09-16-shared-vt-pixels` evidence directory bind this recipe to native MSVC
builds of the world test, GPU smoke test and editor. All three builds pass.
The focused mountain loader/compiler, `vt-direct-source` and
`vt-composed-parallax` checks pass with unchanged source/binary hashes, zero
Vulkan validation errors and no skipped RT checks. POM's existing native gate
covers rotated/scaled receivers, height registration and the deepest endpoint;
the 41-angle endpoint maximum depth error was 0.000000238 m.

## Captures and limits

- `v2/` is retained as a diagnostic, **not visual acceptance**. The first close
  shot ran before VT activation (`active=0`), and later pairs still had changing
  geometry/page residency. The status view does show successful nearby POM
  hits, with a remaining guarded chart-boundary line and unresolved distance.
- The revised capture controller waits for active VT, stable variant count and
  an empty queue before releasing comparisons. It uses temporary neutral
  lighting for readability and restores the user's exact original scene props.
- `v3/` exits cleanly with all eight shots, unchanged sources/binary and no
  validation or command errors. Residency settled after 245.2 s at 789 variants.
  **Camera caveat:** world activation overwrote the early camera command with
  the authored overview camera. The files named `close-*`, `albedo-*` and
  `pom-status` therefore show that overview, not the intended close camera.
  The subsequent explicit grazing camera is correct. Future capture runs must
  reapply the close camera *after* activation and wait for that view to refine.
- The useful settled pair is [POM on](v3/grazing-pom.png) versus
  [POM off](v3/grazing-flat.png). Both have queue=0, 789 variants, 1,309 physical
  pages, zero rejections and zero evictions. The profiles visibly shift with
  POM. The current shapes are still too soft and the distance cutoff remains;
  these images do not establish finished material realism or visual approval.
  [Native RT](v3/grazing-native-rt.png) also completes, but atmosphere differences
  prevent treating the whole rendered frame as a pixel-equivalence gate.
- An independently opened frozen r2 editor was present. These runs are visual
  and functional evidence; they cannot establish isolated performance targets.
- Whole startup still includes forest generation and pipeline initialization.
  V2 logged `bake.reset` at 48.64 s and the first bark job at 89.87 s; those
  job timings do not isolate source generation from first-use setup.
  V3's corresponding times were 46.11 s and 3.13 s. The other bark jobs took
  1.62 s and 1.77 s; the five removed terrain source jobs remain absent.
- POM cost needs attention: the same-run grazing stats report 55.878 ms
  G-buffer time with POM and 11.511 ms without it (overview 78.019 vs 24.373 ms).
  These single-frame observations are not isolated performance acceptance, but
  they clearly warrant profiling before adopting deeper relief broadly.
- Follow-up: the [fade-check optimization](../2026-09-17-pom-fade/README.md)
  skips chart/geometry work after the existing fade reaches zero. Matched settled
  30-sample captures reduce median G-buffer time from 78.225 to 31.600 ms
  overview and 55.956 to 18.501 ms grazing. Full-frame albedo/normal captures
  differ by at most one 8-bit level; native near-depth/seam checks pass. This is
  still a non-isolated development comparison, not whole-frame acceptance.

## Remaining

Improve distinct rock/soil/moss appearance and contact layers, then obtain
settled close/distant/grazing and motion approval. Distance mip continuity and
POM chart boundaries remain open. The generic adjacent-mip sampler experiment
was **deferred and removed from production** after compiler expansion failures;
its patch and findings are retained in `../2026-09-17-vt-mip-filtering/`.
