# LOD and Virtual Texture: How It Actually Works

Date: 2026-08-04
Scope: the whole path from world JavaScript to lit pixels, for LOD geometry and ground texturing.
Method: four parallel read-only code investigations, with the load-bearing claims re-verified by hand. Nothing was built or run; timing figures are quoted from the code's own instrumentation comments and are marked where that matters.

This document answers one question: **why is this system so complicated, and what is the minimum it could be?**

The short answer is at the end of §2. The rest is the evidence.

---

## 1. The pipeline, top down

Seven stages. Each is individually well built; the trouble is at the seams.

### Stage 1 — World JavaScript becomes resolved instances

A world's `.js` is read **three separate times by three separate QuickJS evaluators**: the statics loader (`world_definition_loader.cpp:1759`) for roots and settings, a ScriptHost (`matter_engine.cpp:3091`) for `field()`/`surfaces()`/`biomes()`, and a fresh per-sector ScriptHost during streaming (`matter_engine.cpp:4254`). A shared regex (`find_world_class_name`) exists solely so the first two agree on which class is the world — the duplication is acknowledged in a comment rather than removed.

Worlds split into two mutually exclusive kinds. **Closed worlds** (Demo, Meadow) go through `compose_world`, which produces a fixed manifest up front. **Streaming worlds** (StreamMountain) skip that entirely and receive an empty manifest; all geometry arrives later, one sector at a time.

The unit that comes out is a `ResolvedInstance`: a part hash, a `stable_id`, a transform, and hierarchy stamps. `stable_id` is load-bearing twice over — it keys the expansion memo *and* it keys impostor hierarchy ownership — and a duplicate fails the entire frame.

### Stage 2 — Parts are baked into artifacts

Everything that produces geometry goes through one function, `ScriptHost::bake_source` (`script_host.cpp:2238`). That part is clean. What it produces is not.

For a static part it runs `bake_adaptive_static_lods`, which builds a LOD ladder by re-meshing the part at progressively coarser voxel resolutions and optionally simplifying. **This ladder is where most of the trouble originates** — see §3.

The bake writes `parts/<hash>.part`, and depending on policy also `.flat.part`, `.fimp`, `.impostor`, `.lods`, `.static_lods`, `.hints`, and a chart trailer. Twelve artifact kinds, of which several are never read (§4.3).

### Stage 3 — Sectors stream in

`SectorStreamer` maintains rings around an anchor, assigning each cell a desired rung, with hysteresis on demote and evict, and publish-then-evict ordering so there is never a hole. This machinery is genuinely good: generation tagging prevents late completions from corrupting residency, and rollback is transactional.

Sector bakes run on a worker pool. What remains on the render thread is one indivisible `stream.publish` job per sector — commit, world-state apply, tracer reset, registration. Successive optimisations have made that job *smaller* (conversion and decode moved off it) but never made it *divisible*, so one slow publish still blows a frame.

### Stage 4 — Staged parts become renderer instances

`PartStore::stage_from_snapshot` takes the artifact's finest rung and rebuilds a runtime ladder. For terrain it calls `bake_terrain_lods`; for everything else it may use the serialized ladder.

`build_expansion` then walks the part tree in pre-order, emitting one node per part with geometry, depth-capped at 8 (deeper trees are silently truncated). Each node becomes a `VkSceneInstance` stamped with `hierarchy_id` and `hierarchy_depth`.

Those two fields exist for exactly one feature: far-impostor subtree suppression. The encoding is "a selected impostor owns every *following* record sharing its id at greater depth" — which makes **emission order load-bearing across four files that never reference each other.**

### Stage 5 — Something decides which LOD to draw

Seven live selectors (§4.1). The authority is the GPU cull shader, which walks a per-cluster threshold table and picks the first rung whose threshold the projected size clears.

As of this branch all six camera-driven selectors share one helper, `viewer::lod_projected_size`, and agree on units, scale convention, reference point, pixel budget and LOD bias. That is real progress. But unification was achieved by making six call sites *call the same function*, not by removing six call sites.

### Stage 6 — Ground texture is composited

Despite the name, **"VT" here is not a virtual texture.** There is no on-disk page store and no page cache; the header says so outright. Pages are composited on the GPU at runtime, every session, from the rung's mesh, its chart table, the ground tileset slices, and the world's surface tape.

The `.gtex` ground tileset is a separate offline-baked Wang atlas carrying albedo, normal, ORM, height, and two horizon channels. It is what the POM march steps.

### Stage 7 — Draw

The cull shader buckets each instance by `(cluster, lod)`, reserves a transform slot, and the raster pass issues indirect draws. Impostors bypass this entirely — one non-indirect draw of six vertices per impostor instance.

---

## 2. The thesis

**Four independently-designed systems each acquired a dependency on "the drawn LOD rung," and none of them owns that concept.**

- the adaptive LOD ladder,
- the chart-VT page pool,
- the `.gtex` ground tileset and its POM march,
- the warp field.

Each solved a real problem, each is defensible in isolation, and each reads the rung. Nothing arbitrates. Every cross-cutting defect in this document is a consequence: the dark dome patches, the per-rung texture cost, the impostor silence, the frame-time floor.

The code is not sloppy — invariants are stated, failure modes are named, and one header corrects its own earlier wrong diagnosis in writing. **The complexity is structural, not cultural.**

---

## 3. The defect at the centre: the synthetic error schedule

The adaptive ladder assigns each rung an "error" using `prior + 0.9 × remaining`. This is not a measurement of anything — it exists to force a strictly increasing sequence so thresholds come out distinct. Computed from the real formula with shipped defaults:

| rung | threshold | change |
|---|---:|---|
| 0 | 0.040394 | — |
| 1 | 0.036721 | −9.1% |
| 2 | 0.036391 | −0.9% |
| 3 | 0.036358 | −0.09% |
| ∞ | 0.0363542 | — |

Projected size is inversely proportional to distance, so **every rung after the first switches within about 1% of the same camera distance.** You get two usable rungs regardless of how many you bake. A three-rung branch and a nine-rung branch behave identically — which is exactly the reported symptom that branch LODs never visibly change.

Two consequences compound it:

- **The simplification budget collapses to zero.** The QEM epsilon is the gap between the schedule and the voxel error; once voxel error catches up, the epsilon is *exactly* 0, disabling simplification precisely on the coarse rungs where it is the only remaining lever.
- **Anything above 8,192 triangles is never simplified at all.** So the assets that most need a ladder get one rung.

**No amount of tuning `lodPolicy` fixes this, because every knob feeds a number that is not measured against anything.** This is the single highest-value thing to change.

---

## 4. Inventories

### 4.1 Seven live LOD selectors

The GPU cull shader (authority); the CPU mesh-rung mirror; VT demand; the RT/BLAS pick; skin-queue planning; the terminal-impostor selector (a separate comparison against a threshold living in a different buffer); and the camera-free RT proxy. Plus `SectorLodResolver`, which computes a per-instance LOD level that **nothing in any shipping build reads**.

### 4.2 Five ladder generators, four threshold formulas

Adaptive static; the hardcoded `BakeTargets` ladder (animated parts); the terrain ladder; a third re-implementation for legacy artifacts; and the flatten epsilon search, itself duplicated verbatim in two functions.

Three different notions of "error". Four threshold formulas, **two of which never see the pixel angle** — a part falling back to them is calibrated to no camera at all. Two flatten producers fold `pixel_budget` into the baked value while every runtime selector multiplies by it again, so those ladders are budget-*squared*.

### 4.3 Twelve artifact kinds, several write-only

Confirmed produced and never consumed: `.static_lods` and the `LMSK` trailer (the only reader is the writer's own cache probe — roughly 160 lines of machinery); the chart-atlas serializer (complete, functional, no production caller — so every staged part rebuilds its chart atlases from scratch instead).

`world_flatten.cpp` is test-only scaffolding.

### 4.4 Seven texture-data paths, five live

The chart-VT page pool; the `.gtex` tileset; the `.gtex` horizon channels (never composited into pages, sampled live); tier-2 hemisphere AO enrichment; the impostor atlas; the legacy flat fallback; and a dead GL uploader.

Two independent horizon formats with the same semantics and two separate bakes. Two impostor producers baking the same object twice from two independent hierarchy walks under different source modes.

### 4.5 Confirmed dead

`shaders_gpu/cull.comp`, `gpu_cull_types.h`, `world_composer.*`, `raster_cull.h::cluster_lod_select`, `tileset_provider.*` (zero production references — and the last GL dependency in the engine build), `tileset_macro_slot` and its public C API, `ResolvedInstance::segment`.

---

## 5. Confirmed defects, with evidence

**Per-rung charting.** `build_chart_rung` is called from six sites, once per rung, with no reference to any other rung — different segmentation, chart count, atlas size and packing. A rung change is therefore a *complete texture-data change*, not a mip change. Cost scales as rungs × parts: a five-rung sector wanted at three rungs costs three variants, three pinned pages, three tables, three CPU mesh copies and three independent GPU bakes **of the same surface**. This is the root cost driver in the texture system.

**Terrain bakes a ladder nobody reads.** A `WorldSector` is an ordinary static Part, so it gets the adaptive ladder — roughly nine QEM plus reprojection plus BVH passes, and about twice its mesh in serialized bytes — and `part_store.cpp:1166` discards it unconditionally. It also inflates the StreamMountain cache from 387 MB to 6.6 GB, and it caused the texture-warping bug fixed in `9f30561e`.

**Occlusion reaches a pixel through five channels.** Vertex AO; the page's ORM.r (already containing both the `.gtex` AO bake and tier-2 hemisphere AO); a live `.gtex` AO ratio; the horizon mean; and RT ambient applying the mean again. Three strength knobs, no single owner. Every clamp and floor in that stack is a guardrail against the product reaching zero.

**The horizon query is asked in the wrong frame.** Horizon data is baked in the tile's own frame. The query depends on four per-rung quantities: the drawn mesh's interpolated normal, the warp tangent frame (a nearest-triangle *reprojection* on every rung above 0), the marched UV, and the footprint derivatives. A baked, view-independent, tile-frame quantity interrogated through a per-rung mesh-dependent basis. This is the open dark-patch defect; the boundary tracks the rung split pixel for pixel. Six debug modes for diagnosing it already exist.

**The instance buffer is copied and re-uploaded every frame.** Roughly 14 MB each at 90k instances, because the partition generation increments unconditionally — defeating the very counter built to prevent it. In the same file the author elsewhere replaced `operator=` with a geometric resize to stop a per-frame reallocation, and this path then does `operator=` on a larger vector. Suppressed instances also remain in the dispatch and command sizing, so a far tree costs one impostor draw *plus* thousands of retained records.

**Impostors are silently absent — and it is a cache-identity fault.** Folding the representation-bake version into the impostor asset hash changed every asset hash. Verified by recomputing all 20 links in the Demo cache: **exactly 10 reproduce their hash under current rules, and exactly 10 only under the pre-change formula.** The orphaned half fails `far_imposter::load`, and `adopt_object_far_imposter` swallows that with a bare `return` and a `catch(...)` — **no log line anywhere.** The renderer's one diagnostic is unreachable because the asset never loaded. A total loss of a rendering tier produces zero diagnostics.

**The cache hole one level down.** The representation version reaches the resolve key and the impostor hash but **not the part resolved hash**. Change the ladder algorithm, bump the version, the resolve cache correctly misses — and re-resolution produces the *same* part hashes, so every stale `.part` with its stale ladder is served. The exact bug the version was introduced to fix, one level lower.

**Three unrelated meanings of "sector"** at three pitches in one frame: the streamer's 2D tiles, a 3D spatial grid at pitch 16, and the JS module. No shared code, no shared coordinate convention.

---

## 6. What the minimal coherent design looks like

**One measured error.** Replace the synthetic schedule with a real or genuinely conservative geometric error, and replace the triangle cap with a time budget. Everything in §3 is downstream of this.

**One threshold conversion**, applied at bake, with `pixel_budget` and `lod_bias` applied *only* at runtime. Delete the five bypasses.

**One selection, computed once on the GPU**, emitting a representation id per instance. Make the impostor rung *N+1* of the same threshold table rather than a parallel CPU comparison against a different buffer — then the existing cull walk selects mesh-or-impostor with no new code, and the entire per-frame partition (two state machines, a full array copy, an unconditional re-upload) deletes.

**One consumer of that result.** VT demand, RT and skin planning read the selected id rather than re-deriving it, removing three selectors by construction rather than by discipline.

**One parameterisation per part, shared across rungs.** This is the big one for texturing: it collapses variants roughly fivefold, deletes the cross-rung transition problem, and removes the redundant composite bakes.

**One impostor producer, one artifact set.** Six artifact kinds instead of twelve.

---

## 7. Ranked recommendations

1. **Log the impostor load failure.** One line. It converts an entire invisible failure class into an obvious one, and would have saved this session hours. Print the resident impostor count in the editor stats while you are there.
2. **Fix the error schedule.** It is the reason LOD "doesn't work" — you have two usable rungs no matter what you bake.
3. **Fold the representation version into the part hash**, closing the cache hole so ladder changes actually invalidate parts.
4. **Stop baking the adaptive ladder for terrain sectors.** Pure waste, and it has already caused one visual bug.
5. **Ask the horizon question in the tile's frame.** Closes the open dark-patch defect.
6. **Gate the partition generation on an actual change.** Removes ~14 MB of copy and upload per frame.
7. **Delete the dead paths** — four GL selection files, the write-only artifacts, the dead tileset provider. Behaviour-free, and it removes two entries from the selector list.
8. **Then** consider the structural items: one parameterisation per part, one selection authority, one impostor producer.

Items 1–4 are small and independently valuable. Items 5–7 are contained. Item 8 is the actual architecture, and is worth doing deliberately rather than incrementally.
