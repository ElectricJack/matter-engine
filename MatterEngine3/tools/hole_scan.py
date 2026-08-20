#!/usr/bin/env python3
"""hole_scan.py -- count pixels that see THROUGH the terrain to the background.

The companion to crack_scan.py, and the sharper of the two instruments. It is
only valid on a world whose density is a heightfield with no caves -- SeamLab is
built to be exactly that (see projects/world_demo/scenes/SeamLab/SeamLab.js).
There, every solid point below the surface is solid all the way down, so a
camera above the terrain has nothing legitimate to see except the surface and
the sky above the horizon. A background pixel that is NOT the sky is therefore a
hole, with no tolerance, no ratio and no width limit -- which is what crack_scan
cannot offer, because it has to infer "went through the shell" from a depth
profile rather than being told.

INPUT is the same PNG crack_scan takes: `viewer.debug.debug_view_mode = 2`,
composite.frag's encode_debug_depth, which forces the display pass to
passthrough so the read-back is metric.

THE RULE. Background is `depth >= --far-eps` of the encoder's 8192 m ceiling.
The sky is the background component(s) touching the TOP edge of the frame; every
other background component is a hole. That connectivity test is what lets the
scan run at a grazing pose with the horizon in shot, instead of only at the
straight-down poses where the frame is all terrain.

    * a hole enclosed by terrain      -> component touches no top edge -> HOLE
    * the sky above the ridge line    -> touches the top edge          -> sky
    * a notch in the silhouette       -> connected to the sky          -> not
      reported, and correctly: you can see the sky through it because it IS the
      sky. Poses that put the defect against the sky are the ones this scan is
      blind to, and the suite pairs it with crack_scan for that reason.

Exit status is 0 when it ran, whatever it found; the caller decides what count
is a failure.
"""
import argparse
import json
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
from crack_scan import decode_depth  # noqa: E402  (same encoder, one decoder)


def label_components(mask):
    """8-connected component labelling. Returns (labels, count).

    Run-based with union-find rather than a per-pixel flood fill: the sky is one
    component of several hundred thousand pixels at a grazing pose, and a Python
    per-pixel fill over it takes longer than the editor run that produced the
    frame. Rows have a handful of runs each, so this is a few thousand unions.
    """
    H, W = mask.shape
    parent = [0]

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[max(ra, rb)] = min(ra, rb)

    rows = []            # per row: list of (start, end_exclusive, label)
    for y in range(H):
        m = mask[y]
        if not m.any():
            rows.append([])
            continue
        d = np.diff(np.concatenate(([0], m.view(np.int8), [0])))
        starts = np.where(d == 1)[0]
        ends = np.where(d == -1)[0]
        runs = []
        for s, e in zip(starts, ends):
            parent.append(len(parent))
            runs.append([int(s), int(e), len(parent) - 1])
        # 8-connectivity: runs touch if their spans overlap when each is
        # widened by one pixel.
        for s, e, lab in runs:
            for ps, pe, plab in rows[y - 1] if y else []:
                if ps <= e and s <= pe:
                    union(lab, plab)
        rows.append(runs)

    remap, nxt = {}, 0
    out = np.zeros((H, W), np.int32)
    for y, runs in enumerate(rows):
        for s, e, lab in runs:
            r = find(lab)
            if r not in remap:
                nxt += 1
                remap[r] = nxt
            out[y, s:e] = remap[r]
    return out, nxt


def scan(path, far_eps=0.97, min_size=1):
    rgb = np.asarray(Image.open(path).convert("RGB")).astype(np.float64)
    depth = decode_depth(rgb)
    bg = depth >= 8192.0 * far_eps
    lab, n = label_components(bg)
    sky = set(int(v) for v in np.unique(lab[0, :]) if v)
    holes = []
    for i in range(1, n + 1):
        if i in sky:
            continue
        ys, xs = np.where(lab == i)
        if len(ys) < min_size:
            continue
        holes.append(dict(size=int(len(ys)), label=int(i),
                          x0=int(xs.min()), x1=int(xs.max()),
                          y0=int(ys.min()), y1=int(ys.max()),
                          cx=float(xs.mean()), cy=float(ys.mean())))
    holes.sort(key=lambda c: -c["size"])
    return depth, bg, lab, sky, holes


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("depth", help="PNG from debug_view_mode = 2")
    ap.add_argument("--lit", help="matching lit PNG, for the overlay")
    ap.add_argument("--out", help="write a magenta-marked overlay here")
    ap.add_argument("--json", help="write the hole list here")
    ap.add_argument("--far-eps", type=float, default=0.97,
                    help="fraction of the 8192 m ceiling that counts as background")
    ap.add_argument("--min-size", type=int, default=1)
    a = ap.parse_args()

    _, _, lab, sky, holes = scan(a.depth, a.far_eps, a.min_size)
    total = sum(h["size"] for h in holes)
    print(f"{a.depth}: hole pixels {total}  components {len(holes)}")
    for h in holes[:15]:
        print(f"  {h['size']:6d} px  x[{h['x0']},{h['x1']}] y[{h['y0']},{h['y1']}]"
              f"  centre ({h['cx']:.0f},{h['cy']:.0f})")

    if a.json:
        with open(a.json, "w") as f:
            json.dump(dict(source=a.depth, hole_pixels=int(total),
                           components=holes), f, indent=1)
    if a.out:
        base = np.asarray(Image.open(a.lit or a.depth).convert("RGB")).astype(np.uint8)
        keep = np.isin(lab, [h["label"] for h in holes])
        grown = keep.copy()
        for dy in (-2, -1, 0, 1, 2):
            for dx in (-2, -1, 0, 1, 2):
                grown |= np.roll(np.roll(keep, dy, 0), dx, 1)
        overlay = base.copy()
        overlay[grown & ~keep] = (255, 0, 255)
        Image.fromarray(overlay).save(a.out)
        print("overlay ->", a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
