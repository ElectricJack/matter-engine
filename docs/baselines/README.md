# Replay baselines: the visual gate for the Representation migration

Date established: 2026-08-04, on `feature/representation` at the M0 baseline commit.
Companion: `docs/superpowers/plans/2026-08-04-lod-vt-migration.md`.

## Why the PNGs are not committed

Baseline images are **GPU-, driver- and machine-specific**, and the RT path is not
bit-exact run to run (measured below). A PNG committed here would be wrong on any other
machine and would rot on this one after a driver update. What *is* durable — and what this
directory commits — is the **capture recipe and the measured thresholds**.

The workflow is therefore always *capture-both-sides-locally*:

```bash
# before your change
bash docs/baselines/capture-replay-baseline.sh <issue-dir> docs/baselines/before.png
# ... make the change, rebuild ...
bash docs/baselines/capture-replay-baseline.sh <issue-dir> docs/baselines/after.png
python MatterEngine3/tools/img_diff.py docs/baselines/before.png docs/baselines/after.png \
    --channel-tol 16 --max-diff-pct 0.5      # PomProofBrick gate; see table
```

`docs/baselines/*.png` is gitignored.

## The gates

`img_diff.py` defaults (`--channel-tol 2 --max-diff-pct 0.5`) are **unusable for RT shots** —
two identical runs differ on 22% of pixels at tol 2. Those defaults were calibrated for a
raster path. Use the per-shot settings below.

| Shot | World | Noise floor (worst observed) | **Gate** | Headroom |
|---|---|---|---|---|
| `issues/91919e5d-…` | PomProofBrick | 0.069 % @ tol 16 (10 pairs) | `--channel-tol 16 --max-diff-pct 0.5` | 7× |
| `issues/render-ground-tiling-carpet-repetition` | ChartVtProof | 0.353 % @ tol 32 (3 pairs) | `--channel-tol 32 --max-diff-pct 1.5` | 4× |

**PomProofBrick is the primary gate** — its floor is tight and consistent across ten
independent pairs (0.066–0.069 % at tol 16). ChartVtProof is secondary and converges
*bimodally*: two of three pairs sat at ~1.17 % at tol 16 while the third sat at 0.26 %, so it
needs the wider tolerance and a diff just under the gate should be re-run before being
believed.

StreamMountain (`issues/render-normals-show-tile-lattice`) is deliberately **not** a routine
gate: a cold-cache capture there rebakes 6547 sectors. Use it as an occasional check, warm.

## The baseline goes stale when the UI changes — this is expected

Capture size follows the **docked ImGui layout**, not a fixed framebuffer. Adding a control
changes the viewport width, and `img_diff.py` then refuses the comparison outright
(`DIFF size (1791, 784) vs (1036, 783)`). Seen twice: once when the LOD-tint and wireframe
views added combo entries, and again when M2.5 put the resident-impostor count on the stats
overlay.

That is not a defect in the gate — it is why this directory refuses to commit golden PNGs
and prescribes capture-both-sides-locally. When it happens, re-measure the floor on the
current build (two captures, same binary) rather than comparing across a layout change.
Measured floors, PomProofBrick shot 1: **0.069 %** at the pre-debug-views layout,
**0.080 %** at the post-M2.5 layout. Pin `imgui.ini` between captures, and watch for an open
hover tooltip — one polluted a capture with 94.5 % of its diff inside a single 29-row band.

## Evidence behind the numbers

Measured 2026-08-04, PomProofBrick, five independent captures (two cold-cache, one warm, two
at `MATTER_REPLAY_SETTLE=400`), all ten pairs:

| tolerance | run-to-run difference |
|---|---|
| 2 | 22.39 – 22.73 % |
| 8 | 0.30 – 0.33 % |
| 16 | 0.066 – 0.069 % |
| 32 | 0.008 – 0.010 % |

Character of the noise: mean channel delta **1.08**, max 67, with only 0.01 % of pixels
above 32. It is low-amplitude RT denoiser grain spread broadly across the frame (left half
~32 % of pixels at tol 2, right half ~12 %), not a structural difference.

Two findings worth keeping:

- **More settle frames do not converge it.** `MATTER_REPLAY_SETTLE=400` measured 22.39 % at
  tol 2 versus 22.67 % at the default 90 — indistinguishable. The variance is inherent to the
  RT path, not accumulation lag, so raising settle only costs time.
- **The bake is deterministic; the render is not.** A warm-cache capture (identical baked
  geometry) differed from a cold one by 22.65 %, statistically identical to cold-vs-cold at
  22.67 %. All observed variance is render-side.

## Failability proof (the gate can actually fail)

A threshold nobody has watched fail is not a gate. Proven by perturbation, same shot, same
build:

| Perturbation | tol 16 result | verdict |
|---|---|---|
| none (run-to-run) | 0.069 % | floor |
| `MATTER_VT_ENRICH_STRENGTH=0` | 0.066 % | **no-op** — indistinguishable from the floor |
| `MATTER_VT_WARP=0` | **32.697 %** | fires, 470× the floor |

The enrich result is a real property, not a failed experiment: tier-2 hemisphere AO does not
affect this view (ORM.r is not sampled by it), which is consistent with the earlier
dark-dome-patch investigation. It is retained here as a warning — **it was the first
perturbation tried, and had it been trusted, it would have "proved" a gate that was actually
inert.**

`MATTER_VT_WARP=0` separates from the noise floor by 470×, so the gate at tol 16 /
0.5 % sits 7× above the noise and 65× below a genuine regression.

## M0's specific claim

M0 deletes only code that is provably unreachable (`#ifndef MATTER_VULKAN_ONLY` under a macro
every build defines). It is pure subtraction, so **PomProofBrick must come back under the gate
after M0.** A diff above it means something deleted was reachable — investigate; do not
re-baseline.

Note the cache trap the capture script handles for you: `projects/<proj>/.cache` is
content-addressed on the world's **JS source**, not on engine code, so a warm-cache replay
after an engine change reloads the old bake and reproduces the old pixels exactly — passing
while proving nothing. The script wipes the world's cache before every capture.
