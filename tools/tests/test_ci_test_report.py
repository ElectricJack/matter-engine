import importlib.util
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('ci_report', Path(__file__).parents[1] / 'ci-test-report.py')
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class TestCiReport(unittest.TestCase):
    def test_failure_and_slow_test_are_retained(self):
        with tempfile.TemporaryDirectory() as tmp:
            junit = Path(tmp) / 'junit.xml'
            junit.write_text('<testsuite><testcase name="slow" time="61"><failure/></testcase>'
                             '<testcase name="quick" time="0.5"/></testsuite>')
            inventory = {'tests': [{'name': 'slow', 'properties': [{'name': 'LABELS', 'value': ['ci-full']}]}]}
            result = report.summarize(junit, inventory)
            self.assertEqual(result['tests'], 2)
            self.assertEqual(result['failures'], 1)
            self.assertEqual(result['over_60_seconds'], [result['slowest_20'][0]])
            self.assertEqual(result['slowest_20'][0]['labels'], ['ci-full'])

    def test_error_and_skip_are_visible(self):
        with tempfile.TemporaryDirectory() as tmp:
            junit = Path(tmp) / 'junit.xml'
            junit.write_text('<testsuite><testcase name="broken"><error/></testcase>'
                             '<testcase name="unsupported"><skipped/></testcase></testsuite>')
            result = report.summarize(junit, {'tests': []})
            self.assertEqual(result['failures'], 1)
            self.assertEqual(result['skipped'], 1)


if __name__ == '__main__':
    unittest.main()
