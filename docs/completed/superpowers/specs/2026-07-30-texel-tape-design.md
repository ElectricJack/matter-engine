# Texel-Rate Tape: Per-Texel Classification, 3D Noise, Appearance Lanes

Design spec, 2026-07-30. Builds on the chart-VT system
(`2026-07-29-chart-virtual-texturing-design.md`, glossary at
`MatterEngine3/docs/chart-vt-glossary.md`). Covers three interlocking changes:

1. **Per-texel tape evaluation** — a GPU interpreter for the `surfaces()` tape
   inside the page compositor (weight-seam **mode 3**), replacing barycentric
   interpolation of per-vertex u8 weights.
2. **3D noise ops** — `noise3` / `ridge3` (part-local) and `noise3World` /
   `ridge3World`, with built-in domain warp, plus a `fract` unary for strata
   banding.
3. **Appearance lanes** — tape outputs beyond material weights: per-texel
   albedo tint, roughness bias, and wetness, applied at composite time.

They ship as one programme because (1) is what makes (2) and (3) visible —
today any tape signal finer than mesh vertex spacing is filtered out before
the compositor ever sees it — and because the GPU interpreter's instruction
set, content keys, and test harness should be touched once, not three times.

## 1. Problem

Classification resolution is mesh resolution. `SurfaceRuntime::classify_vertices`
(terrain_field.h) evaluates the tape per chart **vertex**, quantizes to u8, and
`vt_composite.comp` mode 2 interpolates those columns barycentrically. On a
1–2 m-vertex terrain rung, a material boundary is a meter-scale triangle-shaped
gradient sitting on a page with 128 texels of unspent resolution. Crisp snow
edges, lichen patches, thin strata lines are unrepresentable.

All tape noise is 2D over (x, z). A cliff face is vertical: (x, z) noise
projected onto it smears into stretched vertical stripes. The only y-varying
signal a cliff can receive today is raw `altitude`.

The tape's only output is material weights. Low-frequency albedo variation
(the classic anti-tiling megatexture win) and curvature-driven
wetness/roughness have no channel to flow through.

## 2. Goals / non-goals

Goals:

- Classification, noise, and appearance evaluated **per texel at page-fill
  time** — paid once per fill, zero per-frame cost, identical in raster and RT
  (both sample the same pages).
- 3D fbm/ridge noise usable on any surface orientation; strata banding
  expressible in a few tape lines.
- Deterministic pages: same content key ⇒ bit-identical page, preserving the
  double-bake `cmp` gate.
- Fail-soft everywhere: any new-path failure drops to the existing mode-2 /
  legacy behavior, never to black.

Non-goals:

- Per-instance texturing (unchanged: identity is the variant).
- Running the terrain **field program** on the GPU. Field-derived inputs stay
  CPU-evaluated (see §4.3 — the hybrid-rate contract).
- Changing the near-band handoff, aux encoding, or page/pool geometry.
- Per-material tint (appearance lanes apply to the composited result; §6).
- sin/turbulence/voronoi ops — deferred until a tape wants them.

## 3. Change 2 first: op-set additions (CPU side)

New `Op::Kind`s in terrain_field.h (append-only; the canonical-text grammar is
the compatibility surface, and new line forms cannot collide with old hashes):

| Canonical text | Semantics |
|---|---|
| `noise3 seed freq oct gain lac [wseed wfreq wamp]` | 3D value-noise fbm over **part-local** (x, y, z) |
| `ridge3 seed freq oct gain lac [wseed wfreq wamp]` | 3D ridge variant (1 − 2·abs(n − 0.5) per octave) |
| `noise3w seed freq oct gain lac [wseed wfreq wamp]` | same, over **world** (x, y, z) — world-anchored variants only, fallback constant elsewhere (same rule and warn-once diagnostic as `noise2w`) |
| `ridge3w seed freq oct gain lac [wseed wfreq wamp]` | ridge, world 3D |
| `fract rN` | x − floor(x); unary |

DSL surface (world_base.js.h `__surfaceArg`):

```js
s.noise3(seed, freq, oct?, gain?, lac?, warp?)      // warp = {seed, freq, amp}
s.noise3World(seed, freq, oct?, gain?, lac?, warp?)
s.ridge3(...), s.ridge3World(...)
node.fract()
```

**Domain warp is a parameter, not an op.** The field program's `warp2` is a
stateful coordinate-modifying op; that shape is wrong for the tape's pure
expression DSL and is why `warp2` was excluded from the surface op set. Instead
each 3D noise op optionally warps its own sample point:
`p' = p + amp · (noise3(p·wfreq, wseed+axis) · 2 − 1)` per axis (three seeds
derived `wseed`, `wseed^0x9e37`, `wseed^0x7f4a`). Self-contained, one op, and
covers ~90 % of real usage (organic boundary shapes). Op struct grows three
literal fields (`wf0..wf2`); struct layout is internal, text is canonical.

3D value noise extends the existing 2D primitive: `hash3i(ix, iy, iz, seed)`
with the same avalanche mix as `hash2i` plus a third multiply-fold, trilinear
interpolation with the same smoothstep fade as `value_noise`. Bit-exact
integer hashing is the determinism anchor for the GPU twin (§4.4).

The strata idiom this enables (documented in the glossary when it lands):

```js
const bandY = s.altitude.add(s.noise3World(seed^0xB1, 1/23, 3, 0.5, 2.0).mul(6));
const band  = bandY.mul(1/9).fract();                  // 9 m strata period
const strat = band.smoothstep(0.05, 0.12).mul(band.oneMinus().smoothstep(0.02, 0.08));
```

Budget: the dedup'd op cap (`FieldRuntime::kMaxOps`) started at 64. The P4
authoring pass proved 64 binding (strata, speckle, and seep terms were cut to
fit at 64/64), and P2 telemetry showed per-op cost harmless, so the cap was
raised to 96 post-P4. Registers stay u8-addressable (< 0xFF sentinel).

This phase ships alone and is immediately useful: the per-vertex path picks up
the new ops (coarse but correct), and worlds can start authoring against them.

## 4. Change 1: GPU tape interpreter (weight-seam mode 3)

### 4.1 Contract

`vt_material_weights` gains **mode 3**: evaluate the variant's compiled tape
per texel from the texel's reconstructed surface sample; keep top-2 with the
same strict-`>` ascending-scan tie rule, same normalization, same
zero-coverage fallback to the TriEx id as mode 2. Mode 2 remains fully
functional — it is the escape hatch (`MATTER_VT_TAPE_GPU=0` forces it) and the
comparison baseline in tests.

A part promotes to mode 3 when it carries a tape (`surface_tape_hash != 0`),
mirroring how mode 2 promotion works today (`vt_compositor.cpp` ~1086); the
debug-ramp override still wins for tests.

### 4.2 Tape upload

Per registered part, the compositor packs the parsed `SurfaceProgram` into a
flat GPU instruction stream:

```
struct GpuSurfOp {            // std430, 48 B
    uint kind_oct;            // kind | (oct << 16)
    int  a, b, c;             // register operands (-1 unused)
    float f0, f1, f2, f3;     // value/freq/gain/lac/edges
    float wf0, wf1, wf2;      // warp freq/amp + spare (seed derived from `seed`)
    uint seed;
}
```

Ops live in one shared device-local SSBO (arena, same allocator pattern as the
chart/tri buffers in `CompositorPart`); `VtFillRequest`/the GPU request record
gains `tape_ops_offset` / `tape_ops_count`. 64 ops × 48 B = 3 KiB per variant
worst case — noise against the existing per-part chart/tri footprint.

The existing `tape[4]` material-id table in the GPU request is reused
unchanged (ids for weight columns; `tape.z` = column count).

Registers: `float regs[64]` in shader function scope. Bounded loop over
`tape_ops_count`, switch on kind. Divergence is nil (all threads of a fill run
the same tape); occupancy cost is the register file — measured in Phase 2, and
the fallback if it hurts is splitting the fill dispatch, not shrinking the
contract.

### 4.3 Inputs — the hybrid-rate contract

Per-texel inputs the shader computes directly:

- `lx/ly/lz` — the reconstructed part-local position (already computed for
  detail projection).
- `ny`, `slope` — from the interpolated shading normal (already interpolated
  for normal re-projection).
- `wx/wy(altitude)/wz` — `local_to_world · pos`. The fill request gains the
  variant's 4×3 row-major transform (world-anchored variants have exactly one
  instance, so this is per-variant, not per-instance; identity when not
  world-anchored, in which case world inputs already fall back per the
  existing rule).
- `noise2/ridge2/noise2w/ridge2w/noise3*/ridge3*`, `fract`, all arithmetic —
  evaluated in-shader.

**Field-derived inputs stay at vertex rate.** `height`, `moisture`, `relief`,
`biome`, `fieldSlope`, and every `curv` op are full field-program evaluations;
porting the field interpreter to GPU is out of scope and unnecessary — these
signals are smooth at vertex scale by construction (curvature's default probe
radius is 4 m; vertex spacing on rung 0 is 0.5–2 m). They are precomputed
per vertex on the CPU (exactly where `classify_vertices` runs today),
stored as **f16 vertex lanes**, and interpolated barycentrically in the
shader. A tape op whose value is field-derived compiles to a lane read:

- At pack time the CPU scans the tape: each *distinct* field-derived input,
  and each `curv` op with a *distinct radius*, is assigned a lane index.
- Lane cap: **8**. A tape needing more fails the mode-3 promotion for that
  part and stays on mode 2 (warn-once, fail-soft). No real tape is near this
  (StreamMountain uses 0 field lanes at texel rate — its `macro/patch/fine`
  are `noise2w`, which moves to the GPU).
- `GpuTri`'s `wA[4]/wB[4]` u8 weight columns (32 B) are **replaced** by
  `lanes[4]` (8 × f16 = 16 B) per vertex-slot when the part is mode 3;
  mode-2 parts keep the weight-column packing. The struct is internal to
  compositor + shader and versions with them.

This split is a documented contract, not an accident: *field queries at vertex
rate, everything else at texel rate.* Authors get texel-rate noise, banding,
and boundaries; field-shaped masks (gully wetness via `curv`) remain smooth —
which is what they are physically.

### 4.4 Determinism

- Integer hashing (`hash2i`/`hash3i`) is bit-exact between CPU and GPU by
  construction (uint arithmetic).
- Float fbm accumulation is IEEE mul/add in a fixed order; the GLSL twin uses
  `precise`-qualified accumulation to block fma contraction reordering.
  Same driver + same inputs ⇒ same page bits: the double-bake `cmp` gate and
  `MATTER_VT_DEBUG_GENERATIONS` audits hold.
- CPU-vs-GPU equality is **not** required bit-exact (CPU eval feeds only the
  legacy fallback's per-vertex argmax and tests). Tests compare at vertices
  with tolerance 1e-3 on weights; a top-2 flip within tolerance at a vertex is
  accepted by comparing blended *appearance*, not ids.
- Content key: page/tail keys already fold the tape hash; the new line forms
  change the hash when used. Fold the **weight-seam mode** and a bumped
  `kVtBakeVersion` into the key so flipping `MATTER_VT_TAPE_GPU` invalidates
  cleanly instead of mixing modes across resident pages.

### 4.5 What mode 3 deletes

Per-vertex u8 weight **columns** become unnecessary for mode-3 parts (the
weights they encoded are recomputed per texel). `classify_vertices` itself
stays: the legacy fail-closed path still bakes tape-argmax vertex materials,
and mode-2 parts still pack columns. Net vertex payload for mode-3 terrain
drops 32 B → 16 B (or → 0 when no field lanes are read; lanes are allocated
only when used).

## 5. Change 3: appearance lanes

New tape output directives, recorded after `weight()` like `material` lines:

| Canonical text | DSL | Range (clamped at eval) | Composite effect |
|---|---|---|---|
| `tint rR rG rB` | `s.tint(r, g, b)` | each [0, 2], default 1 | `albedo *= tint` after height blend |
| `roughbias rN` | `s.roughnessBias(n)` | [−0.5, 0.5], default 0 | `orm.g = clamp(orm.g + bias)` |
| `wetness rN` | `s.wetness(n)` | [0, 1], default 0 | `albedo *= mix(1.0, 0.55, w)`; `orm.g = mix(orm.g, 0.08, w)` |

Rules:

- At most one directive of each kind per tape (parse error otherwise).
- Applied to the **composited** texel (after top-2 height blend, before BC
  encode), in the order tint → roughbias → wetness. They deliberately do not
  vary per material: the use case is macro variation *across* a surface
  (anti-tiling drift, gully wetness), which is orthogonal to which material
  won. Per-material tint would multiply the packing surface for little gain —
  revisit only with a concrete authoring need.
- Wetness is one scalar driving a fixed, documented response (darken + gloss).
  It is an authoring primitive, not a physical water model.
- Baked into pages ⇒ zero runtime cost, identical in raster/RT and the near
  band (the near-band detail ratio is mean-preserving over the *modulated*
  base, so tint/wetness survive the handoff unchanged).
- Legacy fallback path **ignores** appearance lanes (it is already
  classified-but-flat; fail-soft, documented).
- Aux channel unchanged — near-band detail selection still keys off
  (dominant, secondary, blend).
- Requires mode 3 in practice (per-vertex tint at u8 would reintroduce the
  resolution ceiling); on a mode-2 part the lanes evaluate per vertex and
  interpolate — allowed, but the spec's promise of texel-rate variation holds
  only under mode 3.

Idiomatic payoff (the gully-wetness example this was designed around):

```js
// P4 field note: on StreamMountain-scale terrain, radius 4 with 0.5–2.5 m
// edges is a near-no-op — usable gully signal needs radius 8–12 with
// 0.2–2.0 m edges. Tune per terrain; the original (4, 0.5–2.5) numbers are
// kept out of the example so they don't get cargo-culted.
const gully = s.fieldCurvature(8).smoothstep(0.25, 2.0);  // metres deep
const drift = s.noise3World(seed^0xC4, 1/140, 3).mul(0.10).add(1.0);
s.tint(drift, drift, drift.mul(0.98));                    // ±10 % value drift
s.wetness(gully.mul(s.slope.oneMinus().smoothstep(0.2, 0.6)));
```

## 6. Phases

1. **P1 — CPU ops** (fable-tier math): `hash3i`/`value_noise3`/`fbm3`/ridge +
   warp parameter + `fract`; parser, DSL recorders, `eval_world_tests` +
   surface-tape unit vectors (golden values, warp on/off, world-anchored
   fallback). Ships alone.
2. **P2 — GPU interpreter** (fable-tier): `GpuSurfOp` packing + arena, field
   lane scan/packing, mode-3 shader interpreter + GLSL noise twin, transform
   plumbing, content-key fold, `MATTER_VT_TAPE_GPU` gate.
   Tests: `vt_compositor_tests` mode-3 suite — CPU/GPU vertex cross-check
   (§4.4 tolerance), boundary-sharpness fixture (a step-function tape must
   resolve in ≤ 2 texels where mode 2 smears ≥ 1 vertex span), determinism
   double-fill `cmp`, lane-cap overflow → mode-2 fallback, and a
   `vulkan_smoke_tests` mode asserting 0 validation errors + page hashes.
3. **P3 — appearance lanes** (opus-tier): parse + directives + composite
   apply + tests (identity defaults ⇒ bit-identical pages vs P2; each lane's
   golden fixture).
4. **P4 — StreamMountain payoff** (opus-tier): strata on the rock walls,
   tint drift, gully wetness; A/B screenshot pass at the standard camera set;
   STATSVT fill-time telemetry before/after to size the op-budget question.

Each phase lands green on the full gate (headless suites + smoke modes) before
the next starts; P2 is the only phase that touches runtime shaders and carries
the escape hatch.

## 7. Risks

- **Fill-time regression** (P2): 64-op tapes with several 3–4-octave fbm calls
  per texel × 136² × (8 + 16) fills/frame. Mitigations: ops execute only for
  pages of tape-carrying parts; fills are already budgeted per frame;
  telemetry lands with P2 and the budget envs (`MATTER_VT_FILLS_PER_FRAME`)
  already bound worst-case cost. Not expected to matter on the 4090; measured
  before P4 tunes content.
- **Register pressure / occupancy** in the interpreter loop: measured in P2;
  fallback is dispatch splitting, never contract shrink.
- **fma nondeterminism** across driver updates: pages are a cache, not an
  artifact — a driver update invalidating visual byte-stability is acceptable;
  the determinism gate runs same-session.
- **Vertex-lane cap** surprises: warn-once names the tape and the input that
  overflowed; mode-2 fallback keeps the world correct.
