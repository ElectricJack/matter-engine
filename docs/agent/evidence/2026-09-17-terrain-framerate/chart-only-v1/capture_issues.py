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
pom_path = sys.argv[2] if len(sys.argv) > 2 else 'reference'
assert pom_path in ('reference', 'chart_only')
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
shots = [view + '-' + mode + '.png' for view, _ in cases for mode in ('flat', 'pom')]
(dest / 'timeline.txt').write_text('\n'.join(timeline) + '\n')
camera = states[0]['shots'][0]['camera']
pose = ','.join(map(str, camera['eye'] + camera['target']))
command = ('Get-ChildItem Env:MATTER_* | Remove-Item; & py -3 MatterEngine3/tools/drive.py '
           f'--world StreamMountain --timeline "{win_repo}{(dest / "timeline.txt").relative_to(ROOT)}" '
           f'--out-dir "{win}" --editor "{win_repo}MatterEditor/build/windows-msvc/editor.exe" '
           '--timeout 1500 --hide-ui --env MATTER_HIDE_WINDOW=0 --env MATTER_VK_VALIDATION=0 '
           '--env MATTER_PRESENT_MODE=immediate --env MATTER_FRAME_LIMIT=0 '
           f'--env MATTER_GBUFFER_POM_PATH={pom_path} '
           f'--env MATTER_WINDOW_WIDTH=2796 --env MATTER_WINDOW_HEIGHT=1044 --env MATTER_CAM={pose} '
           f'--env MATTER_VT_TRACE={win}/vt-trace.jsonl; exit $LASTEXITCODE')
(dest / 'launch.ps1').write_text(command + '\n')
binary = sha(exe)
source_hashes = {str(p.relative_to(ROOT)): sha(p) for p in (ROOT / 'MatterEngine3/shaders_vk').glob('*') if p.is_file()}
manifest = {'binary_sha256': binary, 'shader_sources': source_hashes,
            'issues': cases, 'validation': False, 'output': [2796, 1044],
            'presentation': 'immediate', 'visible_window': True,
            'pom_path': pom_path,
            'scope': 'Raster terrain-only issue cameras and properties; no UI, no resolution reduction. POM-off is diagnostic only.'}
(dest / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
(dest / 'capture_issues.py').write_bytes(Path(__file__).read_bytes())
start = time.monotonic()
commands, settles = [], []
def send(lines):
    text = '\n'.join(lines) + '\n'
    with (native / 'cmd.txt').open('a') as f:
        f.write(text)
    commands.extend(lines)
    (dest / 'commands.txt').write_text('\n'.join(commands) + '\n')

def begin_phase(index):
    view, mode = phases[index]
    state = states[[c[0] for c in cases].index(view)]
    camera = state['shots'][0]['camera']
    send(['set render.pom.enabled ' + ('true' if mode == 'pom' else 'false'),
          'cam ' + ' '.join(map(str, camera['eye'] + camera['target'])), 'wait_frames 15'])

phases = [(view, mode) for view, _ in cases for mode in ('flat', 'pom')]
index, phase, probe = 0, 'bake', 0
last_census, stable_since, next_probe, phase_start = None, None, start, start
last_probe_rows = 0
try:
    props.write_text(json.dumps(setup) + '\n')
    with (dest / 'driver.log').open('w') as log:
        process = subprocess.Popen(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe',
                                    '-NoProfile', '-Command', command], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        while process.poll() is None:
            now = time.monotonic()
            contents = (native / 'log.txt').read_text(errors='replace') if (native / 'log.txt').exists() else ''
            if phase == 'bake' and 'viewer: bake ready' in contents:
                begin_phase(index)
                phase, phase_start = 'settle', now
            if phase == 'settle':
                if now >= next_probe:
                    probe += 1
                    send([f'stats ready-{index}-{probe}'])
                    next_probe = now + 2
                rows = re.findall(rf'^STATS,ready-{index}-\d+,([^\n]+)', contents, re.M)
                census = re.findall(rf'^STATSVT,ready-{index}-\d+,[^\n]*?variants=(\d+)/[^\n]*?queue=(\d+),fills=(\d+),', contents, re.M)
                if rows and census and min(len(rows), len(census)) > last_probe_rows:
                    last_probe_rows = min(len(rows), len(census))
                    state = (rows[-1].split(',')[4], *census[-1])
                    if state != last_census or int(state[2]) != 0:
                        stable_since = now
                        last_census = state
                    stable = stable_since is not None and now - stable_since >= 12 and int(state[2]) == 0
                    if stable:
                        tag = '-'.join(phases[index])
                        settles.append({'phase': tag, 'seconds': now-phase_start, 'state': state, 'settled': True})
                        print(json.dumps(settles[-1]), flush=True)
                        lines = []
                        for sample in range(100):
                            lines += [f'stats {tag}-{sample}', 'wait_frames 3']
                        lines += ['shot ' + win + '/' + tag + '.png']
                        send(lines)
                        phase = 'capture'
                if phase == 'settle' and now-phase_start > 480:
                    settles.append({'phase': '-'.join(phases[index]), 'seconds': now-phase_start,
                                    'state': last_census, 'settled': False})
                    send(['quit'])
                    phase = 'shutdown'
            if phase == 'capture' and (native / ('-'.join(phases[index]) + '.png.done')).exists():
                index += 1
                if index == len(phases):
                    send(['quit'])
                    phase = 'shutdown'
                else:
                    begin_phase(index)
                    phase, phase_start, last_census, stable_since = 'settle', now, None, None
                    last_probe_rows = 0
            if now-start > 1440 and phase != 'shutdown' and (native / 'cmd.txt').exists():
                send(['quit'])
                phase = 'shutdown'
            time.sleep(1)
        code = process.wait()
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
    if fields[1].startswith('ready-'):
        continue
    tag = fields[1].rsplit('-', 1)[0]
    samples.setdefault(tag, []).append([float(f) for f in fields[2:]])
summary = {}
for tag, rows in samples.items():
    summary[tag] = {'samples': len(rows)}
    for name, index in [('frame_ms', 0), ('resolve_ms', 1), ('build_ms', 2), ('draw_ms', 3),
                        ('gpu_total_ms', -3), ('gpu_cull_ms', -2), ('gpu_gbuffer_ms', -1)]:
        values = sorted(row[index] for row in rows)
        summary[tag][name] = {'median': statistics.median(values), 'p95': values[int(.95 * (len(values)-1))]}
census_audit = {}
for view, mode in phases:
    tag = view + '-' + mode
    rows = [line for line in log.splitlines() if line.startswith('STATSVT,' + tag + '-')]
    counters = [{k: int(v) for k, v in re.findall(r'(variants|rejected|queue|fills|evictions)=(\d+)', line)} for line in rows]
    census_audit[tag] = {'samples': len(rows), 'counters': {
        k: [min(d[k] for d in counters), max(d[k] for d in counters)] for k in counters[0]} if counters else {}}
(dest / 'census-audit.json').write_text(json.dumps(census_audit, indent=2) + '\n')
audit = {'exit': code, 'seconds': time.monotonic() - start, 'summary': summary, 'settles': settles,
         'presentation': re.findall(r'\[vk\] present [^\n]+', log),
         'diagnostic_path': re.findall(r'\[terrain-profile\][^\n]+', log),
         'sample_counts_complete': all(len(samples.get(view + '-' + mode, [])) == 100 for view, mode in phases),
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
