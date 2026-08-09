# Habitat tape: ecology as data, evaluated by the tape machine we already have

Sketch — 2026-08-08. No code changed by this note.
Context: `docs/terrain-nested-sector-lod-2026-08-08.md` §"Follow-on: scatter, not
VT, is the bake".

## The problem this solves

`sampleHabitat` is ~99% of a sector bake: 14 JS `fbm` calls per candidate,
~105 us, against ~3,385 candidates per 64 m cell. Measured exchange rates
(200k iterations, empty-loop baseline subtracted):

| operation | ns/call |
|---|---|
| `__moistureAt` — boundary + constant field | 567 |
| `__heightAt` — boundary + 4-octave noise field | 562 |
| `hash2` — JS | 463 |
| `valueNoise` — JS (4x hash2) | 2,298 |
| `fbm` 3 octaves — JS (~12x hash2) | 7,488 |

Two things follow. A native binding's price is *entirely* the crossing — a
4-octave field evaluation costs the same as reading a constant. And a QuickJS
call is already 463 ns, so binding `hash2` itself would be a LOSS. The cut has
to be at `fbm` or coarser.

But `sampleHabitat` is game content, not engine content. Hardcoding an alpine
ecology into the kernel is the wrong trade at any speed. The fix is to make the
ecology *data* and ship a generic evaluator — which this engine already has,
twice.

## What already exists (reuse verbatim)

`terrain_field::Op` is ONE instruction type with ONE register machine, already
shared by two consumers. Its own comment says so:

> Read a per-sample input (oct = a SurfaceInput code). **Shared by both tapes**,
> but with different admissible codes: the surfaces() tape takes any of them,
> while FieldProgram::parse takes ONLY kSurfInWorldX and kSurfInWorldZ.

| piece | file | reused as-is? |
|---|---|---|
| `Op` (Const/Noise2/Ridge2/Add/Sub/Mul/Min/Max/Clamp/Blend/Smoothstep/Abs/OneMinus/Pow/Input/Noise2World/…) | `terrain_field.h:15` | yes |
| `SurfaceProgram::parse` — text -> deduplicated ops, `input_mask_`, caps | `terrain_field.h:218` | yes, + one directive |
| **`SurfaceRuntime::eval_regs`** — the register machine | `terrain_field.cpp:1122` | **yes, untouched** |
| `SurfaceWorldContext` — supplies the `FieldRuntime` for world inputs | `terrain_field.h:283` | yes |
| JS recorder: `SurfaceNode` + `__semit` emitting canonical text | `world_base.js.h:126` | yes |

`eval_regs` is the whole point. It is *already* factored out and *already* read
by two different consumers that differ only in which registers they pull:

```cpp
void SurfaceRuntime::weights_at(...)    { float regs[kMaxOps]{}; eval_regs(...); /* read material regs */ }
void SurfaceRuntime::appearance_at(...) { float regs[kMaxOps]{}; eval_regs(...); /* read tint/rough/wet regs */ }
```

A habitat evaluator is the **third** reader of the same registers, shaped
exactly like `appearance_at`.

### The inputs already match exactly

`sampleHabitat({x, z, altitude, slope, worldSeed})` needs four things, and all
four are existing `SurfaceInput` codes — no new input kinds:

| `sampleHabitat` arg | code | note |
|---|---|---|
| `x`, `z` | `kSurfInWorldX` (5), `kSurfInWorldZ` (7) | implied by `noise2w`, so usually free |
| `altitude` | `kSurfInHeight` (8) | field height at (worldX, worldZ) — the same `heightAt` the planner calls |
| `slope` | `kSurfInFieldSlope` (12) | `slope_at()`, and its comment notes it is *"stable across the whole LOD ladder"*, unlike the mesh-normal slope |

That last one is an upgrade taken for free: the planner already uses the field
slope, and the tape's version is explicitly the rung-independent one.

## What is new (small)

1. **One parser directive**, sibling to `material` / `tint` / `wetness`:
   `channel <nameIndex> r<reg>`. Same backward-ref rule, same dedup.
2. **`SurfaceRuntime::channels_at(world_xz, ctx, float* out)`** — allocate
   regs, call `eval_regs`, copy the channel registers out. ~10 lines, mirrors
   `appearance_at`.
3. **A `habitat(h)` JS entry** that reuses `__surfaceArg()` unchanged and
   accepts only the `channel` output directive (as `surfaces()` accepts
   `weight`/`tint`/…).
4. **One binding**, `__habitatAt(x, z)` -> a fixed-layout object or a packed
   array of channels.

Nothing in the op set, the parser's arithmetic, the register machine or the JS
recorder changes.

## The builder API

Identical to `surfaces(s)` — same node type, same methods — differing only in
what it declares. Channel names are the contract between the ecology and its
consumer, so they are declared once:

```js
// shared-lib/alpine_ecology.js
export const HABITAT_CHANNELS = [
  'moisture', 'exposure', 'dryness', 'forest', 'forestEdge',
  'treeCluster', 'shrubPatch', 'meadowPatch', 'flowerPatch', 'groundCoverPatch',
];

export function alpineHabitat(h, worldSeed) {
  const seed = worldSeed >>> 0;

  // moisture = saturate(0.18 + 0.62*fbm(1/300) + 0.20*fbm(1/55))
  const moisture = h.value(0.18)
    .add(h.noise2World(seed + 11, 1 / 300).mul(0.62))
    .add(h.noise2World(seed + 17, 1 / 55).mul(0.20))
    .clamp(0, 1);

  const exposure = h.value(0.10)
    .add(h.noise2World(seed + 23, 1 / 340).mul(0.80))
    .clamp(0, 1);

  // Broad signal sets whole forest regions; finer signals break the edges.
  const forestSignal =
    h.noise2World(seed + 31, 1 / 520, 4).mul(0.72)
     .add(h.noise2World(seed + 37, 1 / 140).mul(0.23))
     .add(h.noise2World(seed + 39, 1 / 55, 2).mul(0.05));
  const forest = forestSignal.smoothstep(0.40, 0.61);
  const forestEdge =
    forestSignal.sub(0.505).abs().smoothstep(0.045, 0.145).oneMinus();

  const groveSignal =
    h.noise2World(seed + 83, 1 / 95, 3).mul(0.68)
     .add(h.noise2World(seed + 89, 1 / 34, 2).mul(0.32));
  const treeCluster =
    forestSignal.mul(0.55).add(groveSignal.mul(0.45))
                .smoothstep(0.42, 0.60);

  const shrubSignal =
    h.noise2World(seed + 41, 1 / 190).mul(0.62)
     .add(h.noise2World(seed + 47, 1 / 62).mul(0.38));
  const meadowSignal =
    h.noise2World(seed + 53, 1 / 240, 4).mul(0.76)
     .add(h.noise2World(seed + 59, 1 / 75).mul(0.24));
  const flowerSignal =
    meadowSignal.mul(0.68).add(h.noise2World(seed + 67, 1 / 52).mul(0.32));
  const groundCoverSignal =
    forestSignal.mul(0.58).add(h.noise2World(seed + 79, 1 / 48).mul(0.42));

  // dryness: the one channel that reads TERRAIN inputs rather than noise.
  // h.height and h.fieldSlope are the existing lazy inputs -- the same two the
  // tree planner queries per candidate today, so folding them in here removes
  // those two boundary crossings as well.
  const dryness = moisture.oneMinus().mul(0.45)
    .add(exposure.mul(0.25))
    .add(h.height.sub(100).mul(1 / 420).clamp(0, 1).mul(0.20))
    .add(h.fieldSlope.mul(1 / 0.8).clamp(0, 1).mul(0.10))
    .clamp(0, 1);

  h.channel('moisture', moisture);
  h.channel('exposure', exposure);
  h.channel('dryness', dryness);
  h.channel('forest', forest);
  h.channel('forestEdge', forestEdge);
  h.channel('treeCluster', treeCluster);
  h.channel('shrubPatch', shrubSignal.smoothstep(0.38, 0.67));
  h.channel('meadowPatch', meadowSignal.smoothstep(0.37, 0.64));
  h.channel('flowerPatch', flowerSignal.smoothstep(0.45, 0.70));
  h.channel('groundCoverPatch', groundCoverSignal.smoothstep(0.39, 0.67));
}
```

That is `sampleHabitat` in full. **Nothing in it needed an op the vocabulary
does not already have** — 14 `noise2World`, plus mul/add/sub/abs/clamp/
oneMinus/smoothstep, plus two terrain inputs.

## Budget

~63 deduplicated ops against `kMaxOps = 96`. It fits with ~a third to spare,
and identical `const` lines dedup at parse time so the literal weights are
nearly free.

One caveat worth stating: that 96 exists to mirror *"the shader's
VT_TAPE_MAX_OPS register file"* — a GPU constraint. A habitat tape is CPU-only
(bake time, per candidate), so it inherits the cap by reusing
`SurfaceProgram::parse` rather than because it needs it. If an ecology ever
wants more, that is a parser parameter, not a redesign.

## Expected cost

One crossing (567 ns) plus the native op walk. The `__heightAt` measurement
says the walk is close to free — a 4-octave field evaluated natively costs the
same as reading a constant — so call it ~1-2 us against **105 us** today:

| | per candidate | level-2 tile | whole disc |
|---|---|---|---|
| today | 105 us | 4,500 ms | 36 CPU-min |
| habitat tape | ~2 us | ~90 ms | **~1 CPU-min** |

And it removes the two `heightAt`/`slopeAt` crossings per candidate as well,
since `dryness` folds them into the same tape evaluation.

## What deliberately stays in JS

The tape produces CHANNELS, not DECISIONS. These do not belong on an arithmetic
register machine and should not move:

- `selectedForm` / `selectAlpineAsset` — table-driven choice over dryness
  states, forms and per-family rows
- `placementIdentity` hashing per (x, z, channel)
- the candidate loop and the O(viable^2) exclusion

That is the cheap side: it runs only for candidates that survive the channels.
It is also exactly the division `surfaces()` already draws — the tape computes
weights, the material *declarations* stay in JS.

## Open questions

1. **Evaluation entry point.** `weights_at(pos, nrm, world, out)` takes
   PART-LOCAL position and transforms by `local_to_world`. Habitat wants world
   (x, z) directly. Either pass identity + world coords, or add a thin
   world-space overload. Cosmetic, but decide it before wiring.
2. **Re-baseline.** `Noise2World` is not the JS `valueNoise`, so this is a
   deliberate re-authoring of the ecology, not a bit-exact port. Every plant in
   every alpine world shifts once. Worth doing in one deliberate step with a
   `MATTER_REPLAY` before/after, exactly as other visual re-baselines are.
3. **Where the tape lives.** `surfaces()` is a World method. Habitat belongs to
   the ECOLOGY (shared-lib), which several worlds import — so either a World
   method that delegates, or a new module-level hook. The channel-name list has
   to be part of that contract.
4. **Batching.** Per-candidate crossings are 567 ns each; ~3,385 per cell is
   ~1.9 ms of pure boundary. A `__habitatAtMany(xs, zs)` taking typed arrays
   would amortise it to near zero and is the natural follow-up if the per-call
   form proves to be the floor.
