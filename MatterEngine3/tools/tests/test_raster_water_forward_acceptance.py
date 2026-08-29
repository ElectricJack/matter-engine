import json
import tempfile
import unittest
from pathlib import Path

from MatterEngine3.tools import raster_water_forward_acceptance as acceptance


class RasterWaterForwardAcceptanceTests(unittest.TestCase):
    REQUIRED_SCREENSHOTS = (
        "shallow-player-low.png",
        "upper-rapids.png",
        "waterfall-side.png",
        "plunge-pool.png",
        "section-handoff.png",
    )

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.baseline = self.root / "baseline"
        self.candidate = self.root / "candidate"
        self.screenshots = self.candidate / "screenshots"
        self.baseline.mkdir()
        self.candidate.mkdir()
        self.screenshots.mkdir()
        self._write_metrics()
        self._write_screenshots()

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def _baseline_metrics():
        return {
            "median_frame_ms": 10.0,
            "p95_frame_ms": 15.0,
            "validation_errors": 0,
            "water_animation_decode_dispatch_delta": 0,
            "water_animation_steady_state_allocation_delta": 0,
        }

    @staticmethod
    def _candidate_metrics():
        return {
            "median_frame_ms": 11.0,
            "p95_frame_ms": 17.0,
            "validation_errors": 0,
            "water_animation_decode_dispatch_delta": 0,
            "water_animation_steady_state_allocation_delta": 0,
            "water_forward_width": 1280,
            "water_forward_height": 720,
            "water_forward_image_bytes": 1280 * 720 * 12,
        }

    def _write_metrics(self, candidate_overrides=None):
        for samples in (1, 10, 16):
            tag = f"{samples:02d}"
            baseline = self._baseline_metrics()
            candidate = self._candidate_metrics()
            baseline.update({"world": "RiverFloatLab", "rt_samples": samples})
            candidate.update({"world": "RiverFloatLab", "rt_samples": samples})
            if candidate_overrides:
                candidate.update(candidate_overrides)
            (self.baseline / f"shadow-{tag}.json").write_text(
                json.dumps(baseline), encoding="utf-8")
            (self.candidate / f"shadow-{tag}.json").write_text(
                json.dumps(candidate), encoding="utf-8")

    def _write_screenshots(self):
        for name in self.REQUIRED_SCREENSHOTS:
            path = self.screenshots / name
            path.write_bytes(b"synthetic-png")
            Path(str(path) + ".done").write_text("captured\n", encoding="utf-8")

    def _compare(self):
        return acceptance.compare_acceptance(
            self.baseline, self.candidate, self.screenshots)

    def test_exact_thresholds_and_all_five_screenshots_pass(self):
        summary = self._compare()
        self.assertTrue(summary["passed"])
        self.assertEqual(summary["failures"], [])
        self.assertEqual(set(summary["screenshots"]), set(self.REQUIRED_SCREENSHOTS))
        self.assertEqual(summary["settings"]["01"]["median_frame_ms"]["limit"], 11.0)
        self.assertEqual(summary["settings"]["01"]["p95_frame_ms"]["limit"], 17.0)

    def test_median_regression_fails(self):
        self._write_metrics({"median_frame_ms": 11.001})
        summary = self._compare()
        self.assertFalse(summary["passed"])
        self.assertIn(
            "shadow-01 median_frame_ms 11.001 exceeds limit 11",
            summary["failures"])

    def test_p95_regression_fails(self):
        self._write_metrics({"p95_frame_ms": 17.001})
        summary = self._compare()
        self.assertFalse(summary["passed"])
        self.assertIn(
            "shadow-01 p95_frame_ms 17.001 exceeds limit 17",
            summary["failures"])

    def test_nonzero_validation_fails(self):
        self._write_metrics({"validation_errors": 1})
        summary = self._compare()
        self.assertIn(
            "shadow-01 validation_errors expected 0, got 1",
            summary["failures"])

    def test_nonzero_decode_fails(self):
        self._write_metrics({"water_animation_decode_dispatch_delta": 2})
        summary = self._compare()
        self.assertIn(
            "shadow-01 water_animation_decode_dispatch_delta expected 0, got 2",
            summary["failures"])

    def test_nonzero_steady_state_allocation_fails(self):
        self._write_metrics({"water_animation_steady_state_allocation_delta": 3})
        summary = self._compare()
        self.assertIn(
            "shadow-01 water_animation_steady_state_allocation_delta expected 0, got 3",
            summary["failures"])

    def test_incorrect_forward_image_byte_formula_fails(self):
        self._write_metrics({"water_forward_image_bytes": 42})
        summary = self._compare()
        self.assertIn(
            "shadow-01 water_forward_image_bytes expected 11059200, got 42",
            summary["failures"])

    def test_wrong_world_identity_fails(self):
        path = self.candidate / "shadow-10.json"
        metrics = json.loads(path.read_text(encoding="utf-8"))
        metrics["world"] = "AnotherWorld"
        path.write_text(json.dumps(metrics), encoding="utf-8")
        summary = self._compare()
        self.assertIn(
            "shadow-10 candidate world expected 'RiverFloatLab', got 'AnotherWorld'",
            summary["failures"])

    def test_wrong_shadow_sample_setting_fails(self):
        path = self.candidate / "shadow-16.json"
        metrics = json.loads(path.read_text(encoding="utf-8"))
        metrics["rt_samples"] = 10
        path.write_text(json.dumps(metrics), encoding="utf-8")
        summary = self._compare()
        self.assertIn(
            "shadow-16 candidate rt_samples expected 16, got 10",
            summary["failures"])

    def test_native_runner_requires_fresh_perf_evidence(self):
        runner = (
            Path(__file__).resolve().parents[1]
            / "run_raster_water_forward_acceptance.ps1"
        ).read_text(encoding="utf-8")
        remove = "Remove-Item -LiteralPath $perfOutput -Force"
        launch = (
            'Invoke-Checked -Description '
            '"RiverFloatLab shadow-$tag performance run"'
        )
        validate = "did not produce fresh performance JSON"
        self.assertIn(remove, runner)
        self.assertIn(launch, runner)
        self.assertIn(validate, runner)
        self.assertLess(runner.index(remove), runner.index(launch))
        self.assertGreater(runner.index(validate), runner.index(launch))

    def test_same_metrics_directory_cannot_self_compare(self):
        for samples in (1, 10, 16):
            metrics = self._candidate_metrics()
            metrics.update({"world": "RiverFloatLab", "rt_samples": samples})
            (self.baseline / f"shadow-{samples:02d}.json").write_text(
                json.dumps(metrics), encoding="utf-8")
        summary = acceptance.compare_acceptance(
            self.baseline, self.baseline, self.screenshots)
        self.assertFalse(summary["passed"])
        self.assertEqual(
            summary["failures"],
            [f"baseline and candidate directories must be distinct: "
             f"{self.baseline.resolve()}"])

    def test_nested_metrics_directory_trees_are_rejected(self):
        nested = self.baseline / "candidate"
        for baseline, candidate in (
                (self.baseline, nested), (nested, self.baseline)):
            with self.subTest(baseline=baseline, candidate=candidate):
                summary = acceptance.compare_acceptance(
                    baseline, candidate, self.screenshots)
                self.assertFalse(summary["passed"])
                self.assertEqual(
                    summary["failures"],
                    ["baseline and candidate directory trees must be disjoint: "
                     f"{Path(baseline).resolve()} <> "
                     f"{Path(candidate).resolve()}"])

    def test_native_runner_validates_path_trees_before_any_write(self):
        runner = (
            Path(__file__).resolve().parents[1]
            / "run_raster_water_forward_acceptance.ps1"
        ).read_text(encoding="utf-8")
        guard = "& py -3 $comparator validate-paths"
        create = "New-Item -ItemType Directory -Force"
        remove = "Remove-Item -LiteralPath $perfOutput -Force"
        self.assertIn(guard, runner)
        self.assertLess(runner.index(guard), runner.index(create))
        self.assertLess(runner.index(guard), runner.index(remove))

    def test_missing_screenshot_fails(self):
        missing = self.screenshots / "waterfall-side.png"
        missing.unlink()
        summary = self._compare()
        self.assertIn(
            f"missing screenshot: {missing}", summary["failures"])

    def test_zero_byte_screenshot_fails(self):
        empty = self.screenshots / "plunge-pool.png"
        empty.write_bytes(b"")
        summary = self._compare()
        self.assertIn(
            f"empty screenshot: {empty}", summary["failures"])

    def test_zero_byte_done_sidecar_fails(self):
        sidecar = Path(str(self.screenshots / "upper-rapids.png") + ".done")
        sidecar.write_bytes(b"")
        summary = self._compare()
        self.assertIn(
            f"empty screenshot sidecar: {sidecar}", summary["failures"])


if __name__ == "__main__":
    unittest.main()
