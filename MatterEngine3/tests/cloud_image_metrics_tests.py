#!/usr/bin/env python3
"""Focused stdlib tests for final cloud-image acceptance metrics."""

import pathlib
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

from cloud_image_metrics import effect_failure, motion_failure, summarize_rgb


def rgb_frame(width, height, edits=()):
    pixels = bytearray(width * height * 3)
    for x, y, value in edits:
        offset = (y * width + x) * 3
        pixels[offset:offset + 3] = bytes((value, value, value))
    return bytes(pixels)


def test_effect_requires_meaningful_changed_area():
    width = height = 8
    before = rgb_frame(width, height)
    after = rgb_frame(width, height, [(3, 3, 64), (4, 3, 64),
                                      (3, 4, 64), (4, 4, 64)])
    metrics = summarize_rgb(width, height, before, after)
    assert effect_failure(metrics, min_mean=1.0, min_changed_pct=1.0,
                          min_active_tiles=1) is None
    assert effect_failure(metrics, min_mean=10.0, min_changed_pct=1.0,
                          min_active_tiles=1) is not None
    assert effect_failure(metrics, min_mean=1.0, min_changed_pct=10.0,
                          min_active_tiles=1) is not None


def test_motion_rejects_a_border_seam_and_single_tile_flash():
    width = height = 8
    before = rgb_frame(width, height)
    border = rgb_frame(width, height,
                       [(x, y, 255) for y in range(height) for x in range(width)
                        if x == 0 or y == 0 or x == width - 1 or y == height - 1])
    seam = summarize_rgb(width, height, before, border)
    assert motion_failure(seam, min_mean=1.0, min_changed_pct=1.0,
                          max_edge_mean=30.0, max_tile_mean=255.0) is not None

    # With the helper's 4x4 grid, this is one complete interior tile: it must
    # not be accepted as legitimate cloud motion merely because it is local.
    flash = rgb_frame(width, height,
                      [(x, y, 255) for y in range(2, 4) for x in range(2, 4)])
    flash_metrics = summarize_rgb(width, height, before, flash)
    assert motion_failure(flash_metrics, min_mean=1.0, min_changed_pct=1.0,
                          max_edge_mean=255.0, max_tile_mean=100.0) is not None


def test_motion_accepts_a_bounded_local_cloud_change():
    width = height = 8
    before = rgb_frame(width, height)
    after = rgb_frame(width, height, [(3, 3, 32), (4, 3, 32),
                                      (3, 4, 32), (4, 4, 32)])
    metrics = summarize_rgb(width, height, before, after)
    assert motion_failure(metrics, min_mean=1.0, min_changed_pct=1.0,
                          max_edge_mean=4.0, max_tile_mean=40.0) is None


if __name__ == "__main__":
    test_effect_requires_meaningful_changed_area()
    test_motion_rejects_a_border_seam_and_single_tile_flash()
    test_motion_accepts_a_bounded_local_cloud_change()
    print("ALL PASS")
