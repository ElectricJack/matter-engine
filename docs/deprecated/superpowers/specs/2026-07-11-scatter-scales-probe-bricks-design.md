# Scatter Scale Distributions + Tiered Per-Sector Probe Bricks

**Date:** 2026-07-11
**Status:** Approved by Jack (design conversation, phase-c-explorer-demo worktree)
**Scope:** ExplorerDemo / MeadowWorld infinite world

## Goals

1. Much denser grass (~5x current).
2. Wide, noise-shaped scale variation: trees 1–3x, grass 1–5x, biased by the
   same patch channels that decide placement.
3. Restore baked lighting (light probes) in the infinite streamed world, where
   the existing one-shot `compose_world` probe bake never runs.

## Non-goals

- No shader changes for probes (existing `useProbes` / `probeAmbient` /
  `probeDominant` / `probeOrigin/Cell/Dims` path is consumed as-is).
- No probe re-bake on scatter tier promotion (grass/rocks barely affect
  bounce light).
- No disk cache for probe bricks (bake once per sector residency).
- Skirts/overlap strips for terrain remain forbidden (unrelated, standing
  policy).

## Part 1 — Scale distributions (`WorldSector.js` only)

Shared shape: `s = base + spread * raw^power`, where `raw` blends a
per-placement random with the local strength of the patch channel that gated
the placement. The noise that decides *where* things go also biases *how big*
they get; the power skews the distribution long-tail (most small, giants
rare). All inputs are tier-independent, so placements and scales stay stable
as tiers change underfoot (same as today).

- **Trees (1.0–3.0x):**
  `groveStrength = clamp((g - 0.10) / 0.90, 0, 1)` (g = grove patch value),
  `raw = 0.65 * c.v + 0.35 * groveStrength`,
  `s = 1 + 2 * raw^1.7`.
  Typical tree lands 1–1.6x; near-3x giants only deep in grove cores.
  `TREE_MIN_DIST` stays 9m — canopy overlap in cores reads as a thicket.
  Root sink is already `0.4 * s` and scales correctly.
- **Grass (1.0–5.0x):**
  `tuftStrength = clamp((t + 0.05) / 1.05, 0, 1)` (t = tuft patch value),
  `raw = random()^2.5 * (0.5 + 0.5 * tuftStrength)`,
  `s = 1 + 4 * raw`.
  Mostly 1–2x; occasional 4–5x clumps at tuft centers.
- Exact exponents/blend weights are tunable by eye during implementation;
  the ranges (1–3, 1–5) and the noise-bias requirement are fixed.
- Boulders/rocks/pebbles unchanged.

## Part 2 — Grass density (`MeadowWorld.js` only)

- `meadow.grass: 600 → 3000`
- `foothills.grass: 160 → 800`

Attempt loop already caps at the biome count. Expected ~150k grass instances
resident in the near ring (each is one flat draw instance post-flatten).
Sector bake time rises (2x attempts × slope/biome queries) — bakes are async
and never block frames. Flight smoke gate verifies fps holds.

## Part 3 — Tiered per-sector probe bricks

### Why not the existing path

`compose_world()` bakes one static probe grid over `tracer.world_bounds()` —
requires a finite AABB, never runs for the streamed world-kind path
(`matter_engine.cpp` install_world + publish_pipeline with empty manifest),
so ExplorerDemo falls back to flat `ambientColor`.

### Components

**Brick store** — per resident sector, a CPU `probe_volume::ProbeVolume`
brick. Cell size follows the streamer ring (same tiers as scatter):

| Ring | Cell size |
|------|-----------|
| near (≤3 sectors) | 4m |
| mid (≤8 sectors)  | 8m |
| far (40 sectors)  | none — flat ambient |

Vertical extent: sector mesh AABB + ~24m canopy pad (not the 256m world
slab). Bricks are freed on sector eviction; baked once per residency; tier
promotion does NOT re-bake a brick (a near-ring re-entry after full eviction
does).

**Baker** — background job per sector, nearest-to-camera first, reusing
existing `probe_bake::bake_probes` + `world_tracer::WorldTracer` over a TLAS
of the currently-resident instances. The TLAS is rebuilt per *epoch* (when
the resident set changes materially) and shared across all brick jobs of that
epoch. BLASes already exist (built at part load). `WorldLights` come from the
world definition exactly as in the compose path. A freshly streamed sector
renders with flat ambient for a few seconds until its brick lands — accepted.

**Compositor** — GPU representation is unchanged: one camera-centered 3D
texture window pair (existing `probe_texture` upload), ~256×128×256m at 4m
cells (64×32×64). For each window cell, trilinearly sample the containing
brick at that world position (whatever the brick's density); cells with no
brick get the flat-ambient color and sun-visibility 1.0 default. Window
re-centers when the camera moves far enough; re-center and brick-arrival both
trigger a re-upload, throttled (≥0.5s apart). No re-tracing on movement.

**Render wiring** — engine sets `useProbes` when the window has ≥1 composited
brick. Shader untouched.

### Failure handling

- Brick bake failure: log once per sector, sector keeps flat ambient.
- Cancelled/evicted mid-bake: job checks a cancellation token, result
  discarded.

## Testing

1. **Compositor unit test** — synthetic bricks with known gradients →
   window resample correctness (including no-brick default cells and mixed
   4m/8m brick densities).
2. **Brick bake smoke** — sector-stream suite asserts bricks appear for
   resident sectors and are freed on eviction.
3. **Flight smoke gate** — extended to require `probe bricks baked > 0`;
   existing gates (zero bake errors, fps summary, resident sectors, clean
   exit) stay green.
4. Visual judgment on native Windows by Jack.

## Build order

1. Scatter scales + grass density (quick, visible, independently shippable).
2. Probe bricks.
