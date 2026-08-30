import copy
import math
import unittest

from MatterEngine3.tools import waterfall_visual_quality as quality


class WaterfallVisualQualityTests(unittest.TestCase):
    @staticmethod
    def _row(row_id, voxel_m, radius_m=0.13, blend_m=0.10):
        return {
            "id": row_id,
            "voxelM": voxel_m,
            "radiusM": radius_m,
            "blendWidthM": blend_m,
            "vertices": 12000,
            "triangles": 24000,
            "connectedComponents": 2,
            "intentionalSprayComponents": 1,
            "openEdges": 0,
            "holes": 0,
            "silhouetteHausdorffM": 0.01,
            "silhouetteHausdorffPixels": 1.0,
            "normalVariationDegrees": 8.0,
            "offSheetCoverageFraction": 0.05,
            "projectedCompleteAnimationFileBytes": 200 * 1024 * 1024,
            "gpuMeshMs": 2.5,
            "meshDigest": f"digest-{row_id}",
        }

    def _report(self):
        oracle = self._row("v075-r013-b010", 0.075)
        oracle.update({
            "silhouetteHausdorffM": 0.0,
            "silhouetteHausdorffPixels": 0.0,
            "normalVariationDegrees": 4.0,
            "offSheetCoverageFraction": 0.0,
        })
        return {
            "schemaVersion": 1,
            "fixture": {
                "fallDistanceM": 12.0,
                "sheetDiameterM": 0.26,
                "frameCount": 30,
                "cameraWidthPixels": 1280,
                "cameraHeightPixels": 720,
                "worldHausdorffBoundToleranceM": 0.001,
                "worldHausdorffNumericalMarginM": 0.00001,
            },
            "oracleRowId": oracle["id"],
            "measuredRiverFloatLabNetworkBytes": 500 * 1024 * 1024,
            "measuredContainingSectionFileBytes": 500 * 1024 * 1024,
            "networkOtherCompleteFileBytes": 300 * 1024 * 1024,
            "containingSectionOtherCompleteFileBytes": 300 * 1024 * 1024,
            "validationErrors": 0,
            "rows": [
                self._row("v150-r013-b010", 0.15),
                self._row("v100-r013-b010", 0.10),
                oracle,
                self._row("v150-r014-b010", 0.15, 0.14, 0.10),
                self._row("v150-r013-b012", 0.15, 0.13, 0.12),
            ],
        }

    def _evaluate(self, mutate=None):
        report = copy.deepcopy(self._report())
        if mutate:
            mutate(report)
        return quality.evaluate_report(report)

    def test_valid_matrix_selects_coarsest_default_candidate(self):
        summary = self._evaluate()
        self.assertTrue(summary["passed"])
        self.assertEqual(summary["selectedCandidateId"], "v150-r013-b010")
        self.assertEqual(summary["failures"], [])

    def test_selection_is_independent_of_input_row_order(self):
        first = self._report()
        second = copy.deepcopy(first)
        second["rows"] = list(reversed(second["rows"]))
        self.assertEqual(
            quality.evaluate_report(first)["selectedCandidateId"],
            quality.evaluate_report(second)["selectedCandidateId"])

    def test_coarse_metric_mutation_changes_choice_not_row_order(self):
        report = self._report()
        for row in report["rows"]:
            if math.isclose(row["voxelM"], 0.15):
                row["silhouetteHausdorffM"] = 0.04
                row["silhouetteHausdorffPixels"] = 2.1
        reversed_report = copy.deepcopy(report)
        reversed_report["rows"] = list(reversed(reversed_report["rows"]))
        self.assertEqual(
            quality.evaluate_report(report)["selectedCandidateId"],
            "v100-r013-b010")
        self.assertEqual(
            quality.evaluate_report(reversed_report)["selectedCandidateId"],
            "v100-r013-b010")

    def test_native_float_roundoff_keeps_required_matrix_identity(self):
        report = self._report()
        for row in report["rows"]:
            row["voxelM"] = float(f"{row['voxelM'] + 6e-9:.9f}")
            row["radiusM"] = float(f"{row['radiusM'] - 5e-9:.9f}")
            row["blendWidthM"] = float(
                f"{row['blendWidthM'] + 1e-9:.9f}")
        summary = quality.evaluate_report(report)
        self.assertTrue(summary["passed"])
        self.assertEqual(summary["selectedCandidateId"], "v150-r013-b010")

    def test_missing_oracle_row_fails_closed(self):
        summary = self._evaluate(
            lambda report: report.update({"oracleRowId": "missing"}))
        self.assertFalse(summary["passed"])
        self.assertIn("oracle row 'missing' is missing", summary["failures"])
        self.assertIsNone(summary["selectedCandidateId"])

    def test_nonfinite_oracle_fails_whole_report(self):
        def mutate(report):
            report["rows"][2]["gpuMeshMs"] = math.nan
        summary = self._evaluate(mutate)
        self.assertFalse(summary["passed"])
        self.assertIn(
            "oracle row v075-r013-b010 failed validation",
            summary["failures"])
        self.assertIsNone(summary["selectedCandidateId"])

    def test_oracle_self_comparison_must_be_exactly_zero(self):
        def mutate(report):
            report["rows"][2]["silhouetteHausdorffM"] = 0.001
            report["rows"][2]["silhouetteHausdorffPixels"] = 1.0
            report["rows"][2]["offSheetCoverageFraction"] = 0.001
        summary = self._evaluate(mutate)
        self.assertFalse(summary["passed"])
        self.assertIn(
            "oracle row v075-r013-b010 self-comparison metrics must be zero",
            summary["failures"])

    def test_nonfinite_metric_rejects_row(self):
        def mutate(report):
            report["rows"][0]["gpuMeshMs"] = math.nan
        summary = self._evaluate(mutate)
        self.assertIn(
            "v150-r013-b010 gpuMeshMs must be finite and positive",
            summary["rows"]["v150-r013-b010"]["failures"])

    def test_zero_complete_animation_file_size_rejects_row(self):
        def mutate(report):
            report["rows"][0]["projectedCompleteAnimationFileBytes"] = 0
        summary = self._evaluate(mutate)
        self.assertIn(
            "v150-r013-b010 projectedCompleteAnimationFileBytes must be a "
            "positive integer",
            summary["rows"]["v150-r013-b010"]["failures"])

    def test_changed_component_count_rejects_row(self):
        def mutate(report):
            report["rows"][0]["connectedComponents"] = 3
        summary = self._evaluate(mutate)
        self.assertIn(
            "connectedComponents 3 differs from oracle 2",
            summary["rows"]["v150-r013-b010"]["failures"])

    def test_new_hole_or_open_edge_rejects_row(self):
        def mutate(report):
            report["rows"][0]["holes"] = 1
            report["rows"][0]["openEdges"] = 2
        summary = self._evaluate(mutate)
        failures = summary["rows"]["v150-r013-b010"]["failures"]
        self.assertIn("holes 1 exceeds oracle 0", failures)
        self.assertIn("openEdges 2 exceeds oracle 0", failures)

    def test_world_silhouette_threshold_is_strict(self):
        def mutate(report):
            report["rows"][0]["silhouetteHausdorffM"] = 0.0375
        summary = self._evaluate(mutate)
        self.assertIn(
            "silhouetteHausdorffM 0.0375 must be below 0.0375",
            summary["rows"]["v150-r013-b010"]["failures"])

    def test_retained_camera_pixel_threshold_rejects_row(self):
        def mutate(report):
            report["rows"][0]["silhouetteHausdorffPixels"] = 2.001
        summary = self._evaluate(mutate)
        self.assertIn(
            "silhouetteHausdorffPixels 2.001 exceeds 2",
            summary["rows"]["v150-r013-b010"]["failures"])

    def test_parameter_variant_off_sheet_coverage_delta_is_bounded(self):
        def mutate(report):
            report["rows"][3]["offSheetCoverageFraction"] = 0.071
        summary = self._evaluate(mutate)
        self.assertIn(
            "offSheetCoverageFraction delta 0.021 exceeds 0.02",
            summary["rows"]["v150-r014-b010"]["failures"])

    def test_parameter_coverage_delta_is_relative_to_default_row(self):
        report = self._report()
        by_id = {row["id"]: row for row in report["rows"]}
        by_id["v075-r013-b010"]["offSheetCoverageFraction"] = 0.0
        by_id["v150-r013-b010"]["offSheetCoverageFraction"] = 0.01
        by_id["v150-r014-b010"]["offSheetCoverageFraction"] = 0.029
        by_id["v150-r013-b012"]["offSheetCoverageFraction"] = 0.029
        summary = quality.evaluate_report(report)
        self.assertTrue(summary["rows"]["v150-r014-b010"]["passed"])
        self.assertTrue(summary["rows"]["v150-r013-b012"]["passed"])

    def test_parameter_variants_reject_invalid_default_coverage(self):
        report = self._report()
        by_id = {row["id"]: row for row in report["rows"]}
        by_id["v150-r013-b010"]["offSheetCoverageFraction"] = math.nan
        by_id["v150-r014-b010"]["offSheetCoverageFraction"] = 0.9
        by_id["v150-r013-b012"]["offSheetCoverageFraction"] = 0.9
        summary = quality.evaluate_report(report)
        for row_id in ("v150-r014-b010", "v150-r013-b012"):
            self.assertIn(
                "default offSheetCoverageFraction must be finite",
                summary["rows"][row_id]["failures"])
        self.assertEqual(summary["selectedCandidateId"], "v100-r013-b010")

    def test_parameter_variant_preserves_intentional_spray_count(self):
        def mutate(report):
            report["rows"][4]["intentionalSprayComponents"] = 0
        summary = self._evaluate(mutate)
        self.assertIn(
            "intentionalSprayComponents 0 differs from oracle 1",
            summary["rows"]["v150-r013-b012"]["failures"])

    def test_containing_section_file_limit_rejects_row(self):
        def mutate(report):
            report["containingSectionOtherCompleteFileBytes"] = (
                900 * 1024 * 1024)
        summary = self._evaluate(mutate)
        self.assertIn(
            "projectedContainingSectionFileBytes 1153433600 exceeds 1073741824",
            summary["rows"]["v150-r013-b010"]["failures"])

    def test_network_file_budget_rejects_row(self):
        def mutate(report):
            report["networkOtherCompleteFileBytes"] = 500 * 1024 * 1024
        summary = self._evaluate(mutate)
        self.assertIn(
            "projectedNetworkFileBytes 734003200 must be below 734003200",
            summary["rows"]["v150-r013-b010"]["failures"])

    def test_oracle_may_exceed_candidate_memory_budget(self):
        report = self._report()
        oracle = report["rows"][2]
        oracle["projectedCompleteAnimationFileBytes"] = 450 * 1024 * 1024
        summary = quality.evaluate_report(report)
        self.assertTrue(summary["passed"])
        self.assertEqual(summary["selectedCandidateId"],
                         "v150-r013-b010")
        self.assertIn(
            "projectedNetworkFileBytes 786432000 must be below 734003200",
            summary["rows"]["v075-r013-b010"]["failures"])

    def test_missing_required_candidate_matrix_fails_closed(self):
        def mutate(report):
            report["rows"] = report["rows"][:-1]
        summary = self._evaluate(mutate)
        self.assertIn(
            "required candidate v150-r013-b012 is missing",
            summary["failures"])
        self.assertIsNone(summary["selectedCandidateId"])

    def test_missing_current_candidate_fails_closed(self):
        def mutate(report):
            report["rows"] = report["rows"][1:]
        summary = self._evaluate(mutate)
        self.assertIn(
            "required candidate v150-r013-b010 is missing",
            summary["failures"])
        self.assertIsNone(summary["selectedCandidateId"])

    def test_unexpected_candidate_row_fails_closed(self):
        def mutate(report):
            report["rows"].append(self._row("v200-r013-b010", 0.20))
        summary = self._evaluate(mutate)
        self.assertIn(
            "unexpected candidate v200-r013-b010", summary["failures"])
        self.assertIsNone(summary["selectedCandidateId"])

    def test_measured_aggregate_must_match_current_replacement(self):
        def mutate(report):
            report["measuredRiverFloatLabNetworkBytes"] += 1
            report["measuredContainingSectionFileBytes"] += 2
        summary = self._evaluate(mutate)
        self.assertIn(
            "measuredRiverFloatLabNetworkBytes 524288001 does not equal "
            "networkOtherCompleteFileBytes plus current replacement "
            "524288000",
            summary["failures"])
        self.assertIn(
            "measuredContainingSectionFileBytes 524288002 does not equal "
            "containingSectionOtherCompleteFileBytes plus current "
            "replacement 524288000",
            summary["failures"])

    def test_validation_error_rejects_whole_report(self):
        summary = self._evaluate(
            lambda report: report.update({"validationErrors": 1}))
        self.assertIn("validationErrors expected 0, got 1", summary["failures"])
        self.assertIsNone(summary["selectedCandidateId"])

    def test_three_run_comparison_ignores_timing_but_locks_decision_topology_digest(self):
        reports = [copy.deepcopy(self._report()) for _ in range(3)]
        reports[1]["rows"][0]["gpuMeshMs"] = 7.25
        reports[2]["rows"][0]["gpuMeshMs"] = 9.5
        comparison = quality.compare_reports(reports)
        self.assertTrue(comparison["passed"])
        self.assertEqual(comparison["selectedCandidateId"],
                         "v150-r013-b010")
        self.assertEqual(comparison["failures"], [])

    def test_three_run_comparison_rejects_changed_topology_digest_or_decision(self):
        reports = [copy.deepcopy(self._report()) for _ in range(3)]
        reports[1]["rows"][0]["triangles"] += 1
        reports[2]["rows"][0]["meshDigest"] = "changed"
        comparison = quality.compare_reports(reports)
        self.assertFalse(comparison["passed"])
        self.assertIn(
            "run 2 v150-r013-b010 triangles changed from 24000 to 24001",
            comparison["failures"])
        self.assertIn(
            "run 3 v150-r013-b010 meshDigest changed from "
            "'digest-v150-r013-b010' to 'changed'",
            comparison["failures"])

    def test_three_run_comparison_requires_exactly_three_reports(self):
        comparison = quality.compare_reports([self._report(), self._report()])
        self.assertEqual(comparison["failures"],
                         ["expected exactly 3 matrix reports, got 2"])

    def test_three_run_comparison_fails_closed_on_nonobject_reports(self):
        comparison = quality.compare_reports([None, None, None])
        self.assertFalse(comparison["passed"])
        self.assertIn(
            "run 1: quality report must be an object",
            comparison["failures"])
        self.assertIsNone(comparison["selectedCandidateId"])

    def test_markdown_fails_closed_on_nonobject_reports(self):
        reports = [None, None, None]
        comparison = quality.compare_reports(reports)
        markdown = quality.render_markdown(reports, comparison)
        self.assertIn("Three-run topology/digest/decision determinism: **fail**",
                      markdown)
        self.assertIn("run 1: quality report must be an object", markdown)

    def test_markdown_fails_closed_on_invalid_row_elements(self):
        reports = [copy.deepcopy(self._report()) for _ in range(3)]
        reports[1]["rows"][0] = None
        comparison = quality.compare_reports(reports)
        markdown = quality.render_markdown(reports, comparison)
        self.assertIn("Three-run topology/digest/decision determinism: **fail**",
                      markdown)
        self.assertIn("run 2: quality report contains an invalid row",
                      markdown)

    def test_markdown_fails_closed_on_cross_run_row_set_change(self):
        reports = [copy.deepcopy(self._report()) for _ in range(3)]
        reports[1]["rows"] = reports[1]["rows"][:-1]
        comparison = quality.compare_reports(reports)
        markdown = quality.render_markdown(reports, comparison)
        self.assertIn("Three-run topology/digest/decision determinism: **fail**",
                      markdown)
        self.assertIn("run 2 row ids changed", markdown)


if __name__ == "__main__":
    unittest.main()
