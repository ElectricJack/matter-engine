#!/usr/bin/env python3
"""Strict RiverFloatLab raster-water forward-optics acceptance comparator."""

import argparse
import json
import math
import sys
from pathlib import Path


SHADOW_TAGS = ("01", "10", "16")
REQUIRED_SCREENSHOTS = (
    "shallow-player-low.png",
    "upper-rapids.png",
    "waterfall-side.png",
    "plunge-pool.png",
    "section-handoff.png",
)
ZERO_METRICS = (
    "validation_errors",
    "water_animation_decode_dispatch_delta",
    "water_animation_steady_state_allocation_delta",
)


def _number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _finite_number(record, key, label, failures):
    if key not in record:
        failures.append(f"{label} missing metric: {key}")
        return None
    value = record[key]
    if not _number(value) or not math.isfinite(value):
        failures.append(f"{label} metric {key} must be finite, got {value!r}")
        return None
    return value


def _format_number(value):
    return format(value, ".12g")


def _path_separation_failure(baseline_dir, candidate_dir):
    baseline_dir = Path(baseline_dir).resolve()
    candidate_dir = Path(candidate_dir).resolve()
    if baseline_dir == candidate_dir:
        return ("baseline and candidate directories must be distinct: "
                f"{baseline_dir}")
    if (candidate_dir.is_relative_to(baseline_dir) or
            baseline_dir.is_relative_to(candidate_dir)):
        return ("baseline and candidate directory trees must be disjoint: "
                f"{baseline_dir} <> {candidate_dir}")
    return None


def _load_json(path, label, failures):
    if not path.is_file():
        failures.append(f"missing metrics file: {path}")
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        failures.append(f"invalid metrics file {path}: {exc}")
        return None
    if not isinstance(value, dict):
        failures.append(f"{label} metrics must be a JSON object: {path}")
        return None
    return value


def _timing_result(name, baseline, candidate, relative_factor,
                   absolute_allowance, label, failures):
    baseline_value = _finite_number(baseline, name, f"{label} baseline", failures)
    candidate_value = _finite_number(candidate, name, f"{label} candidate", failures)
    if baseline_value is None or candidate_value is None:
        return None
    limit = max(baseline_value * relative_factor,
                baseline_value + absolute_allowance)
    result = {
        "baseline": baseline_value,
        "candidate": candidate_value,
        "delta": candidate_value - baseline_value,
        "limit": limit,
    }
    if candidate_value > limit:
        failures.append(
            f"{label} {name} {_format_number(candidate_value)} exceeds limit "
            f"{_format_number(limit)}")
    return result


def compare_acceptance(baseline_dir, candidate_dir, screenshots_dir):
    baseline_dir = Path(baseline_dir).resolve()
    candidate_dir = Path(candidate_dir).resolve()
    screenshots_dir = Path(screenshots_dir).resolve()
    failures = []
    summary = {
        "passed": False,
        "baseline_dir": str(baseline_dir),
        "candidate_dir": str(candidate_dir),
        "screenshots_dir": str(screenshots_dir),
        "settings": {},
        "screenshots": {},
        "failures": failures,
    }
    path_failure = _path_separation_failure(baseline_dir, candidate_dir)
    if path_failure is not None:
        failures.append(path_failure)
        return summary

    for tag in SHADOW_TAGS:
        label = f"shadow-{tag}"
        baseline_path = baseline_dir / f"{label}.json"
        candidate_path = candidate_dir / f"{label}.json"
        baseline = _load_json(baseline_path, f"{label} baseline", failures)
        candidate = _load_json(candidate_path, f"{label} candidate", failures)
        setting = {
            "baseline_path": str(baseline_path),
            "candidate_path": str(candidate_path),
        }
        summary["settings"][tag] = setting
        if baseline is None or candidate is None:
            continue

        expected_samples = int(tag)
        for role, record in (("baseline", baseline),
                             ("candidate", candidate)):
            world = record.get("world")
            if world != "RiverFloatLab":
                failures.append(
                    f"{label} {role} world expected 'RiverFloatLab', "
                    f"got {world!r}")
            samples = record.get("rt_samples")
            if (not _number(samples) or not math.isfinite(samples) or
                    not float(samples).is_integer() or
                    int(samples) != expected_samples):
                failures.append(
                    f"{label} {role} rt_samples expected {expected_samples}, "
                    f"got {samples!r}")

        median = _timing_result(
            "median_frame_ms", baseline, candidate, 1.08, 1.0,
            label, failures)
        p95 = _timing_result(
            "p95_frame_ms", baseline, candidate, 1.10, 2.0,
            label, failures)
        if median is not None:
            setting["median_frame_ms"] = median
        if p95 is not None:
            setting["p95_frame_ms"] = p95

        gates = {}
        setting["gates"] = gates
        for key in ZERO_METRICS:
            baseline_value = _finite_number(
                baseline, key, f"{label} baseline", failures)
            candidate_value = _finite_number(
                candidate, key, f"{label} candidate", failures)
            gates[key] = {
                "baseline": baseline_value,
                "candidate": candidate_value,
                "delta": (candidate_value - baseline_value
                          if baseline_value is not None and candidate_value is not None
                          else None),
                "limit": 0,
            }
            if candidate_value is not None and candidate_value != 0:
                failures.append(
                    f"{label} {key} expected 0, got "
                    f"{_format_number(candidate_value)}")

        width = _finite_number(
            candidate, "water_forward_width", f"{label} candidate", failures)
        height = _finite_number(
            candidate, "water_forward_height", f"{label} candidate", failures)
        image_bytes = _finite_number(
            candidate, "water_forward_image_bytes", f"{label} candidate", failures)
        expected_bytes = None
        dimensions_valid = (
            width is not None and height is not None and
            float(width).is_integer() and float(height).is_integer() and
            width > 0 and height > 0)
        if not dimensions_valid and width is not None and height is not None:
            failures.append(
                f"{label} water forward dimensions must be positive integers, "
                f"got {_format_number(width)}x{_format_number(height)}")
        elif dimensions_valid:
            expected_bytes = int(width) * int(height) * 12
        gates["water_forward_image_bytes"] = {
            "width": width,
            "height": height,
            "candidate": image_bytes,
            "expected": expected_bytes,
            "delta": (image_bytes - expected_bytes
                      if image_bytes is not None and expected_bytes is not None
                      else None),
            "limit": expected_bytes,
        }
        if (image_bytes is not None and expected_bytes is not None and
                image_bytes != expected_bytes):
            failures.append(
                f"{label} water_forward_image_bytes expected {expected_bytes}, "
                f"got {_format_number(image_bytes)}")

    for name in REQUIRED_SCREENSHOTS:
        path = screenshots_dir / name
        sidecar = Path(str(path) + ".done")
        status = {
            "path": str(path),
            "sidecar": str(sidecar),
            "bytes": path.stat().st_size if path.is_file() else None,
            "sidecar_bytes": sidecar.stat().st_size if sidecar.is_file() else None,
        }
        summary["screenshots"][name] = status
        if not path.is_file():
            failures.append(f"missing screenshot: {path}")
        elif path.stat().st_size == 0:
            failures.append(f"empty screenshot: {path}")
        if not sidecar.is_file():
            failures.append(f"missing screenshot sidecar: {sidecar}")
        elif sidecar.stat().st_size == 0:
            failures.append(f"empty screenshot sidecar: {sidecar}")

    summary["passed"] = not failures
    return summary


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    compare = commands.add_parser("compare", help="compare acceptance artifacts")
    compare.add_argument("--baseline", required=True)
    compare.add_argument("--candidate", required=True)
    compare.add_argument("--screenshots", required=True)
    validate = commands.add_parser(
        "validate-paths", help="reject aliased acceptance artifact trees")
    validate.add_argument("--baseline", required=True)
    validate.add_argument("--candidate", required=True)
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    if args.command == "compare":
        summary = compare_acceptance(
            args.baseline, args.candidate, args.screenshots)
        print(json.dumps(summary, sort_keys=True, separators=(",", ":")))
        return 0 if summary["passed"] else 1
    if args.command == "validate-paths":
        failure = _path_separation_failure(args.baseline, args.candidate)
        summary = {
            "passed": failure is None,
            "baseline_dir": str(Path(args.baseline).resolve()),
            "candidate_dir": str(Path(args.candidate).resolve()),
            "failures": [] if failure is None else [failure],
        }
        print(json.dumps(summary, sort_keys=True, separators=(",", ":")))
        return 0 if summary["passed"] else 1
    raise AssertionError(f"unhandled command: {args.command}")


if __name__ == "__main__":
    sys.exit(main())
