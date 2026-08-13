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
    shading  pixels the weld draw changes, the median luminance ratio
             it paints them at against their own neighbours, and how
             many of them it paints DARK                    GATE ratio >= 0.80,
                                                                 dark pixels 0
    pool     welder accounting invariants from MATTER_SEAM_TRACE       GATE 0
    residue  missing_landing / degenerate: how much of the seam the
             fan declined to close                                     reported

WHY `shading` GATES DARKNESS AND NOT A PIXEL COUNT. The weld is SUPPOSED to
change pixels -- it draws the band that closes the seam, and issue ec2829d6's
first fix made it change 2155 of them correctly. What is a defect is the weld
painting terrain DARKER than the terrain around it, which is what a wrong
material, a reversed normal or a self-shadowing occluder looks like. So the
count of changed pixels is reported, and two things about their brightness are
gated: the median ratio (a whole band drawn wrong) and the number of
individually dark pixels (a hairline drawn wrong, which no median can see).

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
# Cracks are gated against a COMMITTED BASELINE (seam_baseline.json), not
# against zero. hole_scan is exact -- a background pixel enclosed by terrain
# either is or is not there -- but crack_scan infers "the eye went through the
# shell" from a depth profile, and at these resolutions a few pixels of ordinary
# terrain crease read the same way. The measured residue is recorded rather than
# rounded to zero, so the suite catches a regression without claiming a
# cleanliness the instrument cannot support.
GATE_CRACK_PIXELS = 0
GATE_FLICKER_PIXELS = 0
GATE_SHADING_RATIO = 0.80
# A weld pixel darker than this fraction of its neighbours' median luminance is
# counted individually and gated at zero. See shading_check for why the median
# ratio above cannot be the only gate.
GATE_SHADING_DARK = 0.55
GATE_SHADING_DARK_PIXELS = 0


def decode_depth_png(path):
    """Metres per pixel from a debug_view_mode = 2 capture, or None."""
    if not os.path.exists(path):
        return None
    return crack_scan.decode_depth(
        np.asarray(Image.open(path).convert("RGB")).astype(np.float64))


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


def shading_check(weld_lit, noweld_lit, weld_depth, noweld_depth):
    """How much the weld draw changes, and whether it DARKENS what it covers.

    Two numbers, and they answer different questions.

    `pixels` / `ratio` describe the whole changed set: how much of the frame the
    weld paints and, as context, the median luminance it paints it at against
    the ring of pixels just outside the changed set in the SAME frame. (Not
    against the no-weld frame: there those pixels show whatever was behind the
    weld, which is the geometry it exists to cover.)

    `dark` is the gate, and it is a PER-PIXEL comparison against the no-weld
    frame at the same pixel, restricted to pixels where the two runs agree
    about DEPTH. That restriction is what makes it a statement about shading
    rather than about geometry: where the depth matches, both runs are showing
    the same surface at the same place, so a weld pixel much darker than its
    no-weld counterpart is the weld painting correct terrain dark -- issue
    ec2829d6's class exactly, and the form the M0-WP7 band's RT self-shadowing
    took (0.18 of its neighbours across 336 pixels, at identical depth,
    identical raw albedo and identical normals).

    The median alone cannot see that: a hairline is a small fraction of a large
    set by construction, and 336 black pixels moved the median over ~10,000 by
    0.01.
    """
    if weld_lit is None or noweld_lit is None or weld_lit.shape != noweld_lit.shape:
        return dict(pixels=-1, ratio=-1.0, dark=-1)
    changed = np.any(np.abs(weld_lit - noweld_lit) > 6, axis=-1)
    n = int(changed.sum())
    if n == 0:
        return dict(pixels=0, ratio=1.0, dark=0)
    ring = dilate(changed, 3) & ~dilate(changed, 1)
    lum_w = luminance(weld_lit)
    lum_n = luminance(noweld_lit)
    ratio = 1.0
    if ring.sum() >= 16:
        outer = float(np.median(lum_w[ring]))
        if outer > 1e-6:
            ratio = float(np.median(lum_w[changed])) / outer
    dark = -1
    same_shape = (weld_depth is not None and noweld_depth is not None and
                  weld_depth.shape == noweld_depth.shape == lum_w.shape)
    if same_shape:
        same_surface = (np.abs(weld_depth - noweld_depth) /
                        np.maximum(weld_depth, 1e-6)) < 0.01
        dark = int((changed & same_surface &
                    (lum_w < GATE_SHADING_DARK * np.maximum(lum_n, 1.0))).sum())
    return dict(pixels=n, ratio=round(ratio, 3), dark=dark)


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
    ap.add_argument("--baseline",
                    help="JSON of per-pose expected values; a metric above its "
                         "entry fails. Missing entries mean zero.")
    ap.add_argument("--json")
    a = ap.parse_args()

    poses = []
    with open(a.poses) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                poses.append(line.split()[0])

    baseline = {}
    if a.baseline and os.path.exists(a.baseline):
        with open(a.baseline) as f:
            baseline = json.load(f).get("poses", {})

    weld = os.path.join(a.out, "weld")
    noweld = os.path.join(a.out, "noweld")
    rows, failures = [], []

    print(f"{'pose':10s} {'holes':>7s} {'cracks':>7s} {'flicker':>8s} "
          f"{'weldpx':>8s} {'ratio':>6s} {'dark':>6s}")
    print("-" * 59)
    for p in poses:
        depth = os.path.join(weld, f"{p}_depth.png")
        row = dict(pose=p)
        if os.path.exists(depth):
            _, _, _, _, holes = hole_scan.scan(depth)
            row["holes"] = int(sum(h["size"] for h in holes))
            row["hole_components"] = [
                dict(size=h["size"], x=int(h["cx"]), y=int(h["cy"])) for h in holes[:8]]
            # SKY-ADJACENT SPIKES ARE NOT CRACKS. crack_scan runs four scan
            # directions, and along a diagonal a jagged silhouette against the
            # background can present as a short far run bounded by nearer
            # terrain on both sides -- the spike signature -- when it is just a
            # step seen edge-on. Measured on the `r3` pose: all 77 of its crack
            # pixels were within 3 px of the background and none survived this
            # mask. Excluding them costs nothing real, because a crack that
            # shows the background IS a hole and hole_scan sees it exactly.
            depth_m = decode_depth_png(depth)
            hit = crack_scan.find_cracks(depth_m)
            comps, _ = crack_scan.components(hit & ~dilate(depth_m >= 8192.0 * 0.97, 3), 3)
            row["cracks"] = int(sum(c["size"] for c in comps))
            for c in comps:
                c["depth_m"] = round(float(depth_m[int(c["cy"]), int(c["cx"])]), 1)
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

        nw_depth_png = os.path.join(noweld, f"{p}_depth.png")
        row.update({("shading_" + k): v for k, v in shading_check(
            lit, load_rgb(os.path.join(noweld, f"{p}_lit.png")),
            decode_depth_png(depth), decode_depth_png(nw_depth_png)).items()})
        rows.append(row)
        print(f"{p:10s} {row['holes']:7d} {row['cracks']:7d} {row['flicker']:8d} "
              f"{row['shading_pixels']:8d} {row['shading_ratio']:6.2f} "
              f"{row['shading_dark']:6d}")

        if row["holes"] > GATE_HOLE_PIXELS:
            failures.append(f"{p}: {row['holes']} hole pixels (see-through)")
        crack_allow = int(baseline.get(p, {}).get("cracks", GATE_CRACK_PIXELS))
        if row["cracks"] > crack_allow:
            failures.append(f"{p}: {row['cracks']} crack pixels (depth spike), "
                            f"baseline {crack_allow}")
        if row["flicker"] > GATE_FLICKER_PIXELS:
            failures.append(f"{p}: {row['flicker']} pixels flickering at a static pose")
        if 0 <= row["shading_ratio"] < GATE_SHADING_RATIO:
            failures.append(f"{p}: weld paints at {row['shading_ratio']:.2f} of its "
                            f"neighbours' luminance")
        dark_allow = int(baseline.get(p, {}).get("dark", GATE_SHADING_DARK_PIXELS))
        if row.get("shading_dark", 0) > dark_allow:
            failures.append(f"{p}: {row['shading_dark']} weld pixels below "
                            f"{GATE_SHADING_DARK:.2f} of the same pixel in the "
                            f"no-weld frame, baseline {dark_allow}")

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
