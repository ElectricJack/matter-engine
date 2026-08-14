#!/usr/bin/env python3
"""crack_scan.py -- find VISIBLE cracks in a rendered frame.

Every seam check this repo had before this one was defined on the geometry:
`weld_face`'s own stats, the plumb-line coverage probe in
seam_integration_tests.cpp, `drawn_level_violations`. They all agreed the
StreamMountain seam was closed -- `gap=0 err=0 viol=0`, VERDICT: PASS -- on the
exact frame a user filed as "gaps in the mountain terrain". A geometric probe
cannot see a crack it has no sample direction for, and it cannot see a seam that
is closed but drawn wrong. This one is defined on the OUTPUT, so it sees
whatever a person sees.

INPUT is a PNG captured with `viewer.debug.debug_view_mode = 2` ("Depth"), i.e.
composite.frag's `encode_debug_depth`: three channels linear in log depth over
[1, 8192] m -- R coarse, G and B a 32-cycle fine ramp in quadrature so a decoder
always has one channel clear of its wrap. That view forces the display pass into
passthrough (ACES is not invertible at 8 bits), which is what makes the read-back
metric rather than decorative.

WHAT COUNTS AS A CRACK. A thin depth SPIKE: a run of at most `max_width` pixels
whose depth is at least `ratio` times FARTHER than the pixels bounding the run on
both sides, with those two bounding depths agreeing to within `side_tol`. That is
exactly "the eye went through the shell and landed on something behind it". The
two things that also make a depth edge do not match it:

    silhouette   a STEP -- far on one side, near on the other, and it stays far
    ridge/crease CONTINUOUS -- no jump at all

A hole all the way to the background is the same signature with the far side
clamped at 8192, so it needs no separate rule.

WHAT IT DOES NOT SEE. A seam that is closed but SHADED wrong -- the class that
produced the report this tool was written for -- has continuous depth and is
invisible here by construction. Diff two lit frames for that (the weld draw
kill-switch MATTER_NO_SEAM_WELD_DRAW=1 gives the reference); check 4 of
MatterEngine3/tools/seam_suite.sh runs exactly that diff alongside this
script's check 2, and docs/baselines/seam-soak.sh runs the longer LOD-churn
acceptance soak over the same welder.

Usage:
    crack_scan.py <depth.png> [--lit lit.png] [--out overlay.png] [--json f]
                  [--max-width N] [--ratio R] [--side-tol T] [--min-size N]

Exit status is 0 when it ran, whatever it found; the caller decides what count
is a failure -- MatterEngine3/tools/seam_suite.sh is that caller for routine
gating.
"""
import argparse
import json
import sys

import numpy as np
from PIL import Image

# Must match encode_debug_depth in MatterEngine3/shaders_vk/composite.frag.
NEAR, FAR, CYCLES = 1.0, 8192.0, 32.0

# Scan directions: horizontal, vertical, and both diagonals. A crack thinner
# than a pixel in one direction is still bounded in another, and running all
# four costs nothing at these resolutions.
DIRS = ((0, 1), (1, 0), (1, 1), (1, -1))


def decode_depth(rgb):
    """Invert encode_debug_depth. Returns linear eye depth in metres."""
    r, g, b = rgb[..., 0] / 255.0, rgb[..., 1] / 255.0, rgb[..., 2] / 255.0
    coarse = r * CYCLES
    idx = np.floor(coarse)
    # Use whichever fine channel is furthest from a wrap; they are half a cycle
    # apart, so one of them always is.
    use_g = np.minimum(g, 1.0 - g) > np.minimum(b, 1.0 - b)
    fine = np.where(use_g, g, np.where(b >= 0.5, b - 0.5, b + 0.5))
    frac = coarse - idx
    idx += np.where(fine - frac > 0.5, -1.0, np.where(frac - fine > 0.5, 1.0, 0.0))
    t = np.clip((idx + fine) / CYCLES, 0.0, 1.0)
    return NEAR * np.power(FAR / NEAR, t)


def _shift(a, dy, dx, k):
    """a translated by k steps along (dy, dx); vacated edge becomes NaN."""
    out = np.full_like(a, np.nan)
    H, W = a.shape
    ys = slice(max(0, -dy * k), H - max(0, dy * k))
    xs = slice(max(0, -dx * k), W - max(0, dx * k))
    yt = slice(max(0, dy * k), H - max(0, -dy * k))
    xt = slice(max(0, dx * k), W - max(0, -dx * k))
    out[ys, xs] = a[yt, xt]
    return out


def find_cracks(depth, max_width=4, ratio=1.06, side_tol=0.06):
    """Boolean mask of pixels inside a thin depth spike."""
    H, W = depth.shape
    hit = np.zeros((H, W), bool)
    for dy, dx in DIRS:
        for w in range(1, max_width + 1):
            left = _shift(depth, dy, dx, -1)
            right = _shift(depth, dy, dx, w)
            run_min = depth.copy()
            for k in range(1, w):
                run_min = np.minimum(run_min, _shift(depth, dy, dx, k))
            with np.errstate(invalid="ignore"):
                agree = (np.abs(left - right) /
                         np.maximum(np.minimum(left, right), 1e-6)) < side_tol
                spike = (run_min / np.maximum(np.maximum(left, right), 1e-6)) > ratio
            # Float, not bool: _shift vacates with NaN, and NaN cast to bool is
            # TRUE -- shifting a bool mask marks the whole border as cracked.
            m = (agree & spike).astype(np.float32)
            # Mark every pixel of the run, not just the one it is anchored at.
            for k in range(w):
                hit |= _shift(m, dy, dx, -k) > 0.5
    return hit


def components(mask, min_size):
    """8-connected components, largest first."""
    H, W = mask.shape
    lab = np.zeros((H, W), np.int32)
    out, nxt = [], 0
    for sy, sx in zip(*np.where(mask)):
        if lab[sy, sx]:
            continue
        nxt += 1
        lab[sy, sx] = nxt
        stack, pix = [(sy, sx)], []
        while stack:
            y, x = stack.pop()
            pix.append((y, x))
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    ny, nx = y + dy, x + dx
                    if (0 <= ny < H and 0 <= nx < W and mask[ny, nx]
                            and not lab[ny, nx]):
                        lab[ny, nx] = nxt
                        stack.append((ny, nx))
        a = np.array(pix)
        out.append(dict(size=len(pix), label=nxt,
                        x0=int(a[:, 1].min()), x1=int(a[:, 1].max()),
                        y0=int(a[:, 0].min()), y1=int(a[:, 0].max()),
                        cx=float(a[:, 1].mean()), cy=float(a[:, 0].mean())))
    out = [c for c in out if c["size"] >= min_size]
    out.sort(key=lambda c: -c["size"])
    return out, lab


def scan(path, max_width=4, ratio=1.06, side_tol=0.06, min_size=3):
    rgb = np.asarray(Image.open(path).convert("RGB")).astype(np.float64)
    depth = decode_depth(rgb)
    hit = find_cracks(depth, max_width, ratio, side_tol)
    comps, lab = components(hit, min_size)
    for c in comps:
        c["depth_m"] = round(float(depth[int(c["cy"]), int(c["cx"])]), 1)
    return depth, comps, lab


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("depth", help="PNG from debug_view_mode = 2")
    ap.add_argument("--lit", help="matching lit PNG, for the overlay")
    ap.add_argument("--out", help="write a magenta-marked overlay here")
    ap.add_argument("--json", help="write the component list here")
    ap.add_argument("--max-width", type=int, default=4)
    ap.add_argument("--ratio", type=float, default=1.06)
    ap.add_argument("--side-tol", type=float, default=0.06)
    ap.add_argument("--min-size", type=int, default=3)
    a = ap.parse_args()

    _, comps, lab = scan(a.depth, a.max_width, a.ratio, a.side_tol, a.min_size)
    total = sum(c["size"] for c in comps)
    print(f"{a.depth}: crack pixels {total}  components {len(comps)}")
    for c in comps[:15]:
        print(f"  {c['size']:5d} px  x[{c['x0']},{c['x1']}] y[{c['y0']},{c['y1']}]"
              f"  centre ({c['cx']:.0f},{c['cy']:.0f})  {c['depth_m']} m")

    if a.json:
        with open(a.json, "w") as f:
            json.dump(dict(source=a.depth, crack_pixels=int(total),
                           components=comps), f, indent=1)
    if a.out:
        base = np.asarray(Image.open(a.lit).convert("RGB")).astype(np.uint8) \
            if a.lit else \
            np.asarray(Image.open(a.depth).convert("RGB")).astype(np.uint8)
        keep = np.isin(lab, [c["label"] for c in comps])
        grown = keep.copy()
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                grown |= np.roll(np.roll(keep, dy, 0), dx, 1)
        overlay = base.copy()
        overlay[grown] = (255, 0, 255)
        Image.fromarray(overlay).save(a.out)
        print("overlay ->", a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
