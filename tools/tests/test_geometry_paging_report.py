import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('paging_report', Path(__file__).parents[1] / 'geometry_paging_report.py')
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class PagingReportTests(unittest.TestCase):
    def test_weighted_windows_and_sampled_gauges(self):
        result = report.summarize('''noise
[geometry] paging_stage name=decode_page count=2 total_ms=10 mean_ms=5 max_ms=7
[geometry] paging_io requests=2 hits=0 reads=1 bytes=100 cpu_payload_bytes=100
[geometry] paging_queue reads=8 completions=2 prepared=1 uploads=3 published=2 reservation_stalls=4
[geometry] paging_stage name=decode_page count=8 total_ms=8 mean_ms=1 max_ms=1
[geometry] paging_io requests=8 hits=1 reads=2 bytes=200 cpu_payload_bytes=50
[geometry] paging_queue reads=4 completions=3 prepared=0 uploads=2 published=8 reservation_stalls=2
''')
        self.assertEqual(result['windows'], 2)
        self.assertEqual(result['stages']['decode_page']['mean_ms'], 1.8)
        self.assertEqual(result['stages']['decode_page']['max_ms'], 7)
        self.assertEqual(result['io']['bytes'], 300)
        self.assertEqual(result['io']['cpu_payload_peak'], 100)
        self.assertEqual(result['queues']['reads_sampled_peak'], 8)
        self.assertEqual(result['queues']['published'], 10)
        self.assertEqual(result['queues']['reservation_stalls'], 6)

    def test_bank_gauges_are_not_summed(self):
        result = report.summarize("""[geometry] paging_bank capacity=1024 occupied=768 largest_free=256 backing_allocations=1
[geometry] paging_bank capacity=1024 occupied=128 largest_free=896 backing_allocations=1
""")
        self.assertEqual(result['bank']['capacity_peak'], 1024)
        self.assertEqual(result['bank']['occupied_peak'], 768)
        self.assertEqual(result['bank']['occupied_last'], 128)
        self.assertEqual(result['bank']['largest_free_last'], 896)
        self.assertEqual(result['bank']['backing_allocations_peak'], 1)

    def test_prefetch_and_decode_queue(self):
        result = report.summarize("""[geometry] paging_io requests=5 hits=4 reads=1 bytes=4096 cpu_payload_bytes=4096 prefetched=12
[geometry] paging_queue reads=0 decode_queue=7 completions=2
[geometry] paging_queue reads=0 decode_queue=3 completions=1
""")
        self.assertEqual(result['io']['prefetched'], 12)
        self.assertEqual(result['queues']['decode_queue_sampled_peak'], 7)

    def test_empty_stage_and_unrelated_log(self):
        result = report.summarize('[geometry] paging_stage name=cache_open count=0 total_ms=0 mean_ms=0 max_ms=0\nSTATS,unrelated,3')
        self.assertEqual(result['stages']['cache_open']['mean_ms'], 0)
        self.assertEqual(result['windows'], 0)


if __name__ == '__main__':
    unittest.main()
