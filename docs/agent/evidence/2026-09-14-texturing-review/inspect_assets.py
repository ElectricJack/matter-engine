"""Read-only cached GTEX inspection. Requires numpy and Pillow.

Reports exact-color coverage and channel correlation after a one-tile shift.
Correlation measures these cached inputs, not rendered perceptual quality.
Does not assert cache freshness against current procedural source.
"""
import hashlib
import io
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def correlation(a, axis, tile_px):
    x = a.astype(np.float64)
    y = np.roll(x, tile_px, axis=axis)
    x -= x.mean(axis=(0, 1), keepdims=True)
    y -= y.mean(axis=(0, 1), keepdims=True)
    denom = np.sqrt(np.sum(x * x) * np.sum(y * y))
    return float(np.sum(x * y) / denom) if denom else None


for arg in sys.argv[1:]:
    data = Path(arg).read_bytes()
    magic, version = struct.unpack_from("<II", data)
    assert magic == 0x58455447 and version in (1, 2)
    channels = {}
    channel_stats = {}
    table = 56 if version == 2 else 48
    for k in range(6 if version == 2 else 4):
        cid, offset, size, width, height = struct.unpack_from("<5I", data, table + k * 20)
        blob = data[offset:offset + size]
        if cid == 3:
            pixels = np.frombuffer(blob, dtype="<u2").reshape(height, width, 1)
        else:
            pixels = np.array(Image.open(io.BytesIO(blob)))
            if pixels.ndim == 2:
                pixels = pixels[:, :, None]
        tile = width // 4
        unique = len({hashlib.sha256(pixels[y:y+tile, x:x+tile].tobytes()).digest()
                      for y in range(0, height, tile) for x in range(0, width, tile)})
        channel_stats[str(cid)] = {
            "width": width, "height": height, "unique_tiles": unique,
            "one_tile_shift_correlation_x": correlation(pixels, 1, tile),
            "one_tile_shift_correlation_y": correlation(pixels, 0, tile),
        }
        if cid == 0:
            rgb = pixels.astype(np.uint32)
            packed = rgb[:, :, 0] | (rgb[:, :, 1] << 8) | (rgb[:, :, 2] << 16)
            colors, counts = np.unique(packed, return_counts=True)
            index = counts.argmax()
            color = int(colors[index])
            dominant = {
                "rgb8": [color & 255, (color >> 8) & 255, (color >> 16) & 255],
                "fraction": float(counts[index] / packed.size),
            }
    print(json.dumps({
        "path": arg, "sha256": hashlib.sha256(data).hexdigest(),
        "file_bytes": len(data), "content_hash": f"{struct.unpack_from('<Q', data, 32)[0]:016x}",
        "dominant_albedo": dominant, "channels": channel_stats,
    }))
