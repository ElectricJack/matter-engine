import concurrent.futures
import json
from pathlib import Path
import re
import shlex
import subprocess

repo = Path('/home/jkern/dev/matter-engine-cpp/.aq/worktrees/slot-2')
out = Path('/tmp/fleet-rapids-48-epoch18')
component = repo / 'libs/MatterGraphics'
commands = {}
for build in ('graphics', 'native'):
    for entry in json.loads((out / build / 'compile_commands.json').read_text()):
        commands[entry['file']] = shlex.split(entry['command'])
includes = [
    'MatterEngine3', 'MatterEngine3/include', 'MatterEngine3/src',
    'MatterEngine3/src/render', 'libs/MatterSurfaceLib/include',
    'libs/MatterSurfaceLib/src', 'libs/SpatialQueryLib/include',
    'libs/MemoryLib/include', 'libs/MathLib/include', 'libs/ProfileLib/include',
    'libs/ParticleFlowLib/include', 'libs/MeshChartingLib/include',
    'third_party/quickjs-ng', 'third_party/raylib/src', 'third_party/box3d/include',
    'third_party/bc7enc', 'third_party/flecs', 'third_party/ozz-animation/include',
    'third_party/Vulkan-Headers/include', 'libs/MatterGraphics/src/native',
    'libs/MatterGraphics/include', 'libs/MatterContracts/include',
]
fallback = ['c++', '-std=c++17', '-DMATTER_HAVE_STREAMLINE=0',
            '-DNOMINMAX', '-DMATTER_PROFILE_ENABLED=0']
fallback += ['-I' + str(repo / p) for p in includes]
fallback += ['-I' + str(out / 'graphics')]
sources = sorted(component.glob('src/**/*.cpp'))
sources.append(repo / 'MatterEngine3/src/render/raster_mesh.cpp')

def audit(item):
    label, source = item
    args = commands.get(str(source), fallback).copy()
    filtered = []
    skip = False
    for arg in args:
        if skip:
            skip = False
            continue
        if arg == '-o':
            skip = True
            continue
        if arg in ('-c', str(source)) or arg.startswith(('-Wno-', '-Werror')):
            continue
        filtered.append(arg)
    actual_source = source
    if label == 'before':
        for i, arg in enumerate(filtered):
            if arg.startswith('-I' + str(repo)):
                candidate = out / 'before' / Path(arg[2:]).relative_to(repo)
                if candidate.exists():
                    filtered[i] = '-I' + str(candidate)
        actual_source = out / 'before' / source.relative_to(repo)
    filtered += ['-Wall', '-Wextra', '-fsyntax-only', str(actual_source)]
    result = subprocess.run(filtered, cwd=repo, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    rel = source.relative_to(repo).as_posix()
    name = rel.replace('/', '__') + '.log'
    log = out / ('warnings-' + label) / name
    log.parent.mkdir(exist_ok=True)
    log.write_text(shlex.join(filtered) + '\n' + result.stdout)
    return dict(source=rel, variant=label, exit_code=result.returncode,
                warnings=len(re.findall(r'\bwarning:', result.stdout)),
                errors=len(re.findall(r'\berror:', result.stdout)), log=str(log))

with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
    results = list(pool.map(audit, [(label, source) for label in ('before', 'after')
                                    for source in sources]))
(out / 'warnings.json').write_text(json.dumps(results, indent=2) + '\n')
for label in ('before', 'after'):
    rows = [r for r in results if r['variant'] == label]
    print(label, len(rows), 'sources;', sum(r['warnings'] for r in rows),
          'warnings;', sum(r['errors'] for r in rows), 'errors', flush=True)
    for row in rows:
        if row['warnings'] or row['errors']:
            print(row, flush=True)
