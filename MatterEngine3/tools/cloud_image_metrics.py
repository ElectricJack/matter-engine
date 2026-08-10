#!/usr/bin/env python3
"""Dependency-free final-acceptance image metrics for cloud captures.

The final harness already relies on ffmpeg for SSIM.  This companion decodes
one RGB frame with the same tool and adds meaningful changed-area, edge-seam,
and localized-flash checks without requiring Pillow or NumPy.
"""

import argparse
import pathlib
import subprocess
import sys


def summarize_rgb(width, height, before, after, changed_threshold=3, tiles=4,
                  edge_fraction=0.1):
    """Return deterministic RGB absolute-difference measurements.

    `before` and `after` are packed rgb24 bytes.  A four-by-four tile grid
    distinguishes a moving cloud from a one-tile flash, and the outer ten per
    cent exposes an obvious near/far seam at the frame boundary.
    """
    expected = width * height * 3
    if width <= 0 or height <= 0 or len(before) != expected or len(after) != expected:
        raise ValueError("rgb24 frame sizes do not match the declared dimensions")
    if tiles <= 0:
        raise ValueError("tile count must be positive")
    edge_x = max(1, int(width * edge_fraction))
    edge_y = max(1, int(height * edge_fraction))
    total = changed = edge_total = edge_count = 0
    tile_totals = [0] * (tiles * tiles)
    tile_counts = [0] * (tiles * tiles)
    for y in range(height):
        for x in range(width):
            offset = (y * width + x) * 3
            difference = (abs(before[offset] - after[offset]) +
                          abs(before[offset + 1] - after[offset + 1]) +
                          abs(before[offset + 2] - after[offset + 2])) / 3.0
            total += difference
            if difference > changed_threshold:
                changed += 1
            tile = min(tiles - 1, y * tiles // height) * tiles + min(
                tiles - 1, x * tiles // width)
            tile_totals[tile] += difference
            tile_counts[tile] += 1
            if x < edge_x or x >= width - edge_x or y < edge_y or y >= height - edge_y:
                edge_total += difference
                edge_count += 1
    tile_means = [value / count for value, count in zip(tile_totals, tile_counts)]
    return {
        "mean_abs": total / (width * height),
        "changed_pct": 100.0 * changed / (width * height),
        "active_tiles": sum(value > changed_threshold for value in tile_means),
        "max_tile_mean": max(tile_means),
        "edge_mean": edge_total / edge_count,
    }


def effect_failure(metrics, min_mean, min_changed_pct, min_active_tiles):
    """Explain why an enabled/disabled or receiver effect is too weak."""
    failures = []
    if metrics["mean_abs"] < min_mean:
        failures.append("mean_abs %.4f < %.4f" % (metrics["mean_abs"], min_mean))
    if metrics["changed_pct"] < min_changed_pct:
        failures.append("changed_pct %.4f < %.4f" %
                        (metrics["changed_pct"], min_changed_pct))
    if metrics["active_tiles"] < min_active_tiles:
        failures.append("active_tiles %d < %d" %
                        (metrics["active_tiles"], min_active_tiles))
    return "; ".join(failures) or None


def motion_failure(metrics, min_mean, min_changed_pct, max_edge_mean,
                   max_tile_mean):
    """Explain a weak movement, boundary seam, or localized-frame flash."""
    failure = effect_failure(metrics, min_mean, min_changed_pct, 1)
    failures = [failure] if failure else []
    if metrics["edge_mean"] > max_edge_mean:
        failures.append("edge_mean %.4f > %.4f" %
                        (metrics["edge_mean"], max_edge_mean))
    if metrics["max_tile_mean"] > max_tile_mean:
        failures.append("max_tile_mean %.4f > %.4f" %
                        (metrics["max_tile_mean"], max_tile_mean))
    return "; ".join(failures) or None


def probe_dimensions(image, ffmpeg):
    executable = pathlib.Path(ffmpeg)
    probe = executable.with_name("ffprobe.exe" if executable.suffix.lower() == ".exe"
                                 else "ffprobe")
    output = subprocess.check_output(
        [str(probe), "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height", "-of", "csv=p=0", image],
        text=True).strip()
    try:
        width, height = (int(value) for value in output.split(","))
    except ValueError as exc:
        raise RuntimeError("ffprobe returned invalid dimensions: %r" % output) from exc
    return width, height


def decode_rgb(image, ffmpeg):
    return subprocess.check_output(
        [ffmpeg, "-v", "error", "-i", image, "-frames:v", "1", "-f",
         "rawvideo", "-pix_fmt", "rgb24", "-"], stderr=subprocess.PIPE)


def print_metrics(metrics):
    print("mean_abs={mean_abs:.4f},changed_pct={changed_pct:.4f},"
          "active_tiles={active_tiles},edge_mean={edge_mean:.4f},"
          "max_tile_mean={max_tile_mean:.4f}".format(**metrics))


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("effect", "motion"))
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--min-mean", type=float, required=True)
    parser.add_argument("--min-changed-pct", type=float, required=True)
    parser.add_argument("--min-active-tiles", type=int, default=1)
    parser.add_argument("--max-edge-mean", type=float, default=float("inf"))
    parser.add_argument("--max-tile-mean", type=float, default=float("inf"))
    args = parser.parse_args(argv)
    width, height = probe_dimensions(args.before, args.ffmpeg)
    other_width, other_height = probe_dimensions(args.after, args.ffmpeg)
    if (width, height) != (other_width, other_height):
        raise RuntimeError("image dimensions differ: %dx%d vs %dx%d" %
                           (width, height, other_width, other_height))
    metrics = summarize_rgb(width, height, decode_rgb(args.before, args.ffmpeg),
                            decode_rgb(args.after, args.ffmpeg))
    print_metrics(metrics)
    if args.mode == "effect":
        failure = effect_failure(metrics, args.min_mean, args.min_changed_pct,
                                 args.min_active_tiles)
    else:
        failure = motion_failure(metrics, args.min_mean, args.min_changed_pct,
                                 args.max_edge_mean, args.max_tile_mean)
    if failure:
        print("ERROR: " + failure, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
