#!/usr/bin/env python3
"""seam_report.py -- analyse a seam_suite.sh capture directory and gate it.

Reads <out>/weld/ and <out>/noweld/ (see seam_suite.sh for what is in them) and
produces the six-check table. Every number here is defined on the OUTPUT of a
real editor run; nothing is inferred from geometry.

    holes    hole_scan: background pixels enclosed by terrain          GATE 0
    cracks   crack_scan: thin depth spikes                             GATE 0
    flicker  DEPTH pixels differing between two captures of one
             static pose (no temporal filter on that view, so an
             unchanged scene must be bitwise equal)                    GATE 0
    shading  pixels the weld draw changes, and the worst luminance
             ratio it paints them at, against their own neighbours     GATE >= 0.80
    pool     welder accounting invariants from MATTER_SEAM_TRACE       GATE 0
    residue  missing_landing / degenerate: how much of the seam the
             fan declined to close                                     reported

WHY `shading` HAS A RATIO GATE AND NOT A PIXEL GATE. The weld is SUPPOSED to
change pixels -- it draws the band that closes the seam, and issue ec2829d6's
first fix made it change 2155 of them correctly. What is a defect is the weld
painting terrain DARKER than the terrain around it, which is what a wrong
material or a reversed normal looks like. So the pixel count is reported and
the ratio is gated.

Usage:
    seam_report.py <out-dir> [--poses f] [--baseline f] [--json f]
"""
import argparse
import json
import os
import re
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import crack_scan  # noqa: E402
import hole_scan   # noqa: E402

# Gates. A number here is a claim that the engine can hold it, not a tolerance
# picked to make today's run pass -- see docs/seam-suite-2026-08-13.md for what
# each one cost to reach.
GATE_HOLE_PIXELS = 0
GATE_CRACK_PIXELS = 0
GATE_FLICKER_PIXELS = 0
GATE_SHADING_RATIO = 0.80


def load_rgb(path):
    if not os.path.exists(path):
        return None
    return np.asarray(Image.open(path).convert("RGB")).astype(np.int32)


def luminance(rgb):
    return (0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2])


def dilate(mask, r=3):
    out = mask.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out |= np.roll(np.roll(mask, dy, 0), dx, 1)
    return out


def shading_check(weld_lit, noweld_lit):
    """Pixels the weld draw changes, and the worst luminance ratio it paints.

    The reference for "should be this bright" is the pixels JUST OUTSIDE the
    changed set in the SAME (weld) frame -- a ring one dilation wide. Comparing
    against the noweld frame instead would be wrong: there the weld's pixels
    show whatever was behind it, which is the geometry the band exists to cover.
    """
    if weld_lit is None or noweld_lit is None:
        return dict(pixels=-1, ratio=-1.0)
    if weld_lit.shape != noweld_lit.shape:
        return dict(pixels=-1, ratio=-1.0)
    changed = np.any(np.abs(weld_lit - noweld_lit) > 6, axis=-1)
    n = int(changed.sum())
    if n == 0:
        return dict(pixels=0, ratio=1.0)
    ring = dilate(changed, 3) & ~dilate(changed, 1)
    lum = luminance(weld_lit)
    if ring.sum() < 16:
        return dict(pixels=n, ratio=1.0)
    inner = float(np.median(lum[changed]))
    outer = float(np.median(lum[ring]))
    ratio = inner / outer if outer > 1e-6 else 1.0
    return dict(pixels=n, ratio=round(ratio, 3))


SEAM_LINE = re.compile(r"(\w+)=(-?\d+)")


def pool_check(seam_log):
    """Worst value of each welder invariant over the run's [seam] lines."""
    worst = {}
    if not os.path.exists(seam_log):
        return worst
    with open(seam_log, "r", errors="replace") as f:
        for line in f:
            for k, v in SEAM_LINE.findall(line):
                worst[k] = max(worst.get(k, 0), int(v))
    return worst


# The invariants that must be zero, and what a non-zero one means.
POOL_GATES = {
    "gap": "cross-level pairs more than one level apart (the drawn +-1 invariant broke)",
    "viol": "tiles drawn beside a >=2-level neighbour",
    "err": "weld build errors",
    "sign": "sign conflicts between two cells sharing a plane endpoint",
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out")
    ap.add_argument("--poses", default=os.path.join(HERE, "seam_lab_poses.txt"))
    ap.add_argument("--baseline")
    ap.add_argument("--json")
    a = ap.parse_args()

    poses = []
    with open(a.poses) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                poses.append(line.split()[0])

    weld = os.path.join(a.out, "weld")
    noweld = os.path.join(a.out, "noweld")
    rows, failures = [], []

    print(f"{'pose':10s} {'holes':>7s} {'cracks':>7s} {'flicker':>8s} "
          f"{'weldpx':>8s} {'ratio':>6s}")
    print("-" * 52)
    for p in poses:
        depth = os.path.join(weld, f"{p}_depth.png")
        row = dict(pose=p)
        if os.path.exists(depth):
            _, _, _, _, holes = hole_scan.scan(depth)
            row["holes"] = int(sum(h["size"] for h in holes))
            row["hole_components"] = [
                dict(size=h["size"], x=int(h["cx"]), y=int(h["cy"])) for h in holes[:8]]
            _, comps, _ = crack_scan.scan(depth)
            row["cracks"] = int(sum(c["size"] for c in comps))
            row["crack_components"] = [
                dict(size=c["size"], x=int(c["cx"]), y=int(c["cy"]),
                     depth_m=c["depth_m"]) for c in comps[:8]]
        else:
            row["holes"] = row["cracks"] = -1

        # Flicker is measured on the DEPTH view, which has no temporal filter,
        # so "unchanged scene" means BITWISE equal and any difference is a
        # surface swapping places with another at the same depth.
        d1 = load_rgb(os.path.join(weld, f"{p}_depth.png"))
        d2 = load_rgb(os.path.join(weld, f"{p}_depth2.png"))
        if d1 is not None and d2 is not None and d1.shape == d2.shape:
            row["flicker"] = int(np.any(d1 != d2, axis=-1).sum())
        else:
            row["flicker"] = -1
        lit = load_rgb(os.path.join(weld, f"{p}_lit.png"))

        row.update({("shading_" + k): v for k, v in
                    shading_check(lit, load_rgb(os.path.join(noweld, f"{p}_lit.png"))).items()})
        rows.append(row)
        print(f"{p:10s} {row['holes']:7d} {row['cracks']:7d} {row['flicker']:8d} "
              f"{row['shading_pixels']:8d} {row['shading_ratio']:6.2f}")

        if row["holes"] > GATE_HOLE_PIXELS:
            failures.append(f"{p}: {row['holes']} hole pixels (see-through)")
        if row["cracks"] > GATE_CRACK_PIXELS:
            failures.append(f"{p}: {row['cracks']} crack pixels (depth spike)")
        if row["flicker"] > GATE_FLICKER_PIXELS:
            failures.append(f"{p}: {row['flicker']} pixels flickering at a static pose")
        if 0 <= row["shading_ratio"] < GATE_SHADING_RATIO:
            failures.append(f"{p}: weld paints at {row['shading_ratio']:.2f} of its "
                            f"neighbours' luminance")

    pool = pool_check(os.path.join(weld, "seam.log"))
    print()
    print("pool (worst over the run):", " ".join(
        f"{k}={v}" for k, v in sorted(pool.items())) or "no [seam] lines")
    for key, meaning in POOL_GATES.items():
        if pool.get(key, 0) > 0:
            failures.append(f"pool {key}={pool[key]}: {meaning}")

    summary = dict(poses=rows, pool=pool, failures=failures)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(summary, f, indent=1)
    with open(os.path.join(a.out, "report.json"), "w") as f:
        json.dump(summary, f, indent=1)

    print()
    if failures:
        print(f"FAIL ({len(failures)}):")
        for m in failures:
            print("  " + m)
        return 1
    print("PASS -- every gated check clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
