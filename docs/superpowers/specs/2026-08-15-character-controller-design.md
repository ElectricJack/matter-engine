# Walking-Mode Character Controller — Design (v2, review-hardened)

**Date:** 2026-08-15
**Status:** Design v2 — supersedes the v1 draft of this file. Every "already exists" and API claim below is verified against the tree at the cited file:line (adversarial review + four verification passes, 2026-08-15). Not yet implemented.
**Depends on:** `2026-07-18-box3d-runtime-physics-design.md` (Box3D runtime physics — which explicitly deferred *character controllers, triangle meshes, heightfields*; this design picks up exactly those), `2026-07-17-flecs-ecs-foundation-design.md`.

## Summary

Add a **walking mode**: a capsule character that walks and slides across streaming
terrain using the engine's existing Box3D physics stack, with slopes steeper than
a configurable limit (default 45°) unclimbable — the character slides back down.

The controller is a **kinematic capsule driven by Box3D's geometric character-mover
query loop** (`b3World_CollideMover` → `b3SolvePlanes` → `b3World_CastMover`,
iterated), grounded by a downward **pogo raycast**, with a **standability gate**
(`dot(n,up) ≥ cos(maxSlope)`) that produces both the "can't climb" and "slide down"
behavior, plus an optional **up-across-down step trace**. It is an ECS component +
a fixed-pipeline system so it is modular: the same controller drives player, NPC,
and scripted movers, differing only in what writes their `MoveIntent`.

Terrain collision — absent today — is added as a **per-sector static collider that
follows the streamer**, in two forms: a **heightfield shape** for heightfield
worlds and a **triangle-mesh shape** for volumetric (cave) worlds, chosen by the
field's own `is_heightfield()` recogniser.

### What v2 fixes over v1 (the six blockers the review found)

| # | v1 was wrong/underspecified about | v2 resolution (verified) |
|---|---|---|
| C1 | `physics_ray_cast` would ground the character on terrain | It casts a **private ECS-entity proxy tree** (`physics_context.cpp:1017,1050`); non-entity terrain bodies are invisible. v2 adds a `b3World_CastRayClosest` wrapper returning shape/body/material. |
| C2 | "45° falls out of `ClipVector`, no special case" | `b3ClipVector` keeps the tangential component → walkable-slope creep **and** steep-slope climb. v2 uses the **pogo-ray + `IsStandableSurface` gate** model from the samples (`sample.cpp:2192`, `sample_character.cpp:856`). |
| C3 | Discrete `SolvePlanes` on full delta → tunneling | The **real** sample loop (`sample.cpp:2234-2261`) sweeps with `b3World_CastMover` each iteration; v2 adopts it verbatim (it differs from Box3D's own `docs/character.md`). |
| C4 | "Step offset" via a down-ray steps down only | v2 ports the 4-phase `TryStep` up-across-down trace (`sample_character.cpp:965`). |
| C5 | Analytic `height_at` safety floor | `height_at` is **cave-blind** (y-independent, `terrain_field.h:126`); on StreamCaverns it teleports to the plateau. v2 gates the floor on `is_heightfield()` and uses a `density_at`>0 guard + hold-until-resident for caves. |
| C6 | Per-sector collider keyed by `(tx,ty,tz)`, "translate to world origin" | Publish-then-evict makes a bare key self-destruct (`sector_streamer.h:319`); y-origin is **mode-dependent**; `lod_mesh_data` is **unwelded** (cross-level holes). v2 keys by level, branches the y-transform, and **skirts** collider meshes. |

## Existing Context (verified)

- **Box3D 0.1.0** (Erin Catto, C17, `b3` API) at `third_party/box3d/`, built/linked,
  driving the live ECS physics subsystem. Supports capsule/sphere/box/hull **plus
  static triangle-mesh and heightfield**, ray/shape casts, overlap queries, and a
  built-in **character mover** (`b3CollideMoverAndMesh`/`…AndHeightField`,
  `b3SolvePlanes`, `b3ClipVector`).
- **flecs ECS + physics** — `PhysicsModule` imported into `ecs_runtime::Runtime`
  (`ecs_runtime.cpp:333`), one persistent Box3D world per `WorldSession`. Fixed
  pipeline: `FixedUpdate → PrePhysics → [Reconcile→Push→Physics→Pull] → PostPhysics`;
  Pull writes back **Dynamic bodies only**, so a `PrePhysics` pose write is
  authoritative for a kinematic body.
- **`CapsuleCollider`** exists, human-scaled (radius 0.5 m, ±0.5 m), with
  `RigidBodyType::{Static,Kinematic,Dynamic}`, gravity −9.81, and imperative
  `physics_ray_cast`/`overlap_sphere`/`teleport`/`set_velocity`/`apply_impulse`
  (`matter/physics.h`). Foot-IK already consumes `physics_ray_cast`.
- **Terrain CPU geometry is resident** per sector: `LoadedPart::lod_mesh_data`,
  a `std::vector<viewer::IndexedPartGeometry>` (flat `float` verts + `uint32_t`
  indices per LOD rung), *"CPU-only; GL upload is lazy"* (`render/part_store.h:148`).
- **Analytic field** — `terrain_field::FieldRuntime`: `height_at(x,z)` (y-independent
  surface), `slope_at(x,z)`, `density_at(x,y,z)` (+`ColumnCache`), `is_heightfield()`
  (`terrain_field.h:126,130,131,150`).
- **Streaming** — `SectorStreamer` policy (`sector_streamer.h`); publish at
  `matter_engine.cpp:7813/8172`, evict funnel `release_sector_entry`
  (`matter_engine.cpp:4929`); off-thread build in `bake_and_stage_sector`
  (`matter_engine.cpp:993/7532`). Resident tiles ≈ **1,200** on StreamMountain
  (not the 6,547 baked-disc figure).
- **Sim model** — Edit/Play/Pause/Step + snapshot-restore (`simulation_control.*`);
  physics advances only in Play. `PhysicsPlayground.js` is the working reference.

### Gaps this design fills

1. **Terrain has no collider.** `physics_shapes.cpp::create_shape` wires only
   sphere/capsule/box/hull — the Box3D world is empty where the ground is.
2. **No runtime input path into the ECS** (only the free-fly camera).
3. **No character controller** — the mover helpers are vendored but unused.

## Decisions of Record

| Question | Decision | Evidence |
|---|---|---|
| Controller model | Kinematic capsule + **geometric mover** query loop (not a dynamic body) | `sample.cpp:2137` `CharacterMover::SolveMove` |
| Mover loop | Iterated ≤5×: `CollideMover`(gather) → `SolvePlanes`(**full** target delta) → `CastMover`(sweep, take fraction) → advance; clip velocity after | `sample.cpp:2234-2261,2298-2308` — **trust the sample over `docs/character.md`** |
| Grounding | Downward **pogo raycast** via a new `b3World_CastRayClosest` wrapper; critically-damped spring holds the capsule at rest length (no creep) | `sample.cpp:2192-2220` |
| Grounding ray API | New engine query hitting the **real Box3D world**, returning `b3ShapeId`/`b3BodyId`/material — `physics_ray_cast` cannot see non-entity terrain | `physics_context.cpp:1017,398-419`; `box3d.h:98` |
| Slope limit / slide-down / anti-climb | `standable ⇔ dot(n,up) ≥ cos(maxSlope)`; non-standable ground → drop pogo support + add downhill gravity (slides) and fail the step gate (can't climb). Default 45°, with hysteresis | `sample_character.cpp:856-860,882-892` |
| Step traversal | 4-phase up-across-down `TryStep` (forward→up→across→down), bounded by `stepUpHeight` (~0.45 m); reject if landing not standable | `sample_character.cpp:965-1094` |
| Self-collision | Exclude own shapes in the `b3MoverFilterFcn`/`b3PlaneResultFcn` by shape id | `sample.cpp:2068,2100` |
| Pushing dynamics | Per-plane `pushLimit` from shape user-data + explicit `b3Body_ApplyLinearImpulse` to contacted dynamic bodies | `sample.cpp:2263-2296`, `types.h:1811` |
| Terrain collider — heightfield worlds | `b3CreateHeightFieldShape` per sector, sampled from `height_at`, shared global min/max for seam-free alignment (no BVH, can't overflow) | precedent `tileset_settle.cpp:106-138` |
| Terrain collider — volumetric worlds | `b3CreateMeshShape` per sector from `lod_mesh_data`, `useMedianSplit=true`, `identifyEdges=true`, **NULL-checked** | `types.h:2043`, `mesh.c:27,974` |
| Collider key | `{tx, ty, tz, level}` where `level = matter_stream::variant_level(rung)` — **never bare `(tx,ty,tz)`** | `sector_streamer.h:83-85,319`; `matter_engine.cpp:1372` |
| Collider world transform | Flat mode: translate x,z (y already world-absolute). Volumetric mode: translate x,y,z. Branch on `world_volumetric_sectors` | `terrain_mesher.cpp:798-799`; `matter_engine.cpp:7934-7953` |
| Seam holes | `lod_mesh_data` is **unwelded**; add a downward/outward **skirt** (≥1 coarse voxel) on −x/−z/−y collider faces. Do not reuse the render welder | `terrain_mesher.cpp:449-454`; `part_store.h:148,187` |
| Collider residency | Only resident tiles within radius **R** of a character; **finest-available** rung ≤ collision budget (distant nested tiles have only coarse rungs, `rung = −level`) | `sector_streamer.h:64-65`; `terrain_mesher.cpp:347-381` |
| Build threading | Build the b3 shape on the **bake worker** in `bake_and_stage_sector`; bounded **attach** at publish, bounded **detach** in `release_sector_entry` | `part_store.h:281-293`; `matter_engine.cpp:7532,7813,4929` |
| Mesh lifetime | Store `b3MeshData*` per collider; on evict destroy **shape/body first, then `b3DestroyMesh`** | `box3d.h:807`; `tileset_settle.cpp:142-145` |
| Anti-fall-through | Heightfield worlds: analytic floor at `height_at`. Volumetric: `density_at>0` push-out guard (via `ColumnCache`) + **hold vertical integration until the foot sector is resident** (new `SectorStreamer::resident_at`) | `terrain_field.h:126,150`; `sector_streamer.h:364-414` |
| Streaming anchor | The active character becomes the **single** streaming owner via **release-before-claim**; restore the prior anchor on exit/Stop | `streaming_systems.cpp:27-64` (single-owner `OwnerAlreadyClaimed`) |
| ECS placement | `CharacterController` (data) + `MoveIntent` (intent) components; system in `PrePhysics` | pattern `transform_system.cpp:237` |
| Move intent seam | Controller reads `MoveIntent`; whoever writes it (keyboard/AI/script/net) is pluggable |  |
| Jump edge event | `jump` is a **latch** consumed-and-cleared in the fixed-step system (the accumulator runs 0 or ≥2 steps/frame); continuous axes are level-sampled | `ecs_runtime.cpp:584-606`; pattern `simulation_control.cpp:61-67` |
| Entity constraints | Player/NPC must be a **hierarchy root**, **unit scale**, **exactly one collider** | `physics_shapes.cpp:242-269` |
| Physics API boundary | Mover + world-ray + mesh/heightfield attach exposed in `matter/physics.h`; no `b3*` type escapes | mirrors existing `physics_ray_cast` wrapping |
| Sim mode | Walking is Play-mode only; `CharacterController` state + anchor tag added to snapshot whitelist | `simulation_control.h:20-37` |
| Authoring | `CharacterController` + `MoveIntent` added to the `ComponentKind` schema | `scene_registry.h:16` |

## Design

### 1. Terrain collision — per-sector collider following the streamer

**Two shapes, chosen by the field recogniser** (`FieldRuntime::is_heightfield()`,
a reliable parse-time flag — `true` for world_demo/StreamMountain, `false` for
StreamCaverns):

- **Heightfield worlds → `b3CreateHeightFieldShape`.** Sample `height_at(x,z)` on
  the sector's `countX×countZ` grid, set a **world-shared** `globalMinimum/MaximumHeight`
  so adjacent sectors quantize identically (seam-free by construction, `types.h:2237`),
  optional per-cell `materialIndices` for friction. No BVH → cannot overflow, one
  `uint16`/point. Exact precedent in `tileset_settle.cpp:106-138`.
- **Volumetric worlds → `b3CreateMeshShape`.** Build a `b3MeshDef` directly from the
  chosen rung's `IndexedPartGeometry` (both arrays are layout-compatible —
  `float[3N]` reinterpret-casts to `b3Vec3*`, `uint32_t`→`int32_t` is safe below
  2³¹, no per-triangle copy). Set `useMedianSplit=true` (**this**, not a triangle
  cap, is the real fix for the `B3_MESH_STACK_SIZE 256` DFS overflow — grid meshes
  are the SAH worst case; median split gives height ≈ log₂(tris) ≈ 14 for a 0.75 m
  64 m sector), `identifyEdges=true` (internal-edge smoothing; mesher winding is
  consistent outward CCW, `terrain_mesher.cpp:1201-1234`), and **NULL-check**
  `b3CreateMesh` before `b3CreateMeshShape` (converts the near-impossible overflow
  from a NULL-deref into a recoverable skip). Collision is two-sided
  (`mesh.c:2270`), so winding can't drop the player through the floor.

**Lifecycle (mirrors rendering residency), keyed by `{tx,ty,tz,level}`:**

- **Build off-thread** in `bake_and_stage_sector` (`matter_engine.cpp:7532`),
  alongside the staged geometry — `b3CreateMesh`/`b3CreateHeightField` is unbounded
  O(tris) work and must not run in the publish job.
- **Attach** (bounded) at publish (`matter_engine.cpp:7813/8172`): add the static
  body to the Box3D world under the level key.
- **Detach** (bounded) in `release_sector_entry` (`matter_engine.cpp:4929`): destroy
  the body/shape, **then** `b3DestroyMesh(meshData)`.
- Keying by **level** (not bare tile, not scatter-tier rung) is mandatory: a rung
  upgrade **publishes-then-evicts** the old variant (`sector_streamer.h:319`); a bare
  key would let the trailing eviction destroy the freshly-upgraded collider. Reuse
  `matter_stream::variant_level(rung)`.

**World transform** applied to the tile-local vertices, **branched on mode**
(getting this wrong collapses every tile into one 64 m band):

- Flat/column (`world_volumetric_sectors == false`): `world = (vx + tx·S, vy, vz + tz·S)` — y is already world-absolute.
- Volumetric (`world_volumetric_sectors == true`): `world = (vx + tx·S, vy + ty·S, vz + tz·S)`.

where `S` is the tile's **level** sector size (`S₀ << level`). Box3D static shapes
take a world transform, so hand it the translation rather than pre-transforming.

**Seam skirts.** `lod_mesh_data` holds only the tile's own **unwelded** mesh; the
runtime welder's output is render-only and absent on disk-staged tiles. Cross-level
boundaries leave a measured 0.88–1.12 fine-voxel gap — capsule-radius-sized, and
since the anchor is the player these holes track the player. Fix: extend each
collider mesh with a **downward/outward skirt** (a vertical collision curtain
≥ one coarse voxel = `2·voxel`) on the under-reaching −x/−z/−y faces. Invisible
colliders pay no visual cost for the skirt the render path can't use.

**Residency = published ∧ within radius R of a character.** Never the full ~1,200-tile
disc — the capsule can only touch nearby ground, so a few tiles. Per tile pick the
**finest rung it actually has** whose voxel ≤ the collision budget; distant nested
tiles top out at `rung = −level` (16 m at level 4), so "always 1 m" is impossible by
construction and must not be assumed.

### 2. Character controller — kinematic geometric mover

Kinematic, not dynamic (a dynamic body fights slopes/steps and can't cleanly
express slide-down). The spine is the geometric `CharacterMover`; slope-limit and
step logic are ported from the dynamic `RigidbodyCharacter` sample onto it (they
are conceptually separable — a normal·up gate and a shape-cast sequence).

**Per fixed sub-step** (`PrePhysics`, before the Box3D `Physics` step), the real
iterated loop from `sample.cpp:2234-2261`:

```
target = pos + dt*velocity + dt*pogoVelocity*up          // pogo spring term
for iter in 0..4:
    planes = b3World_CollideMover(pos, capsule, filter=excludeSelf)   // gather FIRST
    delta  = b3SolvePlanes(target - pos, planes).delta               // FULL target delta
    frac   = b3World_CastMover(pos, capsule, delta, filter=excludeSelf)  // swept → anti-tunnel
    pos   += delta * frac
    if |delta*frac|² < tol²: break
velocity = clipVelocity ? b3ClipVector(velocity, planes) : (pos - startPos)/dt
```

**Grounding & no-creep — pogo ray:** cast `b3World_CastRayClosest` down
`3·radius + radius` from the capsule's lower center; on hit, a critically-damped
spring holds the capsule at `pogoRestLength` above the surface (it floats, so it
never settles/creeps downhill). Miss ⇒ airborne ⇒ integrate gravity. This is why
v1's "`ClipVector` absorbs downhill creep" was wrong — the spring, not the clip,
provides no-creep.

**45° = slide-down + can't-climb, via the standability gate:**
`standable(n) ⇔ dot(n, up) ≥ cos(maxSlopeAngle)` (default 45°, stored as cosine).

- **Grounding:** the pogo hit only counts as ground if its normal is standable.
  On a steep face the pogo support is dropped ⇒ gravity integrates ⇒ the capsule
  **slides down**, and we add the downhill gravity projection explicitly for a
  positive slide (not merely un-arrested fall).
- **Anti-climb:** a steep plane still blocks penetration through `SolvePlanes`
  (you can't pass through the mountain), but because it is never "ground" you gain
  no foothold, and the step trace rejects it — so you cannot ratchet upward.
- **Hysteresis:** classify with a small band around the limit (e.g. enter-steep at
  46°, exit at 44°) so marching-cubes normals dithering near 45° don't stutter.
- **Ceilings (`dot(n,up) ≤ 0`):** non-standable by definition; handled as walls by
  `SolvePlanes`/`ClipVector` (head-bump), never as ground.

**Steps — ported `TryStep` (4 shape casts):** forward probe → up (bounded by
`stepUpHeight` ≈ 0.45 m) → across → down; commit only if the landing surface is
standable and the rise is real. A down-ray alone (v1) can only step *down*; this
is what climbs stairs.

**Self-exclusion & push:** the player's own capsule body is in the world, so the
mover filter (`b3MoverFilterFcn` for casts, `b3PlaneResultFcn` for planes) drops
the player's own shape ids. Dynamic props get a per-plane `pushLimit` from shape
user-data and an explicit `b3Body_ApplyLinearImpulse` for a physical shove.

### 3. Anti-fall-through (residency-independent backstop)

- **Heightfield worlds:** clamp the capsule base to `height_at(x,z)` (minus a skin)
  whenever no collider is yet resident. Exact, cave-free, free, always available.
- **Volumetric worlds:** `height_at` is meaningless under an overhang. Use two
  guards: (a) `density_at(cache, y) > 0` ⇒ the point is inside solid ⇒ march the
  `ColumnCache` to the nearest zero-crossing and push out (a hard "never inside
  rock" backstop, independent of residency); (b) **hold vertical integration**
  until the foot sector is resident, via a new 3-line
  `SectorStreamer::resident_at(tx,ty,tz)` accessor (or a resident-set fed by
  `Coordinator::acknowledge(...,published)`). (a) alone can't stop a fall through
  open cave air before the floor bakes; (b) covers that; together they're safe.

### 4. ECS integration & modularity

```cpp
struct CharacterController {          // data
    float radius = 0.4f, height = 1.8f;
    float move_speed = 4.5f;
    float max_slope_cos = 0.7071f;    // cos(45°)
    float step_up_height = 0.45f;
    float jump_speed = 5.0f;
    // private runtime: Float3 velocity; bool grounded;  (snapshotted)
};
struct MoveIntent {                   // the decoupling seam
    Float3 move_dir{};                // continuous, level-sampled
    bool jump = false;                // edge latch, consume-and-clear
    bool sprint = false;
};
```

- **`CharacterControllerSystem`** in `PrePhysics` (registered like
  `transform_system.cpp:237`), reads `MoveIntent`+`CharacterController`+`LocalTransform`,
  runs §2 via a new `physics_move_character(...)` wrapper, writes the pose (Pull
  won't clobber it — kinematic).
- **`MoveIntent` writers are pluggable** — keyboard bridge now, AI/net later, same
  component. This is the "modular / future game" requirement.
- **Jump latch:** written once per render frame on the key-down edge; **consumed and
  cleared in the fixed-step system** (the accumulator runs 0 or ≥2 steps/frame, so
  clearing on the render side would drop or double-fire it) — mirrors
  `SimulationControl::consume_pending_step`.
- **Entity constraints (enforced by `validate_desired_body`):** the player must be a
  hierarchy **root** (no `ChildOf`), **unit scale** (±1e-5), and carry **exactly one**
  collider. `CharacterController`/`MoveIntent` are not colliders and pass through.
- **Runtime velocity** lives in `PhysicsVelocity` where possible (already
  snapshot-whitelisted); anything in `CharacterController` (grounded flag, cached
  velocity) is added to the snapshot (see §7).

### 5. Input → ECS bridge (editor, new)

During Play + walk-mode, read WASD/Space/Shift via `glfwGetKey` in `main.cpp`
(reuse the math in `camera_controller.cpp:87`), compose a camera-relative
`move_dir`, and write the player's `MoveIntent` **before** `session->tick`. The
only genuinely new subsystem.

### 6. Camera & mode toggle (editor)

A walk/fly toggle beside the existing TAB capture. In walk mode, after the tick,
drive `CameraDesc` from the character's `WorldTransform` (eye at capsule top,
yaw/pitch from mouse). The character becomes the streaming anchor while walk mode
is active (§7).

### 7. Streaming anchor, snapshot, authoring

**Anchor hand-off (single-owner — `OwnerAlreadyClaimed`).** On walk-mode enter:
stash the current owner + `StreamingAnchorState`, `remove<SectorStreaming>` from
the controller anchor entity (fires release/detach), `add<SectorStreaming>` to the
player (now claims), and stop camera-follow so it doesn't fight player motion. On
exit/Stop: reverse it. Note the player is recreated on Stop (it has `SceneEntityId`),
which auto-releases ownership; re-add the tag to the controller anchor explicitly.

**Snapshot whitelist additions (`simulation_control.*`):** the current whitelist
omits `SectorStreaming` and `ConvexHullCollider`. Add `CharacterController` (+
`has_character_controller`) and `has_sector_streaming` to `EntitySnapshot`, capture
them in `capture_snapshot`, restore in `restore_snapshot`, add the include.
`MoveIntent` is transient — do **not** persist it.

**Authoring — full schema fan-out.** Adding `CharacterController` + `MoveIntent` to
`ComponentKind` touches (compiler-forced exhaustive switches marked ✓, silent
sites marked ⚠):
- `scene_registry.h:16` enum; `scene_registry.cpp` field arrays + `s_descriptors[]`;
  `instantiate()` JSON→ECS switch (✓ `:744`).
- `main.cpp` `component_fetch`(✓`:148`)/`store`(✓`:173`)/`add`(⚠`:344`)/`remove`(⚠`:366`).
- `scene_service.cpp` `add_kind`(✓`:14`)/`remove_kind`(✓`:30`)/`copy_components`(⚠`:50`).
- `scene_change_tracker.cpp` `append_component_names`(⚠`:20`) + observers(`:136`).
- Optional custom editors: `specialized_editors.cpp:12`, `properties_panel.cpp:530`.
- Tests: `scene_registry_tests`, `scene_tracker_tests`, `specialized_editors_tests`.

```js
this.entity({ id: "player", name: "Player", components: {
  LocalTransform: { translation: [0, 50, 0] },   // root, unit scale, one collider
  RigidBody: { type: "kinematic" },
  CapsuleCollider: { radius: 0.4 /* … */ },
  CharacterController: { move_speed: 4.5, max_slope_angle_deg: 45 },
}});
```

## Milestones

| # | Milestone | Scope | Verification |
|---|-----------|-------|--------------|
| M0 | Input→ECS bridge + `MoveIntent` + jump latch | GLFW→player intent; consume-and-clear | Headless: set intent, assert system reads/clears it across 0/2-step frames |
| M1 | World-ray query + terrain colliders | `b3World_CastRayClosest` wrapper (C1); heightfield-shape path + mesh-shape path (level-keyed, off-thread, skirted, y-branched); player streaming anchor; anti-fall-through backstop | New ray hits terrain; drive a sphere onto terrain, assert rest height on both a heightfield and a cavern sector; walk a level boundary with no fall-through |
| M2 | Character controller | Geometric mover loop + pogo grounding + 45° standability (slide/anti-climb) + hysteresis + `TryStep` + self-exclude | Ramp scene: climb ≤45°, slide >45°; stair scene: step up; flat scene: no creep |
| M3 | Camera-follow + walk/fly toggle + jump/gravity | Editor wiring, playable | Manual + `drive.py` shot on StreamMountain **and** StreamCaverns |
| M4 | JS schema + polish + NPC | Schema fan-out; snapshot; coyote-time/air-control; a second `MoveIntent`-driven NPC (note: only one streaming anchor — NPCs outside the resident disc need the volumetric backstop) | Author player in scene JS; NPC mover smoke test; Play→Stop restores state |

M0–M3 yields a playable prototype; each milestone is independently testable (M1
before any controller exists, M2 in flat/ramp/stair scenes before terrain).

## Remaining Risks

- **Two reference movers don't compose cleanly** (`sample.cpp` geometric vs
  `sample_character.cpp` dynamic). v2 commits to the geometric spine + ported gate
  + ported step; M2 must prove the graft (pogo grounding with a standability gate
  is not a shipped combination). If it fights, fall back to the dynamic
  `RigidbodyCharacter` wholesale and revisit the kinematic decision.
- **Mover API is flagged experimental** in `docs/character.md`. Pin the vendored
  Box3D commit; treat mover-helper changes as a supply-chain risk.
- **`identifyEdges` ghost snags** — validate flat + convex-ridge traversal in M1;
  fall back to `identifyEdges=false` (accept internal-edge bumps) if needed.
- **Skirt overlap vs. dynamic props** — a downward skirt must not collide with
  props resting in the sector below; keep skirts thin and terrain-category-only.
- **Anchor churn while walking** — level splits/merges under a moving player must
  not thrash colliders within R; hysteresis on R and level keying mitigate.

## Out of Scope (with load-bearing caveats)

Swimming/flying, **crouch** (note: StreamCaverns tunnels may need it — descope the
world or add it), **moving platforms** (note: a scripted mover *carrying* the player
is one — `b3Body_CollideMover` exists for this; the Summary's "scripted movers"
means self-propelled, not platforms), networked prediction, ragdoll, animation
locomotion blending (foot-IK exists, layer later), and a per-frame JS gameplay tick.

## Key Files (verified)

- `third_party/box3d/include/box3d/{box3d.h:98,118,123,807; collision.h:331,380,639,643; types.h:2043,2217}`, `src/{mesh.c:27,974,2270; shape.c:962}` — world ray, mover, mesh/heightfield build, overflow, two-sided collision
- `third_party/box3d/samples/{sample.cpp:2137-2308; sample_character.cpp:856-1094}` — the reference movers
- `MatterEngine3/include/matter/physics.h` — API to extend (world-ray, mesh/heightfield attach, `physics_move_character`)
- `MatterEngine3/src/ecs/{physics_context.cpp:1017,886; physics_shapes.cpp:239,423}`, `ecs_runtime.cpp:333,584` — ray plumbing, kinematic push, validation, registration
- `MatterEngine3/src/ecs/{scene_registry.h:16, scene_registry.cpp:744; simulation_control.*:20}`, `scene/{scene_service.cpp, scene_change_tracker.cpp}` — schema + snapshot
- `MatterEngine3/src/{render/part_store.h:148,187; sector_streamer.h:83,319; terrain_field.h:126,150; terrain_mesher.cpp:798,449,1201}`, `matter_engine.cpp:{4929,7532,7813,7934}` — CPU mesh, streaming, field, y-transform, publish/evict
- `MatterEngine3/src/{tileset_settle.cpp:100; tileset_collider.h}` — heightfield-shape precedent
- `MatterEditor/src/{main.cpp:148,344,1331; camera_controller.cpp:87; streaming_anchor_controller.cpp}`, `streaming_systems.cpp:27` — input, schema sites, anchor
- `projects/world_demo/scenes/{PhysicsPlayground/PhysicsPlayground.js, StreamCaverns/StreamCaverns.js:250}` — authoring + volumetric reference
