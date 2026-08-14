---
name: qa-replay-diff
description: Reproduce a filed issue's screenshot headlessly (MATTER_REPLAY) and diff before/after PNGs with img_diff.py. Use when asked to verify a fix against a filed issue, or compare before/after screenshots of an engine change.
---

## Capture + diff

```bash
bash docs/baselines/capture-replay-baseline.sh <issue-dir> before.png [shot-index]
# ... make the engine change, rebuild ...
bash docs/baselines/capture-replay-baseline.sh <issue-dir> after.png [shot-index]
python MatterEngine3/tools/img_diff.py before.png after.png \
    --channel-tol 16 --max-diff-pct 0.5
```

`<issue-dir>` is `issues/<guid>/` (needs `state.json`); shot-index is
1-based, default 1. `img_diff.py` prints a MATCH/DIFF verdict and exits 1 on
DIFF or a framebuffer-size mismatch (viewport size follows the docked ImGui
layout, not a fixed resolution).

## Cache-wipe caveat

`capture-replay-baseline.sh` **wipes `projects/world_demo/.cache/<world>`
before every capture** — deliberately: the cache is content-addressed on the
world's JS source, not engine code, so a warm-cache replay after an
engine-only change silently reloads the old bake and reproduces the old
pixels, passing while proving nothing.

## RT is not bit-exact — pick the right gate

`img_diff.py`'s defaults (`--channel-tol 2 --max-diff-pct 0.5`) are
calibrated for raster and are **useless for RT shots**: identical runs differ
on ~22% of pixels at tol 2 (denoiser grain). Use a per-shot calibrated gate
from `docs/baselines/README.md`'s table (e.g. PomProofBrick: `--channel-tol
16 --max-diff-pct 0.5`, floor ~0.069%). More `MATTER_REPLAY_SETTLE` frames do
not reduce RT noise.

## What a replay does NOT reproduce

Camera/render state and the ImGui layout, not input or a tick history. DLSS
forces to `Native`. Streaming worlds depend on which sectors are resident.
Bakes are not bit-deterministic across builds (see root `CLAUDE.md`).

See `docs/agent/issue-system.md` for the full replay/issue-schema reference
and `docs/agent/qa-cookbook.md` recipe 5.
