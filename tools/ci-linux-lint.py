"""Mandatory portable build/source qualification, outside behavioral tests.

The GNU probes use the existing engine compiler contract fixtures without
building the engine. Preserve the expected diagnostic, not just any failure.
"""
import json
from pathlib import Path
import re
import subprocess
import time


def main():
    root = Path(__file__).resolve().parents[1]
    evidence = root / 'artifacts/ci-linux'
    build = evidence / 'compiler-probes'
    build.mkdir(parents=True, exist_ok=True)
    rows = []
    started = time.perf_counter()

    def run(name, command, negative_format=False):
        before = time.perf_counter()
        try:
            result = subprocess.run(command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, errors='replace')
        except OSError as error:
            result = subprocess.CompletedProcess(command, 127, stdout=str(error))
        (evidence / f'{name}.log').write_text(result.stdout)
        passed = result.returncode == 0
        if negative_format:
            passed = result.returncode != 0 and bool(re.search(r'\[-Werror=format(?:=)?\]', result.stdout))
        row = {'name': name, 'command': command, 'process_exit_code': result.returncode,
               'passed': passed, 'seconds': time.perf_counter() - before}
        rows.append(row)
        print(f'{name}: {"PASS" if passed else "FAIL"}', flush=True)
        return passed

    run('compiler_format_negative_tests', ['g++', '-std=c++17', '-Werror=format',
        '-IMatterEngine3/include', '-c', 'MatterEngine3/tests/compiler_format_negative.cpp',
        '-o', str(build / 'format-negative.o')], negative_format=True)
    probe = build / 'return-address'
    if run('compiler_return_address_gnu_build', ['g++', '-std=c++17', '-O0', '-IMatterEngine3/include',
            'MatterEngine3/tests/compiler_return_address_gnu_tests.cpp', '-o', str(probe)]):
        run('compiler_return_address_gnu_tests', [str(probe)])
    for name, script in (
            ('ci_test_report_tests', 'tools/tests/test_ci_test_report.py'),
            ('ci_source_lint_tests', 'tools/tests/test_ci_source_lint.py'),
            ('glslc_depfile_tests', 'tools/tests/test_glslc_depfile.py'),
            ('shader_input_list_tests', 'MatterEngine3/tools/tests/test_embed_spirv.py')):
        run(name, ['python3', script])
    summary = {'schema': 'matter.linux-source-lint.v1', 'checks': rows,
               'seconds': time.perf_counter() - started,
               'status': 'passed' if all(row['passed'] for row in rows) else 'failed'}
    (evidence / 'source-lint.json').write_text(json.dumps(summary, indent=2) + '\n')
    return 0 if summary['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
