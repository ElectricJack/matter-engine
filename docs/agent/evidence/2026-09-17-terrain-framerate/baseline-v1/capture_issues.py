"""Matched terrain-only issue cameras; POM-off is a diagnostic, not acceptance."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import statistics
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[4]
OUT = Path(__file__).resolve().parent
label = sys.argv[1]
assert re.fullmatch(r'[a-z0-9-]+', label)
dest = OUT / label
native = Path('/mnt/c/tmp') / ('matter-terrain-' + label)
assert not dest.exists() and not native.exists()
dest.mkdir(parents=True)
win = 'C:/tmp/matter-terrain-' + label
win_repo = 'D:/Shared With Desktop/AI/matter-engine-cpp/'
exe = ROOT / 'MatterEditor/build/windows-msvc/editor.exe'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
props = ROOT / 'projects/world_demo/scenes/streaming/StreamMountain/props.json'
original = props.read_bytes()
(dest / 'original-props.json').write_bytes(original)
cases = [('valley', 'f9ee4cb7-dafa-191e-91a0-29c1e222b823'),
         ('cliff', '14c876bc-00fd-6b9c-e50f-5fd8991afbe9')]
states = [json.loads((ROOT / 'issues' / guid / 'state.json').read_text()) for _, guid in cases]
groups = {k: v for k, v in states[0]['props'].items()
          if k not in ('viewer.session', 'viewer.atmosphere_status')}
groups['render.pom']['enabled'] = False
groups['render.gpu']['dlss_mode'] = 0
setup = {'version': 1, 'groups': groups}
(dest / 'benchmark-props.json').write_text(json.dumps(setup, indent=2) + '\n')
timeline = ['render_path raster', 'set render.gi.enabled false',
            'set render.pom.enabled false', 'dlss native', 'wait_event bake.finished 300']
shots = []
for (view, guid), state in zip(cases, states):
    camera = state['shots'][0]['camera']
    pose = ' '.join(map(str, camera['eye'] + camera['target']))
    timeline += ['set render.pom.enabled false', 'cam ' + pose, 'wait_idle 6 120', 'wait_frames 180']
    for mode in ('flat', 'pom'):
        tag = view + '-' + mode
        timeline += ['set render.pom.enabled ' + ('true' if mode == 'pom' else 'false'), 'wait_frames 90']
        for i in range(100):
            timeline += ['stats ' + tag + '-' + str(i), 'wait_frames 3']
        shots.append(tag + '.png')
        timeline += ['shot ' + win + '/' + shots[-1]]
timeline += ['quit']
(dest / 'timeline.txt').write_text('\n'.join(timeline) + '\n')
camera = states[0]['shots'][0]['camera']
pose = ','.join(map(str, camera['eye'] + camera['target']))
command = ('Get-ChildItem Env:MATTER_* | Remove-Item; & py -3 MatterEngine3/tools/drive.py '
           f'--world StreamMountain --timeline "{win_repo}{(dest / "timeline.txt").relative_to(ROOT)}" '
           f'--out-dir "{win}" --editor "{win_repo}MatterEditor/build/windows-msvc/editor.exe" '
           '--timeout 900 --hide-ui --env MATTER_HIDE_WINDOW=1 --env MATTER_VK_VALIDATION=0 '
           f'--env MATTER_WINDOW_WIDTH=2796 --env MATTER_WINDOW_HEIGHT=1044 --env MATTER_CAM={pose} '
           f'--env MATTER_VT_TRACE={win}/vt-trace.jsonl; exit $LASTEXITCODE')
(dest / 'launch.ps1').write_text(command + '\n')
binary = sha(exe)
source_hashes = {str(p.relative_to(ROOT)): sha(p) for p in (ROOT / 'MatterEngine3/shaders_vk').glob('*') if p.is_file()}
manifest = {'binary_sha256': binary, 'shader_sources': source_hashes,
            'issues': cases, 'validation': False, 'output': [2796, 1044],
            'scope': 'Raster terrain-only issue cameras and properties; no UI, no resolution reduction. POM-off is diagnostic only.'}
(dest / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
start = time.monotonic()
try:
    props.write_text(json.dumps(setup) + '\n')
    with (dest / 'driver.log').open('w') as log:
        code = subprocess.call(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe',
                                '-NoProfile', '-Command', command], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
finally:
    (dest / 'saved-props.json').write_bytes(props.read_bytes())
    props.write_bytes(original)
if native.exists():
    shutil.copytree(native, dest, dirs_exist_ok=True)
log = (dest / 'log.txt').read_text(errors='replace') if (dest / 'log.txt').exists() else ''
samples = {}
for line in log.splitlines():
    if not line.startswith('STATS,'):
        continue
    fields = line.split(',')
    tag = fields[1].rsplit('-', 1)[0]
    samples.setdefault(tag, []).append([float(f) for f in fields[2:]])
summary = {}
for tag, rows in samples.items():
    summary[tag] = {'samples': len(rows)}
    for name, index in [('frame_ms', 0), ('resolve_ms', 1), ('build_ms', 2), ('draw_ms', 3),
                        ('gpu_total_ms', -3), ('gpu_cull_ms', -2), ('gpu_gbuffer_ms', -1)]:
        values = sorted(row[index] for row in rows)
        summary[tag][name] = {'median': statistics.median(values), 'p95': values[int(.95 * (len(values)-1))]}
audit = {'exit': code, 'seconds': time.monotonic() - start, 'summary': summary,
         'binary_unchanged': sha(exe) == binary,
         'shader_changes': [p for p, h in source_hashes.items() if sha(ROOT / p) != h],
         'missing_shots': [s for s in shots if not (dest / (s + '.done')).exists()],
         'wrong_dimensions': [s for s in shots if (dest / s).exists() and struct.unpack('>II', (dest / s).read_bytes()[16:24]) != (2796, 1044)],
         'errors': [line for line in log.splitlines() if re.search(r'VK_ERROR_DEVICE_LOST|Validation Error|VUID-|set:.*(?:unknown|invalid|failed)', line)],
         'connection_warning_count': log.count('[vt-surface-links]'),
         'acceptance_passed': False}
(dest / 'audit.json').write_text(json.dumps(audit, indent=2) + '\n')
print(json.dumps(audit, indent=2), flush=True)
raise SystemExit(code or bool(audit['missing_shots'] or audit['errors'] or audit['wrong_dimensions']))
