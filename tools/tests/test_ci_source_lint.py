import importlib.util
import copy
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

    def fixture_inventory(self):
        manifest = {'source_checks': [{'name': 'negative', 'command_suffix': 'negative.py',
                                     'required_labels': ['compiler-policy']}],
                    'behavioral_policy_checks': [{'name': 'runtime'}]}
        inventory = {'tests': [
            {'name': 'negative', 'command': ['python', '/synthetic/negative.py'],
             'properties': [{'name': 'LABELS', 'value': ['ci-source-lint', 'compiler-policy']}]},
            {'name': 'runtime', 'command': ['/synthetic/runtime'],
             'properties': [{'name': 'LABELS', 'value': ['ci-pr', 'ci-full']}]}]}
        return inventory, manifest

    def test_mandatory_qualifier_failure_is_not_a_green_summary(self):
        inventory, manifest = self.fixture_inventory()
        runner = Mock(return_value=subprocess.CompletedProcess(['fixture'], 1, stdout='negative probe failed'))
        result = lint.qualify(inventory, manifest, runner)
        self.assertEqual(result['status'], 'failed')
        self.assertEqual(result['checks'][0]['process_exit_code'], 1)
        self.assertEqual(result['expected_checks'], ['negative'])

    def test_disappearing_or_substituted_qualifier_fails_before_execution(self):
        for mutation in ['missing', 'duplicate', 'unlabeled', 'substituted', 'runtime_moved', 'extra']:
            inventory, manifest = self.fixture_inventory()
            if mutation == 'missing':
                inventory['tests'].pop(0)
            elif mutation == 'duplicate':
                inventory['tests'].append(copy.deepcopy(inventory['tests'][0]))
            elif mutation == 'unlabeled':
                inventory['tests'][0]['properties'][0]['value'] = ['compiler-policy']
            elif mutation == 'substituted':
                inventory['tests'][0]['command'] = ['python', '/synthetic/unreviewed.py']
            elif mutation == 'runtime_moved':
                inventory['tests'][1]['properties'][0]['value'] = ['ci-source-lint']
            else:
                inventory['tests'].append({'name': 'extra', 'command': ['extra'], 'properties': [
                    {'name': 'LABELS', 'value': ['ci-source-lint']}]})
            runner = Mock()
            result = lint.qualify(inventory, manifest, runner)
            self.assertEqual(result['status'], 'failed', mutation)
            self.assertTrue(result['validation_errors'], mutation)
            runner.assert_not_called()


if __name__ == '__main__':
    unittest.main()
