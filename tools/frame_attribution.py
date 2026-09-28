#!/usr/bin/env python3
"""Tabulate GPU zone statistics from one or more MATTER_PERF_OUTPUT files.

Reads `gpu_pass_statistics.passes` (median/p95/p99/max per zone) and the raw
`frame_times_ms` array, and writes a markdown table so an A/B of render paths
(for example MATTER_GBUFFER_POM_PATH variants) is one command. It reports what
the perf run measured; it establishes no target and infers nothing.

One row per zone, one "median / p95 / p99" column per input file, rows sorted
by the first file's p95 descending (zones without samples last). The
`frame_interval` row is the end-to-end frame cadence. Files written before
perf.json carried `frame_times_ms` fall back to its `median_frame_ms` /
`p95_frame_ms` summary, with no p99.
"""
import argparse
import json
import math
import pathlib
import statistics


class AttributionError(RuntimeError):
    pass


def _load(path):
    try:
        data = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise AttributionError(f"{path}: {error}") from error
    passes = data.get("gpu_pass_statistics", {}).get("passes") if isinstance(data, dict) else None
    if not isinstance(passes, dict):
        raise AttributionError(f"{path}: no gpu_pass_statistics.passes block")
    return passes, data


def _fmt(value):
    return "—" if value is None else f"{value:.2f}"


def _nearest_rank(sorted_values, q):
    # Same index as PerfGpuStats and write_perf_result: sorted[ceil(q * n) - 1].
    return sorted_values[max(0, math.ceil(q * len(sorted_values)) - 1)]


def _frame_interval(data):
    frames = [ms for ms in data.get("frame_times_ms") or [] if ms is not None]
    if frames:
        s = sorted(frames)
        return statistics.median(s), _nearest_rank(s, 0.95), _nearest_rank(s, 0.99)
    return data.get("median_frame_ms"), data.get("p95_frame_ms"), None


def _cell(median, p95, p99):
    return f"{_fmt(median)} / {_fmt(p95)} / {_fmt(p99)}"


def _labels(paths):
    # Evidence folders usually hold one perf.json each; name those by folder.
    paths = [pathlib.Path(p) for p in paths]
    stems = [p.stem for p in paths]
    return [f"{p.parent.name}/{p.stem}" if stems.count(p.stem) > 1 else p.stem for p in paths]


def render(paths):
    loaded = [_load(p) for p in paths]
    names = _labels(paths)
    first = loaded[0][0]

    def order(zone):
        p95 = first.get(zone, {}).get("p95_ms")
        return (zone not in first, p95 is None, -(p95 or 0.0), zone)

    zones = sorted({zone for passes, _ in loaded for zone in passes}, key=order)
    lines = ["| zone | " + " | ".join(f"{n} median / p95 / p99" for n in names) + " |",
             "|" + "---|" * (len(names) + 1)]
    lines.append("| frame_interval | "
                 + " | ".join(_cell(*_frame_interval(data)) for _, data in loaded) + " |")
    for zone in zones:
        cells = []
        for passes, _ in loaded:
            s = passes.get(zone, {})
            cells.append(_cell(s.get("median_ms"), s.get("p95_ms"), s.get("p99_ms")))
        lines.append(f"| {zone} | " + " | ".join(cells) + " |")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("perf_json", nargs="+", help="MATTER_PERF_OUTPUT files; the first sets row order")
    ap.add_argument("--out", help="write the markdown table here instead of stdout")
    args = ap.parse_args()
    try:
        table = render(args.perf_json)
    except AttributionError as error:
        ap.exit(1, f"frame_attribution: {error}\n")
    if args.out:
        pathlib.Path(args.out).write_text(table, encoding="utf-8")
    else:
        print(table, end="")


if __name__ == "__main__":
    main()
