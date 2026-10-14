import importlib.util
from pathlib import Path
import subprocess
import unittest
from unittest.mock import Mock

spec = importlib.util.spec_from_file_location('ci_source_lint', Path(__file__).parents[1] / 'ci-source-lint.py')
lint = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lint)


class TestSourceLint(unittest.TestCase):
    def check(self, output, code=0, **properties):
        runner = Mock(return_value=subprocess.CompletedProcess(['fixture'], code, stdout=output))
        result = lint.run_check({'name': 'fixture', 'command': ['fixture'], 'properties': [
            {'name': key, 'value': value} for key, value in properties.items()]}, runner=runner)
        return result, runner

    def test_success_markers_and_failures_are_preserved(self):
        row, _ = self.check('ALL PASS', PASS_REGULAR_EXPRESSION=['ALL PASS'])
        self.assertEqual(row['exit_code'], 0)
        row, _ = self.check('no marker', PASS_REGULAR_EXPRESSION=['ALL PASS'])
        self.assertEqual(row['exit_code'], 1)
        row, _ = self.check('ALL PASS\nvalidation errors: 2',
                            FAIL_REGULAR_EXPRESSION=['validation errors: [1-9][0-9]*'])
        self.assertEqual(row['exit_code'], 1)
        row, _ = self.check('ALL PASS', code=1)
        self.assertEqual(row['exit_code'], 1)

    def test_environment_and_expected_negative_compile(self):
        row, runner = self.check('expected failure', code=1, WILL_FAIL=True,
                                  ENVIRONMENT=['SYNTHETIC_FLAG=a=b'], TIMEOUT=5)
        self.assertEqual(row['exit_code'], 0)
        self.assertEqual(runner.call_args.kwargs['env']['SYNTHETIC_FLAG'], 'a=b')
        self.assertEqual(runner.call_args.kwargs['timeout'], 5)

    def test_missing_command_is_a_failure(self):
        self.assertEqual(lint.run_check({'name': 'unbuilt'})['exit_code'], 1)


if __name__ == '__main__':
    unittest.main()
