import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from PIL import Image, ImageDraw

from MatterEngine3.tools import water_mesh_continuity_acceptance as acceptance


class WaterMeshContinuityAcceptanceTests(unittest.TestCase):
    FRAMES = (0, 7, 15, 22, 29)
    VIEWS = ("normal", "geometry-normal", "foam-driver", "identity")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.cold_path = self.root / "cold.json"
        self.cache_path = self.root / "cache.json"
        self.edit_path = self.root / "edit.json"
        self.native_path = self.root / "native.json"
        self.screenshots = self.root / "screenshots"
        self.screenshots.mkdir()
        self.cold = self._trace("cold")
        self.cache = self._trace("cache")
        self.edit = self._trace("edit")
        self.native = {
            "validationErrors": 0,
            "waterDecodeDispatches": 0,
            "waterBlasBuilds": 0,
            "waterTlasInstances": 0,
            "waterRtRecords": 0,
            "rasterDirectDraws": 1,
        }
        self._write_all()
        self._write_screenshots()

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def _cut_metrics():
        return [{
            "symmetricHausdorffM": 0.009375,
            "rmsDistanceM": 0.001,
            "minimumNormalDot": 0.995,
            "unmatchedOpenEdges": 0,
            "duplicateCoplanarTriangles": 0,
        } for _ in range(30)]

    @staticmethod
    def _field_metrics():
        return {
            "samplePairs": 8,
            "maximumHeightDeltaM": 0.009375,
            "minimumNormalDot": 0.995,
            "maximumTurbulenceDelta": 0.01,
            "maximumAerationDelta": 0.01,
            "maximumFoamDelta": 0.01,
            "featureLabelsDeterministic": True,
        }

    def _trace(self, kind):
        cold = kind == "cold"
        edit = kind == "edit"
        upper_key = "upper-key"
        upper_digest = "upper-digest"
        lower_key = "lower-key-edit" if edit else "lower-key"
        lower_digest = "lower-digest-edit" if edit else "lower-digest"
        upper_boundary_key = "upper-boundary-key"
        upper_boundary_digest = "upper-boundary-digest"
        lower_boundary_key = (
            "lower-boundary-key-edit" if edit else "lower-boundary-key")
        lower_boundary_digest = (
            "lower-boundary-digest-edit" if edit
            else "lower-boundary-digest")
        handoff_suffix = "edit" if edit else "base"
        trace = {
            "networkState": "Ready",
            "sections": [
                {
                    "id": "upper",
                    "cacheHit": not cold,
                    "simulateMs": 100.0 if cold else 0.0,
                    "animationCacheHit": not cold,
                    "animationSemanticKey": upper_key,
                    "animationPayloadDigest": upper_digest,
                    "animationFrameVertexCounts": list(range(100, 130)),
                    "animationFrameTriangleCounts": list(range(200, 230)),
                    "boundarySourceSemanticKeys": [upper_boundary_key],
                    "boundarySourcePayloadDigests": [upper_boundary_digest],
                },
                {
                    "id": "lower",
                    "cacheHit": kind == "cache",
                    "simulateMs": 0.0 if kind == "cache" else 80.0,
                    "animationCacheHit": kind == "cache",
                    "animationSemanticKey": lower_key,
                    "animationPayloadDigest": lower_digest,
                    "animationFrameVertexCounts": list(range(300, 330)),
                    "animationFrameTriangleCounts": list(range(400, 430)),
                    "boundarySourceSemanticKeys": [lower_boundary_key],
                    "boundarySourcePayloadDigests": [lower_boundary_digest],
                },
            ],
            "handoffs": {
                "pool-one": {
                    "staticCacheHit": kind == "cache",
                    "animationCacheHit": kind == "cache",
                    "semanticKey": f"handoff-key-{handoff_suffix}",
                    "payloadDigest": f"handoff-digest-{handoff_suffix}",
                    "animationSemanticKey": f"handoff-animation-key-{handoff_suffix}",
                    "animationPayloadDigest": f"handoff-animation-digest-{handoff_suffix}",
                    "animationFrameMs": [1.0] * 30,
                    "animationFileBytes": 1024,
                    "boundarySourceBytes": 512,
                    "peakBuildCpuPayloadBytes": 4096,
                    "sourceBlendRequired": True,
                    "loopFrame29To0Synchronized": True,
                    "retainedTemporaryDamSupportContributors": 12,
                    "upstreamCut": self._cut_metrics(),
                    "downstreamCut": self._cut_metrics(),
                    "upstreamField": self._field_metrics(),
                    "downstreamField": self._field_metrics(),
                },
            },
            "networkPeakBuildCpuPayloadBytes": 8192,
        }
        if kind == "cache":
            trace["handoffs"]["pool-one"].update({
                "semanticKey": "handoff-key-base",
                "payloadDigest": "handoff-digest-base",
                "animationSemanticKey": "handoff-animation-key-base",
                "animationPayloadDigest": "handoff-animation-digest-base",
            })
        return trace

    def _write_all(self):
        for path, value in (
                (self.cold_path, self.cold),
                (self.cache_path, self.cache),
                (self.edit_path, self.edit),
                (self.native_path, self.native)):
            path.write_text(json.dumps(value), encoding="utf-8")

    def _write_screenshots(self):
        for frame in self.FRAMES:
            for view in self.VIEWS:
                path = (self.screenshots / f"frame-{frame:02d}-{view}" /
                        "section-handoff.png")
                path.parent.mkdir(parents=True, exist_ok=True)
                image = Image.new("RGB", (24, 12), (152, 122, 111))
                draw = ImageDraw.Draw(image)
                draw.rectangle((0, 0, 7, 11), fill=(97, 138, 139))
                draw.rectangle((8, 0, 15, 11), fill=(120, 143, 120))
                draw.rectangle((16, 0, 23, 11), fill=(64, 76, 108))
                image.save(path)
                Path(str(path) + ".done").write_text(
                    "captured\n", encoding="utf-8")

    def _compare(self):
        return acceptance.compare_stage1(
            self.cold_path, self.cache_path, self.edit_path,
            self.screenshots, self.native_path)

    def test_exact_stage1_evidence_passes(self):
        summary = self._compare()
        self.assertTrue(summary["passed"])
        self.assertEqual(summary["failures"], [])
        self.assertEqual(len(summary["screenshots"]), 20)
        self.assertEqual(summary["worst"]["symmetricHausdorffM"], 0.009375)
        self.assertEqual(summary["worst"]["minimumNormalDot"], 0.995)

    def test_exact_cli_infers_edit_trace_and_native_gate_paths(self):
        root = self.root / "stage1"
        cold_path = root / "cold" / "trace" / "timings.json"
        cache_path = root / "cache" / "trace" / "timings.json"
        edit_path = root / "edit" / "trace" / "timings.json"
        native_path = root / "native" / "native-gates.json"
        screenshots = root / "screenshots"
        for path, value in (
                (cold_path, self.cold),
                (cache_path, self.cache),
                (edit_path, self.edit),
                (native_path, self.native)):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(value), encoding="utf-8")
        for frame in self.FRAMES:
            for view in self.VIEWS:
                path = (screenshots / f"frame-{frame:02d}-{view}" /
                        "section-handoff.png")
                path.parent.mkdir(parents=True, exist_ok=True)
                source = (self.screenshots /
                          f"frame-{frame:02d}-{view}" /
                          "section-handoff.png")
                path.write_bytes(source.read_bytes())
                Path(str(path) + ".done").write_text(
                    "captured\n", encoding="utf-8")
        summary_path = root / "stage1-summary.json"
        with contextlib.redirect_stdout(io.StringIO()):
            exit_code = acceptance.main([
                "stage1", "--cold", str(cold_path),
                "--cache", str(cache_path),
                "--screenshots", str(screenshots),
                "--output", str(summary_path),
            ])
        self.assertEqual(exit_code, 0)
        self.assertTrue(json.loads(
            summary_path.read_text(encoding="utf-8"))["passed"])

    def test_hausdorff_above_quantization_fails(self):
        self.cold["handoffs"]["pool-one"]["downstreamCut"][12][
            "symmetricHausdorffM"] = 0.009376
        self._write_all()
        self.assertIn("exceeds voxel/16", "\n".join(self._compare()["failures"]))

    def test_normal_dot_below_threshold_fails(self):
        self.cold["handoffs"]["pool-one"]["upstreamCut"][4][
            "minimumNormalDot"] = 0.994999
        self._write_all()
        self.assertIn("minimumNormalDot", "\n".join(self._compare()["failures"]))

    def test_open_edge_fails(self):
        self.cold["handoffs"]["pool-one"]["upstreamCut"][7][
            "unmatchedOpenEdges"] = 1
        self._write_all()
        self.assertIn("unmatchedOpenEdges", "\n".join(self._compare()["failures"]))

    def test_duplicate_triangle_fails(self):
        self.cold["handoffs"]["pool-one"]["downstreamCut"][9][
            "duplicateCoplanarTriangles"] = 1
        self._write_all()
        self.assertIn(
            "duplicateCoplanarTriangles", "\n".join(self._compare()["failures"]))

    def test_frame_count_not_thirty_fails(self):
        self.cold["handoffs"]["pool-one"]["upstreamCut"].pop()
        self._write_all()
        self.assertIn("expected 30 frames", "\n".join(self._compare()["failures"]))

    def test_frame_29_to_zero_mismatch_fails(self):
        self.cold["handoffs"]["pool-one"][
            "loopFrame29To0Synchronized"] = False
        self._write_all()
        self.assertIn("frame 29/0", "\n".join(self._compare()["failures"]))

    def test_upstream_resimulation_on_downstream_edit_fails(self):
        upper = self.edit["sections"][0]
        upper["cacheHit"] = False
        upper["simulateMs"] = 1.0
        self._write_all()
        self.assertIn("upstream simulateMs", "\n".join(self._compare()["failures"]))

    def test_boolean_upstream_simulation_time_fails_closed(self):
        self.edit["sections"][0]["simulateMs"] = False
        self._write_all()
        self.assertIn("simulateMs expected 0", "\n".join(
            self._compare()["failures"]))

    def test_invalid_frame_timing_fails_closed(self):
        self.cold["handoffs"]["pool-one"]["animationFrameMs"][4] = "bad"
        self._write_all()
        self.assertIn("animationFrameMs frame 4", "\n".join(
            self._compare()["failures"]))

    def test_zero_frame_geometry_count_fails_closed(self):
        self.cold["sections"][0]["animationFrameVertexCounts"][8] = 0
        self._write_all()
        self.assertIn("animationFrameVertexCounts frame 8", "\n".join(
            self._compare()["failures"]))

    def test_invalid_network_peak_memory_fails_closed(self):
        self.cold["networkPeakBuildCpuPayloadBytes"] = False
        self._write_all()
        self.assertIn("networkPeakBuildCpuPayloadBytes", "\n".join(
            self._compare()["failures"]))

    def test_boolean_field_sample_count_fails_closed(self):
        self.cold["handoffs"]["pool-one"]["upstreamField"][
            "samplePairs"] = True
        self._write_all()
        self.assertIn("field samplePairs", "\n".join(
            self._compare()["failures"]))

    def test_upstream_boundary_sidecar_digest_change_fails(self):
        self.edit["sections"][0]["boundarySourcePayloadDigests"] = ["changed"]
        self._write_all()
        self.assertIn("upstream boundary sidecar", "\n".join(
            self._compare()["failures"]))

    def test_broad_downstream_edit_invalidation_fails(self):
        self.edit["sections"][0]["animationPayloadDigest"] = "changed"
        self._write_all()
        self.assertIn("upstream animation payload", "\n".join(
            self._compare()["failures"]))

    def test_negative_retained_dam_support_count_fails_closed(self):
        self.cold["handoffs"]["pool-one"][
            "retainedTemporaryDamSupportContributors"] = -1
        self._write_all()
        self.assertIn("retained temporary-dam support", "\n".join(
            self._compare()["failures"]))

    def test_boolean_retained_dam_support_count_fails_closed(self):
        self.cold["handoffs"]["pool-one"][
            "retainedTemporaryDamSupportContributors"] = True
        self._write_all()
        self.assertIn("retained temporary-dam support", "\n".join(
            self._compare()["failures"]))

    def test_nonzero_water_rt_counter_fails(self):
        self.native["waterRtRecords"] = 1
        self._write_all()
        self.assertIn("waterRtRecords", "\n".join(self._compare()["failures"]))

    def test_missing_phase_image_fails(self):
        missing = (self.screenshots / "frame-22-foam-driver" /
                   "section-handoff.png")
        missing.unlink()
        self.assertIn("missing screenshot", "\n".join(self._compare()["failures"]))

    def test_empty_phase_sidecar_fails(self):
        sidecar = Path(str(
            self.screenshots / "frame-07-identity" /
            "section-handoff.png") + ".done")
        sidecar.write_bytes(b"")
        self.assertIn("empty screenshot sidecar", "\n".join(
            self._compare()["failures"]))

    def test_identity_background_band_between_water_owners_fails(self):
        path = (self.screenshots / "frame-15-identity" /
                "section-handoff.png")
        image = Image.new("RGB", (24, 12), (152, 122, 111))
        draw = ImageDraw.Draw(image)
        draw.rectangle((0, 0, 6, 11), fill=(97, 138, 139))
        draw.rectangle((10, 0, 15, 11), fill=(120, 143, 120))
        draw.rectangle((16, 0, 23, 11), fill=(64, 76, 108))
        image.save(path)
        self.assertIn("background band separates water owners", "\n".join(
            self._compare()["failures"]))

    def test_invalid_json_fails_closed(self):
        self.cache_path.write_text("{not-json", encoding="utf-8")
        summary = self._compare()
        self.assertFalse(summary["passed"])
        self.assertIn("invalid JSON", "\n".join(summary["failures"]))

    def test_field_discontinuity_fails(self):
        self.cold["handoffs"]["pool-one"]["upstreamField"][
            "maximumAerationDelta"] = 0.011
        self._write_all()
        self.assertIn("maximumAerationDelta", "\n".join(
            self._compare()["failures"]))

    def test_nondeterministic_feature_label_fails(self):
        self.cold["handoffs"]["pool-one"]["downstreamField"][
            "featureLabelsDeterministic"] = False
        self._write_all()
        self.assertIn("feature labels", "\n".join(
            self._compare()["failures"]))

    def test_normal_capture_omits_diagnostic_environment_variable(self):
        runner = (Path(__file__).parents[1] /
                  "run_water_mesh_continuity_acceptance.ps1").read_text(
                      encoding="utf-8")
        self.assertNotIn("'MATTER_WATER_DIAGNOSTIC_VIEW='", runner)
        self.assertIn(
            "Remove-Item Env:MATTER_WATER_DIAGNOSTIC_VIEW", runner)

    def test_runner_requires_explicit_direct_draw_telemetry(self):
        runner = (Path(__file__).parents[1] /
                  "run_water_mesh_continuity_acceptance.ps1").read_text(
                      encoding="utf-8")
        self.assertIn(
            "water animation raster direct draws: (\\d+)", runner)
        self.assertIn("$rasterDirectDraws.Groups[1].Value", runner)

    def test_runner_checks_native_exit_code_without_promoting_stderr(self):
        runner = (Path(__file__).parents[1] /
                  "run_water_mesh_continuity_acceptance.ps1").read_text(
                      encoding="utf-8")
        self.assertIn("$ErrorActionPreference = 'Continue'", runner)
        self.assertIn("$smokeExitCode = $LASTEXITCODE", runner)
        self.assertIn("if ($smokeExitCode -ne 0)", runner)

    def test_runner_rejects_empty_sidecar_without_rewriting_it(self):
        runner = (Path(__file__).parents[1] /
                  "run_water_mesh_continuity_acceptance.ps1").read_text(
                      encoding="utf-8")
        self.assertIn(
            "(Get-Item -LiteralPath $done).Length -eq 0", runner)
        self.assertNotIn(
            "captured by animated-water Stage1 acceptance", runner)

    def test_capture_waits_for_authored_camera_adoption_before_fifo_camera(self):
        timeline = (Path(__file__).parents[1] /
                    "water_mesh_continuity_acceptance.timeline").read_text(
                        encoding="utf-8").splitlines()
        self.assertIn("wait_frames 1", timeline)
        bake_index = timeline.index("wait_event bake.finished 3600")
        adoption_index = timeline.index("wait_frames 1")
        camera_index = next(
            index for index, line in enumerate(timeline)
            if line.startswith("cam "))
        self.assertLess(bake_index, adoption_index)
        self.assertLess(adoption_index, camera_index)


if __name__ == "__main__":
    unittest.main()
