"""Run the component build matrix and the dependency/ownership gates.

Every component configures, builds and runs its own tests alone from its own
directory, then the rule gates run: the source manifests, the dependency
checker (static, duplicates, configure) and its fixture suite. The WASM
conformance gate is the pinned geometry binding smoke with the native C
consumer as its equivalence oracle; it needs an Emscripten SDK, so without one
it is NOT RUN (never a pass) and `--require wasm` turns that into a failure.
The full native editor and package stay with tools/ci-windows.ps1.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

# name, source directory, extra configure arguments
CONFIGURATIONS = (
    ('contracts', 'libs/MatterContracts', []),
    ('geometry-headless', 'libs/ProceduralGeometry', []),
    ('graphics-only', 'libs/MatterGraphics', []),
    ('terrain-only', 'libs/TerrainGeneration', []),
    ('build', 'libs/ProceduralBuild', []),
    ('asset-pipeline', 'libs/AssetPipeline', []),
    ('geometry-c', 'bindings/geometry-c', []),
    ('host-quickjs', 'MatterEngine3/src/script/portable_host', []),
    ('authoring-adapter', 'MatterEngine3/src/component_adapters/authoring', []),
    ('terrain-coordinator', 'MatterEngine3/src/component_adapters/terrain_coordinator', []),
)
GATES = ('source-manifests', 'dependency-rules', 'dependency-fixtures', 'wasm')


def main():
    root = Path(__file__).resolve().parents[1]
    names = [name for name, _, _ in CONFIGURATIONS] + list(GATES)
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--only', action='append', choices=names, help='run only this row (repeatable)')
    parser.add_argument('--require', action='append', default=[], choices=['wasm'],
                        help='fail, rather than report NOT RUN, when this gate cannot run here')
    parser.add_argument('--jobs', type=int, default=min(4, len(os.sched_getaffinity(0))), help='build parallelism')
    parser.add_argument('--emsdk', type=Path, default=os.environ.get('EMSDK'))
    parser.add_argument('--out', type=Path, default=root / 'artifacts/ci-components')
    args = parser.parse_args()
    selected = set(args.only or names)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    rows = []

    def run(name, commands, skipped=None):
        if name not in selected:
            return
        if skipped:
            status = 'failed' if name in args.require else 'not_run'
            rows.append({'name': name, 'status': status, 'reason': skipped})
            print(f'{name}: {status.upper()} ({skipped})', flush=True)
            return
        phases = []
        with (out / f'{name}.log').open('w') as log:
            for phase, command in commands:
                before = time.perf_counter()
                print(f'{name}: {phase}', flush=True)
                log.write(f'$ {" ".join(map(str, command))}\n')
                log.flush()
                result = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT)
                phases.append({'phase': phase, 'command': list(map(str, command)), 'exit_code': result.returncode,
                               'seconds': time.perf_counter() - before})
                if result.returncode:
                    break
        status = 'passed' if all(row['exit_code'] == 0 for row in phases) else 'failed'
        rows.append({'name': name, 'status': status, 'phases': phases})
        print(f'{name}: {status.upper()}', flush=True)

    jobs = str(args.jobs)
    for name, source, extra in CONFIGURATIONS:
        build = out / 'build' / name
        run(name, [('configure', ['cmake', '-S', source, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', *extra]),
                   ('build', ['cmake', '--build', build, '--parallel', jobs]),
                   ('test', ['ctest', '--test-dir', build, '--output-on-failure', '--no-tests=error', '--parallel', '1'])])
    run('source-manifests', [('check', ['python3', 'tools/check-source-manifests.py', '--root', '.'])])
    run('dependency-rules', [('check', ['python3', 'tools/component-separation/check_dependencies.py',
                                        '--checks', 'static,duplicates,configure', '--jobs', jobs,
                                        '--report', out / 'component-deps.json'])])
    run('dependency-fixtures', [('test', ['python3', 'tests/component-separation/test_dependency_checks.py'])])
    native = out / 'build/geometry-c/matter_geometry_c_tests'
    wasm_build = out / 'build/wasm'
    if 'wasm' in selected and wasm_build.exists():
        shutil.rmtree(wasm_build)  # build-smoke.py requires an empty directory and deletes nothing itself
    missing = None
    if args.emsdk is None or not (args.emsdk / 'upstream/emscripten').is_dir():
        missing = 'no Emscripten SDK (pass --emsdk or set EMSDK)'
    elif not native.is_file():
        missing = 'the geometry-c row did not produce the native consumer oracle'
    run('wasm', [('smoke', ['python3', 'bindings/geometry-c/wasm/build-smoke.py', '--emsdk', args.emsdk or '',
                            '--build-dir', wasm_build, '--native-consumer', native])], skipped=missing)

    failed = [row['name'] for row in rows if row['status'] == 'failed']
    summary = {'schema': 'matter.component-matrix-run.v1',
               'source_sha': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
               'jobs': args.jobs, 'required': args.require, 'rows': rows,
               'not_run': [row['name'] for row in rows if row['status'] == 'not_run'], 'failed': failed,
               'status': 'failed' if failed else 'passed'}
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({key: summary[key] for key in ('status', 'failed', 'not_run')}))
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
