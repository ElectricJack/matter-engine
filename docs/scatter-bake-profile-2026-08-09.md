# Where a sector bake's scatter time actually goes — 2026-08-09

Branch: `feature/nested-sector-lod`. Supersedes the "remaining ~65% is
unknown" note in `docs/habitat-tape-sketch-2026-08-08.md` and the estimates in
`projects/world_demo/shared-lib/alpine_ecology.js`.

## Why this document exists

The same question was answered wrongly twice, and both wrong answers were
backed by arithmetic that matched a measured number closely enough to be
convincing:

1. **"`sampleHabitat` is ~99% of a far sector's bake, so a native habitat tape
   is a ~50× win."** The per-candidate cost estimate was right to within 4%.
   The *population* was 3.5× out: the figure came from a naive grid count
   (3,385 per 64 m cell) while `candidatesInRect` is a Poisson-style sampler
   whose real output is ~963. The tape shipped and measured **−35.6%**.

2. **"The residual is `planTrees`' O(viable²) exclusion loop."** The estimate
   landed within 3% of the measured 180.7 ms. It was implemented as an exact
   spatial-hash exclusion grid, verified to emit a bitwise-identical placement
   list (111 / 869 / 10,429 children unchanged), and measured
   **+1.4% / −1.9% / −2.9% — nothing.** The error: `viable` is the candidates
   that *pass* the habitat sample, a small fraction of the 963 that *reach* it.

Both were reasoning about the inside of `build()` from quantities measured
outside it, because nothing could see inside: BakeTrace's `part-bake` span
opens around `bake_source` and closes when it returns.

**The lesson is not "estimate more carefully". It is that a close numerical fit
against one measured total is not evidence** when the model has a free
parameter nobody has observed.

## The instrument

`ScriptProfile` (`MatterEngine3/src/dsl_bindings.h`). Named timers a bake
script opens around its own phases:

```js
const P = profSlot('scatter.trees');   // once, at module scope
profBegin(P); /* work */ profEnd(P);
```

- **Aggregating, not a span tree.** BakeTrace answers "where did *this* run
  go". This question spans 6,547 sectors across a dozen workers, where no
  single run's tree says anything. The output is the flat sum.
- **Self time as well as total**, so a label that merely encloses other labels
  reads ~0 rather than as its own cost plus its children's.
- **Off unless `MATTER_SCRIPT_PROFILE`**, and off means `slot()` returns −1 and
  every begin/end is an early return — so instrumentation stays in the ecology
  permanently.
- Reported at the periodic `[stream.rate]` census and again over the whole fill.

Two ways to read it:

```bash
make -C MatterEngine3/tests run-scatterprof
```

```bash
MATTER_SCRIPT_PROFILE=1 MATTER_WORLD=StreamMountain ./build/windows/editor.exe
```

The harness (`MatterEngine3/tests/sector_scatter_profile.cpp`) drives the real
`WorldSector.js` and the real StreamMountain habitat tape, compiled through the
same `load_world_definition → eval_world → SurfaceProgram::parse` path
`install_world` uses. It is single-bake and single-threaded, so it describes
the **composition** of a bake and says nothing about fill throughput — the
engine's own block is the cross-check.

## The answer

Band 3 = the far edge of where vegetation is planted at all, and the bulk of
the disc. Self time over 8 bakes:

| label | before | | after native grid | |
|---|---:|---:|---:|---:|
| `plan.candidates` | **596.4 ms** | **46.6%** | 17.9 ms | 2.3% |
| `plan.habitat` | 189.9 ms | 14.8% | 199.2 ms | 26.1% |
| `plan.evaluate` (self) | 165.4 ms | 12.9% | 179.4 ms | 23.5% |
| `plan.exclude` | 152.5 ms | 11.9% | 170.3 ms | 22.4% |
| `plan.select` | 139.1 ms | 10.9% | 158.4 ms | 20.8% |
| `sector.terrain` (native mesher) | 32.0 ms | 2.5% | 31.9 ms | 4.2% |

**Candidate *generation*, not per-candidate evaluation.** Neither earlier theory
went near it. The exclusion pass — the second theory's entire subject — is
11.9%.

### Why the grid was so expensive

Per grid cell the JS evaluated `cellCandidate` **ten times**: once for the
cell, then again for the cell plus its eight neighbours inside `survives()`.
Each allocated a six-field object read once and discarded.

A 96 m tree rect (a 64 m cell padded 16 m per side) at 1.65 m spacing is 59×59
cells → **~35,000 allocations and ~244,000 hash rounds to return ~1,300
candidates.** The grass families are denser still: 0.63 m spacing over a bare
64 m cell is 101×101.

Nothing in it is game-specific — `(seed, kind, minDist, rect)` in,
deterministic points out — so it belongs to the engine, not to an ecology.

### Result

| | wall ms/bake before | after | |
|---|---:|---:|---:|
| band 3 (far edge) | 180.4 | 116.1 | **−35.6%** |
| band 4 | 278.2 | 154.9 | **−44.3%** |
| band 5 (nearest) | 966.4 | 491.1 | **−49.2%** |

`plan.candidates` itself: **33× faster** at band 3.

### Bitwise identity is the contract

Every placement in every world derives from these hashes, so a result differing
in the last bit moves trees. `uint32_t` arithmetic reproduces JS exactly:

- `Math.imul(a,b)` == `(uint32)a * (uint32)b` truncated to 32 bits
- `x >>> n` == `(uint32)x >> n`
- `a ^ b` == `ToInt32(a) ^ ToInt32(b)`, the same bits as a uint32 XOR
- `h + Math.imul(...)` is an exact integer sum of two int32s, and the `ToInt32`
  the following `^=` applies is the same bit pattern uint32 addition wraps to

That argument is not taken on trust. The JS remains in `scatter_grid.js` as
`candidatesInRectJs` **as the specification**, and `sector_bake_tests` compares
the two inside a real bake over five real rects — padded tree grid, densest
grass grid, sparsest boulder grid, a rect far from the origin, a non-integer
origin — with **strict double equality**. A tolerance there would pass exactly
the drift the test exists to catch. A second case asserts the binding is
actually *reached*, because a `typeof` guard falling through to the JS would
pass every equality check while delivering none of the speedup.

## Engine cross-check

A real StreamMountain run agrees with the harness on shape after the change:

```
plan.select      26.3%    plan.evaluate 23.9%    plan.exclude 17.4%
plan.habitat     14.9%    sector.terrain 9.0%    plan.candidates 2.9%
```

Roughly even four ways, no dominant term left. **That is the honest place to
stop**: the next change here would be worth ~20% of scatter at best, and the
history above says the estimate would probably be wrong anyway.

## Incidental fixes

- **Three JS preludes, not two.** `part_base.js.h`, `world_base.js.h`, and a
  third inside `world_definition_loader.cpp`. A shared-lib module carrying
  `prof()` calls at module scope is imported into all three; covering two made
  world load die with a bare `ReferenceError: profSlot is not defined`. Fixed
  in both directions — the loader's prelude gets the stubs, and the modules
  resolve through `typeof` so a fourth prelude cannot repeat it.
- **`world_definition_tests` now walks `worlds/`** and asserts every shipped
  world loads. The previous coverage was a hand-maintained table of six;
  StreamMountain and StreamMeadow were never in it. Verified failable.
- **`[stream.fill] COMPLETE`** is now unconditional. It printed only under
  `MATTER_STREAM_FILL_PROFILE`, so a streamed world had no completion signal
  unless you already knew that env var existed.
- **`[bake-timing] (world-kind)`** says `ROOTS ONLY` and points at
  `[stream.fill]`. Reading its total as a fill time produced a retracted 8.7×
  claim.
- **The surfaces op cap is one constant.** `terrain_field.cpp`'s `kMaxOps` and
  `vt_compositor`'s `kTapeSlotOps` are aliases of `kMaxSurfaceOps`;
  `FieldRuntime`'s private copy was dead and is gone. `vt_compositor`'s comment
  still said 64, from before the raise — the drift the aliases remove. The one
  mirror the compiler still cannot check is `VT_TAPE_MAX_OPS` in
  `shaders_vk/vt_surface_tape.glsl`.

## Not done

- **Visual re-baseline of the re-authored ecology.** The habitat tape's
  `Noise2World` is not the JS `valueNoise`, so plant placement shifted once,
  deliberately. A `MATTER_REPLAY` before/after has still not been captured.
  The native candidate grid does *not* add to this — it is bitwise identical.
- **`__habitatAtMany`.** `plan.habitat` is now 26.1% at 19 µs/call across ~1,300
  calls per cell. A batched form taking typed arrays would amortise the
  crossing. Worth measuring before building — the crossing is 567 ns, so it is
  at most ~3% of that 19 µs, and the rest is the tape doing real work.
