"""Run source/build contracts as a separate lint phase, outside behavioral CTest.

Use the configured command inventory so no duplicated hand-maintained command
list can silently drop a historical regression check. Run serially because the
shader fixtures and editor census mutate their own generated build inputs.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import time


def run_check(check, runner=subprocess.run):
    properties = {p['name']: p['value'] for p in check.get('properties', [])}
    before = time.perf_counter()
    env = os.environ.copy()
    for assignment in properties.get('ENVIRONMENT', []):
        key, value = assignment.split('=', 1)
        env[key] = value
    command = check.get('command')
    row = {'name': check['name'], 'exit_code': 1}
    if not command or properties.get('DISABLED'):
        row['error'] = 'Configured lint command is unavailable or disabled'
        row['seconds'] = time.perf_counter() - before
        return row
    try:
        result = runner(command, cwd=properties.get('WORKING_DIRECTORY'), env=env,
                        timeout=properties.get('TIMEOUT'), stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True, errors='replace')
        print(result.stdout, end='', flush=True)
        passed = result.returncode != 0 if properties.get('WILL_FAIL') else result.returncode == 0
        required = properties.get('PASS_REGULAR_EXPRESSION', [])
        forbidden = properties.get('FAIL_REGULAR_EXPRESSION', [])
        if required:
            passed = passed and any(re.search(pattern, result.stdout) for pattern in required)
        if any(re.search(pattern, result.stdout) for pattern in forbidden):
            passed = False
        row['exit_code'] = 0 if passed else 1
        row['process_exit_code'] = result.returncode
    except (OSError, subprocess.TimeoutExpired) as error:
        row['error'] = str(error)
    row['seconds'] = time.perf_counter() - before
    return row


def select_checks(inventory, manifest):
    """Fail closed on missing, renamed, relabeled or substituted qualifiers."""
    tests = {test['name']: test for test in inventory['tests']}
    counts = Counter(test['name'] for test in inventory['tests'])
    expected = {check['name'] for check in manifest['source_checks']}
    errors = [f'duplicate inventory name: {name}' for name, count in counts.items() if count != 1]
    selected = []
    for contract in manifest['source_checks']:
        name = contract['name']
        test = tests.get(name)
        if test is None:
            errors.append(f'missing mandatory qualifier: {name}')
            continue
        properties = {p['name']: p['value'] for p in test.get('properties', [])}
        labels = set(properties.get('LABELS', []))
        if not set(contract['required_labels'] + ['ci-source-lint']) <= labels:
            errors.append(f'missing qualifier labels: {name}')
        if labels & {'ci-pr', 'ci-full', 'ci-fast'}:
            errors.append(f'qualifier also selected as behavioral CTest: {name}')
        command = test.get('command', [])
        suffix = contract['command_suffix'].lower()
        if not any(str(arg).replace('\\', '/').lower().endswith('/' + suffix)
                   or str(arg).replace('\\', '/').lower() == suffix for arg in command):
            errors.append(f'missing expected qualifier command: {name}')
        selected.append(test)
    for name, test in tests.items():
        properties = {p['name']: p['value'] for p in test.get('properties', [])}
        if 'ci-source-lint' in properties.get('LABELS', []) and name not in expected:
            errors.append(f'unreviewed qualifier: {name}')
    for contract in manifest['behavioral_policy_checks']:
        name = contract['name']
        properties = {p['name']: p['value'] for p in tests.get(name, {}).get('properties', [])}
        labels = set(properties.get('LABELS', []))
        if not {'ci-pr', 'ci-full'} <= labels or 'ci-source-lint' in labels:
            errors.append(f'behavioral policy regression absent from runtime inventory: {name}')
        if not tests.get(name, {}).get('command') or properties.get('DISABLED'):
            errors.append(f'behavioral policy command unavailable: {name}')
    return selected, errors


def qualify(inventory, manifest, runner=subprocess.run):
    start = time.perf_counter()
    selected, errors = select_checks(inventory, manifest)
    rows = []
    # Do not execute an unreviewed/substituted command. Retain the complete
    # inventory and explicit validation failures for the enclosing CI outcome.
    if not errors:
        for check in selected:
            print('LINT:', check['name'], flush=True)
            rows.append(run_check(check, runner))
    return {'schema': 'matter.source-lint.v1', 'checks': rows,
            'expected_checks': [check['name'] for check in manifest['source_checks']],
            'validation_errors': errors, 'seconds': time.perf_counter() - start,
            'status': 'passed' if not errors and rows and all(r['exit_code'] == 0 for r in rows) else 'failed'}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--inventory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, default=Path(__file__).parents[1] / 'cmake/ci-checks.json')
    args = parser.parse_args()
    inventory = json.loads(args.inventory.read_text(encoding='utf-8-sig'))
    manifest_bytes = args.manifest.read_bytes()
    summary = qualify(inventory, json.loads(manifest_bytes))
    summary['manifest_sha256'] = hashlib.sha256(manifest_bytes).hexdigest()
    for error in summary['validation_errors']:
        print('LINT INVENTORY FAILURE:', error, flush=True)
    args.output.write_text(json.dumps(summary, indent=2) + '\n')
    return 0 if summary['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
