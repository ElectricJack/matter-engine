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

`--hitches` writes the repeat-run summary instead: one row per file with the
GPU `total` zone's median / p99 / max, the frame interval's median / p99 / max
and its counts over 100 ms and over 1 s, then a frame-interval histogram with
one column per file plus the pooled frames. A sibling `<stem>.gpu_during.csv`
(the `nvidia-smi` log `tools/streammountain_attribution.sh` writes) adds the
run's peak whole-GPU `memory.used`.
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


# Frame-interval histogram edges in ms. A frame lands in the first bucket whose
# upper edge it does not exceed, so 100.0 ms is in 50-100 and not a hitch.
HITCH_EDGES_MS = (16.7, 33.3, 50.0, 100.0, 250.0, 500.0, 1000.0, 2000.0)
HITCH_THRESHOLDS_MS = (100.0, 1000.0)


def _frames(path, data):
    frames = [ms for ms in data.get("frame_times_ms") or [] if ms is not None]
    if not frames:
        raise AttributionError(f"{path}: no frame_times_ms samples to count hitches in")
    return frames


def _peak_vram_mib(path):
    # nvidia-smi --format=csv: "timestamp, utilization.gpu [%], memory.used [MiB]".
    log = pathlib.Path(path).with_suffix(".gpu_during.csv")
    try:
        lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None
    used = []
    for line in lines[1:]:
        cells = [c.strip() for c in line.split(",")]
        if len(cells) >= 3 and cells[2].endswith("MiB"):
            try:
                used.append(float(cells[2][:-3]))
            except ValueError:
                pass
    return max(used) if used else None


def _bucket_label(lo, hi):
    return f"> {lo:g} ms" if hi is None else f"{lo:g}–{hi:g} ms"


def render_hitches(paths):
    loaded = [_load(p) for p in paths]
    names = _labels(paths)
    frame_sets = [_frames(p, data) for p, (_, data) in zip(paths, loaded)]
    lines = ["| run | frames | GPU total median / p99 / max ms | frame interval median / p99 / max ms"
             " | > 100 ms | > 1 s | static uploads in window | peak VRAM MiB |",
             "|---|---|---|---|---|---|---|---|"]

    def interval(frames):
        s = sorted(frames)
        return f"{statistics.median(s):.2f} / {_nearest_rank(s, 0.99):.2f} / {s[-1]:.2f}"

    def over(frames, threshold):
        return sum(1 for ms in frames if ms > threshold)

    for name, (passes, data), frames, path in zip(names, loaded, frame_sets, paths):
        total = passes.get("total", {})
        gpu = f"{_fmt(total.get('median_ms'))} / {_fmt(total.get('p99_ms'))} / {_fmt(total.get('max_ms'))}"
        uploads = data.get("static_vertex_upload_delta")
        vram = _peak_vram_mib(path)
        lines.append(f"| {name} | {len(frames)} | {gpu} | {interval(frames)} | "
                     + " | ".join(str(over(frames, t)) for t in HITCH_THRESHOLDS_MS)
                     + f" | {'—' if uploads is None else uploads} | {'—' if vram is None else f'{vram:.0f}'} |")
    pooled = [ms for frames in frame_sets for ms in frames]
    if len(frame_sets) > 1:
        lines.append(f"| pooled | {len(pooled)} | — | {interval(pooled)} | "
                     + " | ".join(str(over(pooled, t)) for t in HITCH_THRESHOLDS_MS) + " | — | — |")

    edges = (0.0,) + HITCH_EDGES_MS
    buckets = [(edges[i], edges[i + 1]) for i in range(len(HITCH_EDGES_MS))] + [(HITCH_EDGES_MS[-1], None)]
    columns = frame_sets + ([pooled] if len(frame_sets) > 1 else [])
    headers = names + (["pooled"] if len(frame_sets) > 1 else [])
    lines += ["", "| frame interval | " + " | ".join(headers) + " |", "|" + "---|" * (len(headers) + 1)]
    for lo, hi in buckets:
        counts = [sum(1 for ms in frames if (lo == 0.0 or ms > lo) and (hi is None or ms <= hi))
                  for frames in columns]
        lines.append(f"| {_bucket_label(lo, hi)} | " + " | ".join(map(str, counts)) + " |")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("perf_json", nargs="+", help="MATTER_PERF_OUTPUT files; the first sets row order")
    ap.add_argument("--out", help="write the markdown table here instead of stdout")
    ap.add_argument("--hitches", action="store_true",
                    help="write the per-run GPU total / hitch summary and frame-interval histogram")
    args = ap.parse_args()
    try:
        table = (render_hitches if args.hitches else render)(args.perf_json)
    except AttributionError as error:
        ap.exit(1, f"frame_attribution: {error}\n")
    if args.out:
        pathlib.Path(args.out).write_text(table, encoding="utf-8")
    else:
        print(table, end="")


if __name__ == "__main__":
    main()
