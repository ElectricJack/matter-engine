"""Run source/build contracts as a separate lint phase, outside behavioral CTest.

Use the configured command inventory so no duplicated hand-maintained command
list can silently drop a historical regression check. Run serially because the
shader fixtures and editor census mutate their own generated build inputs.
"""
import argparse
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--inventory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    inventory = json.loads(args.inventory.read_text(encoding='utf-8-sig'))
    rows = []
    start = time.perf_counter()
    for check in inventory['tests']:
        properties = {p['name']: p['value'] for p in check.get('properties', [])}
        if 'ci-source-lint' not in properties.get('LABELS', []):
            continue
        print('LINT:', check['name'], flush=True)
        rows.append(run_check(check))
    summary = {'schema': 'matter.source-lint.v1', 'checks': rows,
               'seconds': time.perf_counter() - start,
               'status': 'passed' if rows and all(r['exit_code'] == 0 for r in rows) else 'failed'}
    args.output.write_text(json.dumps(summary, indent=2) + '\n')
    return 0 if summary['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
