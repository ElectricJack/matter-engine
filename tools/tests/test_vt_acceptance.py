import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1] / "vt_acceptance.py"
SPEC = importlib.util.spec_from_file_location("vt_acceptance", MODULE)
vt = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(vt)


class VtAcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "trace.jsonl"
        self.columns = sorted(vt.REQUIRED)

    def fixture(self, count=601):
        header = {"schema": 1, "max_rows": 32768, "columns": self.columns,
                  "gpu_time_basis": "latest_retired_readback_not_current_frame"}
        rows = []
        for i in range(count):
            data = {name: 0 for name in self.columns}
            data.update(active=1, variants=2, pool_capacity=256, pool_used=4,
                        pinned=2, fills_total=4, gpu_vt_ms=None,
                        cpu_begin_ms=.01, cpu_pre_pass_ms=.02,
                        cpu_post_pass_ms=.01, cpu_registration_ms=.01,
                        gpu_readback_sequence=i + 1)
            rows.append({"serial": i + 1, "vt_serial": i + 1, "elapsed_ms": i * 16.7,
                         "marker": "begin" if i == 0 else "end" if i == count - 1 else "",
                         "values": [data[name] for name in self.columns]})
        return [header, *rows, {"end": True, "rows": count, "dropped_rows": 0}]

    def write(self, records):
        self.path.write_text("\n".join(json.dumps(r) for r in records) + "\n")

    def analyze(self, records):
        self.write(records)
        return vt.analyze_trace(self.path, "begin", "end")

    def test_complete_stable_window_does_not_claim_gpu_or_full_acceptance(self):
        result = self.analyze(self.fixture())
        self.assertTrue(result["settled_interval_counter_gate"])
        self.assertAlmostEqual(result["cpu_vt_hooks_and_registration"]["p95_ms"], .05)
        self.assertIsNone(result["gpu_vt"]["p95_ms"])
        self.assertFalse(result["gpu_vt_available"])
        self.assertFalse(result["full_vt_acceptance"])
        self.assertFalse(result["queue_classes"]["available"])

    def test_queue_classes_report_age_and_reject_missing_work(self):
        self.columns = sorted(vt.REQUIRED | set(vt.QUEUE_CLASSES))
        records = self.fixture()
        row = records[20]["values"]
        for name, value in [("queue", 3), ("mandatory_queue", 1),
                            ("detail_queue", 2), ("oldest_mandatory_age_frames", 17)]:
            row[self.columns.index(name)] = value
        result = self.analyze(records)
        self.assertFalse(result["settled_interval_counter_gate"])
        self.assertEqual(result["queue_classes"]["max_oldest_mandatory_age_frames"], 17)
        row[self.columns.index("queue")] = 2
        with self.assertRaises(vt.CaptureError):
            self.analyze(records)
        records[0]["columns"].remove("mandatory_queue")
        with self.assertRaises(vt.CaptureError):
            self.analyze(records)

    def test_full_cpu_cost_requires_demand_and_counts_each_frame(self):
        legacy = self.analyze(self.fixture())
        self.assertFalse(legacy["cpu_vt_render_thread_available"])
        self.assertIsNone(legacy["cpu_vt_render_thread"]["p95_ms"])
        self.columns = sorted(vt.REQUIRED | {"cpu_demand_ms"})
        records = self.fixture()
        index = self.columns.index("cpu_demand_ms")
        for row in records[1:-1]:
            row["values"][index] = .35
        result = self.analyze(records)
        self.assertTrue(result["cpu_vt_render_thread_available"])
        self.assertAlmostEqual(result["cpu_vt_render_thread"]["p95_ms"], .40)
        self.assertAlmostEqual(result["cpu_vt_hooks_and_registration"]["p95_ms"], .05)
        self.assertEqual(result["cpu_vt_demand"]["samples"], 601)
        records[10]["values"][index] = None
        result = self.analyze(records)
        self.assertFalse(result["cpu_vt_render_thread_available"])
        self.assertIsNone(result["cpu_vt_render_thread"]["p95_ms"])
        self.assertEqual(result["cpu_vt_render_thread_missing_frames"], 1)

    def test_measured_zero_demand_is_distinct_from_missing_demand(self):
        self.columns = sorted(vt.REQUIRED | {"cpu_demand_ms"})
        result = self.analyze(self.fixture())
        self.assertTrue(result["cpu_vt_render_thread_available"])
        self.assertEqual(result["cpu_vt_demand"]["p95_ms"], 0)
        self.assertAlmostEqual(result["cpu_vt_render_thread"]["p95_ms"], .05)

    def test_truncated_capture_and_missing_counter_are_rejected(self):
        records = self.fixture()
        self.write(records[:-1])
        with self.assertRaises(vt.CaptureError):
            vt.load_trace(self.path)
        records[0]["columns"] = [name for name in self.columns if name != "invalidations_total"]
        self.write(records)
        with self.assertRaises(vt.CaptureError):
            vt.load_trace(self.path)

    def test_durable_dirty_work_and_stale_publication_fail_settled_gate(self):
        self.columns = sorted(vt.REQUIRED | set(vt.REPLACEMENT))
        records = self.fixture()
        records[20]["values"][self.columns.index("dirty_pages")] = 2
        result = self.analyze(records)
        self.assertFalse(result["settled_interval_counter_gate"])
        self.assertEqual(result["replacement"]["max_dirty_pages"], 2)
        records[20]["values"][self.columns.index("dirty_pages")] = 0
        records[20]["values"][self.columns.index("fills_stale_total")] = 1
        self.assertFalse(self.analyze(records)["settled_interval_counter_gate"])
        records[20]["values"][self.columns.index("replacement_reserve_pages")] = 255
        with self.assertRaises(vt.CaptureError):
            self.analyze(records)

    def test_intermediate_change_cannot_hide_behind_matching_endpoints(self):
        records = self.fixture()
        records[20]["values"][self.columns.index("vertex_uploads")] = 5
        result = self.analyze(records)
        self.assertFalse(result["settled_interval_counter_gate"])
        self.assertEqual(result["changed_frame_pairs"]["vertex_uploads"], 2)

    def test_dropped_rows_and_repeated_vt_snapshot_do_not_pass(self):
        records = self.fixture()
        records[-1]["dropped_rows"] = 1
        self.assertFalse(self.analyze(records)["settled_interval_counter_gate"])
        records[-1]["dropped_rows"] = 0
        for row in records[1:-1]:
            row["vt_serial"] = 1
        result = self.analyze(records)
        self.assertFalse(result["settled_interval_counter_gate"])
        self.assertEqual(result["distinct_active_vt_frames"], 1)

    def test_settled_gate_rejects_new_request_drops_but_allows_old_ones(self):
        records = self.fixture()
        index = self.columns.index("requests_dropped_total")
        for row in records[1:-1]:
            row["values"][index] = 9
        self.assertTrue(self.analyze(records)["settled_interval_counter_gate"])
        for row in records[20:-1]:
            row["values"][index] = 10
        result = self.analyze(records)
        self.assertFalse(result["settled_interval_counter_gate"])
        self.assertEqual(result["changed_frame_pairs"]["requests_dropped_total"], 1)

    def test_gpu_readback_is_deduplicated_and_true_zero_is_valid(self):
        records = self.fixture()
        for row in records[1:-1]:
            row["values"][self.columns.index("gpu_readback_sequence")] = 8
            row["values"][self.columns.index("gpu_vt_ms")] = 0
        result = self.analyze(records)
        self.assertTrue(result["gpu_vt_available"])
        self.assertEqual(result["gpu_vt"]["samples"], 1)
        self.assertEqual(result["gpu_vt"]["p95_ms"], 0)

    def test_invalid_numbers_and_missing_markers_fail_closed(self):
        records = self.fixture()
        records[2]["values"][0] = float("nan")
        with self.assertRaises(vt.CaptureError):
            self.analyze(records)
        records = self.fixture()
        records[1]["marker"] = "wrong"
        with self.assertRaises(vt.CaptureError):
            self.analyze(records)


if __name__ == "__main__":
    unittest.main()
