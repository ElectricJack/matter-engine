#!/usr/bin/env python3
"""Attribute native/browser matched-view depth differences to rasterizer
subpixel precision: ray-cast the browser's own world triangles (the
`triangles` of a browser_matched.py --dump) at every covered pixel centre,
with vertices unsnapped and snapped to 1/2^bits px, and report each
renderer's depth error against each.
usage: subpixel_snap_check.py <native.json> <browser.json>"""
import json, math, sys
native, browser = (json.load(open(p)) for p in sys.argv[1:3])
W, H = native["width"], native["height"]
# The authoring e2e camera: x [-2,4], y [-1,6] (row 0 = top), depth 0.1z+0.8.
X0, XW, Y0, YH = -2.0, 6.0, -1.0, 7.0


def triangles(bits):
    out = []
    for inst in browser["triangles"]:
        c = inst["corners"]
        for t in range(0, len(c), 18):
            tri = []
            for k in range(3):
                x, y, z, *n = c[t + 6 * k:t + 6 * k + 6]
                if bits is not None:
                    q = 2 ** bits
                    x = round((x - X0) / XW * W * q) / q * XW / W + X0
                    y = round((y - Y0) / YH * H * q) / q * YH / H + Y0
                tri.append((x, y, z))
            out.append(tri)
    return out


def nearest_z(tris, x, y):
    best = None
    for (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) in tris:
        d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if d <= 1e-15:  # back-facing or degenerate in view
            continue
        a = ((y1 - y2) * (x - x2) + (x2 - x1) * (y - y2)) / d
        b = ((y2 - y0) * (x - x2) + (x0 - x2) * (y - y2)) / d
        if min(a, b, 1 - a - b) < -1e-9:
            continue
        z = a * z0 + b * z1 + (1 - a - b) * z2
        best = z if best is None else max(best, z)
    return best


for bits in (None, 8, 4):
    tris = triangles(bits)
    err = {"native": [0.0, 0], "browser": [0.0, 0]}
    hits = 0
    for i in range(W * H):
        if native["identity"][i] < 0:
            continue
        r, col = divmod(i, W)
        z = nearest_z(tris, X0 + XW * (col + 0.5) / W, Y0 + YH - YH * (r + 0.5) / H)
        if z is None:
            continue
        hits += 1
        for name, rec in (("native", native), ("browser", browser)):
            e = abs(rec["depth"][i] - (0.1 * z + 0.8))
            err[name][0] = max(err[name][0], e)
            err[name][1] += e > 1e-5
    label = "exact" if bits is None else f"1/{2 ** bits} px snap"
    print(f"{label:>14}: native depth err max {err['native'][0]:.2e} ({err['native'][1]} px > 1e-5), "
          f"browser max {err['browser'][0]:.2e} ({err['browser'][1]} px > 1e-5) of {hits} px")
