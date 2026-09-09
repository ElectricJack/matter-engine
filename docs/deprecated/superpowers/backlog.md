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

## Memoize the per-frame dynamic command-layout relayout (animation/physics)

**Deferred deliberately.** This does NOT affect pure static streaming worlds
(StreamMountain, Meadow) — those never enter the code path. Pick it up when
animation/physics content in a *large streamed* world becomes a target. Captured
from a 2026-08-07 Fable analysis of the "O(world) command-layout" family.

**The cost.** `VkSceneRenderer::apply_dynamic_command_layout()`
(`MatterEngine3/src/render/vk_scene_renderer.cpp`, ~line 8679) re-walks **every
cluster in the entire resident static world × kVkMaxLod (9)** to re-derive each
draw command's `first_instance` transform-bucket offset. It runs from
`prepare_frame()` (~line 9461) whenever `dynamic_instance_count_ > 0` (plus one
trailing frame to restore the static baseline), i.e. **every frame any dynamic
entity slot is bound**, against a 16.6 ms budget. Estimated ~0.5–2 ms/frame at
streaming scale.

**Who actually pays.** The dynamic lane is fed *only* by
`update_dynamic_instances()` (~line 12233), which consumes Bind/Transform/Remove
`DynamicSlotChange` records from `matter::render::DynamicInstanceSlots` —
animated / physics / gameplay entities carrying an `entity_id` and
`animation_instance_slot`. Static streamed geometry (terrain, vegetation,
sectors) goes through the *static* `update_instances` path and never touches
`dynamic_instance_count_`.

- **Gate:** presence of even one dynamic object.
- **Cost:** scales with the *total static resident world*, not the entity count.
- **Worst case:** one animated character in a big streamed world → the full
  O(resident-world) relayout fires every frame, forever, for that one entity.

**The fix (≈30 lines, provably output-identical).** The function is documented
idempotent and is a pure function of exactly three inputs: `part_instance_counts_`
(the static baseline, never mutated here), `dynamic_instance_part_slots_` (the
merged deltas), and the static template's identity. Memoize:

1. Stamp a `static_layout_generation_` from every successful
   `rebuild_command_template()`.
2. Cache `last_applied_merged_counts_` + **`last_applied_dynamic_staging_size_`**
   + `last_applied_static_layout_generation_`.
3. If all three match this frame, **skip the cluster walk** — idempotence
   guarantees `command_template_[].first_instance`, `skin_transform_base_`,
   `draw_transform_slots_`, and `part_command_ranges_` already hold the walk's
   output.
4. Invalidate the memo in `rebuild_command_template` success, `release_part`,
   and `reset()`.

Dynamic entity sets are stable frame-to-frame, so this becomes a per-frame
O(parts) vector compare, firing the full walk only on spawn/despawn/static-rebuild
frames.

**Two traps.**
- The memo key **must** include `dynamic_instance_staging_.size()`, not just the
  counts: `draw_transform_slots_ = first_instance + staging.size()` (~line 8740)
  and the skin tail (~line 9218) size off it, so an inactive (`UINT32_MAX`) slot
  growing the staging changes the layout without changing any merged count.
- The restore frame (`dynamic_command_layout_applied_ && count == 0`) should just
  run the full walk — rare, and it re-establishes byte-equality with the static
  baseline.

**Safety net (shared with the Loop #1 static-rebuild work, if that lands too).**
Keep `rebuild_command_template` as an oracle: a `MATTER_VK_VERIFY_CMD_LAYOUT=1`
debug path that runs the full recompute into scratch after any fast path and
memcmps `command_template_` + all parallel arrays (`raster_command_enabled_`,
`part_instance_counts_`, `part_command_ranges_`, `skin_transform_base_`,
`draw_transform_slots_`), aborting on mismatch. Extend the CPU-vs-GPU cull
comparison in `MatterEngine3/tests/vulkan_smoke_tests.cpp` (`run_cpu_cull` /
`run_vk_cull`) with a dynamic add/remove/re-add sequence and assert the cull
`overflowed` stat stays 0 — that stat is the canary for a silently dropped draw
(the "dynamic entities cast RT shadows but never rasterize" failure class,
comment ~line 8663).

**Risk:** low. A *skip* cannot produce a layout the full path wouldn't; the only
bug surface is memo invalidation, which the oracle catches. Contrast with the
sibling **static `rebuild_command_template` cascade** (per-publish, affects *all*
streaming worlds) — that one is NOT a safe incremental target (prefix-sum ripple
from shared-part instance growth + interior range recycling) and should be
profiled with `MATTER_VK_BUILD_PROFILE=1` before any change; see the 2026-08-07
analysis notes. This dynamic-relayout memo is the clean, isolated half.

## Seam welder — emit the overlap band only where the fan failed to land

**Why it exists.** The runtime welder (M0, `MatterEngine3/src/seam_weld.cpp`)
closes a cross-level plane with *two* mechanisms that are deliberately additive:

- the **vertex fan**, which joins both tiles' own boundary vertices 2:1 through
  `floor_div2` — the true weld, exact, zero redundant area;
- the **overlap band**, the fine side's surface extended two voxels past the
  plane (the revival of the retired `reach`), copied verbatim from the mesher.

The band is emitted **unconditionally** for every cross-level plane
(`seam_weld.cpp:103`), but it only *has* to exist where the fan cannot land:
`stats.missing_coarse_pair` (`seam_weld.cpp:209`) — the coarse side has no
vertex pair near the plane at all, because the feature is smaller than a coarse
voxel, so there is no second side to weld to. Measured holes there run
1.75–12 m, so the band is not optional; it is just far larger than the residue
it covers. In `seam_integration_tests.cpp` roughly **4% of crossings** hit
`missing_coarse_pair`, meaning the large majority of the emitted band is a
redundant sheet of fine surface laid over coarse surface that the fan already
joined correctly.

**What that redundancy costs.** Nothing correctness-wise — coverage is a union
and the tests scan mesh+fan, mesh+band and mesh+fan+band separately. But the
sheet is real geometry: weld triangles, BLAS/TLAS entries, and a second surface
a few centimetres off the coarse one, which is what makes weld *shading* defects
visible as ribbons instead of as invisible hole-fill. Two such defects were
found and fixed in 2026-08-11 (`7173c974` pre-tape `material_index`, `f382a616`
triplanar hemisphere) precisely because the band paints over ground that was
already correct. Shrinking it shrinks the blast radius of the next one, and it
is the cheap alternative to the rejected "draw welds in a second stencil-masked
pass" idea — which would need a stencil-capable depth format (today
`VK_FORMAT_D32_SFLOAT`, `vk_scene_renderer.cpp:2547`), a per-instance pass flag
`cull.comp` does not have, and would fix only the raster lane while leaving the
sheet in the TLAS.

**The work.**
1. Run the fan loop first and record the tangential cells that exit via
   `missing_coarse_pair` (and, arguably, `missing_fine` — decide deliberately;
   `missing_fine` means a tile is about to arrive, so covering it may be wrong).
2. Emit band triangles only where they overlap that set, dilated by one cell so
   the band's edge still meets the fan's.
3. Restructure `weld_plane` so the band copy happens *after* the fan rather than
   before it (today it is at the top of the function).

**The trap.** `OverlapBucket` (`seam_boundary.h:169`) is raw triangle soup —
world-absolute positions and normals, **no cell index**. Filtering therefore
needs either (a) the tangential cell derived per triangle from position, the
plane axis and the fine rung's voxel size (feasible with no mesher change; use a
conservative any-vertex-in-set test), or (b) a parallel per-triangle cell index
added to the bucket at export time in the mesher. Prefer (a) first — it keeps
the band a verbatim mesher artifact, which is the property the design note at
`seam_weld.cpp:83` says makes it a faithful revival of `reach` rather than a
second guess at it.

**Gate.** The existing union-coverage rows harness must stay at zero gaps for
every drawn cross-level pairing, and `crack_scan.py` on a StreamMountain
fly-through must not gain components. Report band triangle counts before/after
in the `seam-weld summary`; the expected win is roughly an order of magnitude.

**Priority: low.** Filed 2026-08-11 after the two shading defects were fixed —
with those closed the remaining artifacts are not bad enough to schedule.
