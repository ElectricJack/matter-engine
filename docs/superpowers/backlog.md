# Backlog

Long-term ideas captured for later. Not scheduled. Each entry should eventually
graduate into its own brainstorm → spec → plan cycle.

## Draw-into-lattice authoring (multi-resolution lattices)

**Vision.** Build objects by *drawing primitives into a multi-resolution
lattice*, then mesh. Instead of hand-placing particles, an authoring algorithm
draws shapes additively and subtractively into occupancy:

- **Primitives:** sphere, capsule, block, line segment (extensible).
- **Additive / subtractive:** draws add or carve matter.
- **Per-primitive resolution:** some primitives (capsule, block) fill *every*
  resolution tier down to the configured finest detail; others (line segment)
  are drawn at a caller-specified resolution.
- **Mesh after authoring:** all draws complete, then the mesher runs.
- **Composition:** an object may be made from more than one lattice. Ideally
  parts are authored separately, baked to mesh(es), and reused.

**Integration point (already built).** The tiered surface lattice work
(`specs/2026-06-14-tiered-surface-lattice-design.md`) introduces a per-particle
`detail_size` / tier concept that drives mesh resolution. Today the tier is
*computed* from depth-below-surface. The drawing API will *author* it instead:
a primitive drawn at resolution `r` sets `detail_size = r` on the matter it
writes. Same field, different source — no rework of the mesh-resolution path
needed.

**Open questions for its future brainstorm:**
- Lattice representation: stay sparse `Occupancy`, or hierarchical/octree?
- How tiers are stored vs. computed once authoring writes them.
- SDF primitives in the mesher beyond spheres (capsule/box) — and whether the
  shared smooth-min field needs a harder `min` for faceted looks.
- Multi-lattice composition + part baking/reuse pipeline.

## Particle shapes beyond spheres

Rotated rounded-boxes / capsules as alternate particle primitives for richer
surface texture. Deferred from the 2026-06-14 appearance work; revisit only if
finer tiers + mesh resolution don't get the surface out of "looks like a wall."
Requires a box/capsule SDF in the mesher plus per-particle orientation threaded
through `StaticParticle` → cell carve → BLAS bake.

## Ground tileset — remaining phases & follow-ups (post 2026-07-21 merge)

From the tileset Vulkan/parallax effort (spec
`specs/2026-07-21-tileset-vulkan-parallax-macro-design.md`, phases 0-2 +
horizon lighting shipped). Remaining, roughly in priority order:

- **Phase 3 — macro tileset (frequency split).** Author `ForestFloorMacro`
  (16 m tiles) through the unchanged bake pipeline, bind the already-plumbed
  `groundMacroSlot`, composite macro-deviation-from-mean under the detail
  layer (spec §Phase 3). Kills far-field tiling and near-field repetition.
  Blocked on nothing; the live-settle machinery works.
- **Rock/plane datum alignment.** POM recesses the visual ground below the
  mesh plane by `datum_bias`; real props stand on the plane and read as
  floating. Raise `MeadowGround` (and future ground meshes) by the settled
  datum-bias value, or sink scattered props equivalently.
- **Horizon-lighting artist pass.** The occluded-GI local-bounce constant
  (0.35) and reflected-ground sky cap (0.7) are documented approximations;
  the 0.30 m horizon scan radius ignores relief beyond it.
- **GI shimmer tuning.** Steady-state sky-GI crawl (by-design 5% alpha
  floor): firefly clamp before temporal accumulation, longer history with
  anti-lag clamping; later blue-noise/importance-sampled sky rays.
- **Voxel-box impostor integration (trees / heavy props).** The `.vxi`
  research tech in MatterSurfaceLib + the dormant `DrawInstance.is_imposter`
  hook; RT intersection-shader path. Needs a fresh spec against the Vulkan
  pipeline. (A ground voxel-slab variant was considered and declined on
  memory cost — horizon maps chosen instead.)
- **Volumetrics emitter wire audit.** `VkVolumetrics::update_emitters` has
  no caller; chimney smoke/mist emitters may not reach the GPU buffer.
  Pre-existing; investigate when volumetrics content returns.
- **Test hygiene.** `vulkan_smoke_tests` link rot (a written tileset device
  test waits inside it); delete the stale extensionless Linux ELF
  `vk_scene_renderer_tests` that shadows the Windows `.exe`; the three
  tileset GPU test targets still hardcode Linux-only link flags.
- **Restore/replace the full Meadow.** The 816x816 streaming world is
  backed up at `examples/world_demo/backup/`; the current 48 m verification
  meadow (with dense grass) is deliberately minimal. A real world should
  exercise tilesets on slopes (planar-projection stretch) and, with the
  dense grass now in place, provides the load case for the impostor tier.
