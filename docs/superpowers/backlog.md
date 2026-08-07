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
