#!/usr/bin/env python3
"""Validate and select a bounded waterfall visual-meshing candidate."""

import argparse
import json
import math
import sys
from pathlib import Path


WORLD_SILHOUETTE_LIMIT_M = 0.15 / 4.0
CAMERA_SILHOUETTE_LIMIT_PIXELS = 2.0
PARAMETER_COVERAGE_DELTA_LIMIT = 0.02
SECTION_FILE_LIMIT_BYTES = 1024 * 1024 * 1024
NETWORK_FILE_LIMIT_BYTES = 700 * 1024 * 1024

REQUIRED_ROWS = {
    "v150-r013-b010": (0.15, 0.13, 0.10),
    "v100-r013-b010": (0.10, 0.13, 0.10),
    "v075-r013-b010": (0.075, 0.13, 0.10),
    "v150-r014-b010": (0.15, 0.14, 0.10),
    "v150-r013-b012": (0.15, 0.13, 0.12),
}


def _number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _finite(value):
    return _number(value) and math.isfinite(value)


def _integer(value):
    return isinstance(value, int) and not isinstance(value, bool)


def _format_number(value):
    return f"{value:g}" if _finite(value) else repr(value)


def _validate_fixture(report, failures):
    fixture = report.get("fixture")
    if not isinstance(fixture, dict):
        failures.append("fixture must be an object")
        return
    checks = (
        ("fallDistanceM", 12.0),
        ("sheetDiameterM", 0.26),
        ("frameCount", 30),
        ("cameraWidthPixels", 1280),
        ("cameraHeightPixels", 720),
        ("worldHausdorffBoundToleranceM", 0.001),
        ("worldHausdorffNumericalMarginM", 0.00001),
    )
    for key, expected in checks:
        value = fixture.get(key)
        if not _finite(value) or value != expected:
            failures.append(
                f"fixture {key} expected {_format_number(expected)}, "
                f"got {value!r}")


def _validate_row_shape(row, row_id, failures):
    positive_ints = (
        "vertices", "triangles", "connectedComponents",
        "projectedCompleteAnimationFileBytes")
    nonnegative_ints = (
        "intentionalSprayComponents", "openEdges", "holes")
    positive_numbers = (
        "voxelM", "radiusM", "blendWidthM", "gpuMeshMs")
    nonnegative_numbers = (
        "silhouetteHausdorffM", "silhouetteHausdorffPixels",
        "normalVariationDegrees", "offSheetCoverageFraction")
    for key in positive_ints:
        value = row.get(key)
        if not _integer(value) or value <= 0:
            failures.append(f"{row_id} {key} must be a positive integer")
    for key in nonnegative_ints:
        value = row.get(key)
        if not _integer(value) or value < 0:
            failures.append(f"{row_id} {key} must be a nonnegative integer")
    for key in positive_numbers:
        value = row.get(key)
        if not _finite(value) or value <= 0:
            failures.append(f"{row_id} {key} must be finite and positive")
    for key in nonnegative_numbers:
        value = row.get(key)
        if not _finite(value) or value < 0:
            failures.append(
                f"{row_id} {key} must be finite and nonnegative")
    digest = row.get("meshDigest")
    if not isinstance(digest, str) or not digest:
        failures.append(f"{row_id} meshDigest must be a nonempty string")


def _validate_candidate(row, row_id, oracle, default_row,
                        network_other_bytes, section_other_bytes, failures):
    world_error = row.get("silhouetteHausdorffM")
    if _finite(world_error) and world_error >= WORLD_SILHOUETTE_LIMIT_M:
        failures.append(
            f"silhouetteHausdorffM {_format_number(world_error)} must be "
            f"below {_format_number(WORLD_SILHOUETTE_LIMIT_M)}")
    pixel_error = row.get("silhouetteHausdorffPixels")
    if (_finite(pixel_error) and
            pixel_error > CAMERA_SILHOUETTE_LIMIT_PIXELS):
        failures.append(
            f"silhouetteHausdorffPixels {_format_number(pixel_error)} "
            f"exceeds {_format_number(CAMERA_SILHOUETTE_LIMIT_PIXELS)}")

    for key in ("connectedComponents", "intentionalSprayComponents"):
        value = row.get(key)
        oracle_value = oracle.get(key)
        if _integer(value) and _integer(oracle_value) and value != oracle_value:
            failures.append(f"{key} {value} differs from oracle {oracle_value}")
    for key in ("openEdges", "holes"):
        value = row.get(key)
        oracle_value = oracle.get(key)
        if _integer(value) and _integer(oracle_value) and value > oracle_value:
            failures.append(f"{key} {value} exceeds oracle {oracle_value}")

    is_parameter_variant = (
        row_id in ("v150-r014-b010", "v150-r013-b012"))
    coverage = row.get("offSheetCoverageFraction")
    default_coverage = (default_row.get("offSheetCoverageFraction")
                        if isinstance(default_row, dict) else None)
    if is_parameter_variant:
        if not _finite(default_coverage):
            failures.append(
                "default offSheetCoverageFraction must be finite")
        elif _finite(coverage):
            delta = abs(coverage - default_coverage)
            if delta > PARAMETER_COVERAGE_DELTA_LIMIT:
                failures.append(
                    f"offSheetCoverageFraction delta {_format_number(delta)} "
                    f"exceeds {_format_number(PARAMETER_COVERAGE_DELTA_LIMIT)}")

    file_bytes = row.get("projectedCompleteAnimationFileBytes")
    if _integer(file_bytes) and file_bytes > SECTION_FILE_LIMIT_BYTES:
        failures.append(
            f"projectedCompleteAnimationFileBytes {file_bytes} exceeds "
            f"{SECTION_FILE_LIMIT_BYTES}")
    if (_integer(file_bytes) and _integer(network_other_bytes) and
            file_bytes >= 0 and network_other_bytes >= 0):
        network_bytes = network_other_bytes + file_bytes
        if network_bytes >= NETWORK_FILE_LIMIT_BYTES:
            failures.append(
                f"projectedNetworkFileBytes {network_bytes} must be below "
                f"{NETWORK_FILE_LIMIT_BYTES}")
    if (_integer(file_bytes) and _integer(section_other_bytes) and
            file_bytes >= 0 and section_other_bytes >= 0):
        section_bytes = section_other_bytes + file_bytes
        if section_bytes > SECTION_FILE_LIMIT_BYTES:
            failures.append(
                f"projectedContainingSectionFileBytes {section_bytes} "
                f"exceeds {SECTION_FILE_LIMIT_BYTES}")


def evaluate_report(report):
    """Return strict validation and a deterministic implementation choice."""
    summary = {
        "passed": False,
        "failures": [],
        "oracleRowId": None,
        "selectedCandidateId": None,
        "rows": {},
        "limits": {
            "worldSilhouetteM": WORLD_SILHOUETTE_LIMIT_M,
            "cameraSilhouettePixels": CAMERA_SILHOUETTE_LIMIT_PIXELS,
            "parameterCoverageDelta": PARAMETER_COVERAGE_DELTA_LIMIT,
            "sectionFileBytes": SECTION_FILE_LIMIT_BYTES,
            "networkFileBytesExclusive": NETWORK_FILE_LIMIT_BYTES,
        },
    }
    failures = summary["failures"]
    if not isinstance(report, dict):
        failures.append("quality report must be an object")
        return summary
    if report.get("schemaVersion") != 1:
        failures.append(
            f"schemaVersion expected 1, got {report.get('schemaVersion')!r}")
    _validate_fixture(report, failures)
    validation_errors = report.get("validationErrors")
    if not _integer(validation_errors) or validation_errors != 0:
        failures.append(
            f"validationErrors expected 0, got {validation_errors!r}")
    network_other_bytes = report.get("networkOtherCompleteFileBytes")
    if not _integer(network_other_bytes) or network_other_bytes < 0:
        failures.append(
            "networkOtherCompleteFileBytes must be a nonnegative integer")
    section_other_bytes = report.get("containingSectionOtherCompleteFileBytes")
    if not _integer(section_other_bytes) or section_other_bytes < 0:
        failures.append(
            "containingSectionOtherCompleteFileBytes must be a "
            "nonnegative integer")
    measured_network_bytes = report.get("measuredRiverFloatLabNetworkBytes")
    if not _integer(measured_network_bytes) or measured_network_bytes < 0:
        failures.append(
            "measuredRiverFloatLabNetworkBytes must be a nonnegative integer")
    measured_section_bytes = report.get("measuredContainingSectionFileBytes")
    if not _integer(measured_section_bytes) or measured_section_bytes < 0:
        failures.append(
            "measuredContainingSectionFileBytes must be a nonnegative integer")

    rows_value = report.get("rows")
    rows = {}
    if not isinstance(rows_value, list):
        failures.append("rows must be an array")
        rows_value = []
    for row in rows_value:
        if not isinstance(row, dict) or not isinstance(row.get("id"), str):
            failures.append("quality report contains an invalid row")
            continue
        row_id = row["id"]
        if row_id in rows:
            failures.append(f"duplicate row {row_id}")
            continue
        rows[row_id] = row
        row_failures = []
        _validate_row_shape(row, row_id, row_failures)
        summary["rows"][row_id] = {
            "passed": False,
            "failures": row_failures,
            "voxelM": row.get("voxelM"),
            "radiusM": row.get("radiusM"),
            "blendWidthM": row.get("blendWidthM"),
        }

    for row_id, expected in REQUIRED_ROWS.items():
        row = rows.get(row_id)
        if row is None:
            failures.append(f"required candidate {row_id} is missing")
            continue
        actual = (row.get("voxelM"), row.get("radiusM"),
                  row.get("blendWidthM"))
        settings_match = all(
            _finite(value) and math.isclose(
                value, wanted, rel_tol=0.0, abs_tol=1e-6)
            for value, wanted in zip(actual, expected))
        if not settings_match:
            failures.append(
                f"required candidate {row_id} settings expected {expected}, "
                f"got {actual}")
    for row_id in sorted(set(rows) - set(REQUIRED_ROWS)):
        failures.append(f"unexpected candidate {row_id}")

    current = rows.get("v150-r013-b010")
    current_file_bytes = (current.get("projectedCompleteAnimationFileBytes")
                          if current else None)
    if (_integer(measured_network_bytes) and
            _integer(network_other_bytes) and
            _integer(current_file_bytes)):
        expected_network_bytes = network_other_bytes + current_file_bytes
        if measured_network_bytes != expected_network_bytes:
            failures.append(
                f"measuredRiverFloatLabNetworkBytes {measured_network_bytes} "
                "does not equal networkOtherCompleteFileBytes plus current "
                f"replacement {expected_network_bytes}")
    if (_integer(measured_section_bytes) and
            _integer(section_other_bytes) and
            _integer(current_file_bytes)):
        expected_section_bytes = section_other_bytes + current_file_bytes
        if measured_section_bytes != expected_section_bytes:
            failures.append(
                "measuredContainingSectionFileBytes "
                f"{measured_section_bytes} does not equal "
                "containingSectionOtherCompleteFileBytes plus current "
                f"replacement {expected_section_bytes}")

    oracle_id = report.get("oracleRowId")
    summary["oracleRowId"] = oracle_id if isinstance(oracle_id, str) else None
    oracle = rows.get(oracle_id) if isinstance(oracle_id, str) else None
    if oracle is None:
        failures.append(f"oracle row {oracle_id!r} is missing")
        return summary
    if oracle_id != "v075-r013-b010":
        failures.append(
            "oracleRowId must name the 0.075/0.13/0.10 row")
    oracle_self_metrics = (
        oracle.get("silhouetteHausdorffM"),
        oracle.get("silhouetteHausdorffPixels"),
        oracle.get("offSheetCoverageFraction"),
    )
    if (all(_finite(value) for value in oracle_self_metrics) and
            any(value != 0.0 for value in oracle_self_metrics)):
        failures.append(
            f"oracle row {oracle_id} self-comparison metrics must be zero")
    oracle_reference_valid = not summary["rows"][oracle_id]["failures"]

    for row_id, row in rows.items():
        row_summary = summary["rows"][row_id]
        _validate_candidate(
            row, row_id, oracle, current, network_other_bytes,
            section_other_bytes, row_summary["failures"])
        row_summary["passed"] = not row_summary["failures"]
        if _integer(row.get("projectedCompleteAnimationFileBytes")) and \
                _integer(network_other_bytes):
            row_summary["projectedNetworkFileBytes"] = (
                network_other_bytes +
                row["projectedCompleteAnimationFileBytes"])
        if _integer(row.get("projectedCompleteAnimationFileBytes")) and \
                _integer(section_other_bytes):
            row_summary["projectedContainingSectionFileBytes"] = (
                section_other_bytes +
                row["projectedCompleteAnimationFileBytes"])

    if not oracle_reference_valid:
        failures.append(f"oracle row {oracle_id} failed validation")

    if failures:
        return summary

    passing = [
        row for row in rows.values()
        if summary["rows"][row["id"]]["passed"]]
    if not passing:
        failures.append("no waterfall visual-meshing candidate passed")
        return summary

    def choice_key(row):
        parameter_delta = (
            abs(row["radiusM"] - 0.13) +
            abs(row["blendWidthM"] - 0.10))
        return (-row["voxelM"], parameter_delta,
                row["radiusM"], row["blendWidthM"], row["id"])

    selected = min(passing, key=choice_key)
    summary["selectedCandidateId"] = selected["id"]
    summary["passed"] = True
    return summary


DETERMINISTIC_ROW_FIELDS = (
    "vertices", "triangles", "connectedComponents",
    "intentionalSprayComponents", "openEdges", "holes",
    "projectedCompleteAnimationFileBytes", "meshDigest",
)


def compare_reports(reports):
    """Require three valid runs with identical decision/topology/digests."""
    result = {
        "passed": False,
        "failures": [],
        "selectedCandidateId": None,
        "runSummaries": [],
    }
    if len(reports) != 3:
        result["failures"].append(
            f"expected exactly 3 matrix reports, got {len(reports)}")
        return result
    summaries = [evaluate_report(report) for report in reports]
    result["runSummaries"] = summaries
    for index, summary in enumerate(summaries, start=1):
        for failure in summary["failures"]:
            result["failures"].append(f"run {index}: {failure}")
        if not summary["passed"]:
            result["failures"].append(f"run {index} has no valid decision")
    baseline_decision = summaries[0]["selectedCandidateId"]
    result["selectedCandidateId"] = baseline_decision
    for index, summary in enumerate(summaries[1:], start=2):
        if summary["selectedCandidateId"] != baseline_decision:
            result["failures"].append(
                f"run {index} selectedCandidateId changed from "
                f"{baseline_decision!r} to "
                f"{summary['selectedCandidateId']!r}")

    def rows_by_id(report):
        if not isinstance(report, dict) or not isinstance(
                report.get("rows"), list):
            return {}
        return {
            row.get("id"): row for row in report["rows"]
            if isinstance(row, dict) and isinstance(row.get("id"), str)
        }

    baseline_rows = rows_by_id(reports[0])
    for run_index, report in enumerate(reports[1:], start=2):
        rows = rows_by_id(report)
        if set(rows) != set(baseline_rows):
            result["failures"].append(
                f"run {run_index} row ids changed from "
                f"{sorted(baseline_rows)} to {sorted(rows)}")
            continue
        for row_id in sorted(baseline_rows):
            baseline = baseline_rows[row_id]
            row = rows[row_id]
            for field in DETERMINISTIC_ROW_FIELDS:
                if row.get(field) != baseline.get(field):
                    result["failures"].append(
                        f"run {run_index} {row_id} {field} changed from "
                        f"{baseline.get(field)!r} to {row.get(field)!r}")
    result["passed"] = not result["failures"]
    return result


def render_markdown(reports, comparison, report_paths=None):
    lines = [
        "# Waterfall visual quality matrix",
        "",
        f"Decision: **{comparison['selectedCandidateId']}**",
        "",
        ("Three-run topology/digest/decision determinism: **pass**"
         if comparison["passed"] else
         "Three-run topology/digest/decision determinism: **fail**"),
        "",
    ]
    if report_paths:
        lines.append("Native reports:")
        lines.append("")
        for path in report_paths:
            lines.append(f"- `{Path(path).resolve()}`")
        lines.append("")
    render_numbers = (
        "gpuMeshMs", "silhouetteHausdorffM",
        "silhouetteHausdorffPixels")
    render_integers = (
        "connectedComponents", "openEdges", "holes",
        "projectedCompleteAnimationFileBytes")
    def renderable_rows(report):
        if (not isinstance(report, dict) or
                not isinstance(report.get("rows"), list)):
            return None
        result = {}
        for row in report["rows"]:
            if (not isinstance(row, dict) or
                    not isinstance(row.get("id"), str) or
                    row["id"] in result or
                    not all(_finite(row.get(key)) for key in render_numbers) or
                    not all(_integer(row.get(key))
                            for key in render_integers)):
                return None
            result[row["id"]] = row
        return result

    candidate_rows = [renderable_rows(report) for report in reports]
    baseline_ids = (set(candidate_rows[0])
                    if len(candidate_rows) == 3 and
                    candidate_rows[0] is not None else set())
    summary_rows = (comparison.get("runSummaries", [{}])[0].get("rows", {})
                    if comparison.get("runSummaries") else {})
    valid_rows = (
        len(candidate_rows) == 3 and
        len(comparison.get("runSummaries", [])) == 3 and
        all(rows is not None and set(rows) == baseline_ids
            for rows in candidate_rows) and
        all(row_id in summary_rows for row_id in baseline_ids))
    if not valid_rows:
        if comparison["failures"]:
            lines.append("Failures:")
            lines.append("")
            lines.extend(
                f"- {failure}" for failure in comparison["failures"])
            lines.append("")
        return "\n".join(lines)

    first = reports[0]
    summary = comparison["runSummaries"][0]
    lines.extend([
        "| Candidate | Pass | World upper bound / pixel silhouette | Topology "
        "(components/open/holes) | 30-frame file | GPU ms (runs 1/2/3) "
        "| Reason |",
        "|---|---:|---:|---:|---:|---:|---|",
    ])
    rows_by_run = candidate_rows
    for row in first["rows"]:
        row_id = row["id"]
        decision = summary["rows"][row_id]
        reason = "; ".join(decision["failures"]) or "all gates pass"
        timings = "/".join(
            f"{rows[row_id]['gpuMeshMs']:.3f}" for rows in rows_by_run)
        topology = (f"{row['connectedComponents']}/"
                    f"{row['openEdges']}/{row['holes']}")
        silhouette = (f"{row['silhouetteHausdorffM']:.6f} m / "
                      f"{row['silhouetteHausdorffPixels']:.3f} px")
        lines.append(
            f"| {row_id} | {'yes' if decision['passed'] else 'no'} | "
            f"{silhouette} | {topology} | "
            f"{row['projectedCompleteAnimationFileBytes']} | {timings} | "
            f"{reason} |")
    lines.extend([
        "",
        f"Measured RiverFloatLab animation network: "
        f"{first.get('measuredRiverFloatLabNetworkBytes')} bytes.",
        f"Measured containing upper-section file: "
        f"{first.get('measuredContainingSectionFileBytes')} bytes.",
        f"Vulkan validation errors: "
        f"{[report.get('validationErrors') for report in reports]}.",
        "",
    ])
    if comparison["failures"]:
        lines.append("Failures:")
        lines.append("")
        lines.extend(f"- {failure}" for failure in comparison["failures"])
        lines.append("")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("reports", nargs="+", type=Path)
    parser.add_argument("--summary", type=Path)
    parser.add_argument("--markdown", type=Path)
    args = parser.parse_args(argv)
    reports = []
    for path in args.reports:
        try:
            reports.append(json.loads(path.read_text(encoding="utf-8")))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            print(f"waterfall quality: invalid report {path}: {exc}",
                  file=sys.stderr)
            return 2
    summary = (evaluate_report(reports[0]) if len(reports) == 1 else
               compare_reports(reports))
    encoded = json.dumps(summary, indent=2, sort_keys=True, allow_nan=False)
    if args.summary:
        args.summary.parent.mkdir(parents=True, exist_ok=True)
        args.summary.write_text(encoded + "\n", encoding="utf-8")
    if args.markdown:
        if len(reports) != 3:
            print("waterfall quality: --markdown requires three reports",
                  file=sys.stderr)
            return 2
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text(
            render_markdown(reports, summary, args.reports),
            encoding="utf-8")
    print(encoded)
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
