#!/usr/bin/env python3
"""Strict Stage 1 animated-water section continuity acceptance."""

import argparse
from collections import deque
import json
import math
import sys
from pathlib import Path

from PIL import Image, UnidentifiedImageError


FRAMES = (0, 7, 15, 22, 29)
VIEWS = ("normal", "geometry-normal", "foam-driver", "identity")
QUANTIZATION_TOLERANCE_M = 0.15 / 16.0
MINIMUM_NORMAL_DOT = 0.995
MAXIMUM_FIELD_SCALAR_DELTA = 0.01
ZERO_NATIVE_METRICS = (
    "validationErrors",
    "waterDecodeDispatches",
    "waterBlasBuilds",
    "waterTlasInstances",
    "waterRtRecords",
)

IDENTITY_OWNER_COLORS = (
    (97, 138, 139),   # upstream/cyan
    (120, 143, 120),  # handoff/green
    (64, 76, 108),    # downstream/blue
)
IDENTITY_COLOR_RADIUS_SQUARED = 18 * 18
MINIMUM_DOMINANT_OWNER_FRACTION = 0.5


def _number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _finite(value):
    return _number(value) and math.isfinite(value)


def _load_json(path, label, failures):
    path = Path(path).resolve()
    if not path.is_file():
        failures.append(f"missing {label} JSON: {path}")
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        failures.append(f"invalid JSON for {label}: {path}: {exc}")
        return None
    if not isinstance(value, dict):
        failures.append(f"{label} JSON must be an object: {path}")
        return None
    return value


def _sections(trace, label, failures):
    values = trace.get("sections")
    if not isinstance(values, list):
        failures.append(f"{label} sections must be an array")
        return {}
    result = {}
    for value in values:
        if not isinstance(value, dict) or not isinstance(value.get("id"), str):
            failures.append(f"{label} contains an invalid section row")
            continue
        section_id = value["id"]
        if section_id in result:
            failures.append(f"{label} contains duplicate section {section_id}")
        result[section_id] = value
    if set(result) != {"upper", "lower"}:
        failures.append(
            f"{label} section ids expected upper/lower, got {sorted(result)}")
    return result


def _handoff(trace, label, failures):
    handoffs = trace.get("handoffs")
    if not isinstance(handoffs, dict) or set(handoffs) != {"pool-one"}:
        failures.append(f"{label} must contain exactly handoff pool-one")
        return None
    value = handoffs["pool-one"]
    if not isinstance(value, dict):
        failures.append(f"{label} pool-one handoff must be an object")
        return None
    return value


def _require_trace_shape(trace, label, failures):
    if trace.get("networkState") != "Ready":
        failures.append(f"{label} networkState must be Ready")
    sections = _sections(trace, label, failures)
    handoff = _handoff(trace, label, failures)
    return sections, handoff


def _require_bool(record, key, expected, label, failures):
    value = record.get(key)
    if value is not expected:
        failures.append(f"{label} {key} expected {expected}, got {value!r}")


def _require_zero(record, key, label, failures):
    value = record.get(key)
    if not _finite(value) or value != 0:
        failures.append(f"{label} {key} expected 0, got {value!r}")


def _require_positive(record, key, label, failures):
    value = record.get(key)
    if not _finite(value) or value <= 0:
        failures.append(f"{label} {key} must be positive, got {value!r}")


def _require_frame_array(record, key, label, failures, item_kind=None):
    values = record.get(key)
    if not isinstance(values, list) or len(values) != 30:
        failures.append(f"{label} {key} expected 30 frames")
        return None
    for frame, value in enumerate(values):
        if item_kind == "positive-int":
            valid = (isinstance(value, int) and
                     not isinstance(value, bool) and value > 0)
        elif item_kind == "positive-number":
            valid = _finite(value) and value > 0
        else:
            valid = True
        if not valid:
            failures.append(
                f"{label} {key} frame {frame} must be a {item_kind}, "
                f"got {value!r}")
    return values


def _validate_cut(cut, label, failures, worst):
    if not isinstance(cut, dict):
        failures.append(f"{label} cut metric must be an object")
        return
    hausdorff = cut.get("symmetricHausdorffM")
    normal_dot = cut.get("minimumNormalDot")
    rms = cut.get("rmsDistanceM")
    if not _finite(hausdorff):
        failures.append(f"{label} symmetricHausdorffM must be finite")
    else:
        worst["symmetricHausdorffM"] = max(
            worst["symmetricHausdorffM"], hausdorff)
        if hausdorff > QUANTIZATION_TOLERANCE_M:
            failures.append(
                f"{label} symmetricHausdorffM {hausdorff} exceeds voxel/16 "
                f"{QUANTIZATION_TOLERANCE_M}")
    if not _finite(rms) or rms < 0:
        failures.append(f"{label} rmsDistanceM must be finite and nonnegative")
    else:
        worst["rmsDistanceM"] = max(worst["rmsDistanceM"], rms)
    if not _finite(normal_dot):
        failures.append(f"{label} minimumNormalDot must be finite")
    else:
        worst["minimumNormalDot"] = min(
            worst["minimumNormalDot"], normal_dot)
        if normal_dot < MINIMUM_NORMAL_DOT:
            failures.append(
                f"{label} minimumNormalDot {normal_dot} below "
                f"{MINIMUM_NORMAL_DOT}")
    for key in ("unmatchedOpenEdges", "duplicateCoplanarTriangles"):
        value = cut.get(key)
        if not isinstance(value, int) or isinstance(value, bool) or value != 0:
            failures.append(f"{label} {key} expected 0, got {value!r}")


def _validate_field(field, label, failures, worst):
    if not isinstance(field, dict):
        failures.append(f"{label} field metric must be an object")
        return
    sample_pairs = field.get("samplePairs")
    if (not isinstance(sample_pairs, int) or
            isinstance(sample_pairs, bool) or sample_pairs <= 0):
        failures.append(f"{label} field samplePairs must be positive")
    bounds = {
        "maximumHeightDeltaM": QUANTIZATION_TOLERANCE_M,
        "maximumTurbulenceDelta": MAXIMUM_FIELD_SCALAR_DELTA,
        "maximumAerationDelta": MAXIMUM_FIELD_SCALAR_DELTA,
        "maximumFoamDelta": MAXIMUM_FIELD_SCALAR_DELTA,
    }
    for key, limit in bounds.items():
        value = field.get(key)
        if not _finite(value) or value < 0:
            failures.append(f"{label} {key} must be finite and nonnegative")
            continue
        worst[key] = max(worst[key], value)
        if value > limit:
            failures.append(f"{label} {key} {value} exceeds {limit}")
    normal_dot = field.get("minimumNormalDot")
    if not _finite(normal_dot):
        failures.append(f"{label} field minimumNormalDot must be finite")
    else:
        worst["fieldMinimumNormalDot"] = min(
            worst["fieldMinimumNormalDot"], normal_dot)
        if normal_dot < MINIMUM_NORMAL_DOT:
            failures.append(
                f"{label} field minimumNormalDot {normal_dot} below "
                f"{MINIMUM_NORMAL_DOT}")
    if field.get("featureLabelsDeterministic") is not True:
        failures.append(f"{label} feature labels are not deterministic")


def _validate_cold(cold, failures, worst):
    sections, handoff = _require_trace_shape(cold, "cold", failures)
    _require_positive(cold, "networkPeakBuildCpuPayloadBytes",
                      "cold network", failures)
    for section_id, section in sections.items():
        label = f"cold {section_id}"
        _require_bool(section, "cacheHit", False, label, failures)
        _require_bool(section, "animationCacheHit", False, label, failures)
        _require_positive(section, "simulateMs", label, failures)
        _require_frame_array(
            section, "animationFrameVertexCounts", label, failures,
            "positive-int")
        _require_frame_array(
            section, "animationFrameTriangleCounts", label, failures,
            "positive-int")
        for key in ("animationSemanticKey", "animationPayloadDigest"):
            if not isinstance(section.get(key), str) or not section[key]:
                failures.append(f"{label} {key} must be nonempty")
        for key in ("boundarySourceSemanticKeys",
                    "boundarySourcePayloadDigests"):
            values = section.get(key)
            if not isinstance(values, list) or len(values) != 1 or not values[0]:
                failures.append(f"{label} {key} must contain one identity")
    if handoff is None:
        return
    _require_bool(handoff, "staticCacheHit", False, "cold handoff", failures)
    _require_bool(handoff, "animationCacheHit", False, "cold handoff", failures)
    _require_frame_array(
        handoff, "animationFrameMs", "cold handoff", failures,
        "positive-number")
    if handoff.get("loopFrame29To0Synchronized") is not True:
        failures.append("cold handoff frame 29/0 is not synchronized")
    retained_dam_support = handoff.get(
        "retainedTemporaryDamSupportContributors")
    if (not isinstance(retained_dam_support, int) or
            isinstance(retained_dam_support, bool) or
            retained_dam_support < 0):
        failures.append(
            "cold handoff retained temporary-dam support count must be a "
            f"nonnegative integer, got {retained_dam_support!r}")
    if handoff.get("sourceBlendRequired") is not True:
        failures.append("cold handoff sourceBlendRequired must be true")
    for key in ("animationFileBytes", "boundarySourceBytes",
                "peakBuildCpuPayloadBytes"):
        _require_positive(handoff, key, "cold handoff", failures)
    network_peak = cold.get("networkPeakBuildCpuPayloadBytes")
    handoff_peak = handoff.get("peakBuildCpuPayloadBytes")
    if (_finite(network_peak) and _finite(handoff_peak) and
            network_peak < handoff_peak):
        failures.append(
            "cold networkPeakBuildCpuPayloadBytes is below the handoff peak")
    for cut_name in ("upstreamCut", "downstreamCut"):
        values = _require_frame_array(
            handoff, cut_name, "cold handoff", failures)
        if values is not None:
            for frame, value in enumerate(values):
                _validate_cut(value, f"cold {cut_name} frame {frame}",
                              failures, worst)
    _validate_field(handoff.get("upstreamField"), "cold upstream cut",
                    failures, worst)
    _validate_field(handoff.get("downstreamField"), "cold downstream cut",
                    failures, worst)
    for key in ("semanticKey", "payloadDigest", "animationSemanticKey",
                "animationPayloadDigest"):
        if not isinstance(handoff.get(key), str) or not handoff[key]:
            failures.append(f"cold handoff {key} must be nonempty")


def _validate_cache(cold, cache, failures):
    cold_sections, cold_handoff = _require_trace_shape(
        cold, "cold cache comparison", failures)
    cache_sections, cache_handoff = _require_trace_shape(cache, "cache", failures)
    for section_id, section in cache_sections.items():
        label = f"cache {section_id}"
        _require_bool(section, "cacheHit", True, label, failures)
        _require_bool(section, "animationCacheHit", True, label, failures)
        _require_zero(section, "simulateMs", label, failures)
        cold_section = cold_sections.get(section_id, {})
        for key in ("animationSemanticKey", "animationPayloadDigest",
                    "boundarySourceSemanticKeys",
                    "boundarySourcePayloadDigests"):
            if section.get(key) != cold_section.get(key):
                failures.append(f"{label} changed immutable {key}")
    if cache_handoff is None or cold_handoff is None:
        return
    _require_bool(cache_handoff, "staticCacheHit", True,
                  "cache handoff", failures)
    _require_bool(cache_handoff, "animationCacheHit", True,
                  "cache handoff", failures)
    for key in ("semanticKey", "payloadDigest", "animationSemanticKey",
                "animationPayloadDigest"):
        if cache_handoff.get(key) != cold_handoff.get(key):
            failures.append(f"cache handoff changed immutable {key}")


def _validate_edit(cold, edit, failures):
    cold_sections, cold_handoff = _require_trace_shape(
        cold, "cold edit comparison", failures)
    edit_sections, edit_handoff = _require_trace_shape(edit, "edit", failures)
    upper = edit_sections.get("upper", {})
    cold_upper = cold_sections.get("upper", {})
    _require_bool(upper, "cacheHit", True, "edit upstream", failures)
    _require_bool(upper, "animationCacheHit", True, "edit upstream", failures)
    _require_zero(upper, "simulateMs", "edit upstream", failures)
    if upper.get("animationSemanticKey") != cold_upper.get("animationSemanticKey"):
        failures.append("edit changed upstream animation semantic key")
    if upper.get("animationPayloadDigest") != cold_upper.get("animationPayloadDigest"):
        failures.append("edit changed upstream animation payload")
    if (upper.get("boundarySourceSemanticKeys") !=
            cold_upper.get("boundarySourceSemanticKeys") or
            upper.get("boundarySourcePayloadDigests") !=
            cold_upper.get("boundarySourcePayloadDigests")):
        failures.append("edit changed upstream boundary sidecar identity/digest")

    lower = edit_sections.get("lower", {})
    cold_lower = cold_sections.get("lower", {})
    _require_bool(lower, "cacheHit", False, "edit downstream", failures)
    _require_bool(lower, "animationCacheHit", False,
                  "edit downstream", failures)
    _require_positive(lower, "simulateMs", "edit downstream", failures)
    for key in ("animationSemanticKey", "animationPayloadDigest",
                "boundarySourceSemanticKeys", "boundarySourcePayloadDigests"):
        if lower.get(key) == cold_lower.get(key):
            failures.append(f"edit did not invalidate downstream {key}")

    if edit_handoff is None or cold_handoff is None:
        return
    _require_bool(edit_handoff, "staticCacheHit", False,
                  "edit handoff", failures)
    _require_bool(edit_handoff, "animationCacheHit", False,
                  "edit handoff", failures)
    for key in ("semanticKey", "payloadDigest", "animationSemanticKey",
                "animationPayloadDigest"):
        if edit_handoff.get(key) == cold_handoff.get(key):
            failures.append(f"edit did not invalidate dependent handoff {key}")


def _validate_native(native, failures):
    for key in ZERO_NATIVE_METRICS:
        value = native.get(key)
        if not _finite(value) or value != 0:
            failures.append(f"native {key} expected 0, got {value!r}")
    draws = native.get("rasterDirectDraws")
    if not _finite(draws) or draws <= 0:
        failures.append(f"native rasterDirectDraws must be positive, got {draws!r}")


def _screen_water_owner_coverage(path):
    with Image.open(path) as source:
        image = source.convert("RGB")
    width, height = image.size
    pixels = image.tobytes()
    owners = bytearray(width * height)
    totals = [0, 0, 0]
    for index in range(width * height):
        offset = index * 3
        red, green, blue = pixels[offset:offset + 3]
        distances = []
        for owner_red, owner_green, owner_blue in IDENTITY_OWNER_COLORS:
            delta_red = red - owner_red
            delta_green = green - owner_green
            delta_blue = blue - owner_blue
            distances.append(delta_red * delta_red +
                             delta_green * delta_green +
                             delta_blue * delta_blue)
        owner = min(range(3), key=distances.__getitem__)
        if distances[owner] <= IDENTITY_COLOR_RADIUS_SQUARED:
            owners[index] = owner + 1
            totals[owner] += 1

    minimum_pixels = max(12, (width * height) // 1000)
    if any(total < minimum_pixels for total in totals):
        return {
            "passed": False,
            "size": [width, height],
            "ownerPixels": totals,
            "dominantOwnerPixels": [0, 0, 0],
            "dominantOwnerFractions": [0.0, 0.0, 0.0],
            "reason": "identity view does not contain all three water owners",
        }

    visited = bytearray(width * height)
    dominant = [0, 0, 0]
    for start in range(width * height):
        if owners[start] == 0 or visited[start]:
            continue
        component = [0, 0, 0]
        pending = deque([start])
        visited[start] = 1
        while pending:
            current = pending.popleft()
            component[owners[current] - 1] += 1
            x = current % width
            y = current // width
            for dy in (-1, 0, 1):
                next_y = y + dy
                if next_y < 0 or next_y >= height:
                    continue
                for dx in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    next_x = x + dx
                    if next_x < 0 or next_x >= width:
                        continue
                    neighbor = next_y * width + next_x
                    if owners[neighbor] != 0 and not visited[neighbor]:
                        visited[neighbor] = 1
                        pending.append(neighbor)
        if sum(component) > sum(dominant):
            dominant = component

    fractions = [dominant[index] / totals[index] for index in range(3)]
    passed = all(fraction >= MINIMUM_DOMINANT_OWNER_FRACTION
                 for fraction in fractions)
    return {
        "passed": passed,
        "size": [width, height],
        "ownerPixels": totals,
        "dominantOwnerPixels": dominant,
        "dominantOwnerFractions": fractions,
        "reason": (None if passed else
                   "background band separates water owners"),
    }


def _validate_screenshots(screenshots_dir, failures):
    root = Path(screenshots_dir).resolve()
    evidence = {}
    for frame in FRAMES:
        for view in VIEWS:
            key = f"frame-{frame:02d}-{view}"
            path = root / key / "section-handoff.png"
            sidecar = Path(str(path) + ".done")
            evidence[key] = {
                "path": str(path),
                "bytes": path.stat().st_size if path.is_file() else None,
                "sidecar": str(sidecar),
                "sidecarBytes": (
                    sidecar.stat().st_size if sidecar.is_file() else None),
            }
            if not path.is_file():
                failures.append(f"missing screenshot: {path}")
            elif path.stat().st_size == 0:
                failures.append(f"empty screenshot: {path}")
            if not sidecar.is_file():
                failures.append(f"missing screenshot sidecar: {sidecar}")
            elif sidecar.stat().st_size == 0:
                failures.append(f"empty screenshot sidecar: {sidecar}")
            if view == "identity" and path.is_file() and path.stat().st_size:
                try:
                    coverage = _screen_water_owner_coverage(path)
                except (OSError, UnidentifiedImageError, ValueError) as exc:
                    coverage = {"passed": False,
                                "reason": f"invalid identity PNG: {exc}"}
                evidence[key]["waterOwnerCoverage"] = coverage
                if not coverage["passed"]:
                    failures.append(
                        f"{key} {coverage['reason']}: {path}")
    return evidence


def compare_stage1(cold_path, cache_path, edit_path, screenshots_dir,
                   native_path):
    failures = []
    worst = {
        "symmetricHausdorffM": 0.0,
        "rmsDistanceM": 0.0,
        "minimumNormalDot": 1.0,
        "maximumHeightDeltaM": 0.0,
        "maximumTurbulenceDelta": 0.0,
        "maximumAerationDelta": 0.0,
        "maximumFoamDelta": 0.0,
        "fieldMinimumNormalDot": 1.0,
    }
    cold = _load_json(cold_path, "cold trace", failures)
    cache = _load_json(cache_path, "cache trace", failures)
    edit = _load_json(edit_path, "edit trace", failures)
    native = _load_json(native_path, "native gates", failures)
    summary = {
        "passed": False,
        "cold": str(Path(cold_path).resolve()),
        "cache": str(Path(cache_path).resolve()),
        "edit": str(Path(edit_path).resolve()),
        "native": str(Path(native_path).resolve()),
        "screenshotsDir": str(Path(screenshots_dir).resolve()),
        "worst": worst,
        "screenshots": {},
        "failures": failures,
    }
    if cold is not None:
        _validate_cold(cold, failures, worst)
    if cold is not None and cache is not None:
        _validate_cache(cold, cache, failures)
    if cold is not None and edit is not None:
        _validate_edit(cold, edit, failures)
    if native is not None:
        _validate_native(native, failures)
    summary["screenshots"] = _validate_screenshots(screenshots_dir, failures)
    summary["passed"] = not failures
    return summary


def _inferred_sibling(cache_path, sibling, filename):
    path = Path(cache_path).resolve()
    if path.name == "timings.json" and path.parent.name == "trace":
        root = path.parent.parent.parent
        if sibling == "native":
            return root / sibling / filename
        return root / sibling / "trace" / filename
    return path.parent / sibling / filename


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    stage1 = commands.add_parser("stage1")
    stage1.add_argument("--cold", required=True)
    stage1.add_argument("--cache", required=True)
    stage1.add_argument("--edit")
    stage1.add_argument("--native")
    stage1.add_argument("--screenshots", required=True)
    stage1.add_argument("--output")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    if args.command != "stage1":
        raise AssertionError(f"unhandled command {args.command}")
    edit = args.edit or _inferred_sibling(args.cache, "edit", "timings.json")
    native = args.native or _inferred_sibling(
        args.cache, "native", "native-gates.json")
    summary = compare_stage1(
        args.cold, args.cache, edit, args.screenshots, native)
    encoded = json.dumps(summary, sort_keys=True, indent=2)
    if args.output:
        output = Path(args.output).resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
