# Phase C: ExplorerDemo — Meadow Valley

**Date:** 2026-07-08
**Status:** Approved design. Single combined spec covering world content at scale,
engine work, the demo app, and packaging.
**Roadmap:** Phase C of `2026-07-07-engine-editor-roadmap.md` (first public post).
**Contract note:** Written against the Phase B design
(`2026-07-08-phase-b-async-bake-design.md`) while Phase B execution finishes. A
reconciliation pass against the merged Phase B API happens before the
implementation plan is finalized.

## Goal

A downloadable Windows demo: a ~10x-larger Meadow — meadow valley, foothills,
flyable baked mountain range — that any machine computes entirely from code. No
shipped cache. The world is flyable within one minute of first launch; detail is
uniform everywhere and the bake continuously refines toward wherever the camera
flies.

**Headline exit criterion:** someone with no dev tools unzips a ~tens-of-MB zip
and is flying through a whole-valley silhouette in under 1 minute on an
RTX 3060-class machine, with terrain refining to meadow-grade detail ahead of
them as they fly.

## Locked decisions

- **Audience:** dev communities (Reddit/HN) + social clips; soft launch to
  friends first. Staged camera shots during initial assembly are the clip
  footage.
- **Content:** one world ("Meadow Valley"), ~10x area, flyable baked mountains.
- **Detail model:** constant level of detail everywhere; only *when* a region
  bakes varies, driven by camera proximity. No permanently-coarser outer ring.
- **Distribution:** tiny zip, no cache files. Every machine cold-bakes from
  code. Local cache persists after first run, so later runs start instantly.
- **Target hardware:** recent gaming PC (RTX 3060-class), GL 4.6 driver.
- **Tree content:** Jack iterates on Tree.js himself, on his own track. Phase C
  does not depend on or block on tree state; the demo ships whatever the tree
  track has produced.

## 1. World design: Meadow Valley

Grow from 256×256 to **~810×810 units (≈10x area)** in three concentric bands:

- **Center (~256×256):** today's Meadow, roughly as-is — spawn point, densest
  scatter.
- **Foothills:** rising terrain, sparser scatter (rocks, thinning grass),
  transitional noise blending meadow into slopes.
- **Mountain range:** full flyable peaks. Same heightfield pipeline
  (`terrain_noise::heightAt` + per-tile quad-grid parts) extended with ridged
  mountain octaves; slope/height-based material (rock/snow high, grass/dirt
  low). Mountains are big-amplitude terrain tiles, not a separate sculpted-part
  pipeline.

**Uniform detail target:** every tile has the same full-resolution bake target
(64×64 quads + full QEM ladder) and per-band scatter density rules. At ~51
tiles/side that is ~2,600 terrain tiles and roughly 100–150k instances
(outer bands are scatter-sparse).

**World seed:** promote `SCATTER_SEED` to a single world seed driving both
terrain noise (currently unseeded — seeding it is in scope) and all scatter
placement. "New seed" rerolls the entire valley.

## 2. Engine work

All four pieces extend Phase B machinery; no new subsystems.

### 2.1 Camera-driven bake scheduler

Phase B's worker drains a command loop with no priority notion. Add a
priority-queue mode: jobs keyed by distance from a **bake focus point** set by
the app each frame via `WorldSession::set_bake_focus(pos)` (kernel stays
camera-agnostic). Re-sorting happens at Phase B's existing between-parts
checkpoints — no new cancellation machinery. Priority bands:

1. **Coarse pass** (fixed high band): every tile at low resolution (e.g. 8×8
   quads) so the whole valley silhouette lands in the first minute.
2. **Full-res refinement + scatter** by distance-to-focus, continuously
   re-sorted as the camera moves.

The queue never has to finish; the world sharpens toward wherever you fly.

### 2.2 Two-pass tiles via content addressing

Coarse and full-res tiles are the same schema with different resolution params
→ different part hashes → both live naturally in the cache. Refinement =
publish full-res part, retire the coarse instance. No cache format changes.

### 2.3 Residency policy (part-level)

`.part` files keep the full QEM ladder on disk. Residency is at the **part
level**, which the two-pass tile design provides naturally: coarse tile parts
stay resident everywhere; full-res tile parts are loaded whole within a camera
radius and evicted whole on radius exit (new `release_part` seam in
GpuCuller/PartStore — the current architecture stores all LOD rungs in one
monolithic VBO per part, so per-rung residency is not viable and is not
attempted). This bounds VRAM at 3060 scale regardless of world size. It is
the seed of real streaming later without claiming to be that yet.

### 2.4 Seed plumbing + scale hardening

World seed threads into `terrain_noise` and all scatter RNG. GPU culler and
flatten validated at ~100–150k instances / ~2,600 tiles (dynamic slot growth
exists; per-part flatten memory is unchanged, so the known OOM mode gets no
worse).

### 2.5 New events

Refine-progress events alongside Phase B's bake events, so the app can show
both "world assembling" (coarse pass done/total) and "sharpening near you"
(refine queue depth).

### Explicitly out (engine)

- Imposters stay unintegrated (QEM's 1% rung covers 810-unit distances).
- No sector unload/reload of world structure.
- Deferred OOM fixes #2/#3 (per-part flatten budget, streaming flatten).

## 3. ExplorerDemo app

New top-level sub-project `ExplorerDemo/` (standard monorepo layout, ~1–2k
LOC). Kernel-only per the dependency rule: links `libmatter_engine3.a` +
raylib; consumes only the four public headers (`engine_context.h`,
`world_session.h`, `events.h`, `query.h`). No imgui — loading UI is raylib
overlays; the app doubles as the canonical "what a game would do" reference.

**Frame loop:** poll events → `pump_gpu_jobs(budget)` →
`set_bake_focus(camera.pos)` → `tick` → `render` → overlay. App owns window,
GL 4.6 context, and loop (Phase A boundary).

**Camera:** fly camera, mouse/kb + gamepad, speed modifiers, defaults tuned to
valley scale.

**Loading experience:** no separate loading screen — in the world from frame
one. Progress strip shows bake events plus refine-queue depth. During the
initial coarse pass the camera runs staged moves (slow meadow orbit, pull-back
revealing the range); any input grabs control. These shots are the social-clip
footage.

**New seed:** escape menu offers regenerate/new-seed. Vegetation part variants
are seed-independent, so a reroll only cold-bakes terrain and re-flattens —
faster than first run, still visibly assembles.

**Errors:** `BakeError` events render as a non-fatal toast; skip-and-continue
means a bad part never kills the demo.

## 4. Packaging & distribution

**Zip contents:** `explorer.exe` (monolithic: static raylib, embedded
shaders), `WorldData/` (world manifest + `.js` schemas — content is source,
and this audience will read it), `README.txt`. Total in the tens of MB. Cache
directory is created beside the exe on first run and persists.

**No shipped cache** retires the roadmap's warm-cache assumption and promotes
the coarse pass to a hard requirement: the 1-minute time-to-flying number is
delivered by the coarse pass alone, from nothing, and is what the scheduler is
tuned against.

**Shader warm-up** (~60s in the viewer today) counts inside the 1-minute
budget. If it serializes before the coarse pass, compile-while-baking overlap
or a trimmed demo shader set is in scope.

**Build:** `make windows` target mirroring MatterViewer's. Usual disciplines:
clean rebuild after struct/header changes; `make windows` after every engine
change.

**README:** controls, sysreq (GL 4.6, RTX 3060-class recommended), and a short
technical explanation (content-addressed procedural parts, camera-driven
refinement) — for this audience the explanation is part of the demo.

## 5. Testing

- **Kernel additions** (scheduler, `set_bake_focus`, residency, seeds):
  headless tests in `MatterEngine3/tests`, wired into `build-all.sh test`.
  Per-task runs use only covering suites; full sweep at the final gate.
- **Refinement correctness:** headless — coarse part publishes, full-res
  supersedes, coarse retires; instance counts/slots consistent across many
  focus moves.
- **Seeds, structural not byte-level:** same seed → same part-hash set and
  instance counts. Byte-level cold-bake determinism is a known pre-existing
  gap; not gated here.
- **Residency:** scripted flight across the valley asserts resident-fine-rung
  count stays under the radius-derived cap.
- **Valley visuals:** via MatterViewer's FIFO harness (`viewer_shots.sh`,
  `GALLIUM_DRIVER=d3d12`). ExplorerDemo gets a self-terminating smoke mode
  (env var: run N seconds, screenshot, quit).
- **Wall-clock gates:** time-to-flying ≤ 1 min and steady frame rate during
  bake-behind, measured on the Windows build. Friends soft-launch is the
  clean-machine test.

## 6. Risks

| Risk | Mitigation |
|------|------------|
| Coarse pass misses the 1-min budget | Coarse resolution is a knob; center-out priority; shader-compile overlap in scope if measurement demands it |
| VRAM/RAM at 10x | Residency radius is a knob; measured early in the plan, not at the end |
| Phase B API drift (unmerged at spec time) | Reconciliation pass against merged API before the plan is finalized |
| Combined-spec size | Plan sequences the three streams (world content → engine at scale → app/packaging, interleaved where useful) with the demo milestone protected from scope creep |
| Tree content state | Owned by Jack on a separate track; demo ships whatever that track has produced; no Phase C dependency |

## Out of scope

- Imposters in the render path
- Sector streaming / world unload
- OOM fixes #2/#3 from ROADMAP.md
- Any EditorLib/editing features (Phase D+)
- Web/WASM builds; non-Windows packaging
