import importlib.util
import io
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('ci_report', Path(__file__).parents[1] / 'ci-test-report.py')
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class TestCiReport(unittest.TestCase):
    def test_failure_and_slow_test_are_retained(self):
        junit = io.StringIO('<testsuite><testcase name="slow" time="61"><failure/></testcase>'
                            '<testcase name="quick" time="0.5"/></testsuite>')
        inventory = {'tests': [{'name': 'slow', 'properties': [{'name': 'LABELS', 'value': ['ci-full']}]}]}
        result = report.summarize(junit, inventory)
        self.assertEqual(result['tests'], 2)
        self.assertEqual(result['failures'], 1)
        self.assertEqual(result['over_60_seconds'], [result['slowest_20'][0]])
        self.assertEqual(result['slowest_20'][0]['labels'], ['ci-full'])

    def test_error_and_skip_are_visible(self):
        junit = io.StringIO('<testsuite><testcase name="broken"><error/></testcase>'
                            '<testcase name="unsupported"><skipped/></testcase></testsuite>')
        result = report.summarize(junit, {'tests': []})
        self.assertEqual(result['failures'], 1)
        self.assertEqual(result['skipped'], 1)

    def test_complete_inventory_and_serial_resource_floor(self):
        inventory = {'tests': [
            {'name': name, 'properties': [{'name': 'LABELS', 'value': ['ci-full']},
                                         {'name': 'RESOURCE_LOCK', 'value': ['gpu']}]}
            for name in ['a', 'b', 'missing']]}
        junit = io.StringIO('<testsuite><testcase name="a" time="30"/>'
                            '<testcase name="b" time="40"/><testcase name="b" time="1"/>'
                            '<testcase name="extra" time="2"/></testsuite>')
        result = report.summarize(junit, inventory, label='ci-full', parallelism=8)
        self.assertEqual(result['missing_tests'], ['missing'])
        self.assertEqual(result['unexpected_tests'], ['extra'])
        self.assertEqual(result['duplicate_tests'], ['b'])
        self.assertEqual(result['measured_parallel_floor_seconds'], 71)
        self.assertEqual(len(result['all_tests']), 4)

    def test_invalid_timing_cannot_be_evidence(self):
        for value in ['-1', 'nan', 'inf']:
            with self.assertRaises(ValueError):
                report.summarize(io.StringIO(f'<testsuite><testcase name="a" time="{value}"/></testsuite>'), {'tests': []})


if __name__ == '__main__':
    unittest.main()
