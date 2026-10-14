"""Run the portable Linux CI job and retain wall time and failure evidence.

MemoryLib's current phony test target compiles and runs together. Its duration is
reported as build+test rather than pretending that a warm build was a no-op.
"""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    root = Path(__file__).resolve().parents[1]
    evidence = root / 'artifacts/ci-linux'
    evidence.mkdir(parents=True, exist_ok=True)
    cores = len(os.sched_getaffinity(0))
    # This job comprises the three portable foundation suites from the existing
    # reusable-workflow proposal. Do not call it the entire native CTest suite.
    suites = {
        'MemoryLib': [('build_and_test', ['make', '-C', 'libs/MemoryLib', '-j1', 'test'])],
        'MeshChartingLib': [('build', ['make', '-C', 'libs/MeshChartingLib/tests', '-j1', 'build/mesh_charting_tests']),
                           ('test', ['./libs/MeshChartingLib/tests/build/mesh_charting_tests'])],
        'AssetStoreLib': [('build', ['make', '-C', 'libs/AssetStoreLib', '-j1']),
                          ('build', ['make', '-C', 'libs/AssetStoreLib/tests', '-j1', 'build/asset_store_tests']),
                          ('test', ['./libs/AssetStoreLib/tests/build/asset_store_tests'])],
    }
    sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    dirty = bool(subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=root))
    script_sha = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    jobs = min(2, cores, len(suites))
    started = time.perf_counter()

    def run_suite(item):
        name, commands = item
        rows = []
        with (evidence / f'{name}.log').open('w') as log:
            for phase, command in commands:
                before = time.perf_counter()
                print(f'{name}: {phase}', flush=True)
                result = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT)
                rows.append({'phase': phase, 'command': command, 'seconds': time.perf_counter() - before,
                             'exit_code': result.returncode})
                if result.returncode:
                    break
        return {'name': name, 'phases': rows, 'passed': all(row['exit_code'] == 0 for row in rows),
                'seconds': sum(row['seconds'] for row in rows)}

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        rows = list(pool.map(run_suite, suites.items()))
    elapsed = time.perf_counter() - started
    summary = {'schema': 'matter.linux-ci-run.v1', 'source_sha': sha, 'source_dirty': dirty,
               'ci_script_sha256': script_sha,
               'hosted': os.environ.get('GITHUB_ACTIONS') == 'true', 'parallelism': jobs,
               'scope': 'portable-foundations', 'suites': rows, 'slowest_20': sorted(rows, key=lambda r: -r['seconds']),
               'total_seconds': elapsed, 'build_and_test_under_60_seconds': elapsed < 60,
               'status': 'passed' if all(row['passed'] for row in rows) else 'failed'}
    (evidence / 'run-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0 if summary['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
