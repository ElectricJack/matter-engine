"""Native streamed-terrain development capture; no visual acceptance by fiat."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

out = Path(__file__).resolve().parent
repo = out.parents[3]
label, prefix = sys.argv[1:3]
assert re.fullmatch(r'stream-v[0-9]+', label)
assert all(arg == '--no-pom' for arg in sys.argv[3:])
no_pom = '--no-pom' in sys.argv[3:]
build = json.loads((out.parent/'2026-09-16-shared-vt-pixels'/f'{prefix}-build-manifest.json').read_text())
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
assert not build['source_changes'] and all(b['exit'] == 0 for b in build['builds'])
assert all(sha(repo/p) == h for p,h in build['sources'].items())
exe = repo/'MatterEditor/build/windows-msvc/editor.exe'
binary = sha(exe)
assert build['binaries'][str(exe.relative_to(repo))] == binary
dest = out/label
native = Path('/mnt/c/tmp')/f'matter-sector-pom-{label}'
assert not dest.exists() and not native.exists()
dest.mkdir()
win = f'C:/tmp/matter-sector-pom-{label}'
win_repo = 'D:/Shared With Desktop/AI/matter-engine-cpp/'
props = repo/'projects/world_demo/scenes/streaming/StreamMountain/props.json'
original = props.read_bytes()
(dest/'original-props.json').write_bytes(original)
timeline = dest/'start.txt'
timeline.write_text(('set render.pom.enabled false\n' if no_pom else '') + 'wait_event bake.finished 300\n')
command = f'''Get-ChildItem Env:MATTER_* | Remove-Item; & py -3 MatterEngine3/tools/drive.py --world StreamMountain --timeline "{win_repo+str(timeline.relative_to(repo))}" --out-dir "{win}" --editor "{win_repo}MatterEditor/build/windows-msvc/editor.exe" --timeout 900 --hide-ui --env MATTER_HIDE_WINDOW=1 --env MATTER_VK_VALIDATION=1 --env MATTER_WINDOW_WIDTH=1280 --env MATTER_WINDOW_HEIGHT=800 --env MATTER_PROFILE_LOG=1 --env MATTER_VT_TRACE={win}/vt-trace.jsonl --env MATTER_BAKE_TRACE={win}/bake-trace.json; exit $LASTEXITCODE'''
(dest/'launch.ps1').write_text(command+'\n')
views = [
    ('overview', '380 90 1600 420 55 1420'),
    ('sector-close', '383.9 20.3 1600 384.1 18.24 1596'),
    ('sector-grazing', '383.8 20.25 1600 390 20.25 1550'),
]
shots = [f'{view}-{mode}.png' for view,_ in views
         for mode in ('raster-lit','albedo','connections','native-rt','flat')]
commands = []
settles = []
def send(text):
    if no_pom:
        text = text.replace('set render.pom.enabled true', 'set render.pom.enabled false')
    commands.append(text)
    with (native/'cmd.txt').open('a') as f:
        f.write(text+'\n')
    (dest/'commands.txt').write_text('\n'.join(commands)+'\n')

def move(index):
    send('render_path raster\nset viewer.debug.debug_view_mode 0\nset render.pom.horizon_debug 0\n'
         f'set render.pom.enabled true\ncam {views[index][1]}\nwait_idle 4 90')

def capture(index):
    name = views[index][0]
    lines = ['wait_frames 90', f'stats {name}-raster-lit', f'shot {win}/{name}-raster-lit.png',
        'set viewer.debug.debug_view_mode 4','wait_frames 30', f'shot {win}/{name}-albedo.png',
        'set render.pom.horizon_debug 9','wait_frames 30', f'shot {win}/{name}-connections.png',
        'set render.pom.horizon_debug 0','set viewer.debug.debug_view_mode 0',
        'render_path native_rt','set render.gi.enabled true','wait_frames 90',
        f'stats {name}-native-rt', f'shot {win}/{name}-native-rt.png',
        'render_path raster','set render.pom.enabled false','wait_frames 60',
        f'shot {win}/{name}-flat.png','set render.pom.enabled true']
    send('\n'.join(lines))

start = time.monotonic()
phase, index, probe, next_probe = 'bake', 0, 0, start
phase_start, previous, stable_start = start, None, None
code = None
try:
    with (dest/'driver.log').open('w') as log:
        process = subprocess.Popen(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe',
            '-NoProfile','-Command',command],cwd=repo,stdout=log,stderr=subprocess.STDOUT)
        while process.poll() is None:
            now = time.monotonic()
            text = (native/'log.txt').read_text(errors='replace') if (native/'log.txt').exists() else ''
            if phase == 'bake' and 'viewer: bake ready' in text:
                send('set render.lighting.sun_elevation_deg 70\nset render.lighting.sun_azimuth_deg 0\n'
                     'set render.lighting.sun_tint [1,1,1]\nset render.lighting.sky_tint [1,1,1]\n'
                     'set render.lighting.sun_multiplier 1\nset render.lighting.exposure_ev 0\n'
                     'set render.lighting.day_ambient_multiplier 0.5\nset render.lighting.sky_irradiance_multiplier 0.7\n'
                     'set render.fog.density 0\nset render.clouds.layer0_max_density 0\nset render.cloud_shadows.enabled false')
                move(index)
                phase, phase_start, previous, stable_start = 'settle', now, None, None
            if phase == 'settle':
                if now >= next_probe:
                    probe += 1
                    next_probe = now+4
                    send(f'stats ready-{index}-{probe}')
                matches = re.findall(rf'STATSVT,ready-{index}-\d+,active=(\d+),variants=(\d+)/[^\n]*?queue=(\d+),',text)
                state = (len(matches), matches[-1]) if matches else None
                if state and state != previous:
                    active,count,queue = map(int,state[1])
                    if active and count>100 and queue == 0 and previous and state[1] == previous[1]:
                        if stable_start is None: stable_start = now
                    else: stable_start = None
                    previous = state
                settled = stable_start is not None and now-stable_start >= 8
                if settled or now-phase_start > 150:
                    settles.append({'view':views[index][0],'settled':settled,'seconds':now-phase_start,
                                    'vt':previous[1] if previous else None})
                    print(json.dumps(settles[-1]),flush=True)
                    capture(index)
                    phase = 'shots'
            if phase == 'shots' and (native/f'{views[index][0]}-flat.png.done').exists():
                index += 1
                if index == len(views):
                    send('quit')
                    phase = 'shutdown'
                else:
                    move(index)
                    phase, phase_start, previous, stable_start = 'settle', now, None, None
            if now-start>840 and phase != 'shutdown' and (native/'cmd.txt').exists():
                send('quit')
                phase = 'shutdown'
            time.sleep(1)
        code = process.wait()
finally:
    if props.exists(): (dest/'saved-props.json').write_bytes(props.read_bytes())
    props.write_bytes(original)
if native.exists(): shutil.copytree(native,dest,dirs_exist_ok=True)
text = (dest/'log.txt').read_text(errors='replace') if (dest/'log.txt').exists() else ''
errors = [l for l in text.splitlines() if re.search(r'Validation Error|VK_ERROR_DEVICE_LOST|VUID-',l)]
links = [l for l in text.splitlines() if re.search(r'vt-surface-links|destinations overlap|connection.*budget exceeded',l)]
audit = {'editor_exit':code,'seconds':time.monotonic()-start,'settles':settles,
    'editor_sha256':binary,'binary_unchanged':sha(exe)==binary,
    'source_changes':[p for p,h in build['sources'].items() if sha(repo/p)!=h],
    'missing_shots':[n for n in shots if not (dest/n).exists() or not (dest/(n+'.done')).exists()],
    'wrong_dimensions':[n for n in shots if (dest/n).exists() and struct.unpack('>II',(dest/n).read_bytes()[16:24])!=(1280,800)],
    'validation_errors':errors,'connection_warnings':links,
    'command_failures':[l for l in text.splitlines() if re.search(r'unknown prop|set:.*(?:invalid|failed|unknown)|render_path:.*unavailable',l)],
    'rt_effective':'Vulkan RT observed effective=true' in text,
    'visual_approval':False,'automatic_connections_proven':False,'pom_disabled_control':no_pom,
    'scope':'Development capture under neutral diagnostic lighting; native per-border continuity still needs inspection and instrumentation.',
    'stats':[l for l in text.splitlines() if l.startswith(('STATS,','STATSVT,'))]}
audit['capture_passed'] = code == 0 and audit['binary_unchanged'] and audit['rt_effective'] and len(settles)==len(views) and all(s['settled'] for s in settles) and not any(audit[k] for k in ['source_changes','missing_shots','wrong_dimensions','validation_errors','connection_warnings','command_failures'])
(dest/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
print(json.dumps({k:v for k,v in audit.items() if k!='stats'},indent=2),flush=True)
raise SystemExit(0 if audit['capture_passed'] else 1)
