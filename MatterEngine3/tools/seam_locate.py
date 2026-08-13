#!/usr/bin/env python3
"""seam_locate.py -- put a screen-space seam defect back on the tile grid.

hole_scan and crack_scan say a defect is at pixel (x, y). That is not something
you can fix. This turns it into a world position and then into the tile
boundary it sits on, which is: which axis the shared plane is perpendicular to,
which level's grid it belongs to, and how far off the nearest border of each
level's grid it lands.

The camera model is composite.frag's `compute_view_ray` verbatim, including its
world-up flip at |fwd.y| > 0.999 -- the straight-down poses in seam_lab_poses.txt
are exactly the case that flip exists for, so getting it wrong silently rotates
every answer by 90 degrees.

Depth is eye-space LINEAR depth (composite_linear_depth), not distance along the
ray, so the ray parameter is `depth / dot(ray, forward)` -- the same correction
the shader applies before it reconstructs a world position.

Usage:
    seam_locate.py <depth.png> --eye X,Y,Z --target X,Y,Z
                   [--sector-size 32] [--levels 4] [--fov-deg 45]
                   [--mode holes|cracks] [--top N]
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import crack_scan  # noqa: E402
import hole_scan   # noqa: E402


def basis(eye, target):
    fwd = target - eye
    fwd = fwd / np.linalg.norm(fwd)
    world_up = np.array([0.0, 1.0, 0.0]) if abs(fwd[1]) < 0.999 \
        else np.array([0.0, 0.0, 1.0])
    right = np.cross(fwd, world_up)
    right = right / np.linalg.norm(right)
    up = np.cross(right, fwd)
    return fwd, right, up


def world_at(px, py, W, H, eye, fwd, right, up, tan_half_fov, depth):
    ndc_x = (px + 0.5) / W * 2.0 - 1.0
    ndc_y = 1.0 - (py + 0.5) / H * 2.0
    ray = fwd + right * ndc_x * (W / H) * tan_half_fov + up * ndc_y * tan_half_fov
    ray = ray / np.linalg.norm(ray)
    t = depth / max(float(np.dot(ray, fwd)), 1e-4)
    return eye + ray * t


def grid_report(p, sector_size, levels):
    """Distance from p to the nearest tile border of each level, per axis."""
    out = []
    for lv in range(levels):
        size = sector_size * (1 << lv)
        d = []
        for axis in range(3):
            q = p[axis] / size
            d.append(abs(q - round(q)) * size)
        out.append((lv, size, d))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("depth")
    ap.add_argument("--eye", required=True)
    ap.add_argument("--target", required=True)
    ap.add_argument("--fov-deg", type=float, default=45.0)
    ap.add_argument("--sector-size", type=float, default=32.0)
    ap.add_argument("--levels", type=int, default=4)
    ap.add_argument("--mode", default="holes", choices=("holes", "cracks"))
    ap.add_argument("--top", type=int, default=10)
    a = ap.parse_args()

    eye = np.array([float(v) for v in a.eye.split(",")])
    target = np.array([float(v) for v in a.target.split(",")])
    fwd, right, up = basis(eye, target)
    tan_half = np.tan(np.radians(a.fov_deg) * 0.5)

    rgb = np.asarray(Image.open(a.depth).convert("RGB")).astype(np.float64)
    depth = crack_scan.decode_depth(rgb)
    H, W = depth.shape

    if a.mode == "holes":
        _, _, _, _, comps = hole_scan.scan(a.depth)
    else:
        _, comps, _ = crack_scan.scan(a.depth)

    print(f"{a.depth}: {len(comps)} {a.mode} components, "
          f"eye {eye} fwd {np.round(fwd, 3)}")
    for c in comps[:a.top]:
        cx, cy = int(round(c["cx"])), int(round(c["cy"]))
        # A hole's own depth is the background; sample the ring around its
        # bounding box instead, which is the terrain the hole is a hole IN.
        x0, x1 = max(0, c["x0"] - 2), min(W - 1, c["x1"] + 2)
        y0, y1 = max(0, c["y0"] - 2), min(H - 1, c["y1"] + 2)
        ring = np.concatenate([depth[y0, x0:x1 + 1], depth[y1, x0:x1 + 1],
                               depth[y0:y1 + 1, x0], depth[y0:y1 + 1, x1]])
        ring = ring[ring < 8192.0 * 0.97]
        if ring.size == 0:
            print(f"  {c['size']:6d} px at ({cx},{cy}) -- no terrain around it")
            continue
        d = float(np.median(ring))
        p = world_at(cx, cy, W, H, eye, fwd, right, up, tan_half, d)
        line = (f"  {c['size']:6d} px at ({cx},{cy})  depth {d:7.1f} m  "
                f"world ({p[0]:8.2f},{p[1]:8.2f},{p[2]:8.2f})")
        best = None
        for lv, size, dist in grid_report(p, a.sector_size, a.levels):
            for axis, dv in enumerate(dist):
                if best is None or dv < best[2]:
                    best = (lv, "xyz"[axis], dv, size)
        line += f"  | nearest border: L{best[0]} ({best[3]:.0f} m) " \
                f"{best[1]}-plane, {best[2]:.2f} m off"
        print(line)
        for lv, size, dist in grid_report(p, a.sector_size, a.levels):
            print(f"        L{lv} {size:5.0f} m grid: dx {dist[0]:7.2f} "
                  f"dy {dist[1]:7.2f} dz {dist[2]:7.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
