import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
TOOL = HERE.parent / "frame_attribution.py"
spec = importlib.util.spec_from_file_location("frame_attribution", TOOL)
fa = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fa)


def fixture(gbuffer, total):
    return {"gpu_pass_statistics": {"passes": {
        "gbuffer": {"samples": 3, "median_ms": gbuffer, "p95_ms": gbuffer * 1.5, "p99_ms": gbuffer * 2, "max_ms": gbuffer * 3},
        "total": {"samples": 3, "median_ms": total, "p95_ms": total * 1.5, "p99_ms": total * 2, "max_ms": total * 3},
        "vt": {"samples": 0, "median_ms": None, "p95_ms": None, "p99_ms": None, "max_ms": None}}},
        "frame_times_ms": [10, 12, 30]}


def table_rows(table):
    return [line for line in table.splitlines() if line.startswith("| ")]


class FrameAttributionTests(unittest.TestCase):
    def write(self, directory, name, data):
        path = pathlib.Path(directory) / name
        path.write_text(json.dumps(data))
        return path

    def test_table_sorted_by_first_file_p95_and_nulls_render_as_dash(self):
        with tempfile.TemporaryDirectory() as d:
            a = self.write(d, "a.json", fixture(20, 60))
            b = self.write(d, "b.json", fixture(5, 40))
            rows = table_rows(fa.render([a, b]))
            self.assertIn("a median / p95 / p99", rows[0])
            self.assertIn("b median / p95 / p99", rows[0])
            self.assertTrue(rows[1].startswith("| frame_interval"))
            self.assertIn("total", rows[2])
            self.assertIn("gbuffer", rows[3])
            self.assertIn("vt", rows[-1])
            self.assertIn("—", rows[-1])
            self.assertIn("| 60.00 / 90.00 / 120.00 | 40.00 / 60.00 / 80.00 |", rows[2])

    def test_missing_gpu_block_is_an_error(self):
        with tempfile.TemporaryDirectory() as d:
            p = self.write(d, "x.json", {})
            with self.assertRaises(fa.AttributionError):
                fa.render([p])

    def test_frame_interval_uses_the_engines_nearest_rank(self):
        # write_perf_result and PerfGpuStats index sorted[ceil(q * n) - 1];
        # the table must agree with the perf.json summary fields it sits beside.
        data = fixture(1, 2)
        data["frame_times_ms"] = list(range(20, 0, -1))  # capture order, unsorted
        with tempfile.TemporaryDirectory() as d:
            rows = table_rows(fa.render([self.write(d, "run.json", data)]))
        self.assertEqual(rows[1], "| frame_interval | 10.50 / 19.00 / 20.00 |")

    def test_perf_file_without_frame_array_falls_back_to_summary_fields(self):
        data = fixture(1, 2)
        del data["frame_times_ms"]
        data["median_frame_ms"] = 16.5
        data["p95_frame_ms"] = 21.25
        with tempfile.TemporaryDirectory() as d:
            rows = table_rows(fa.render([self.write(d, "old.json", data)]))
        self.assertEqual(rows[1], "| frame_interval | 16.50 / 21.25 / — |")

    def test_zone_absent_from_the_first_file_still_gets_a_row(self):
        extra = fixture(5, 40)
        extra["gpu_pass_statistics"]["passes"]["water_forward"] = {
            "samples": 3, "median_ms": 1, "p95_ms": 2, "p99_ms": 3, "max_ms": 4}
        with tempfile.TemporaryDirectory() as d:
            a = self.write(d, "a.json", fixture(20, 60))
            b = self.write(d, "b.json", extra)
            rows = table_rows(fa.render([a, b]))
        self.assertEqual(rows[-1], "| water_forward | — / — / — | 1.00 / 2.00 / 3.00 |")

    def test_same_file_name_in_two_folders_is_labelled_by_folder(self):
        with tempfile.TemporaryDirectory() as d:
            for run in ("before", "after"):
                (pathlib.Path(d) / run).mkdir()
            a = self.write(d, "before/perf.json", fixture(20, 60))
            b = self.write(d, "after/perf.json", fixture(5, 40))
            header = table_rows(fa.render([a, b]))[0]
        self.assertEqual(header, "| zone | before/perf median / p95 / p99 | after/perf median / p95 / p99 |")

    def test_cli_writes_the_table_to_out(self):
        with tempfile.TemporaryDirectory() as d:
            a = self.write(d, "a.json", fixture(20, 60))
            out = pathlib.Path(d) / "table.md"
            subprocess.run([sys.executable, str(TOOL), str(a), "--out", str(out)], check=True)
            self.assertEqual(out.read_text(encoding="utf-8"), fa.render([a]))


if __name__ == "__main__":
    unittest.main()
