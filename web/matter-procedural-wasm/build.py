#!/usr/bin/env python3
"""Build the standalone Linux WASM package using the shared pinned compiler profile."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emsdk', type=Path, default=os.environ.get('EMSDK'))
    parser.add_argument('--build-dir', type=Path, default=HERE / 'build')
    args = parser.parse_args()
    if args.emsdk is None:
        parser.error('pass --emsdk /path/to/emsdk or activate EMSDK')
    sdk, build = args.emsdk.resolve(), args.build_dir.resolve()
    lock_path = ROOT / 'bindings/geometry-c/wasm/toolchain.json'
    lock = json.loads(lock_path.read_text())
    env = os.environ.copy()
    env.update(EMSDK=str(sdk), EM_CONFIG=str(sdk / '.emscripten'))
    env['PATH'] = os.pathsep.join([str(sdk / 'upstream/emscripten'),
        str(sdk / 'node' / f"{lock['node']}_64bit/bin"), env.get('PATH', '')])
    versions = {}
    for tool in ['emcc', 'node', 'cmake', 'ninja', 'tsc']:
        executable = shutil.which(tool, path=env['PATH'])
        if not executable:
            parser.error(f'missing build prerequisite: {tool}')
        versions[tool] = subprocess.check_output([executable, '--version'], env=env, text=True).strip()
    if lock['emscripten_revision'] not in versions['emcc']:
        parser.error(f"activate Emscripten {lock['emscripten']} ({lock['emscripten_revision']})")
    if versions['tsc'] != 'Version 5.9.3':
        parser.error('TypeScript 5.9.3 is required (npm install --global typescript@5.9.3)')
    if versions['node'] != f"v{lock['node']}":
        parser.error(f"SDK Node {lock['node']} is required")
    start = time.monotonic()
    commands = [
        ['cmake', '--preset', 'linux-wasm-release', '-B', str(build)],
        ['cmake', '--build', str(build), '--target', 'matter_geometry_wasm', '--parallel', '2'],
        ['tsc', '-p', str(ROOT / 'integrations/web-procedural/runtime/tsconfig.json'), '--outDir', str(build / 'runtime')],
    ]
    for command in commands:
        print('RUN:', json.dumps(command), flush=True)
        subprocess.run(command, cwd=HERE, env=env, check=True)
    dist = HERE / 'dist'
    dist.mkdir(exist_ok=True)
    for path in (HERE / 'src').glob('*.mjs'):
        shutil.copy2(path, dist / path.name)
    for name in ['matter_geometry_wasm.mjs', 'matter_geometry_wasm.wasm']:
        shutil.copy2(build / 'geometry' / name, dist / name)
    shutil.copy2(ROOT / 'bindings/geometry-c/wasm/geometry.mjs', dist / 'geometry.mjs')
    (dist / 'runtime').mkdir(exist_ok=True)
    for name in ['index.mjs', 'protocol.mjs']:
        shutil.copy2(build / 'runtime' / name, dist / 'runtime' / name)
    (dist / 'authoring').mkdir(exist_ok=True)
    for path in (ROOT / 'packages/matter-authoring').iterdir():
        if path.name.endswith(('.mjs', '.d.mts')):
            shutil.copy2(path, dist / 'authoring' / path.name)
    (dist / 'index.d.ts').write_text((HERE / 'index.d.ts').read_text().replace(
        '../../packages/matter-authoring/index.mjs', './authoring/index.mjs'))
    shutil.copy2(dist / 'index.d.ts', dist / 'index.d.mts')
    # NodeNext must classify index.d.ts as ESM even if dist is copied alone.
    (dist / 'package.json').write_text('{"type":"module"}\n')
    wasm = (dist / 'matter_geometry_wasm.wasm').read_bytes()
    report = {'profile': lock['profile'], 'tools': versions, 'commands': commands,
        'toolchain_lock_sha256': hashlib.sha256(lock_path.read_bytes()).hexdigest(),
        'build_seconds': round(time.monotonic() - start, 3), 'wasm_bytes': len(wasm),
        'wasm_sha256': hashlib.sha256(wasm).hexdigest()}
    (dist / 'build-info.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
