#!/usr/bin/env python3
"""Analyze MATTER_VT_TRACE captures without treating absent evidence as zero.

Capture lifecycle remains owned by MatterEngine3/tools/drive.py. This analyzer
accepts the bounded JSONL trace written after an editor session, optionally
restricted by two `stats` markers. It reports only the gates it can establish;
GPU VT time, request latency and page-identity proofs need their own evidence.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path


class CaptureError(ValueError):
    pass


REQUIRED = {
    "active", "variants", "pool_used", "pool_capacity", "pinned", "queue",
    "fills_total", "evictions_total", "invalidations_total", "pages_dropped_total",
    "fills_failed_total", "requests_dropped_total", "enrich_total", "enrich_queue",
    "resident_sectors", "vertex_uploads", "cluster_uploads", "dlss_resets",
    "cpu_begin_ms", "cpu_pre_pass_ms", "cpu_post_pass_ms", "cpu_registration_ms",
    "gpu_vt_ms", "gpu_readback_sequence",
}
UNCHANGED = (
    "variants", "fills_total", "evictions_total", "invalidations_total",
    "pages_dropped_total", "fills_failed_total", "requests_dropped_total", "enrich_total",
    "resident_sectors", "vertex_uploads", "cluster_uploads",
)
CPU = ("cpu_begin_ms", "cpu_pre_pass_ms", "cpu_post_pass_ms", "cpu_registration_ms")
QUEUE_CLASSES = ("mandatory_queue", "detail_queue", "oldest_mandatory_age_frames",
                 "oldest_detail_age_frames")
REPLACEMENT = ("replacement_reserve_pages", "dirty_pages", "fills_stale_total")


def integer(value, name):
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise CaptureError(f"{name} must be a nonnegative integer")
    return value


def load_trace(path):
    try:
        records = [json.loads(line) for line in Path(path).read_text(encoding="utf-8").splitlines()
                   if line.strip()]
    except (OSError, ValueError) as error:
        raise CaptureError(f"cannot read complete trace: {error}") from error
    if len(records) < 2 or not all(isinstance(r, dict) for r in records):
        raise CaptureError("trace needs a schema header and terminal record")
    header, *rows, footer = records
    if header.get("schema") != 1:
        raise CaptureError("unsupported trace schema")
    columns = header.get("columns")
    if not isinstance(columns, list) or not all(isinstance(c, str) for c in columns):
        raise CaptureError("columns must be a list of names")
    if len(set(columns)) != len(columns) or not REQUIRED.issubset(columns):
        raise CaptureError("duplicate or missing required columns")
    if set(columns).intersection(QUEUE_CLASSES) and not set(QUEUE_CLASSES).issubset(columns):
        raise CaptureError("partial mandatory/detail queue schema")
    if set(columns).intersection(REPLACEMENT) and not set(REPLACEMENT).issubset(columns):
        raise CaptureError("partial replacement schema")
    if footer.get("end") is not True or integer(footer.get("rows"), "rows") != len(rows):
        raise CaptureError("missing terminal record or mismatched row count")
    dropped = integer(footer.get("dropped_rows"), "dropped_rows")
    capacity = integer(header.get("max_rows"), "max_rows")
    if not capacity or len(rows) > capacity:
        raise CaptureError("capture exceeds its declared capacity")
    if header.get("gpu_time_basis") != "latest_retired_readback_not_current_frame":
        raise CaptureError("GPU readback time basis is missing or unsupported")
    previous_serial, previous_time = -1, -1.0
    decoded = []
    for row in rows:
        serial = integer(row.get("serial"), "serial")
        integer(row.get("vt_serial"), "vt_serial")
        elapsed = row.get("elapsed_ms")
        if isinstance(elapsed, bool) or not isinstance(elapsed, (int, float)) or not math.isfinite(elapsed):
            raise CaptureError("elapsed_ms must be finite")
        if serial <= previous_serial or elapsed < previous_time:
            raise CaptureError("presentation serial/time is not monotonic")
        if not isinstance(row.get("marker"), str):
            raise CaptureError("marker must be a string")
        values = row.get("values")
        if not isinstance(values, list) or len(values) != len(columns):
            raise CaptureError("row width differs from the schema")
        for name, value in zip(columns, values):
            if value is None:
                if not (name.startswith("cpu_") or name.startswith("gpu_")):
                    raise CaptureError(f"missing counter {name}")
            elif (isinstance(value, bool) or not isinstance(value, (int, float))
                  or not math.isfinite(value) or value < 0):
                raise CaptureError(f"invalid numeric value for {name}")
        decoded.append({**row, "data": dict(zip(columns, values))})
        if set(QUEUE_CLASSES).issubset(columns):
            data = decoded[-1]["data"]
            if data["mandatory_queue"] + data["detail_queue"] != data["queue"]:
                raise CaptureError("queue classes do not sum to total pending work")
        if set(REPLACEMENT).issubset(columns):
            data = decoded[-1]["data"]
            if (data["replacement_reserve_pages"] + data["pool_used"] > data["pool_capacity"] or
                    data["dirty_pages"] > data["pool_used"]):
                raise CaptureError("replacement state exceeds pool capacity")
        previous_serial, previous_time = serial, elapsed
    return header, decoded, dropped


def percentile(values, fraction):
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * fraction) - 1)] if values else None


def distribution(values):
    return {"samples": len(values), "p50_ms": percentile(values, .50),
            "p95_ms": percentile(values, .95), "p99_ms": percentile(values, .99),
            "max_ms": max(values) if values else None}


def analyze_trace(path, begin_marker=None, end_marker=None, required_frames=600):
    if bool(begin_marker) != bool(end_marker):
        raise CaptureError("provide both begin and end markers")
    if required_frames < 1:
        raise CaptureError("required_frames must be positive")
    header, rows, dropped = load_trace(path)
    if begin_marker:
        def locate(marker):
            indices = [i for i, row in enumerate(rows) if row["marker"] == marker]
            if len(indices) != 1:
                raise CaptureError(f"marker {marker!r} must occur exactly once")
            return indices[0]
        first, last = locate(begin_marker), locate(end_marker)
        if first >= last:
            raise CaptureError("markers are reversed or select an empty interval")
        rows = rows[first:last + 1]
    if not rows:
        raise CaptureError("no captured rows in selected interval")
    replacement_available = set(REPLACEMENT).issubset(header["columns"])
    unchanged = UNCHANGED + (("fills_stale_total",) if replacement_available else ())
    changes = {name: sum(a["data"][name] != b["data"][name]
                         for a, b in zip(rows, rows[1:])) for name in unchanged}
    resets = sum(b["vt_serial"] < a["vt_serial"] for a, b in zip(rows, rows[1:]))
    active = [r for r in rows if r["data"]["active"] == 1 and r["vt_serial"] > 0]
    unique = []
    previous = None
    for row in active:
        if row["vt_serial"] != previous:
            unique.append(row)
        previous = row["vt_serial"]
    cpu = [sum(r["data"][k] for k in CPU) for r in unique
           if all(r["data"][k] is not None for k in CPU)]
    # Legacy captures omit demand selection. Keep their explicitly named hook
    # metric comparable, but never substitute zero for the missing CPU stage.
    demand = [r["data"]["cpu_demand_ms"] for r in unique
              if r["data"].get("cpu_demand_ms") is not None]
    complete_cpu = [sum(r["data"][k] for k in (*CPU, "cpu_demand_ms"))
                    for r in unique
                    if all(r["data"].get(k) is not None for k in (*CPU, "cpu_demand_ms"))]
    cpu_missing = len(unique) - len(complete_cpu)
    cpu_available = bool(unique) and cpu_missing == 0
    gpu, seen_gpu = [], set()
    for row in active:
        sequence, value = row["data"]["gpu_readback_sequence"], row["data"]["gpu_vt_ms"]
        if value is not None and sequence and sequence not in seen_gpu:
            gpu.append(value)
            seen_gpu.add(sequence)
    queue_empty = all(r["data"]["queue"] == 0 and r["data"]["enrich_queue"] == 0 for r in rows)
    dirty_empty = all(r["data"]["dirty_pages"] == 0 for r in rows) if replacement_available else None
    no_changes = not any(changes.values())
    settled = (bool(begin_marker) and dropped == 0 and resets == 0
               and len(active) == len(rows) and len(unique) >= required_frames
               and queue_empty and dirty_empty is not False and no_changes)
    return {
        "schema": 1, "trace": str(Path(path)), "selected_rows": len(rows),
        "distinct_active_vt_frames": len(unique), "dropped_rows": dropped,
        "owner_frame_resets": resets, "changed_frame_pairs": changes,
        "queue_empty_throughout": queue_empty,
        "replacement": ({"available": True, "dirty_empty_throughout": dirty_empty,
                         "max_dirty_pages": max(r["data"]["dirty_pages"] for r in rows),
                         "reserve_pages": max(r["data"]["replacement_reserve_pages"] for r in rows)}
                        if replacement_available else {"available": False}),
        "queue_classes": ({"available": True,
                           **{f"max_{name}": max(r["data"][name] for r in rows)
                              for name in QUEUE_CLASSES}}
                          if set(QUEUE_CLASSES).issubset(header["columns"])
                          else {"available": False}),
        "settled_interval_counter_gate": settled,
        "required_frames": required_frames,
        "cpu_scope": header.get("cpu_scope", "unspecified"),
        "gpu_scope": header.get("gpu_scope", "unspecified"),
        "cpu_vt_hooks_and_registration": distribution(cpu),
        "cpu_vt_demand": distribution(demand),
        "cpu_vt_render_thread": distribution(complete_cpu if cpu_available else []),
        "cpu_vt_render_thread_available": cpu_available,
        "cpu_vt_render_thread_missing_frames": cpu_missing,
        "gpu_vt": distribution(gpu),
        "gpu_vt_available": bool(gpu),
        "full_vt_acceptance": False,
        "limits": ["Counter stability does not prove image continuity or page identity.",
                   "Request-to-visible latency is not measured by this schema.",
                   "GPU VT time must be supplied by dedicated timestamps; null is unavailable.",
                   "The marked camera/quality settings require the capture manifest and images."],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--begin-marker")
    parser.add_argument("--end-marker")
    parser.add_argument("--required-frames", type=int, default=600)
    parser.add_argument("--require-settled", action="store_true")
    args = parser.parse_args()
    try:
        result = analyze_trace(args.trace, args.begin_marker, args.end_marker, args.required_frames)
    except CaptureError as error:
        print(json.dumps({"error": str(error)}))
        return 2
    text = json.dumps(result, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return int(args.require_settled and not result["settled_interval_counter_gate"])


if __name__ == "__main__":
    raise SystemExit(main())
